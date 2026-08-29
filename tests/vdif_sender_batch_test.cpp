#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"
#include "rdma_dada/simulation/vdif_sender_batch.h"
#include "rdma_dada/simulation/vdif_sender_sim.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

sim::VdifSenderSimConfig MakeConfig(std::uint16_t station,
                                    const std::string& payload_mode) {
    sim::VdifSenderSimConfig config = {};
    config.schema_version = 2;
    config.source_ip = "127.0.0.1";
    config.source_port = static_cast<std::uint16_t>(41000U + station);
    config.destination_ip = "127.0.0.1";
    config.destination_port = 4010;
    config.path_mtu = 1500;
    config.station_id = station;
    config.geometry.first_channel_id = 7;
    config.geometry.nchan = 1;
    config.geometry.npol = 2;
    config.geometry.nsamp_per_packet = 2;
    config.geometry.component_bits = 8;
    config.geometry.payload_bytes = 8;
    config.reference_epoch = 52;
    config.start_seconds = 100;
    config.sample_interval_ps = UINT64_C(300000000000);
    config.group_count = 8;
    config.mode = "PACED";
    config.start_utc = "2030-01-01-00:00:00";
    config.target_payload_bits_per_second = UINT64_C(10000000);
    config.batch_packets = 4;
    config.payload_mode = payload_mode;
    return config;
}

unpack::ProjectVdifHeader Decode(const sim::VdifPacketView& packet) {
    unpack::ProjectVdifHeader header = {};
    std::string error;
    Expect(unpack::DecodeProjectVdifV1(packet.data, packet.bytes,
                                       &header, &error),
           "batch packet header decodes: " + error);
    return header;
}

void TestStableRepeatBatch() {
    sim::VdifSenderBatch batch;
    std::string error;
    const sim::VdifSenderSimConfig config = MakeConfig(101, "REPEAT_TEMPLATE");
    Expect(batch.Initialize(config, &error), "repeat batch initializes: " + error);
    Expect(batch.capacity() == 4U, "batch capacity follows configuration");
    Expect(batch.Prepare(0, 4, &error), "first batch prepares: " + error);
    const std::uint8_t* addresses[4] = {};
    for (std::uint32_t i = 0; i < 4U; ++i) {
        addresses[i] = batch.packet(i).data;
        Expect(batch.packet(i).group_index == i, "first batch group index advances");
        const unpack::ProjectVdifHeader header = Decode(batch.packet(i));
        Expect(header.station_id == 101U, "Station ID remains fixed in batch");
    }
    Expect(std::equal(batch.packet(0).data + 32, batch.packet(0).data + 40,
                      batch.packet(1).data + 32),
           "repeat mode reuses identical payload across groups");
    const std::uint8_t expected_prefix[] = {0x65U, 0x66U, 0x70U, 0x71U};
    Expect(std::equal(expected_prefix, expected_prefix + 4,
                      batch.packet(0).data + 32),
           "Station 101 TFP payload begins with both polarization IQ pairs");

    Expect(batch.Prepare(4, 4, &error), "second batch prepares: " + error);
    for (std::uint32_t i = 0; i < 4U; ++i) {
        Expect(batch.packet(i).data == addresses[i],
               "packet storage address remains stable after Prepare");
        Expect(batch.packet(i).group_index == 4U + i,
               "second batch group index advances");
    }
    Expect(!batch.Prepare(7, 2, &error), "group range beyond transfer is rejected");
    Expect(!batch.Prepare(0, 5, &error), "packet count above capacity is rejected");
}

void TestStationTemplatesDiffer() {
    sim::VdifSenderBatch first;
    sim::VdifSenderBatch second;
    std::string error;
    Expect(first.Initialize(MakeConfig(101, "REPEAT_TEMPLATE"), &error),
           "Station 101 batch initializes");
    Expect(second.Initialize(MakeConfig(102, "REPEAT_TEMPLATE"), &error),
           "Station 102 batch initializes");
    Expect(first.Prepare(0, 1, &error) && second.Prepare(0, 1, &error),
           "Station batches prepare");
    Expect(!std::equal(first.packet(0).data + 32, first.packet(0).data + 40,
                       second.packet(0).data + 32),
           "different Stations have distinguishable repeat templates");
}

void TestDeterministicBatchMatchesReference() {
    const sim::VdifSenderSimConfig config = MakeConfig(101, "DETERMINISTIC");
    sim::VdifSenderBatch batch;
    std::string error;
    Expect(batch.Initialize(config, &error), "deterministic batch initializes");
    Expect(batch.Prepare(2, 1, &error), "deterministic batch prepares");
    std::vector<std::uint8_t> expected;
    Expect(sim::BuildVdifSenderRecord(config, 2, &expected, &error),
           "reference record builds");
    Expect(expected.size() == batch.packet(0).bytes &&
           std::equal(expected.begin(), expected.end(), batch.packet(0).data),
           "deterministic batch is byte-identical to reference record");
}

void TestMultiStationTimeMajorRotatingOrder() {
    sim::VdifSenderSimConfig config = MakeConfig(100, "REPEAT_TEMPLATE");
    config.schema_version = 3;
    config.station_ids = {100, 101, 102};
    config.group_count = 3;
    config.batch_packets = 6;
    sim::VdifSenderBatch batch;
    std::string error;
    Expect(batch.Initialize(config, &error),
           "multi-Station batch initializes: " + error);
    Expect(batch.Prepare(0, 6, &error),
           "two complete multi-Station groups prepare: " + error);
    const std::uint64_t expected_groups[] = {0, 0, 0, 1, 1, 1};
    const std::uint16_t expected_stations[] = {100, 101, 102, 101, 102, 100};
    const std::uint32_t expected_station_indices[] = {0, 1, 2, 1, 2, 0};
    for (std::uint32_t index = 0; index < 6U; ++index) {
        const unpack::ProjectVdifHeader header = Decode(batch.packet(index));
        Expect(batch.packet(index).group_index == expected_groups[index],
               "flattened packet preserves its astronomical group");
        Expect(header.station_id == expected_stations[index],
               "each group rotates its first Station without omissions");
        Expect(batch.packet(index).station_index ==
                   expected_station_indices[index],
               "packet exposes the O(1) configured Station index");
    }
}

}  // namespace

int main() {
    TestStableRepeatBatch();
    TestStationTemplatesDiffer();
    TestDeterministicBatchMatchesReference();
    TestMultiStationTimeMajorRotatingOrder();
    if (failures) return 1;
    std::cout << "vdif_sender_batch_test passed\n";
    return 0;
}
