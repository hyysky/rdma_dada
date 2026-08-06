#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/io/psrdada/header_codec.h"
#include "rdma_dada/pipeline/dada_header_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <ascii_header.h>
#include <dada_def.h>
}

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
        std::cerr << "usage: dada_header_roundtrip_test CONFIG\n";
        return 2;
    }

    rdma_dada::PipelineConfig config;
    rdma_dada::PipelineLayout layout;
    std::string error;
    Expect(rdma_dada::LoadPipelineConfig(argv[1], &config, &error),
           "config load: " + error);
    Expect(rdma_dada::ComputePipelineLayout(config, &layout, &error),
           "layout compute: " + error);

    dada_header_t expected;
    Expect(rdma_dada::BuildPipelineDadaHeader(
               config, layout, rdma_dada::DataStage::kRaw, &expected, &error),
           "header build: " + error);

    const std::size_t header_size =
        static_cast<std::size_t>(DADA_DEFAULT_HEADER_SIZE);
    std::vector<char> buffer(header_size, '\0');
    std::snprintf(buffer.data(), buffer.size(),
                  "HDR_VERSION 1.0\nHDR_SIZE %zu\nOBS_ID preserved\n",
                  buffer.size());
    Expect(write_dada_header(expected, buffer.data()) == 0,
           "header serialization");

    dada_header_t actual;
    Expect(read_dada_header(buffer.data(), &actual) == 0,
           "header deserialization");
    Expect(actual.pipeline_version == DADA_PIPELINE_CONTRACT_VERSION,
           "PIPELINE_VERSION round trip");
    Expect(std::string(actual.data_stage) == "RAW", "DATA_STAGE round trip");
    Expect(std::string(actual.utc_start) == config.utc_start, "UTC_START round trip");
    Expect(std::fabs(actual.mjd - 61253.0) < 1.0e-12, "MJD_START round trip");
    Expect(actual.nant == config.nant, "NANT round trip");
    Expect(actual.nchan == config.nchan, "NCHAN round trip");
    Expect(actual.npol == config.npol, "NPOL round trip");
    Expect(actual.nbit == 16, "NBIT=16 bit round trip");
    Expect(std::string(actual.order) == "TFP", "ORDER round trip");
    Expect(actual.record_header_bytes == 32, "raw header retained");
    Expect(actual.record_bytes == 2080, "raw record size round trip");
    Expect(actual.resolution == 2080, "RESOLUTION round trip");
    Expect(actual.bytes_per_second == UINT64_C(16000000000),
           "payload byte rate round trip");
    Expect(actual.raw_bytes_per_second == UINT64_C(16250000000),
           "raw byte rate round trip");

    char obs_id[32];
    Expect(ascii_header_get(buffer.data(), "OBS_ID", "%31s", obs_id) == 1 &&
               std::string(obs_id) == "preserved",
           "unknown template field is preserved");

    dada_header_t compute_expected;
    Expect(rdma_dada::BuildPipelineDadaHeader(
               config, layout, rdma_dada::DataStage::kCompute,
               &compute_expected, &error),
           "compute header build: " + error);
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::snprintf(buffer.data(), buffer.size(),
                  "HDR_VERSION 1.0\nHDR_SIZE %zu\nOBS_ID preserved\n",
                  buffer.size());
    Expect(write_dada_header(compute_expected, buffer.data()) == 0,
           "compute header serialization");
    Expect(read_dada_header(buffer.data(), &actual) == 0,
           "compute header deserialization");
    Expect(std::string(actual.data_stage) == "COMPUTE",
           "compute DATA_STAGE round trip");
    Expect(std::string(actual.order) == "TFPA", "compute ORDER round trip");
    Expect(actual.record_header_bytes == 0,
           "compute record excludes packet header");
    Expect(actual.record_bytes == 16, "compute TFPA frame round trip");
    Expect(actual.resolution == 16, "compute RESOLUTION round trip");

    Expect(ascii_header_set(buffer.data(), "PIPELINE_VERSION", "%d", 2) == 0,
           "test should mutate contract version");
    Expect(read_dada_header(buffer.data(), &actual) != 0,
           "unsupported contract version must be rejected");

    if (failures != 0) return 1;
    std::cout << "dada_header_roundtrip_test passed\n";
    return 0;
}
