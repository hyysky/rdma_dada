#include "rdma_dada/config/observation_artifacts.h"

#include "rdma_dada/pipeline/ascii_metadata.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectText(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, const std::string& expected) {
    std::string actual;
    Expect(metadata.GetString(key, &actual) && actual == expected,
           key + " mismatch");
}

void ExpectUint(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, std::uint64_t expected) {
    std::uint64_t actual = 0;
    Expect(metadata.GetUint64(key, &actual) && actual == expected,
           key + " mismatch");
}

void ExpectDouble(const rdma_dada::pipeline::Metadata& metadata,
                  const std::string& key, double expected) {
    double actual = 0.0;
    Expect(metadata.GetDouble(key, &actual) && actual == expected,
           key + " mismatch");
}

std::string ReadFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void RemoveArtifacts(const std::string& directory) {
    static const char* const files[] = {
        "resolved_observation.json", "ring_plan.json", "raw.header",
        "unpacked.header", "validation_report.json", "MANIFEST.sha256"
    };
    for (std::size_t index = 0; index < sizeof(files) / sizeof(files[0]);
         ++index) {
        std::remove((directory + "/" + files[index]).c_str());
    }
    rmdir(directory.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: observation_artifacts_test OBSERVATION\n";
        return 2;
    }

    rdma_dada::ObservationArtifacts artifacts;
    std::string error;
    Expect(rdma_dada::BuildObservationArtifacts(argv[1], &artifacts, &error),
           "artifact build: " + error);
    if (failures != 0) return 1;

    const rdma_dada::pipeline::Metadata& raw = artifacts.raw_header;
    ExpectText(raw, "CONFIG_ID", artifacts.plan.config_id);
    ExpectText(raw, "GEOMETRY_ID", artifacts.plan.geometry_id);
    ExpectText(raw, "OBSERVATION_ID", "ca-functional-v1");
    ExpectText(raw, "TELESCOPE", "CA");
    ExpectText(raw, "DATA_STAGE", "RAW");
    ExpectText(raw, "ORDER", "TFP");
    ExpectText(raw, "UTC_START", "2026-08-08-00:00:00");
    ExpectDouble(raw, "MJD_START", 61260.0);
    ExpectText(raw, "STATION_IDS", "101,102");
    ExpectUint(raw, "BANDWIDTH_HZ", 300000000U);
    ExpectUint(raw, "CENTER_FREQUENCY_HZ", 1250000000U);
    ExpectUint(raw, "NANT", 2U);
    ExpectUint(raw, "NCHAN", 2U);
    ExpectUint(raw, "NPOL", 2U);
    ExpectUint(raw, "PKT_HEADER", 32U);
    ExpectUint(raw, "PKT_DATA", 4096U);
    ExpectUint(raw, "PKT_NSAMP", 512U);
    ExpectUint(raw, "RECORD_HEADER_BYTES", 32U);
    ExpectUint(raw, "RECORD_BYTES", 4128U);
    ExpectUint(raw, "RESOLUTION", 4128U);
    ExpectUint(raw, "BLOCK_BYTES", 8454144U);
    ExpectUint(raw, "RING_BYTES", 67633152U);
    ExpectUint(raw, "BYTES_PER_SECOND", 16000000U);
    ExpectUint(raw, "RAW_BYTES_PER_SECOND", 16125000U);
    ExpectUint(raw, "FILE_SIZE", 0U);
    ExpectUint(raw, "GROUP_PERIOD_PS", 512000000U);
    ExpectUint(raw, "GROUP_START_REFERENCE_EPOCH", 53U);
    ExpectUint(raw, "GROUP_START_SECONDS", 3283200U);
    ExpectUint(raw, "GROUP_START_FRAME", 0U);
    ExpectUint(raw, "EXPECTED_GROUPS", 15141U);

    const rdma_dada::pipeline::Metadata& unpacked = artifacts.unpacked_header;
    ExpectText(unpacked, "CONFIG_ID", artifacts.plan.config_id);
    ExpectText(unpacked, "GEOMETRY_ID", artifacts.plan.geometry_id);
    ExpectText(unpacked, "TELESCOPE", "CA");
    ExpectText(unpacked, "DATA_STAGE", "UNPACKED");
    ExpectText(unpacked, "ORDER", "ATFP");
    ExpectUint(unpacked, "RECORD_HEADER_BYTES", 0U);
    ExpectUint(unpacked, "RECORD_BYTES", 8388608U);
    ExpectUint(unpacked, "RESOLUTION", 16U);
    ExpectUint(unpacked, "BLOCK_NTIME", 524288U);
    ExpectUint(unpacked, "OUTPUT_BLOCK_BYTES", 8388608U);
    ExpectUint(unpacked, "BLOCK_BYTES", 8388608U);
    ExpectUint(unpacked, "RING_BYTES", 67108864U);
    ExpectUint(unpacked, "TRANSFER_SIZE", 124035072U);

    Expect(artifacts.resolved_plan_json.find(artifacts.plan.config_id) !=
               std::string::npos,
           "resolved JSON contains CONFIG_ID");
    Expect(artifacts.ring_plan_json.find(
               "\"raw\":{\"block_bytes\":8454144") !=
               std::string::npos,
           "ring plan contains raw block geometry");
    Expect(artifacts.validation_report_json.find("\"valid\":true") !=
               std::string::npos,
           "validation report records success");
    Expect(artifacts.validation_report_json.find("\"formulas\":") !=
               std::string::npos,
           "validation report records formulas");
    Expect(artifacts.validation_report_json.find("\"stage_headers\":[\"RAW\",\"UNPACKED\"]") !=
               std::string::npos,
           "validation report records generated stages");

    std::ostringstream directory;
    directory << "/tmp/rdma_dada_artifacts_" << getpid();
    RemoveArtifacts(directory.str());
    error.clear();
    Expect(rdma_dada::WriteObservationArtifacts(
               artifacts, directory.str(), &error),
           "atomic artifact write: " + error);
    struct stat status = {};
    Expect(stat((directory.str() + "/raw.header").c_str(), &status) == 0 &&
               status.st_size == 4096,
           "raw.header is one DADA header block");
    Expect(stat((directory.str() + "/unpacked.header").c_str(), &status) == 0 &&
               status.st_size == 4096,
           "unpacked.header is one DADA header block");
    Expect(ReadFile(directory.str() + "/MANIFEST.sha256").find(
               "  resolved_observation.json\n") != std::string::npos,
           "manifest uses sha256sum-compatible spacing");

    std::vector<char> raw_bytes(4096U, 0);
    const std::string raw_file = ReadFile(directory.str() + "/raw.header");
    Expect(raw_file.size() == raw_bytes.size(), "raw header read size");
    if (raw_file.size() == raw_bytes.size()) {
        std::copy(raw_file.begin(), raw_file.end(), raw_bytes.begin());
        rdma_dada::pipeline::Metadata parsed;
        error.clear();
        Expect(rdma_dada::pipeline::ParseAsciiMetadata(
                   raw_bytes.data(), raw_bytes.size(), &parsed, &error),
               "written raw header parses: " + error);
        ExpectText(parsed, "CONFIG_ID", artifacts.plan.config_id);
    }

    error.clear();
    Expect(!rdma_dada::WriteObservationArtifacts(
               artifacts, directory.str(), &error),
           "existing artifact directory is never overwritten");
    RemoveArtifacts(directory.str());

    if (failures != 0) return 1;
    std::cout << "observation_artifacts_test passed\n";
    return 0;
}
