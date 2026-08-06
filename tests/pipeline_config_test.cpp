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
    Expect(config.payload_order == "TFP", "raw payload order is fixed to TFP");

    rdma_dada::PipelineLayout layout;
    error.clear();
    Expect(rdma_dada::ComputePipelineLayout(config, &layout, &error),
           "example config should validate: " + error);
    if (failures == 0) {
        Expect(layout.raw_record_bytes == 2080, "raw record includes 32-byte header");
        Expect(layout.compute_record_bytes == 16,
               "compute record is one complete TFPA time frame");
        Expect(layout.packets_per_antenna_per_block == 4096,
               "block contains 4096 UDP packets per antenna");
        Expect(layout.samples_per_block == UINT64_C(2097152),
               "T is packet samples times packets per antenna per block");
        Expect(layout.raw_resolution == 2080, "raw RESOLUTION is one raw record");
        Expect(layout.compute_resolution == 16,
               "compute RESOLUTION is one TFPA time frame");
        Expect(layout.raw_block_bytes == UINT64_C(34078720), "raw block size");
        Expect(layout.compute_block_bytes == UINT64_C(33554432), "compute block size");
        Expect(layout.raw_file_bytes == UINT64_C(340787200),
               "raw file is FILE_BLOCKS raw blocks");
        Expect(layout.compute_file_bytes == UINT64_C(335544320),
               "compute file is FILE_BLOCKS compute blocks");
        Expect(layout.payload_bytes_per_second == UINT64_C(16000000000),
               "BYTES_PER_SECOND is payload-only rate");
        Expect(layout.raw_bytes_per_second == UINT64_C(16250000000),
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
    invalid.nchan = 256;
    invalid.packet_payload_bytes = UINT64_C(524288);
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "Project VDIF NCHAN must fit the Word 5 UINT8 field");

    invalid = config;
    invalid.npol = 3;
    invalid.packet_payload_bytes = 3072;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "Project VDIF NPOL must be one or two");

    invalid = config;
    invalid.payload_order = "UNKNOWN";
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "Project VDIF v1 must reject payload order other than TFP");

    invalid = config;
    invalid.packet_header_bytes = 64;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "Project VDIF v1 must reject a non-32-byte header");

    invalid = config;
    invalid.packet_payload_bytes = 8192;
    error.clear();
    Expect(!rdma_dada::ComputePipelineLayout(invalid, &layout, &error),
           "packet payload bytes must match T x F x P x CI8 geometry");

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
    Expect(std::string(raw_header.order) == "TFP", "raw ORDER is retained");
    Expect(raw_header.mjd == 61253.0, "MJD_START is derived from UTC_START");
    Expect(raw_header.record_header_bytes == 32, "raw record retains packet header");
    Expect(raw_header.resolution == UINT64_C(2080), "raw header RESOLUTION");
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
    Expect(std::string(compute_header.order) == "TFPA",
           "compute ORDER is unpacked and antenna-aggregated TFPA");
    Expect(compute_header.record_header_bytes == 0,
           "compute record excludes packet header");
    Expect(compute_header.record_bytes == UINT64_C(16),
           "compute RECORD_BYTES is one TFPA time frame");
    Expect(compute_header.resolution == UINT64_C(16),
           "compute header RESOLUTION");

    if (failures != 0) return 1;
    std::cout << "pipeline_config_test passed\n";
    return 0;
}
