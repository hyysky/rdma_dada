#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"
#include "rdma_dada/pipeline/metadata.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

rdma_dada::PipelineConfig MakePipeline(double sample_interval_us,
                                       std::uint64_t packet_samples) {
    rdma_dada::PipelineConfig pipeline = {};
    pipeline.packet_samples = packet_samples;
    pipeline.sample_interval_us = sample_interval_us;
    return pipeline;
}

rdma_dada::pipeline::Metadata MakeHeader(std::uint64_t period_ps,
                                         std::uint64_t epoch,
                                         std::uint64_t seconds,
                                         std::uint64_t frame,
                                         std::uint64_t expected_groups) {
    rdma_dada::pipeline::Metadata header;
    header.SetUint64("GROUP_PERIOD_PS", period_ps);
    header.SetUint64("GROUP_START_REFERENCE_EPOCH", epoch);
    header.SetUint64("GROUP_START_SECONDS", seconds);
    header.SetUint64("GROUP_START_FRAME", frame);
    header.SetUint64("EXPECTED_GROUPS", expected_groups);
    return header;
}

void TestNonIntegralGroupsPerSecondRoundTrip() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const rdma_dada::PipelineConfig pipeline = MakePipeline(200000.0, 3U);
    const rdma_dada::pipeline::Metadata header =
        MakeHeader(UINT64_C(600000000000), 12U, 100U, 0U, 4U);
    unpack::VdifTimeline timeline = {};
    std::string error;
    Expect(unpack::ParseVdifTimeline(header, pipeline, &timeline, &error),
           "non-integral groups/second timeline parses: " + error);
    Expect(timeline.group_period_ps == UINT64_C(600000000000),
           "group period is preserved");
    Expect(timeline.start_reference_epoch == 12U,
           "reference epoch is preserved");
    Expect(timeline.start_seconds == 100U, "start seconds are preserved");
    Expect(timeline.start_frame == 0U, "start frame is zero");
    Expect(timeline.expected_groups == 4U, "expected boundary is preserved");

    const std::uint32_t expected_seconds[] = {100U, 100U, 101U, 101U};
    const std::uint32_t expected_frames[] = {0U, 1U, 0U, 1U};
    for (std::uint64_t ordinal = 0; ordinal < 4U; ++ordinal) {
        std::uint32_t seconds = 0;
        std::uint32_t frame = 0;
        error.clear();
        Expect(unpack::VdifOrdinalToTime(timeline, ordinal, &seconds, &frame,
                                         &error),
               "ordinal converts to canonical time: " + error);
        Expect(seconds == expected_seconds[ordinal],
               "ordinal seconds match expected rollover");
        Expect(frame == expected_frames[ordinal],
               "ordinal frame matches expected rollover");

        std::uint64_t recovered = std::numeric_limits<std::uint64_t>::max();
        error.clear();
        Expect(unpack::VdifTimeToOrdinal(
                   timeline, 12U, seconds, frame, &recovered, &error),
               "canonical time maps back to ordinal: " + error);
        Expect(recovered == ordinal, "timeline round trip is exact");
    }
}

void TestTimeValidation() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const rdma_dada::PipelineConfig pipeline = MakePipeline(200000.0, 3U);
    const rdma_dada::pipeline::Metadata header =
        MakeHeader(UINT64_C(600000000000), 12U, 100U, 0U, 4U);
    unpack::VdifTimeline timeline = {};
    std::string error;
    Expect(unpack::ParseVdifTimeline(header, pipeline, &timeline, &error),
           "validation timeline parses: " + error);

    std::uint64_t ordinal = 0;
    Expect(!unpack::VdifTimeToOrdinal(timeline, 13U, 100U, 0U, &ordinal,
                                      &error),
           "mismatched reference epoch is rejected");
    Expect(!unpack::VdifTimeToOrdinal(timeline, 12U, 99U, 0U, &ordinal,
                                      &error),
           "time before configured start is rejected");
    Expect(!unpack::VdifTimeToOrdinal(timeline, 12U, 100U, 2U, &ordinal,
                                      &error),
           "non-canonical frame within a second is rejected");
    Expect(!unpack::VdifTimeToOrdinal(timeline, 12U, 102U, 0U, &ordinal,
                                      &error),
           "time at exclusive expected boundary is rejected");

    std::uint32_t seconds = 0;
    std::uint32_t frame = 0;
    Expect(!unpack::VdifOrdinalToTime(timeline, 4U, &seconds, &frame, &error),
           "ordinal at exclusive expected boundary is rejected");
    Expect(!unpack::VdifOrdinalToTime(timeline, 0U, NULL, &frame, &error),
           "null seconds output is rejected");
    Expect(!unpack::VdifTimeToOrdinal(timeline, 12U, 100U, 0U, NULL, &error),
           "null ordinal output is rejected");
}

