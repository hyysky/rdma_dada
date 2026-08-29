#include "rdma_dada/config/json_value.h"
#include "rdma_dada/simulation/udp_vdif_sender.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

namespace json = rdma_dada::json;
namespace sim = rdma_dada::simulation;
int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestMachineReadableStatistics() {
    sim::VdifSenderSimConfig config = {};
    config.schema_version = 2;
    config.source_ip = "127.0.0.1";
    config.source_port = 41001;
    config.destination_ip = "127.0.0.1";
    config.destination_port = 1000;
    config.station_id = 101;
    config.target_payload_bits_per_second = UINT64_C(10000000);
    config.batch_packets = 16;
    config.payload_mode = "REPEAT_TEMPLATE";

    sim::VdifSenderStats stats = {};
    stats.scheduled_packets = 64;
    stats.sent_packets = 64;
    stats.retried_packets = 2;
    stats.payload_bytes = 264192;
    stats.elapsed_ns = UINT64_C(211353600);
    stats.pacing_start_monotonic_ns = UINT64_C(999999000);
    stats.first_send_monotonic_ns = UINT64_C(1000000000);
    stats.last_send_monotonic_ns = UINT64_C(1211353600);
    stats.batches = 4;
    stats.short_batches = 1;
    stats.overrun_batches = 3;
    stats.backend = "SENDMMSG";
    stats.payload_prefix_hex = "65667071";

    const std::string text = sim::FormatVdifSenderStatsJson(config, stats);
    json::Value root;
    std::string error;
    Expect(json::Parse(text, &root, &error),
           "statistics are valid JSON: " + error);
    Expect(root.type() == json::Value::kObject,
           "statistics JSON root is an object");
    if (root.type() != json::Value::kObject) return;
    const json::Value::Object& object = root.object();
    Expect(object.find("station_id")->second.text() == "101",
           "statistics preserve Station ID");
    Expect(object.find("source_port")->second.text() == "41001",
           "statistics preserve bound source port");
    Expect(object.find("sent_packets")->second.text() == "64" &&
           object.find("failed_packets")->second.text() == "0",
           "statistics reconcile sent and failed counts");
    Expect(object.find("backend")->second.text() == "SENDMMSG",
           "statistics identify the UDP batch backend");
    Expect(object.find("payload_prefix_hex")->second.text() == "65667071",
           "statistics expose the transmitted payload prefix");
    Expect(object.find("pacing_start_monotonic_ns")->second.text() ==
               "999999000" &&
           object.find("first_send_monotonic_ns")->second.text() ==
               "1000000000" &&
           object.find("last_send_monotonic_ns")->second.text() ==
               "1211353600",
           "statistics expose the pacing, first and last send timestamps");
}

void TestMultiStationMachineReadableStatistics() {
    sim::VdifSenderSimConfig config = {};
    config.schema_version = 3;
    config.source_ip = "127.0.0.1";
    config.source_port = 41001;
    config.destination_ip = "127.0.0.1";
    config.destination_port = 1000;
    config.station_id = 100;
    config.station_ids = {100, 101, 102};
    config.target_payload_bits_per_second = UINT64_C(30000000);
    config.batch_packets = 6;
    config.payload_mode = "REPEAT_TEMPLATE";

    sim::VdifSenderStats stats = {};
    stats.scheduled_packets = 6;
    stats.sent_packets = 6;
    stats.payload_bytes = 24768;
    stats.elapsed_ns = UINT64_C(6604800);
    stats.backend = "SENDMMSG";
    stats.payload_prefix_hex = "64656f70";
    stats.station_ids = {100, 101, 102};
    stats.station_scheduled_packets = {2, 2, 2};
    stats.station_sent_packets = {2, 2, 2};

    json::Value root;
    std::string error;
    Expect(json::Parse(sim::FormatVdifSenderStatsJson(config, stats),
                       &root, &error),
           "multi-Station statistics are valid JSON: " + error);
    if (root.type() != json::Value::kObject) return;
    const json::Value::Object& object = root.object();
    const json::Value& station_ids = object.find("station_ids")->second;
    const json::Value& station_counts = object.find("station_counts")->second;
    Expect(station_ids.type() == json::Value::kArray &&
               station_ids.array().size() == 3U &&
               station_ids.array()[2].text() == "102",
           "statistics preserve the complete ordered Station list");
    Expect(station_counts.type() == json::Value::kArray &&
               station_counts.array().size() == 3U &&
               station_counts.array()[1].object().find("station_id")->second.text() ==
                   "101" &&
               station_counts.array()[1].object().find("sent_packets")->second.text() ==
                   "2",
           "statistics prove per-Station packet closure");
}

}  // namespace

int main() {
    TestMachineReadableStatistics();
    TestMultiStationMachineReadableStatistics();
    if (failures) return 1;
    std::cout << "udp_vdif_sender_test passed\n";
    return 0;
}
