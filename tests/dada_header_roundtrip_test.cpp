#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/io/psrdada/header_codec.h"
#include "rdma_dada/pipeline/ascii_metadata.h"

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
        std::cerr << "usage: dada_header_roundtrip_test OBSERVATION_CONFIG\n";
        return 2;
    }

    rdma_dada::ObservationArtifacts artifacts;
    std::string error;
    Expect(rdma_dada::BuildObservationArtifacts(argv[1], &artifacts, &error),
           "artifact build: " + error);

    const std::size_t header_size =
        static_cast<std::size_t>(DADA_DEFAULT_HEADER_SIZE);
    std::vector<char> buffer(header_size, '\0');
    Expect(rdma_dada::pipeline::SerializeAsciiMetadata(
               artifacts.raw_header, buffer.data(), buffer.size(), &error),
           "artifact header serialization: " + error);

    dada_header_t actual;
    Expect(read_dada_header(buffer.data(), &actual) == 0,
           "header deserialization");
    double expected_mjd = 0.0;
    Expect(artifacts.raw_header.GetDouble("MJD_START", &expected_mjd),
           "compiled header contains MJD_START");
    Expect(actual.pipeline_version == DADA_PIPELINE_CONTRACT_VERSION,
           "PIPELINE_VERSION round trip");
    Expect(std::string(actual.data_stage) == "RAW", "DATA_STAGE round trip");
    Expect(std::string(actual.utc_start) == artifacts.plan.source.utc_start,
           "UTC_START round trip");
    Expect(std::fabs(actual.mjd - expected_mjd) < 1.0e-12,
           "MJD_START round trip");
    Expect(actual.nant == artifacts.plan.nant, "NANT round trip");
    Expect(actual.nchan == artifacts.plan.source.nchan, "NCHAN round trip");
    Expect(actual.npol == artifacts.plan.source.npol, "NPOL round trip");
    Expect(actual.nbit == 16, "NBIT=16 bit round trip");
    Expect(std::string(actual.order) == "TFP", "ORDER round trip");
    Expect(actual.record_header_bytes == 32, "raw header retained");
    Expect(actual.record_bytes == artifacts.plan.raw_record_bytes,
           "raw record size round trip");
    Expect(actual.resolution == artifacts.plan.raw_record_bytes,
           "RESOLUTION round trip");
    Expect(actual.bytes_per_second == artifacts.plan.payload_bytes_per_second,
           "payload byte rate round trip");
    Expect(actual.raw_bytes_per_second == artifacts.plan.raw_bytes_per_second,
           "raw byte rate round trip");

    char config_id[65] = {};
    char geometry_id[65] = {};
    Expect(ascii_header_get(buffer.data(), "CONFIG_ID", "%64s", config_id) == 1 &&
               std::string(config_id) == artifacts.plan.config_id,
           "CONFIG_ID round trip");
    Expect(ascii_header_get(buffer.data(), "GEOMETRY_ID", "%64s", geometry_id) == 1 &&
               std::string(geometry_id) == artifacts.plan.geometry_id,
           "GEOMETRY_ID round trip");

    Expect(ascii_header_set(buffer.data(), "PIPELINE_VERSION", "%d", 2) == 0,
           "test should mutate contract version");
    Expect(read_dada_header(buffer.data(), &actual) != 0,
           "unsupported contract version must be rejected");

    if (failures != 0) return 1;
    std::cout << "dada_header_roundtrip_test passed\n";
    return 0;
}
