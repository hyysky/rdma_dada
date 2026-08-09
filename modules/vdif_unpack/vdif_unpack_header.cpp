#include "rdma_dada/modules/vdif_unpack/vdif_unpack_header.h"

#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"

#include <cmath>
#include <limits>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {
bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}
bool RequireUint(const pipeline::Metadata& metadata, const char* key,
                 std::uint64_t expected, std::string* error) {
    std::uint64_t actual = 0;
    if (!metadata.GetUint64(key, &actual) || actual != expected)
        return Fail(std::string("raw header ") + key + " conflicts with configuration", error);
    return true;
}
bool RequireText(const pipeline::Metadata& metadata, const char* key,
                 const std::string& expected, std::string* error) {
    std::string actual;
    if (!metadata.GetString(key, &actual) || actual != expected)
        return Fail(std::string("raw header ") + key + " conflicts with configuration", error);
    return true;
}
bool RequireDouble(const pipeline::Metadata& metadata, const char* key,
                   double expected, std::string* error) {
    double actual = 0.0;
    const double scale = std::fabs(expected) > 1.0 ? std::fabs(expected) : 1.0;
    if (!metadata.GetDouble(key, &actual) ||
        std::fabs(actual - expected) > scale * 1.0e-12)
        return Fail(std::string("raw header ") + key + " conflicts with configuration", error);
    return true;
}
bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result, std::string* error) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail("TRANSFER_SIZE exceeds uint64 range", error);
    }
    *result = left * right;
    return true;
}
}  // namespace

bool BuildVdifUnpackOutputHeader(const pipeline::Metadata& input,
                                 const VdifUnpackConfig& config,
                                 const PipelineConfig& pipeline_config,
                                 const PipelineLayout& pipeline_layout,
                                 const VdifUnpackLayout& unpack_layout,
                                 pipeline::Metadata* output,
                                 std::string* error) {
    if (!output) return Fail("output metadata pointer is null", error);
    if (!RequireText(input, "CONFIG_ID", config.config_id, error) ||
        !RequireText(input, "GEOMETRY_ID", config.geometry_id, error) ||
        !RequireText(input, "DATA_STAGE", "RAW", error) ||
        !RequireText(input, "ORDER", "TFP", error) ||
        !RequireText(input, "UTC_START", pipeline_config.utc_start, error) ||
        !RequireUint(input, "PIPELINE_VERSION", 1, error) ||
        !RequireUint(input, "NANT", pipeline_config.nant, error) ||
        !RequireUint(input, "NCHAN", pipeline_config.nchan, error) ||
        !RequireUint(input, "NPOL", pipeline_config.npol, error) ||
        !RequireUint(input, "NBIT", pipeline_config.packet_nbit, error) ||
        !RequireUint(input, "PKT_HEADER", pipeline_config.packet_header_bytes, error) ||
        !RequireUint(input, "PKT_DATA", pipeline_config.packet_payload_bytes, error) ||
        !RequireUint(input, "PKT_NSAMP", pipeline_config.packet_samples, error) ||
        !RequireDouble(input, "PKT_TSAMP", pipeline_config.sample_interval_us, error) ||
        !RequireUint(input, "RECORD_HEADER_BYTES", pipeline_config.packet_header_bytes, error) ||
        !RequireUint(input, "RECORD_BYTES", unpack_layout.raw_record_bytes, error) ||
        !RequireUint(input, "RESOLUTION", unpack_layout.raw_record_bytes, error) ||
        !RequireUint(input, "BYTES_PER_SECOND", pipeline_layout.payload_bytes_per_second, error) ||
        !RequireUint(input, "RAW_BYTES_PER_SECOND", pipeline_layout.raw_bytes_per_second, error) ||
        !RequireUint(input, "FILE_SIZE", pipeline_layout.raw_file_bytes, error)) return false;
    if (unpack_layout.compute_block_bytes != pipeline_layout.compute_block_bytes)
        return Fail("unpack and pipeline compute block geometry conflict", error);
    VdifTimeline timeline = {};
    if (!ParseVdifTimeline(input, pipeline_config, &timeline, error))
        return false;
    std::uint64_t transfer_size = 0;
    if (!CheckedMultiply(timeline.expected_groups, unpack_layout.group_bytes,
                         &transfer_size, error)) {
        return false;
    }

    pipeline::Metadata result = input;
    result.SetString("DATA_STAGE", "UNPACKED");
    result.SetString("ORDER", "ATFP");
    result.SetString("LAYOUT_SCOPE", "BLOCK");
    result.SetString("SAMPLE_FORMAT", "CI8");
    result.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    result.SetString("COMPONENT_ORDER", "IQ");
    result.SetString("ENDIAN", "LITTLE");
    result.SetString("MEMORY", config.output_memory);
    result.SetString("LOSS_POLICY", "ZERO_FILL");
    result.SetUint64("COMPONENT_NBIT", 8);
    result.SetUint64("SAMPLE_NBIT", 16);
    result.SetUint64("RECORD_HEADER_BYTES", 0);
    result.SetUint64("RECORD_BYTES", unpack_layout.compute_block_bytes);
    result.SetUint64("RESOLUTION", pipeline_layout.compute_resolution);
    result.SetUint64("BLOCK_NTIME", pipeline_layout.samples_per_block);
    result.SetUint64("OUTPUT_BLOCK_BYTES", unpack_layout.compute_block_bytes);
    result.SetUint64("BLOCK_BYTES", unpack_layout.compute_block_bytes);
    result.SetUint64("RING_BYTES", pipeline_layout.compute_ring_bytes);
    result.SetUint64("BYTES_PER_SECOND", pipeline_layout.payload_bytes_per_second);
    result.SetUint64("FILE_SIZE", pipeline_layout.compute_file_bytes);
    result.SetUint64("TRANSFER_SIZE", transfer_size);
    result.SetUint64("FIRST_CHANNEL_ID", config.first_channel_id);
    result.SetUint64("UNPACK_WINDOW_BLOCKS", config.window_blocks);
    result.Erase("COMPONENT_SIGNED");
    result.Erase("SOURCE_COMPONENT_SIGNED");
    *output = result;
    return true;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
