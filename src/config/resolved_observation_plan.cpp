#include "rdma_dada/config/resolved_observation_plan.h"

#include "rdma_dada/config/beamform_weight_metadata.h"

#include <limits>
#include <set>

namespace rdma_dada {
namespace {

const std::uint64_t kPicosecondsPerSecond = UINT64_C(1000000000000);
const std::uint64_t kVdifFrameLengthLimit = UINT64_C(0xffffff);
const std::uint64_t kDirectIoSectorBytes = 512U;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                const char* name, std::uint64_t* output,
                std::string* error) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = left + right;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const char* name, std::uint64_t* output,
                     std::string* error) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = left * right;
    return true;
}

bool RoundedBytesPerSecond(std::uint64_t bytes_per_group,
                           std::uint64_t group_period_ps,
                           const char* name,
                           std::uint64_t* output,
                           std::string* error) {
    if (group_period_ps == 0U) {
        return Fail(std::string(name) + " has zero group period", error);
    }
    const __uint128_t numerator =
        static_cast<__uint128_t>(bytes_per_group) * kPicosecondsPerSecond;
    const __uint128_t rounded =
        (numerator + group_period_ps / 2U) / group_period_ps;
    if (rounded > std::numeric_limits<std::uint64_t>::max()) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = static_cast<std::uint64_t>(rounded);
    return true;
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era =
        static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year =
        (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
        year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
        static_cast<std::int64_t>(day_of_era) - 719468;
}

bool ResolveVdifStart(const std::string& text,
                      std::uint8_t* reference_epoch,
                      std::uint32_t* seconds,
                      std::string* error) {
    UtcDateTime value = UtcDateTime();
    if (!ParseUtcDateTime(text, &value, error)) return false;
    const int half = value.month >= 7 ? 1 : 0;
    const int epoch = (value.year - 2000) * 2 + half;
    if (epoch < 0 || epoch > 63) {
        return Fail("UTC_START is outside the six-bit VDIF reference-epoch range",
                    error);
    }
    const unsigned epoch_month = half == 0 ? 1U : 7U;
    const std::int64_t day_delta =
        DaysFromCivil(value.year, static_cast<unsigned>(value.month),
                      static_cast<unsigned>(value.day)) -
        DaysFromCivil(value.year, epoch_month, 1U);
    if (day_delta < 0) return Fail("UTC_START precedes its VDIF epoch", error);
    const std::uint64_t second_value =
        static_cast<std::uint64_t>(day_delta) * 86400U +
        static_cast<std::uint64_t>(value.hour) * 3600U +
        static_cast<std::uint64_t>(value.minute) * 60U +
        static_cast<std::uint64_t>(value.second);
    if (second_value > (UINT64_C(1) << 30U) - 1U) {
        return Fail("UTC_START seconds exceed the VDIF 30-bit field", error);
    }
    *reference_epoch = static_cast<std::uint8_t>(epoch);
    *seconds = static_cast<std::uint32_t>(second_value);
    return true;
}

bool ResolveProcessingGeometry(const ObservationConfig& config,
                               ResolvedObservationPlan* result,
                               std::string* error) {
    if (config.modules.empty()) return true;
    if (config.output_sample_format != "AUTO") {
        return Fail("processing.output.sample_format must be AUTO", error);
    }
    if (config.modules[0].kind != ObservationModuleKind::kBeamform) {
        return Fail("processing.modules must start with beamform", error);
    }
    if (config.modules.size() > 3U) {
        return Fail("processing module chain is too long", error);
    }
    const ObservationModuleConfig& beamform = config.modules[0];
    if (beamform.weights_order != "FPAB2") {
        return Fail("beamform weights_order must be FPAB2", error);
    }
    BeamformWeightMetadata weights = BeamformWeightMetadata();
    if (!ReadBeamformWeightMetadata(beamform.weights_file, &weights, error)) {
        return false;
    }
    if (weights.nchan != config.nchan || weights.npol != config.npol ||
        weights.nant != result->nant) {
        return Fail("beamform weight F/P/A shape conflicts with observation",
                    error);
    }
    std::uint64_t weight_complex_bytes = 0U;
    if (!CheckedMultiply(weights.component_bytes, 2U,
                         "weight complex sample bytes",
                         &weight_complex_bytes, error)) {
        return false;
    }
    if (weight_complex_bytes != result->complex_sample_bytes) {
        return Fail("beamform weight dtype must match antenna input width",
                    error);
    }
    result->nbeam = weights.nbeam;
    if (!CheckedMultiply(result->compute_block_bytes, 4U,
                         "converted CF32 block bytes",
                         &result->converted_block_bytes, error)) {
        return false;
    }
    std::uint64_t beamformed_frame_bytes = 0U;
    if (!CheckedMultiply(config.nchan, config.npol, "beamformed F*P",
                         &beamformed_frame_bytes, error) ||
        !CheckedMultiply(beamformed_frame_bytes, result->nbeam,
                         "beamformed F*P*B", &beamformed_frame_bytes,
                         error) ||
        !CheckedMultiply(beamformed_frame_bytes, 8U,
                         "beamformed CF32 frame bytes",
                         &beamformed_frame_bytes, error) ||
        !CheckedMultiply(result->samples_per_block, beamformed_frame_bytes,
                         "beamformed block bytes",
                         &result->beamformed_block_bytes, error)) {
        return false;
    }
    result->product_block_bytes = result->beamformed_block_bytes;
    result->output_samples_per_block = result->samples_per_block;
    result->output_data_stage = "BEAMFORMED";
    result->output_order = "TFPB";
    result->output_sample_format = "CF32";

    if (config.modules.size() >= 2U) {
        const ObservationModuleKind product = config.modules[1].kind;
        std::uint64_t product_frame_bytes = 0U;
        if (product == ObservationModuleKind::kPower) {
            if (!CheckedMultiply(config.nchan, config.npol, "power F*P",
                                 &product_frame_bytes, error) ||
                !CheckedMultiply(product_frame_bytes, result->nbeam,
                                 "power F*P*B", &product_frame_bytes,
                                 error) ||
                !CheckedMultiply(product_frame_bytes, 4U,
                                 "power F32 frame bytes",
                                 &product_frame_bytes, error)) {
                return false;
            }
            result->output_data_stage = "POWER";
            result->output_order = "TFPB";
        } else if (product == ObservationModuleKind::kStokes) {
            if (config.npol != 2U) {
                return Fail("stokes requires NPOL=2", error);
            }
            if (!CheckedMultiply(config.nchan, result->nbeam,
                                 "stokes F*B", &product_frame_bytes,
                                 error) ||
                !CheckedMultiply(product_frame_bytes, 4U,
                                 "stokes product count",
                                 &product_frame_bytes, error) ||
                !CheckedMultiply(product_frame_bytes, 4U,
                                 "stokes F32 frame bytes",
                                 &product_frame_bytes, error)) {
                return false;
            }
            result->output_data_stage = "POLARIZATION_PRODUCTS";
            result->output_order = "TFBS";
        } else {
            return Fail("beamform may be followed only by power or stokes",
                        error);
        }
        if (!CheckedMultiply(result->samples_per_block, product_frame_bytes,
                             "product block bytes",
                             &result->product_block_bytes, error)) {
            return false;
        }
        result->output_sample_format = "F32";
    }
    result->output_block_bytes = result->product_block_bytes;

    if (config.modules.size() == 3U) {
        const ObservationModuleConfig& integrate = config.modules[2];
        if (integrate.kind != ObservationModuleKind::kIntegrate ||
            integrate.integration_length == 0U) {
            return Fail("third module must be a positive integration", error);
        }
        if (result->samples_per_block % integrate.integration_length != 0U) {
            return Fail("integration length must divide block sample count",
                        error);
        }
        result->output_samples_per_block =
            result->samples_per_block / integrate.integration_length;
        result->output_block_bytes =
            result->product_block_bytes / integrate.integration_length;
        result->output_data_stage =
            result->output_data_stage == "POWER" ?
                "POWER_INTEGRATED" :
                "POLARIZATION_PRODUCTS_INTEGRATED";
    }
    if (!CheckedMultiply(result->output_block_bytes,
                         config.compute_ring_blocks, "output ring bytes",
                         &result->output_ring_bytes, error)) {
        return false;
    }
    return true;
}

}  // namespace

