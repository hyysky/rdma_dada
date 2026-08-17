#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"

#include <cmath>
#include <limits>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

const std::uint64_t kPicosecondsPerSecond = UINT64_C(1000000000000);
const std::uint64_t kMaximumVdifSeconds = (UINT64_C(1) << 30U) - 1U;
const std::uint64_t kMaximumVdifFrame = (UINT64_C(1) << 24U) - 1U;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                std::uint64_t* result) {
    if (!result || right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    *result = left + right;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0U &&
         right > std::numeric_limits<std::uint64_t>::max() / left))
        return false;
    *result = left * right;
    return true;
}

bool SampleIntervalPicoseconds(const PipelineConfig& pipeline,
                               std::uint64_t* sample_interval_ps,
                               std::string* error) {
    if (!sample_interval_ps || !(pipeline.sample_interval_us > 0.0) ||
        !std::isfinite(pipeline.sample_interval_us)) {
        return Fail("PKT_TSAMP must be positive and finite", error);
    }
    const long double value =
        static_cast<long double>(pipeline.sample_interval_us) * 1000000.0L;
    const long double rounded = std::floor(value + 0.5L);
    const long double magnitude = std::fabs(value) > 1.0L
                                      ? std::fabs(value)
                                      : 1.0L;
    const long double tolerance =
        magnitude * static_cast<long double>(
                        std::numeric_limits<double>::epsilon()) * 4.0L;
    if (rounded < 1.0L ||
        rounded > static_cast<long double>(
                      std::numeric_limits<std::uint64_t>::max()) ||
        std::fabs(value - rounded) > tolerance) {
        return Fail("PKT_TSAMP must be exactly representable in integer "
                    "picoseconds", error);
    }
    *sample_interval_ps = static_cast<std::uint64_t>(rounded);
    return true;
}

bool RequireUint(const pipeline::Metadata& header, const char* name,
                 std::uint64_t* value, std::string* error) {
    if (!header.GetUint64(name, value))
        return Fail(std::string("missing or invalid ") + name, error);
    return true;
}

bool ValidateTimelineShape(const VdifTimeline& timeline,
                           std::string* error) {
    if (timeline.group_period_ps == 0U)
        return Fail("GROUP_PERIOD_PS must be positive", error);
    if (timeline.groups_per_second > kMaximumVdifFrame + 1U)
        return Fail("groups_per_second exceeds VDIF frame range", error);
    if (timeline.start_reference_epoch > 63U)
        return Fail("GROUP_START_REFERENCE_EPOCH exceeds six-bit range",
                    error);
    if (timeline.start_seconds > kMaximumVdifSeconds)
        return Fail("GROUP_START_SECONDS exceeds VDIF 30-bit range", error);
    if (timeline.start_frame != 0U)
        return Fail("GROUP_START_FRAME must be zero in schema version 1",
                    error);
    if (timeline.expected_groups == 0U)
        return Fail("EXPECTED_GROUPS must be positive", error);
    return true;
}

}  // namespace

bool ParseVdifTimeline(const pipeline::Metadata& header,
                       const PipelineConfig& pipeline,
                       VdifTimeline* timeline,
                       std::string* error) {
    if (!timeline) return Fail("timeline output pointer is null", error);

    std::uint64_t group_period_ps = 0;
    std::uint64_t reference_epoch = 0;
    std::uint64_t start_seconds = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t expected_groups = 0;
    if (!RequireUint(header, "GROUP_PERIOD_PS", &group_period_ps, error) ||
        !RequireUint(header, "GROUP_START_REFERENCE_EPOCH", &reference_epoch,
                     error) ||
        !RequireUint(header, "GROUP_START_SECONDS", &start_seconds, error) ||
        !RequireUint(header, "GROUP_START_FRAME", &start_frame, error) ||
        !RequireUint(header, "EXPECTED_GROUPS", &expected_groups, error)) {
        return false;
    }
    if (reference_epoch > std::numeric_limits<std::uint8_t>::max() ||
        start_seconds > std::numeric_limits<std::uint32_t>::max() ||
        start_frame > std::numeric_limits<std::uint32_t>::max()) {
        return Fail("timeline header field exceeds storage range", error);
    }

    VdifTimeline parsed = {};
    parsed.group_period_ps = group_period_ps;
    parsed.groups_per_second = 0U;
    parsed.start_reference_epoch =
        static_cast<std::uint8_t>(reference_epoch);
    parsed.start_seconds = static_cast<std::uint32_t>(start_seconds);
    parsed.start_frame = static_cast<std::uint32_t>(start_frame);
    parsed.expected_groups = expected_groups;
    if (!ValidateTimelineShape(parsed, error)) return false;

    std::uint64_t sample_interval_ps = 0;
    std::uint64_t expected_period_ps = 0;
    if (pipeline.packet_samples == 0U)
        return Fail("PKT_NSAMP must be positive", error);
    if (!SampleIntervalPicoseconds(pipeline, &sample_interval_ps, error))
        return false;
    if (!CheckedMultiply(pipeline.packet_samples, sample_interval_ps,
                         &expected_period_ps)) {
        return Fail("PKT_NSAMP times PKT_TSAMP overflows uint64", error);
    }
    if (parsed.group_period_ps != expected_period_ps) {
        return Fail("GROUP_PERIOD_PS conflicts with PKT_NSAMP and PKT_TSAMP",
                    error);
    }

    *timeline = parsed;
    return true;
}

