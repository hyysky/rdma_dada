#include "rdma_dada/config/observation_config.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream input(path.c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string ReplaceOnce(const std::string& input, const std::string& before,
                        const std::string& after) {
    const std::string::size_type position = input.find(before);
    if (position == std::string::npos) return std::string();
    std::string result = input;
    result.replace(position, before.size(), after);
    return result;
}

bool LoadText(const std::string& text, rdma_dada::ObservationConfig* config,
              std::string* error) {
    std::ostringstream path;
    path << "/tmp/rdma_dada_observation_config_test_" << getpid() << ".json";
    {
        std::ofstream output(path.str().c_str());
        output << text;
    }
    const bool loaded =
        rdma_dada::LoadObservationConfig(path.str(), config, error);
    std::remove(path.str().c_str());
    return loaded;
}

void ExpectDuration(const std::string& text, bool expected,
                    std::uint64_t expected_value) {
    std::uint64_t value = 0;
    std::string error;
    const bool parsed = rdma_dada::ParseExactSecondsToPicoseconds(
        text, &value, &error);
    Expect(parsed == expected, "duration parse result for " + text + ": " + error);
    if (parsed) {
        Expect(value == expected_value, "duration value for " + text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: observation_config_test OBSERVATION_JSON\n";
        return 2;
    }

    ExpectDuration("10", true, UINT64_C(10000000000000));
    ExpectDuration("0.000000000001", true, 1);
    ExpectDuration("7.752192", true, UINT64_C(7752192000000));
    ExpectDuration("0", false, 0);
    ExpectDuration("-1", false, 0);
    ExpectDuration("1e3", false, 0);
    ExpectDuration("00.1", false, 0);
    ExpectDuration("0.0000000000001", false, 0);
    ExpectDuration("18446744.073709551616", false, 0);

    rdma_dada::ObservationConfig config;
    std::string error;
    Expect(rdma_dada::LoadObservationConfig(argv[1], &config, &error),
           "example observation should load: " + error);
    if (failures == 0) {
        Expect(config.schema_version == 1, "schema version");
        Expect(config.observation_id == "ca-functional-v1", "observation id");
        Expect(config.utc_start == "2026-08-08-00:00:00", "UTC start");
        Expect(config.duration_ps == UINT64_C(7752192000000), "duration ps");
        Expect(config.station_ids.size() == 2 && config.station_ids[0] == 101 &&
                   config.station_ids[1] == 102,
               "Station order defines A");
        Expect(config.first_channel_id == 100 && config.nchan == 2 &&
                   config.npol == 2,
               "channel and polarization geometry");
        Expect(config.sample_interval_ps == UINT64_C(1000000),
               "single-sample interval");
        Expect(config.telescope == "CA", "telescope metadata");
        Expect(config.bandwidth_hz == UINT64_C(300000000), "bandwidth metadata");
        Expect(config.center_frequency_hz == UINT64_C(1250000000),
               "center frequency metadata");
        Expect(config.samples_per_packet == 512, "packet T");
        Expect(config.groups_per_block == 1024 && config.raw_ring_blocks == 8 &&
                   config.compute_ring_blocks == 8 && config.window_blocks == 2,
               "block and window policy");
        Expect(config.raw_key == 0x00d2U && config.compute_key == 0x00d4U,
               "ring keys");
        Expect(config.receiver_device == "mlx5_0" &&
                   config.destination_ip == "174.0.1.111" &&
                   config.destination_port == 1000,
               "receiver endpoint");
        Expect(config.modules.empty(), "example is raw/unpack-only");
        Expect(config.wire_profile_path.find("packet_formats/frontend.example-v1.json") !=
                   std::string::npos,
               "wire profile path is resolved relative to observation JSON");
    }

    const std::string valid = ReadFile(argv[1]);
    rdma_dada::ObservationConfig ignored;

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"schema_version\": 1",
                                 "\"schema_version\": 1, \"extra\": true"),
                     &ignored, &error),
           "unknown root field must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"observation_id\": \"ca-functional-v1\",", ""),
                     &ignored, &error),
           "missing required field must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"telescope\": \"CA\"",
                                 "\"telescope\": \"\""),
                     &ignored, &error),
           "empty telescope must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"bandwidth_hz\": 300000000",
                                 "\"bandwidth_hz\": 0"),
                     &ignored, &error),
           "zero bandwidth must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"station_ids\": [101, 102]",
                                 "\"station_ids\": []"),
                     &ignored, &error),
           "empty Station list must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "101, 102", "101, 101"),
                     &ignored, &error),
           "duplicate Station ID must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"npol\": 2", "\"npol\": 3"),
                     &ignored, &error),
           "NPOL outside one/two must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"groups_per_block\": 1024",
                                 "\"groups_per_block\": 0"),
                     &ignored, &error),
           "zero block policy must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "2026-08-08-00:00:00",
                                 "2026-02-31-00:00:00"),
                     &ignored, &error),
           "invalid UTC calendar date must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "98:03:9b:aa:99:d8", "bad-mac"),
                     &ignored, &error),
           "invalid destination MAC must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "174.0.1.111", "999.0.1.111"),
                     &ignored, &error),
           "invalid destination IPv4 must be rejected");

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid, "\"raw_key\": \"0x00d2\"",
                                 "\"raw_key\": \"invalid\""),
                     &ignored, &error),
           "invalid ring key must be rejected");

    const std::string invalid_order = ReplaceOnce(
        valid, "\"modules\": []",
        "\"modules\": [{\"type\":\"power\"}]");
    error.clear();
    Expect(!LoadText(invalid_order, &ignored, &error),
           "Power without Beamform must be rejected");

    const std::string valid_chain = ReplaceOnce(
        valid, "\"modules\": []",
        "\"modules\":["
        "{\"type\":\"beamform\",\"weights_file\":\"weights.npy\","
        "\"weights_order\":\"FPAB2\",\"weights_id\":\"test\","
        "\"weights_scale\":\"0.0078125\",\"compute_mode\":\"FP32\"},"
        "{\"type\":\"power\"},"
        "{\"type\":\"integrate\",\"length\":128,"
        "\"operation\":\"MEAN\"}]");
    error.clear();
    Expect(LoadText(valid_chain, &ignored, &error),
           "Beamform-Power-Integrate chain should load: " + error);
    if (error.empty() && ignored.modules.size() == 3) {
        Expect(ignored.modules[0].kind ==
                   rdma_dada::ObservationModuleKind::kBeamform,
               "Beamform module parsed");
        Expect(ignored.modules[2].integration_length == 128,
               "integration length parsed");
        Expect(ignored.modules[0].weights_file.find("/weights.npy") !=
                   std::string::npos,
               "weight path is resolved relative to observation JSON");
    }

    error.clear();
    Expect(!LoadText(ReplaceOnce(valid_chain, "0.0078125", "1e-3"),
                     &ignored, &error),
           "exponent weight scale must be rejected");

    if (failures != 0) return 1;
    std::cout << "observation_config_test passed\n";
    return 0;
}
