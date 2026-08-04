#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/pipeline/dada_header_builder.h"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pipeline_config_test CONFIG\n";
        return 2;
    }

    rdma_dada::PipelineConfig config;
    std::string error;
    Expect(rdma_dada::LoadPipelineConfig(argv[1], &config, &error),
           "example config should load: " + error);
    Expect(config.disk_enabled, "converted legacy config keeps disk sink enabled");

    rdma_dada::PipelineLayout layout;
    error.clear();
    Expect(rdma_dada::ComputePipelineLayout(config, &layout, &error),
           "example config should validate: " + error);
    if (failures == 0) {
        Expect(layout.raw_record_bytes == 8256, "raw record includes 64-byte header");
        Expect(layout.compute_record_bytes == 8192, "compute record excludes header");
        Expect(layout.packets_per_antenna_per_block == 4096,
               "block contains 4096 UDP packets per antenna");
        Expect(layout.samples_per_block == UINT64_C(2097152),
               "T is packet samples times packets per antenna per block");
        Expect(layout.raw_resolution == 8256, "raw RESOLUTION is one raw record");
        Expect(layout.compute_resolution == 8192,
               "compute RESOLUTION is one payload record");
        Expect(layout.raw_block_bytes == UINT64_C(135266304), "raw block size");
        Expect(layout.compute_block_bytes == UINT64_C(134217728), "compute block size");
        Expect(layout.raw_file_bytes == UINT64_C(1352663040),
               "raw file is FILE_BLOCKS raw blocks");
        Expect(layout.compute_file_bytes == UINT64_C(1342177280),
               "compute file is FILE_BLOCKS compute blocks");
        Expect(layout.payload_bytes_per_second == UINT64_C(16000000000),
               "BYTES_PER_SECOND is payload-only rate");
        Expect(layout.raw_bytes_per_second == UINT64_C(16125000000),
               "raw rate includes packet header");
    }

    rdma_dada::PipelineConfig invalid = config;
    invalid.records_per_block = 4;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "DIRECT_IO must reject a non-512-byte raw block");

    invalid = config;
    invalid.disk_enabled = false;
    invalid.records_per_block = 3;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "a block must contain complete NANT-sized UDP packet groups");

    invalid = config;
    invalid.packet_header_bytes = 32;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "contract v1 must reject a non-64-byte application header");

    invalid = config;
    invalid.packet_nbit = 8;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "contract v1 must reject NBIT other than 16");

    invalid = config;
    invalid.packet_payload_bytes = std::numeric_limits<std::uint64_t>::max();
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "size arithmetic must reject overflow");

    invalid = config;
    invalid.disk_enabled = false;
    invalid.file_blocks = 0;
    error.clear();
    Expect(rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "disabled disk sink must not require a file size: " + error);
    Expect(layout.raw_file_bytes == 0 && layout.compute_file_bytes == 0,
           "disabled disk sink may derive zero file sizes");

    dada_header_t raw_header;
    error.clear();
    Expect(rdma_dada::BuildPipelineDadaHeader(
               config, layout, rdma_dada::DataStage::kRaw, &raw_header, &error),
           "raw DADA header should build: " + error);
    Expect(raw_header.pipeline_version == 1, "pipeline header contract version");
    Expect(std::string(raw_header.data_stage) == "RAW", "raw DATA_STAGE");
    Expect(raw_header.nbit == 16, "NBIT uses bit units");
    Expect(raw_header.nchan == 1, "NCHAN is retained");
    Expect(raw_header.mjd == 61253.0, "MJD_START is derived from UTC_START");
    Expect(raw_header.record_header_bytes == 64, "raw record retains packet header");
    Expect(raw_header.resolution == UINT64_C(8256), "raw header RESOLUTION");
    Expect(raw_header.bytes_per_second == UINT64_C(16000000000),
           "header BYTES_PER_SECOND remains payload-only");

    dada_header_t compute_header;
    error.clear();
    Expect(rdma_dada::BuildPipelineDadaHeader(
               config, layout, rdma_dada::DataStage::kCompute,
               &compute_header, &error),
           "compute DADA header should build: " + error);
    Expect(std::string(compute_header.data_stage) == "COMPUTE",
           "compute DATA_STAGE");
    Expect(compute_header.record_header_bytes == 0,
           "compute record excludes packet header");
    Expect(compute_header.resolution == UINT64_C(8192),
           "compute header RESOLUTION");

    if (failures != 0) return 1;
    std::cout << "pipeline_config_test passed\n";
    return 0;
}