bool VdifOrdinalToTime(const VdifTimeline& timeline,
                       std::uint64_t ordinal,
                       std::uint32_t* seconds,
                       std::uint32_t* frame,
                       std::string* error) {
    if (!seconds || !frame)
        return Fail("timeline time output pointer is null", error);
    if (!ValidateTimelineShape(timeline, error)) return false;
    if (ordinal >= timeline.expected_groups)
        return Fail("group ordinal is outside EXPECTED_GROUPS", error);

    if (timeline.groups_per_second != 0U) {
        const std::uint64_t delta_seconds =
            ordinal / timeline.groups_per_second;
        std::uint64_t absolute_seconds = 0;
        if (!CheckedAdd(timeline.start_seconds, delta_seconds,
                        &absolute_seconds) ||
            absolute_seconds > kMaximumVdifSeconds) {
            return Fail("group time exceeds VDIF 30-bit seconds range", error);
        }
        *seconds = static_cast<std::uint32_t>(absolute_seconds);
        *frame = static_cast<std::uint32_t>(
            ordinal % timeline.groups_per_second);
        return true;
    }

    std::uint64_t elapsed_ps = 0;
    if (!CheckedMultiply(ordinal, timeline.group_period_ps, &elapsed_ps))
        return Fail("group ordinal time overflows uint64", error);
    const std::uint64_t delta_seconds =
        elapsed_ps / kPicosecondsPerSecond;
    std::uint64_t absolute_seconds = 0;
    if (!CheckedAdd(timeline.start_seconds, delta_seconds,
                    &absolute_seconds) ||
        absolute_seconds > kMaximumVdifSeconds) {
        return Fail("group time exceeds VDIF 30-bit seconds range", error);
    }
    const std::uint64_t within_second_ps =
        elapsed_ps % kPicosecondsPerSecond;
    const std::uint64_t frame_number =
        within_second_ps / timeline.group_period_ps;
    if (frame_number > kMaximumVdifFrame)
        return Fail("group frame exceeds VDIF 24-bit frame range", error);

    *seconds = static_cast<std::uint32_t>(absolute_seconds);
    *frame = static_cast<std::uint32_t>(frame_number);
    return true;
}

bool VdifTimeToOrdinal(const VdifTimeline& timeline,
                       std::uint8_t reference_epoch,
                       std::uint32_t seconds,
                       std::uint32_t frame,
                       std::uint64_t* ordinal,
                       std::string* error) {
    if (!ordinal) return Fail("group ordinal output pointer is null", error);
    if (!ValidateTimelineShape(timeline, error)) return false;
    if (reference_epoch != timeline.start_reference_epoch)
        return Fail("packet reference epoch conflicts with timeline", error);
    if (seconds < timeline.start_seconds)
        return Fail("packet time precedes timeline start", error);
    if (seconds > kMaximumVdifSeconds || frame > kMaximumVdifFrame)
        return Fail("packet time exceeds Project VDIF field range", error);

    if (timeline.groups_per_second != 0U) {
        if (frame >= timeline.groups_per_second)
            return Fail("packet frame exceeds fixed groups per second", error);
        std::uint64_t base = 0;
        if (!CheckedMultiply(
                static_cast<std::uint64_t>(seconds - timeline.start_seconds),
                timeline.groups_per_second, &base) ||
            !CheckedAdd(base, frame, ordinal)) {
            return Fail("fixed-rate packet ordinal overflows", error);
        }
        if (*ordinal >= timeline.expected_groups)
            return Fail("packet time is outside EXPECTED_GROUPS", error);
        return true;
    }

    const std::uint64_t delta_seconds =
        static_cast<std::uint64_t>(seconds - timeline.start_seconds);
    std::uint64_t elapsed_second_ps = 0;
    if (!CheckedMultiply(delta_seconds, kPicosecondsPerSecond,
                         &elapsed_second_ps)) {
        return Fail("packet seconds-to-picoseconds conversion overflows",
                    error);
    }
    std::uint64_t first_ordinal =
        elapsed_second_ps / timeline.group_period_ps;
    if (elapsed_second_ps % timeline.group_period_ps != 0U) {
        if (first_ordinal == std::numeric_limits<std::uint64_t>::max())
            return Fail("first ordinal ceiling division overflows", error);
        ++first_ordinal;
    }
    std::uint64_t candidate = 0;
    if (!CheckedAdd(first_ordinal, frame, &candidate))
        return Fail("packet frame ordinal overflows", error);
    if (candidate >= timeline.expected_groups)
        return Fail("packet time is outside EXPECTED_GROUPS", error);

    std::uint32_t regenerated_seconds = 0;
    std::uint32_t regenerated_frame = 0;
    if (!VdifOrdinalToTime(timeline, candidate, &regenerated_seconds,
                           &regenerated_frame, error)) {
        return false;
    }
    if (regenerated_seconds != seconds || regenerated_frame != frame)
        return Fail("packet time tuple is not a canonical group time", error);

    *ordinal = candidate;
    return true;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