void TestHeaderValidation() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const rdma_dada::PipelineConfig pipeline = MakePipeline(1.0, 512U);
    const std::uint64_t period = UINT64_C(512000000);
    const rdma_dada::pipeline::Metadata valid =
        MakeHeader(period, 7U, 1234U, 0U, 100U);
    unpack::VdifTimeline timeline = {};
    std::string error;
    Expect(unpack::ParseVdifTimeline(valid, pipeline, &timeline, &error),
           "integer-picosecond header parses: " + error);

    rdma_dada::pipeline::Metadata invalid = valid;
    invalid.Erase("EXPECTED_GROUPS");
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "missing expected boundary is rejected");
    invalid = valid;
    invalid.SetUint64("GROUP_PERIOD_PS", 0U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "zero period is rejected");
    invalid = valid;
    invalid.SetUint64("EXPECTED_GROUPS", 0U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "zero expected group count is rejected");
    invalid = valid;
    invalid.SetUint64("GROUP_START_FRAME", 1U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "non-zero first-version start frame is rejected");
    invalid = valid;
    invalid.SetUint64("GROUP_START_REFERENCE_EPOCH", 64U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "reference epoch outside six bits is rejected");
    invalid = valid;
    invalid.SetUint64("GROUP_START_SECONDS", UINT64_C(1) << 30U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "start seconds outside VDIF 30-bit field is rejected");
    invalid = valid;
    invalid.SetUint64("GROUP_PERIOD_PS", period + 1U);
    Expect(!unpack::ParseVdifTimeline(invalid, pipeline, &timeline, &error),
           "period conflicting with packet samples and PKT_TSAMP is rejected");

    const rdma_dada::PipelineConfig fractional_ps =
        MakePipeline(0.0000005, 1U);
    invalid = MakeHeader(1U, 7U, 1234U, 0U, 100U);
    Expect(!unpack::ParseVdifTimeline(invalid, fractional_ps, &timeline,
                                      &error),
           "sub-picosecond sample interval is rejected");

    const rdma_dada::PipelineConfig overflowing = MakePipeline(
        1000000.0, std::numeric_limits<std::uint64_t>::max());
    invalid = MakeHeader(1U, 7U, 1234U, 0U, 100U);
    Expect(!unpack::ParseVdifTimeline(invalid, overflowing, &timeline,
                                      &error),
           "packet-period multiplication overflow is rejected");
}

void TestConversionOverflowIsRejected() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    unpack::VdifTimeline timeline = {};
    timeline.group_period_ps = 1U;
    timeline.start_reference_epoch = 1U;
    timeline.start_seconds = 0U;
    timeline.start_frame = 0U;
    timeline.expected_groups = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t ordinal = 0;
    std::string error;
    Expect(!unpack::VdifTimeToOrdinal(timeline, 1U, 1000000000U, 0U,
                                      &ordinal, &error),
           "seconds-to-picoseconds overflow is rejected");
}

}  // namespace

int main() {
    TestNonIntegralGroupsPerSecondRoundTrip();
    TestTimeValidation();
    TestHeaderValidation();
    TestConversionOverflowIsRejected();
    if (failures != 0) return 1;
    std::cout << "vdif_timeline_test passed\n";
    return 0;
}
