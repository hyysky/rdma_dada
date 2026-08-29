#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"
#include "rdma_dada/simulation/vdif_sender_sim.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace sim = rdma_dada::simulation;
namespace unpack = rdma_dada::modules::vdif_unpack;
int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadText(const char* path) {
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void ExpectInvalidMutation(const std::string& source,
                           const std::string& before,
                           const std::string& after,
                           const std::string& label) {
    std::string mutated = source;
    const std::string::size_type position = mutated.find(before);
    Expect(position != std::string::npos, label + " fixture marker exists");
    if (position == std::string::npos) return;
    mutated.replace(position, before.size(), after);
    std::ostringstream path;
    path << "/tmp/rdma_dada_vdif_sender_" << static_cast<long>(getpid())
         << '_' << label << ".json";
    { std::ofstream output(path.str().c_str()); output << mutated; }
    sim::VdifSenderSimConfig config = {};
    std::string error;
    Expect(!sim::LoadVdifSenderSimConfig(path.str(), &config, &error),
           label + " is rejected");
    std::remove(path.str().c_str());
}

sim::VdifSenderSimConfig MakeConfig(std::uint16_t station,
                                    std::uint8_t component_bits) {
    sim::VdifSenderSimConfig result = {};
    result.destination_ip = "127.0.0.1";
    result.destination_port = 4010;
    result.path_mtu = 1500;
    result.station_id = station;
    result.geometry.first_channel_id = 7;
    result.geometry.nchan = 1;
    result.geometry.npol = 2;
    result.geometry.nsamp_per_packet = 2;
    result.geometry.component_bits = component_bits;
    result.geometry.payload_bytes = component_bits == 8 ? 8 : 16;
    result.reference_epoch = 52;
    result.start_seconds = 100;
    result.sample_interval_ps = UINT64_C(300000000000);
    result.group_count = 5;
    result.mode = "BURST";
    return result;
}

unpack::ProjectVdifHeader Decode(const std::vector<std::uint8_t>& record) {
    unpack::ProjectVdifHeader header = {};
    std::string error;
    Expect(unpack::DecodeProjectVdifV1(record.data(), record.size(),
                                       &header, &error),
           "generated header decodes: " + error);
    return header;
}

void TestStrictExampleConfig(const char* path) {
    sim::VdifSenderSimConfig config = {};
    std::string error;
    Expect(sim::LoadVdifSenderSimConfig(path, &config, &error),
           "example sender config loads: " + error);
    Expect(config.destination_ip == "127.0.0.1" &&
           config.destination_port == 4010 && config.path_mtu == 9000,
           "destination and IPv4 path MTU parse");
    Expect(config.station_id == 101, "one config represents one Station ID");
    Expect(config.sample_interval_ps == 1000000U,
           "decimal picosecond text parses without floating point");
    Expect(config.geometry.payload_bytes == 4096U,
           "payload bytes derive from TFP complex geometry");

    std::ifstream source(path);
    std::ostringstream contents;
    contents << source.rdbuf();
    std::string invalid = contents.str();
    const std::string original = "\"duplicate_groups\": [2]";
    const std::string::size_type position = invalid.find(original);
    Expect(position != std::string::npos, "example duplicate fault marker exists");
    if (position != std::string::npos) {
        invalid.replace(position, original.size(), "\"duplicate_groups\": [2, 2]");
        std::ostringstream temp;
        temp << "/tmp/rdma_dada_vdif_sender_" << static_cast<long>(getpid())
             << ".json";
        { std::ofstream output(temp.str().c_str()); output << invalid; }
        sim::VdifSenderSimConfig unpublished = config;
        Expect(!sim::LoadVdifSenderSimConfig(temp.str(), &unpublished, &error),
               "duplicate group index inside a fault list is rejected");
        std::remove(temp.str().c_str());
    }
}

void TestStrictPacedConfig(const char* path) {
    sim::VdifSenderSimConfig config = {};
    std::string error;
    Expect(sim::LoadVdifSenderSimConfig(path, &config, &error),
           "paced sender config loads: " + error);
    Expect(config.schema_version == 2U,
           "paced example uses schema v2");
    Expect(config.source_ip == "127.0.0.1" && config.source_port == 41001U,
           "schema v2 parses explicit source endpoint");
    Expect(config.mode == "PACED" &&
           config.target_payload_bits_per_second == UINT64_C(10000000),
           "0.01 Gbps parses as an exact paced payload bit rate");
    Expect(config.batch_packets == 16U &&
           config.payload_mode == "REPEAT_TEMPLATE",
           "paced batch and payload mode parse");

    const std::string source = ReadText(path);
    ExpectInvalidMutation(source, "\"ip\": \"127.0.0.1\"",
                          "\"ip\": \"0.0.0.0\"", "wildcard_source_ip");
    ExpectInvalidMutation(source, "\"port\": 41001", "\"port\": 0",
                          "zero_source_port");
    ExpectInvalidMutation(source, "\"port\": 41001",
                          "\"port\": 41001, \"weight\": 1",
                          "unknown_source_field");
    ExpectInvalidMutation(source, "\"mode\": \"PACED\"",
                          "\"mode\": \"BURST\"", "v2_burst_mode");
    ExpectInvalidMutation(source, "\"target_gbps\": 0.01",
                          "\"target_gbps\": 0", "zero_target_rate");
    ExpectInvalidMutation(source, "\"batch_packets\": 16",
                          "\"batch_packets\": 65", "oversize_batch");
    ExpectInvalidMutation(source, "\"payload_mode\": \"REPEAT_TEMPLATE\"",
                          "\"payload_mode\": \"UNKNOWN\"",
                          "unknown_payload_mode");
}

void TestStrictMultiStationPacedConfig(const char* path) {
    std::string source = ReadText(path);
    std::string::size_type position = source.find("\"schema_version\": 2");
    Expect(position != std::string::npos, "paced schema marker exists");
    if (position == std::string::npos) return;
    source.replace(position, std::string("\"schema_version\": 2").size(),
                   "\"schema_version\": 3");
    position = source.find("\"station_id\": 101");
    Expect(position != std::string::npos, "paced Station marker exists");
    if (position == std::string::npos) return;
    source.replace(position, std::string("\"station_id\": 101").size(),
                   "\"station_ids\": [1000, 1001, 1002]");

    std::ostringstream path_text;
    path_text << "/tmp/rdma_dada_vdif_sender_multi_"
              << static_cast<long>(getpid()) << ".json";
    { std::ofstream output(path_text.str().c_str()); output << source; }

    sim::VdifSenderSimConfig config = {};
    std::string error;
    Expect(sim::LoadVdifSenderSimConfig(path_text.str(), &config, &error),
           "schema v3 multi-Station sender config loads: " + error);
    Expect(config.station_ids == std::vector<std::uint16_t>({1000, 1001, 1002}),
           "schema v3 preserves the ordered Station list");

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    Expect(sim::BuildVdifSenderRecordForStation(
               config, 4, 1000, &first, &error),
           "schema v3 first Station record builds: " + error);
    Expect(sim::BuildVdifSenderRecordForStation(
               config, 4, 1002, &second, &error),
           "schema v3 second Station record builds: " + error);
    const unpack::ProjectVdifHeader first_header = Decode(first);
    const unpack::ProjectVdifHeader second_header = Decode(second);
    Expect(first_header.station_id == 1000U &&
               second_header.station_id == 1002U,
           "one time group can carry every configured Station ID");
    Expect(first_header.seconds_from_reference_epoch ==
               second_header.seconds_from_reference_epoch &&
               first_header.frame_number_within_second ==
               second_header.frame_number_within_second,
           "multi-Station records in one group share the exact time key");

    std::remove(path_text.str().c_str());
    ExpectInvalidMutation(source, "\"station_ids\": [1000, 1001, 1002]",
                          "\"station_ids\": [1000, 1000]",
                          "duplicate_station_ids");
    ExpectInvalidMutation(source, "\"station_ids\": [1000, 1001, 1002]",
                          "\"station_ids\": []", "empty_station_ids");
}

void TestDeterministicCi8AndTimeRollover() {
    sim::VdifSenderSimConfig station101 = MakeConfig(101, 8);
    sim::VdifSenderSimConfig station102 = station101;
    station102.station_id = 102;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    std::string error;
    Expect(sim::BuildVdifSenderRecord(station101, 2, &first, &error),
           "Station 101 group builds: " + error);
    Expect(sim::BuildVdifSenderRecord(station102, 2, &second, &error),
           "Station 102 group builds: " + error);
    Expect(first.size() == 40U && second.size() == 40U,
           "CI8 record is exact 32-byte header plus 8-byte payload");
    const unpack::ProjectVdifHeader first_header = Decode(first);
    const unpack::ProjectVdifHeader second_header = Decode(second);
    Expect(first_header.seconds_from_reference_epoch == 101U &&
           first_header.frame_number_within_second == 0U,
           "1.2-second offset advances seconds and resets frame ordinal");
    Expect(second_header.seconds_from_reference_epoch ==
               first_header.seconds_from_reference_epoch &&
           second_header.frame_number_within_second ==
               first_header.frame_number_within_second,
           "same group index across servers has an identical time key");
    Expect(first_header.station_id == 101U && second_header.station_id == 102U,
           "each server injects its configured Station ID");
    Expect(!std::equal(first.begin() + 32, first.end(), second.begin() + 32),
           "different Station IDs produce distinguishable deterministic payloads");
    const std::vector<std::uint8_t> expected_payload = {
        115, 116, 126, 127, 118, 119, 129, 130
    };
    Expect(std::vector<std::uint8_t>(first.begin() + 32, first.end()) ==
               expected_payload,
           "CI8 TFP/IQ payload matches the documented bounded formula");

    std::vector<std::uint8_t> group3;
    Expect(sim::BuildVdifSenderRecord(station101, 3, &group3, &error),
           "next group builds");
    const unpack::ProjectVdifHeader group3_header = Decode(group3);
    Expect(group3_header.seconds_from_reference_epoch == 101U &&
           group3_header.frame_number_within_second == 1U,
           "non-integral groups per second use ordinal within current second");
}

void TestFixedPacketsPerSecondTimestamp() {
    sim::VdifSenderSimConfig config = MakeConfig(101, 8);
    config.groups_per_second = 227108U;
    config.group_count = 227109U;
    std::vector<std::uint8_t> record;
    std::string error;
    Expect(sim::BuildVdifSenderRecord(config, 227108U, &record, &error),
           "fixed-rate second boundary record builds: " + error);
    const unpack::ProjectVdifHeader header = Decode(record);
    Expect(header.seconds_from_reference_epoch == 101U &&
               header.frame_number_within_second == 0U,
           "fixed packet count rolls over at exactly 227108 packets");
}

void TestCi16LittleEndianPayload() {
    sim::VdifSenderSimConfig config = MakeConfig(1000, 16);
    std::vector<std::uint8_t> record;
    std::string error;
    Expect(sim::BuildVdifSenderRecord(config, 2, &record, &error),
           "CI16 group builds: " + error);
    Expect(record.size() == 48U, "CI16 exact record length");
    const unpack::ProjectVdifHeader header = Decode(record);
    Expect(header.component_bits == 16U && header.frame_length_units_8_bytes == 6U,
           "CI16 header carries component and frame geometry");
    Expect(record[32] == 0xf6U && record[33] == 0x03U &&
           record[34] == 0xf7U && record[35] == 0x03U,
           "CI16 I/Q components use little-endian two's-complement bits");
}

void TestMtuRangeAndFaultInjection() {
    sim::VdifSenderSimConfig config = MakeConfig(101, 8);
    std::string error;
    std::vector<std::uint8_t> record;
    config.path_mtu = 67;
    Expect(!sim::BuildVdifSenderRecord(config, 0, &record, &error),
           "IPv4 MTU rejects a 40-byte UDP record when only 39 bytes fit");
    config.path_mtu = 1500;
    Expect(!sim::BuildVdifSenderRecord(config, 5, &record, &error),
           "group index must be below configured group_count");
    config.invalid_header_groups.push_back(2);
    Expect(sim::BuildVdifSenderRecord(config, 2, &record, &error),
           "invalid-header fault record is still generated");
    unpack::ProjectVdifHeader decoded = {};
    Expect(!unpack::DecodeProjectVdifV1(record.data(), record.size(),
                                        &decoded, &error),
           "invalid-header fault is observable by the production decoder");

    sim::VdifSenderSimConfig unsorted = MakeConfig(101, 8);
    unsorted.drop_groups.push_back(3);
    unsorted.drop_groups.push_back(1);
    Expect(!sim::BuildVdifSenderRecord(unsorted, 0, &record, &error),
           "programmatically constructed fault lists must also be sorted");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: vdif_sender_sim_test V1_CONFIG V2_CONFIG\n";
        return 2;
    }
    TestStrictExampleConfig(argv[1]);
    TestStrictPacedConfig(argv[2]);
    TestStrictMultiStationPacedConfig(argv[2]);
    TestDeterministicCi8AndTimeRollover();
    TestFixedPacketsPerSecondTimestamp();
    TestCi16LittleEndianPayload();
    TestMtuRangeAndFaultInjection();
    if (failures) return 1;
    std::cout << "vdif_sender_sim_test passed\n";
    return 0;
}