bool ResolveObservationPlan(const ObservationConfig& config,
                            const PacketFormatConfig& wire,
                            ResolvedObservationPlan* plan,
                            std::string* error) {
    if (!plan) return Fail("resolved plan output pointer is null", error);
    if (!ValidatePacketFormatConfig(wire, error)) return false;
    if (config.schema_version != 1U) {
        return Fail("observation schema_version must be 1", error);
    }
    if (config.station_ids.empty() ||
        config.station_ids.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Fail("station_ids must define a non-empty uint32 NANT", error);
    }
    const std::set<std::uint16_t> stations(config.station_ids.begin(),
                                            config.station_ids.end());
    if (stations.size() != config.station_ids.size()) {
        return Fail("station_ids contains a duplicate", error);
    }
    if (config.nchan == 0U || config.nchan > 255U ||
        static_cast<std::uint32_t>(config.first_channel_id) + config.nchan >
            65536U) {
        return Fail("channel selection exceeds Project VDIF fields", error);
    }
    if (config.npol != 1U && config.npol != 2U) {
        return Fail("NPOL must be 1 or 2", error);
    }
    if (config.samples_per_packet == 0U || config.sample_interval_ps == 0U ||
        config.duration_ps == 0U || config.groups_per_block == 0U ||
        config.raw_ring_blocks == 0U || config.compute_ring_blocks == 0U ||
        config.window_blocks < 2U) {
        return Fail("timing, block, ring and window values must be positive",
                    error);
    }
    if (config.samples_per_packet >
        std::numeric_limits<std::uint32_t>::max()) {
        return Fail("samples_per_packet exceeds the VDIF UINT32 field", error);
    }
    std::uint64_t parsed_duration_ps = 0;
    if (!ParseExactSecondsToPicoseconds(config.duration_seconds,
                                        &parsed_duration_ps, error)) {
        return false;
    }
    if (parsed_duration_ps != config.duration_ps) {
        return Fail("duration_seconds conflicts with duration_ps", error);
    }
    if (config.disk_enabled && config.blocks_per_file == 0U) {
        return Fail("enabled storage requires blocks_per_file", error);
    }

    ResolvedObservationPlan result = ResolvedObservationPlan();
    result.source = config;
    result.wire = wire;
    result.nant = static_cast<std::uint32_t>(config.station_ids.size());
    result.complex_sample_bytes = 2U;

    if (!CheckedMultiply(config.samples_per_packet, config.nchan,
                         "packet T*F", &result.payload_bytes, error) ||
        !CheckedMultiply(result.payload_bytes, config.npol,
                         "packet T*F*P", &result.payload_bytes, error) ||
        !CheckedMultiply(result.payload_bytes, result.complex_sample_bytes,
                         "packet payload bytes", &result.payload_bytes, error) ||
        !CheckedAdd(wire.application_header_bytes, result.payload_bytes,
                    "raw record bytes", &result.raw_record_bytes, error)) {
        return false;
    }
    if (result.raw_record_bytes % 8U != 0U ||
        result.raw_record_bytes / 8U > kVdifFrameLengthLimit) {
        return Fail("raw record does not fit the VDIF 24-bit frame-length field",
                    error);
    }
    if (!CheckedMultiply(config.samples_per_packet,
                         config.sample_interval_ps, "group period",
                         &result.group_period_ps, error)) {
        return false;
    }
    if ((kPicosecondsPerSecond - 1U) / result.group_period_ps >
        kVdifFrameLengthLimit) {
        return Fail("group rate exceeds the VDIF 24-bit frame field", error);
    }
    if (config.duration_ps % result.group_period_ps != 0U) {
        return Fail("duration_seconds is not an integer number of groups", error);
    }
    result.expected_groups = config.duration_ps / result.group_period_ps;
    if (result.expected_groups == 0U) {
        return Fail("observation must contain at least one group", error);
    }

    if (!CheckedMultiply(config.groups_per_block, result.nant,
                         "records per block", &result.records_per_block, error) ||
        !CheckedMultiply(config.samples_per_packet, config.groups_per_block,
                         "samples per block", &result.samples_per_block, error) ||
        !CheckedMultiply(result.raw_record_bytes, result.records_per_block,
                         "raw block bytes", &result.raw_block_bytes, error) ||
        !CheckedMultiply(result.payload_bytes, result.records_per_block,
                         "compute block bytes", &result.compute_block_bytes,
                         error) ||
        !CheckedMultiply(config.window_blocks, config.groups_per_block,
                         "window groups", &result.window_groups, error) ||
        !CheckedMultiply(config.window_blocks, result.compute_block_bytes,
                         "window payload bytes", &result.window_payload_bytes,
                         error)) {
        return false;
    }
    std::uint64_t validity_bits = 0;
    std::uint64_t rounded_validity_bits = 0;
    if (!CheckedMultiply(result.window_groups, result.nant,
                         "window validity bits", &validity_bits, error) ||
        !CheckedAdd(validity_bits, 7U, "window validity rounding",
                    &rounded_validity_bits, error)) {
        return false;
    }
    result.window_validity_bytes = rounded_validity_bits / 8U;

    if (!CheckedMultiply(result.raw_block_bytes, config.raw_ring_blocks,
                         "raw ring bytes", &result.raw_ring_bytes, error) ||
        !CheckedMultiply(result.compute_block_bytes, config.compute_ring_blocks,
                         "compute ring bytes", &result.compute_ring_bytes,
                         error)) {
        return false;
    }
    if (config.disk_enabled) {
        if (!CheckedMultiply(result.raw_block_bytes, config.blocks_per_file,
                             "raw file bytes", &result.raw_file_bytes, error) ||
            !CheckedMultiply(result.compute_block_bytes, config.blocks_per_file,
                             "compute file bytes", &result.compute_file_bytes,
                             error)) {
            return false;
        }
    }

    std::uint64_t aggregate_payload_bytes = 0;
    std::uint64_t aggregate_raw_bytes = 0;
    if (!CheckedMultiply(result.payload_bytes, result.nant,
                         "aggregate payload bytes per group",
                         &aggregate_payload_bytes, error) ||
        !CheckedMultiply(result.raw_record_bytes, result.nant,
                         "aggregate raw bytes per group", &aggregate_raw_bytes,
                         error) ||
        !RoundedBytesPerSecond(aggregate_payload_bytes, result.group_period_ps,
                               "payload byte rate",
                               &result.payload_bytes_per_second, error) ||
        !RoundedBytesPerSecond(aggregate_raw_bytes, result.group_period_ps,
                               "raw byte rate", &result.raw_bytes_per_second,
                               error)) {
        return false;
    }

    if (config.disk_enabled && config.direct_io) {
        if (result.raw_block_bytes % kDirectIoSectorBytes != 0U ||
            result.compute_block_bytes % kDirectIoSectorBytes != 0U) {
            return Fail("DIRECT_IO requires 512-byte-aligned ring blocks", error);
        }
        std::uint64_t compute_resolution = 0;
        std::uint64_t raw_file_multiple = 0;
        std::uint64_t compute_file_multiple = 0;
        if (!CheckedMultiply(config.nchan, config.npol,
                             "compute F*P", &compute_resolution, error) ||
            !CheckedMultiply(compute_resolution, result.nant,
                             "compute F*P*A", &compute_resolution, error) ||
            !CheckedMultiply(compute_resolution, result.complex_sample_bytes,
                             "compute resolution", &compute_resolution, error) ||
            !CheckedMultiply(result.raw_record_bytes, kDirectIoSectorBytes,
                             "raw direct-I/O file multiple", &raw_file_multiple,
                             error) ||
            !CheckedMultiply(compute_resolution, kDirectIoSectorBytes,
                             "compute direct-I/O file multiple",
                             &compute_file_multiple, error)) {
            return false;
        }
        if (result.raw_file_bytes % raw_file_multiple != 0U ||
            result.compute_file_bytes % compute_file_multiple != 0U) {
            return Fail("DIRECT_IO file size is not 512*RESOLUTION aligned",
                        error);
        }
    }

    if (!ResolveVdifStart(config.utc_start,
                          &result.group_start_reference_epoch,
                          &result.group_start_seconds, error)) {
        return false;
    }
    result.group_start_frame = 0U;
    if (!ResolveProcessingGeometry(config, &result, error)) return false;
    *plan = result;
    return true;
}

bool BuildPipelineRuntimeFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    PipelineConfig* config,
    PipelineLayout* layout,
    std::string* error) {
    if (!config || !layout) {
        return Fail("pipeline runtime output pointer is null", error);
    }
    ResolvedObservationPlan verified;
    if (!ResolveObservationPlan(plan.source, plan.wire, &verified, error)) {
        return false;
    }
    if (verified.nant != plan.nant ||
        verified.complex_sample_bytes != plan.complex_sample_bytes ||
        verified.payload_bytes != plan.payload_bytes ||
        verified.raw_record_bytes != plan.raw_record_bytes ||
        verified.group_period_ps != plan.group_period_ps ||
        verified.expected_groups != plan.expected_groups ||
        verified.records_per_block != plan.records_per_block ||
        verified.samples_per_block != plan.samples_per_block ||
        verified.raw_block_bytes != plan.raw_block_bytes ||
        verified.compute_block_bytes != plan.compute_block_bytes ||
        verified.window_groups != plan.window_groups ||
        verified.window_payload_bytes != plan.window_payload_bytes ||
        verified.window_validity_bytes != plan.window_validity_bytes ||
        verified.raw_ring_bytes != plan.raw_ring_bytes ||
        verified.compute_ring_bytes != plan.compute_ring_bytes ||
        verified.nbeam != plan.nbeam ||
        verified.converted_block_bytes != plan.converted_block_bytes ||
        verified.beamformed_block_bytes != plan.beamformed_block_bytes ||
        verified.product_block_bytes != plan.product_block_bytes ||
        verified.output_samples_per_block != plan.output_samples_per_block ||
        verified.output_block_bytes != plan.output_block_bytes ||
        verified.output_ring_bytes != plan.output_ring_bytes ||
        verified.output_data_stage != plan.output_data_stage ||
        verified.output_order != plan.output_order ||
        verified.output_sample_format != plan.output_sample_format ||
        verified.raw_file_bytes != plan.raw_file_bytes ||
        verified.compute_file_bytes != plan.compute_file_bytes ||
        verified.payload_bytes_per_second != plan.payload_bytes_per_second ||
        verified.raw_bytes_per_second != plan.raw_bytes_per_second ||
        verified.group_start_reference_epoch !=
            plan.group_start_reference_epoch ||
        verified.group_start_seconds != plan.group_start_seconds ||
        verified.group_start_frame != plan.group_start_frame) {
        return Fail("resolved plan contains stale derived geometry", error);
    }

    PipelineConfig runtime = PipelineConfig();
    runtime.nant = plan.nant;
    runtime.nchan = plan.source.nchan;
    runtime.npol = plan.source.npol;
    runtime.payload_order = "TFP";
    runtime.packet_header_bytes = plan.wire.application_header_bytes;
    runtime.packet_payload_bytes = plan.payload_bytes;
    runtime.packet_samples = plan.source.samples_per_packet;
    runtime.packet_nbit = 16U;
    runtime.sample_interval_us =
        static_cast<double>(plan.source.sample_interval_ps) / 1000000.0;
    runtime.records_per_block = plan.records_per_block;
    runtime.raw_ring_blocks = plan.source.raw_ring_blocks;
    runtime.compute_ring_blocks = plan.source.compute_ring_blocks;
    runtime.file_blocks = plan.source.blocks_per_file;
    runtime.disk_enabled = plan.source.disk_enabled;
    runtime.direct_io = plan.source.direct_io;
    runtime.utc_start = plan.source.utc_start;
    PipelineLayout computed;
    if (!ComputePipelineLayout(runtime, &computed, error)) return false;
    if (computed.raw_record_bytes != plan.raw_record_bytes ||
        computed.samples_per_block != plan.samples_per_block ||
        computed.raw_block_bytes != plan.raw_block_bytes ||
        computed.compute_block_bytes != plan.compute_block_bytes ||
        computed.raw_ring_bytes != plan.raw_ring_bytes ||
        computed.compute_ring_bytes != plan.compute_ring_bytes ||
        computed.raw_file_bytes != plan.raw_file_bytes ||
        computed.compute_file_bytes != plan.compute_file_bytes ||
        computed.payload_bytes_per_second != plan.payload_bytes_per_second ||
        computed.raw_bytes_per_second != plan.raw_bytes_per_second) {
        return Fail("pipeline runtime conflicts with resolved plan", error);
    }
    *config = runtime;
    *layout = computed;
    return true;
}

}  // namespace rdma_dada
