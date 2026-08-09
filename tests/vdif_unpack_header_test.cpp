#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_header.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void ExpectText(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, const std::string& expected) {
    std::string actual;
    Expect(metadata.GetString(key, &actual) && actual == expected,
           key + " expected " + expected + ", got " + actual);
}
void ExpectUint(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, std::uint64_t expected) {
    std::uint64_t actual = 0;
    Expect(metadata.GetUint64(key, &actual) && actual == expected,
           key + " mismatch");
}
}  // namespace

int main() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    rdma_dada::PipelineConfig pipeline = {};
    pipeline.nant = 4; pipeline.nchan = 1; pipeline.npol = 2;
    pipeline.payload_order = "TFP"; pipeline.packet_header_bytes = 32;
    pipeline.packet_payload_bytes = 2048; pipeline.packet_samples = 512;
    pipeline.packet_nbit = 16; pipeline.sample_interval_us = 1.0;
    pipeline.records_per_block = 16; pipeline.raw_ring_blocks = 8;
    pipeline.compute_ring_blocks = 8; pipeline.file_blocks = 3;
    pipeline.disk_enabled = true; pipeline.direct_io = false;
    pipeline.utc_start = "2026-08-01-00:00:00";
    rdma_dada::PipelineLayout pipeline_layout = {};
    std::string error;
    Expect(rdma_dada::ComputePipelineLayout(pipeline, &pipeline_layout, &error),
           "pipeline layout computes: " + error);

    unpack::VdifUnpackConfig config = {};
    config.first_channel_id = 100; config.window_blocks = 2;
    config.output_memory = "HOST"; config.antenna_map = {10, 11, 12, 13};
    config.config_id = "config-id-fixture";
    config.geometry_id = "geometry-id-fixture";
    unpack::VdifUnpackLayout layout = {};
    layout.raw_record_bytes = 2080; layout.records_per_raw_block = 16;
    layout.group_bytes = 8192; layout.window_capacity_groups = 8;
    layout.window_bytes = 65536; layout.compute_block_bytes = 32768;

    rdma_dada::pipeline::Metadata input;
    input.SetString("DATA_STAGE", "RAW"); input.SetString("ORDER", "TFP");
    input.SetString("CONFIG_ID", config.config_id);
    input.SetString("GEOMETRY_ID", config.geometry_id);
    input.SetString("UTC_START", pipeline.utc_start);
    input.SetString("TELESCOPE", "PFT");
    input.SetUint64("PIPELINE_VERSION", 1);
    input.SetUint64("NANT", 4); input.SetUint64("NCHAN", 1);
    input.SetUint64("NPOL", 2); input.SetUint64("NBIT", 16);
    input.SetUint64("PKT_HEADER", 32); input.SetUint64("PKT_DATA", 2048);
    input.SetUint64("PKT_NSAMP", 512); input.SetUint64("RECORD_HEADER_BYTES", 32);
    input.SetUint64("RECORD_BYTES", 2080); input.SetUint64("RESOLUTION", 2080);
    input.SetDouble("PKT_TSAMP", 1.0);
    input.SetUint64("BYTES_PER_SECOND", 16000000);
    input.SetUint64("RAW_BYTES_PER_SECOND", 16250000);
    input.SetUint64("FILE_SIZE", 99840);
    input.SetUint64("BLOCK_BYTES", pipeline_layout.raw_block_bytes);
    input.SetUint64("RING_BYTES", pipeline_layout.raw_ring_bytes);
    input.SetUint64("GROUP_PERIOD_PS", 512000000);
    input.SetUint64("GROUP_START_REFERENCE_EPOCH", 11);
    input.SetUint64("GROUP_START_SECONDS", 1234);
    input.SetUint64("GROUP_START_FRAME", 0);
    input.SetUint64("EXPECTED_GROUPS", 12);

    rdma_dada::pipeline::Metadata output;
    Expect(unpack::BuildVdifUnpackOutputHeader(input, config, pipeline,
                                               pipeline_layout, layout,
                                               &output, &error),
           "output header builds: " + error);
    ExpectText(output, "TELESCOPE", "PFT");
    ExpectText(output, "CONFIG_ID", config.config_id);
    ExpectText(output, "GEOMETRY_ID", config.geometry_id);
    ExpectText(output, "UTC_START", pipeline.utc_start);
    ExpectText(output, "DATA_STAGE", "UNPACKED");
    ExpectText(output, "ORDER", "ATFP");
    ExpectText(output, "LAYOUT_SCOPE", "BLOCK");
    ExpectText(output, "SAMPLE_FORMAT", "CI8");
    ExpectText(output, "SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    ExpectText(output, "COMPONENT_ORDER", "IQ");
    ExpectText(output, "ENDIAN", "LITTLE");
    ExpectText(output, "MEMORY", "HOST");
    ExpectText(output, "LOSS_POLICY", "ZERO_FILL");
    ExpectUint(output, "COMPONENT_NBIT", 8);
    ExpectUint(output, "SAMPLE_NBIT", 16);
    ExpectUint(output, "RECORD_HEADER_BYTES", 0);
    ExpectUint(output, "RECORD_BYTES", 32768);
    ExpectUint(output, "RESOLUTION", 16);
    ExpectUint(output, "BLOCK_NTIME", 2048);
    ExpectUint(output, "OUTPUT_BLOCK_BYTES", 32768);
    ExpectUint(output, "BLOCK_BYTES", 32768);
    ExpectUint(output, "RING_BYTES", pipeline_layout.compute_ring_bytes);
    ExpectUint(output, "FILE_SIZE", 98304);
    ExpectUint(output, "TRANSFER_SIZE", 98304);
    ExpectUint(output, "FIRST_CHANNEL_ID", 100);
    ExpectUint(output, "UNPACK_WINDOW_BLOCKS", 2);
    ExpectUint(output, "GROUP_PERIOD_PS", 512000000);
    ExpectUint(output, "GROUP_START_REFERENCE_EPOCH", 11);
    ExpectUint(output, "GROUP_START_SECONDS", 1234);
    ExpectUint(output, "GROUP_START_FRAME", 0);
    ExpectUint(output, "EXPECTED_GROUPS", 12);
    Expect(!output.Has("COMPONENT_SIGNED"),
           "UNPACKED header does not publish COMPONENT_SIGNED");
    Expect(!output.Has("SOURCE_COMPONENT_SIGNED"),
           "UNPACKED header does not publish SOURCE_COMPONENT_SIGNED");

    rdma_dada::pipeline::Metadata conflict = input;
    rdma_dada::pipeline::Metadata unpublished;
    conflict.SetString("CONFIG_ID", "wrong-config-id");
    unpublished.SetString("SENTINEL", "unchanged");
    Expect(!unpack::BuildVdifUnpackOutputHeader(conflict, config, pipeline,
                                                pipeline_layout, layout,
                                                &unpublished, &error),
           "CONFIG_ID mismatch is rejected");
    ExpectText(unpublished, "SENTINEL", "unchanged");

    conflict = input;
    conflict.SetString("GEOMETRY_ID", "wrong-geometry-id");
    unpublished = rdma_dada::pipeline::Metadata();
    unpublished.SetString("SENTINEL", "unchanged");
    Expect(!unpack::BuildVdifUnpackOutputHeader(conflict, config, pipeline,
                                                pipeline_layout, layout,
                                                &unpublished, &error),
           "GEOMETRY_ID mismatch is rejected");
    ExpectText(unpublished, "SENTINEL", "unchanged");

    conflict = input;
    conflict.SetUint64("NANT", 5);
    unpublished = rdma_dada::pipeline::Metadata();
    unpublished.SetString("SENTINEL", "unchanged");
    Expect(!unpack::BuildVdifUnpackOutputHeader(conflict, config, pipeline,
                                                pipeline_layout, layout,
                                                &unpublished, &error),
           "raw header geometry conflict rejected");
    ExpectText(unpublished, "SENTINEL", "unchanged");

    conflict = input;
    conflict.Erase("EXPECTED_GROUPS");
    unpublished = rdma_dada::pipeline::Metadata();
    unpublished.SetString("SENTINEL", "unchanged");
    Expect(!unpack::BuildVdifUnpackOutputHeader(conflict, config, pipeline,
                                                pipeline_layout, layout,
                                                &unpublished, &error),
           "missing timeline boundary is rejected");
    ExpectText(unpublished, "SENTINEL", "unchanged");

    if (failures) return 1;
    std::cout << "vdif_unpack_header_test passed\n";
    return 0;
}
