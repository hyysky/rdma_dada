#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace unpack = rdma_dada::modules::vdif_unpack;
int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

rdma_dada::PipelineConfig MakePipeline() {
    rdma_dada::PipelineConfig result = {};
    result.nant = 4;
    result.nchan = 2;
    result.npol = 1;
    result.payload_order = "TFP";
    result.packet_header_bytes = 32;
    result.packet_payload_bytes = 8;
    result.packet_samples = 2;
    result.packet_nbit = 16;
    result.sample_interval_us = 1.0;
    result.records_per_block = 8;
    result.raw_ring_blocks = 4;
    result.compute_ring_blocks = 4;
    result.file_blocks = 0;
    result.disk_enabled = false;
    result.direct_io = false;
    result.utc_start = "2026-08-01-00:00:00";
    return result;
}

rdma_dada::PipelineConfig MakeSmallPipeline() {
    rdma_dada::PipelineConfig result = MakePipeline();
    result.records_per_block = 4;
    return result;
}

unpack::VdifUnpackConfig MakeConfig() {
    unpack::VdifUnpackConfig result = {};
    result.first_channel_id = 20;
    result.antenna_map = {101, 102, 103, 104};
    result.window_blocks = 2;
    result.max_window_bytes = 128;
    result.output_memory = "HOST";
    return result;
}

unpack::VdifUnpackLayout MakeLayout() {
    unpack::VdifUnpackLayout result = {};
    result.raw_record_bytes = 40;
    result.records_per_raw_block = 8;
    result.group_bytes = 32;
    result.window_capacity_groups = 4;
    result.window_bytes = 128;
    result.compute_block_bytes = 64;
    return result;
}

std::vector<std::uint8_t> MakeRecord(std::uint32_t frame,
                                     std::uint16_t station,
                                     std::uint8_t antenna_index,
                                     bool invalid_data = false) {
    std::vector<std::uint8_t> record(40, 0);
    unpack::ProjectVdifHeader header = {};
    header.invalid_data = invalid_data;
    header.seconds_from_reference_epoch = 1000;
    header.reference_epoch = 52;
    header.frame_number_within_second = frame;
    header.station_id = station;
    header.first_channel_id = 20;
    header.nchan = 2;
    header.npol = 1;
    header.nsamp_per_packet = 2;
    header.component_bits = 8;
    header.frame_length_units_8_bytes = 5;
    std::string error;
    Expect(unpack::EncodeProjectVdifV1(header, record.data(), 32, &error),
           "test record header encodes: " + error);
    for (std::uint8_t index = 0; index < 8; ++index) {
        record[32 + index] = static_cast<std::uint8_t>(
            (frame - 10U) * 64U + antenna_index * 16U + index);
    }
    return record;
}

void Append(std::vector<std::uint8_t>* block,
            const std::vector<std::uint8_t>& record) {
    block->insert(block->end(), record.begin(), record.end());
}

std::vector<std::uint8_t> ExpectedGroup(std::uint32_t frame) {
    std::vector<std::uint8_t> expected;
    for (std::uint8_t tfp = 0; tfp < 4; ++tfp) {
        for (std::uint8_t antenna = 0; antenna < 4; ++antenna) {
            expected.push_back(static_cast<std::uint8_t>(
                (frame - 10U) * 64U + antenna * 16U + tfp * 2U));
            expected.push_back(static_cast<std::uint8_t>(
                (frame - 10U) * 64U + antenna * 16U + tfp * 2U + 1U));
        }
    }
    return expected;
}

struct EmittedGroup {
    unpack::VdifGroupKey key;
    std::vector<std::uint8_t> bytes;
};

unpack::VdifGroupEmitter Collect(std::vector<EmittedGroup>* emitted) {
    return [emitted](const unpack::VdifGroupKey& key,
                     const std::uint8_t* bytes, std::uint64_t size,
                     std::string*) {
        EmittedGroup group;
        group.key = key;
        group.bytes.assign(bytes, bytes + size);
        emitted->push_back(group);
        return true;
    };
}

void TestCrossBlockCompletion() {
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "cross-block engine configures: " + error);
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    std::vector<std::uint8_t> first;
    Append(&first, MakeRecord(10, 103, 2));
    Append(&first, MakeRecord(10, 101, 0));
    Expect(engine.ConsumeRawBlock(first.data(), first.size(), 20,
                                  collector, &error),
           "first half block consumes: " + error);
    Expect(emitted.empty(), "split group is retained after first raw block");
    std::vector<std::uint8_t> second;
    Append(&second, MakeRecord(10, 104, 3));
    Append(&second, MakeRecord(10, 102, 1));
    Expect(engine.ConsumeRawBlock(second.data(), second.size(), 21,
                                  collector, &error),
           "second half block consumes: " + error);
    Expect(emitted.size() == 1U && emitted[0].bytes == ExpectedGroup(10),
           "group split over N/N+1 raw blocks emits once as TFPA");
    Expect(engine.Finish(collector, &error), "cross-block engine finishes");
    Expect(emitted.size() == 1U, "Finish does not re-emit completed group");
}

void TestHorizonZeroFill() {
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "horizon engine configures: " + error);
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    std::vector<std::uint8_t> first;
    Append(&first, MakeRecord(10, 101, 0));
    Expect(engine.ConsumeRawBlock(first.data(), first.size(), 30,
                                  collector, &error),
           "incomplete group enters window");
    std::vector<std::uint8_t> second;
    Append(&second, MakeRecord(11, 101, 0));
    Append(&second, MakeRecord(11, 102, 1));
    Append(&second, MakeRecord(11, 103, 2));
    Append(&second, MakeRecord(11, 104, 3));
    Expect(engine.ConsumeRawBlock(second.data(), second.size(), 31,
                                  collector, &error),
           "next block advances horizon: " + error);
    Expect(emitted.size() == 1U, "only the aged stable prefix emits");
    if (emitted.size() == 1U) {
        const std::vector<std::uint8_t> zero_filled = {
            0,1, 0,0, 0,0, 0,0, 2,3, 0,0, 0,0, 0,0,
            4,5, 0,0, 0,0, 0,0, 6,7, 0,0, 0,0, 0,0
        };
        Expect(emitted[0].key.frame_number_within_second == 10U &&
               emitted[0].bytes == zero_filled,
               "missing Station slices remain zero in TFPA positions");
    }
    Expect(engine.Finish(collector, &error), "horizon engine finishes");
    Expect(emitted.size() == 2U &&
           emitted[1].key.frame_number_within_second == 11U,
           "young complete group remains buffered until stable or EOD");
    const unpack::VdifUnpackStatistics& stats = engine.statistics();
    Expect(stats.completed_groups == 1U && stats.incomplete_groups == 1U,
           "complete and incomplete groups counted separately");
    Expect(stats.missing_station_packets == 3U &&
           stats.expected_station_packets_for_observed_groups == 8U,
           "loss numerator and observed-group denominator are exact");
    Expect(stats.window_evictions == 1U, "horizon expiry counts one eviction");
}

void TestEodFlush() {
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "EOD engine configures");
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    const std::vector<std::uint8_t> record = MakeRecord(10, 102, 1);
    Expect(engine.ConsumeRawBlock(record.data(), record.size(), 40,
                                  collector, &error), "EOD record consumes");
    Expect(emitted.empty(), "young incomplete group waits before EOD");
    Expect(engine.Finish(collector, &error), "EOD flush succeeds");
    Expect(emitted.size() == 1U, "EOD emits observed incomplete group");
    const unpack::VdifUnpackStatistics& stats = engine.statistics();
    Expect(stats.incomplete_groups == 1U && stats.missing_station_packets == 3U,
           "EOD missing Stations are counted");
    Expect(stats.window_evictions == 0U, "EOD flush is not a window eviction");
}

void TestPacketRejectionsAndLateArrival() {
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "rejection engine configures");
    std::vector<std::uint8_t> block;
    std::vector<std::uint8_t> bad_header = MakeRecord(10, 101, 0);
    bad_header[28] = 1;
    Append(&block, bad_header);
    Append(&block, MakeRecord(10, 101, 0, true));
    Append(&block, MakeRecord(10, 999, 0));
    Append(&block, MakeRecord(10, 101, 0));
    Append(&block, MakeRecord(10, 101, 0));
    Append(&block, MakeRecord(10, 102, 1));
    Append(&block, MakeRecord(10, 103, 2));
    Append(&block, MakeRecord(10, 104, 3));
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    Expect(engine.ConsumeRawBlock(block.data(), block.size(), 50,
                                  collector, &error),
           "bad packets are counted without stopping: " + error);
    std::vector<std::uint8_t> advance;
    Append(&advance, MakeRecord(11, 101, 0));
    Expect(engine.ConsumeRawBlock(advance.data(), advance.size(), 51,
                                  collector, &error),
           "next block makes completed group stable");
    const std::vector<std::uint8_t> late = MakeRecord(10, 101, 0);
    Expect(engine.ConsumeRawBlock(late.data(), late.size(), 52,
                                  collector, &error),
           "late record is counted without stopping");
    const unpack::VdifUnpackStatistics& stats = engine.statistics();
    Expect(stats.received_records == 10U, "all good and rejected records counted");
    Expect(stats.accepted_packets == 5U, "only unique known valid packets accepted");
    Expect(stats.invalid_header_packets == 1U, "malformed header counted");
    Expect(stats.invalid_data_packets == 1U, "VDIF invalid-data flag counted");
    Expect(stats.unknown_station_packets == 1U, "unknown Station ID counted");
    Expect(stats.duplicate_packets == 1U, "duplicate active packet counted");
    Expect(stats.late_packets == 1U, "packet for emitted group counted late");
    Expect(emitted.size() == 2U &&
           emitted[0].key.frame_number_within_second == 10U,
           "rejections do not duplicate output; the later observed group may age out");
}

void TestCompleteGroupWaitsForUnseenEarlierKey() {
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "out-of-order group engine configures");
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    std::vector<std::uint8_t> newer;
    for (std::uint16_t station = 101; station <= 104; ++station)
        Append(&newer, MakeRecord(11, station,
                                 static_cast<std::uint8_t>(station - 101)));
    Expect(engine.ConsumeRawBlock(newer.data(), newer.size(), 80,
                                  collector, &error),
           "newer complete group consumes");
    Expect(emitted.empty(),
           "complete group waits because an unseen earlier key may still arrive");

    std::vector<std::uint8_t> older;
    for (std::uint16_t station = 101; station <= 104; ++station)
        Append(&older, MakeRecord(10, station,
                                 static_cast<std::uint8_t>(station - 101)));
    Expect(engine.ConsumeRawBlock(older.data(), older.size(), 81,
                                  collector, &error),
           "older group arrives inside reorder horizon");
    Expect(emitted.empty(), "unstable older prefix holds later group");

    const std::vector<std::uint8_t> advance = MakeRecord(12, 101, 0);
    Expect(engine.ConsumeRawBlock(advance.data(), advance.size(), 82,
                                  collector, &error),
           "third block advances stable prefix");
    Expect(emitted.size() == 2U &&
           emitted[0].key.frame_number_within_second == 10U &&
           emitted[1].key.frame_number_within_second == 11U,
           "late-observed older group is emitted before newer complete group");
}

void TestFullArenaPolicy() {
    unpack::VdifUnpackConfig config = MakeConfig();
    config.max_window_bytes = 64;
    unpack::VdifUnpackLayout layout = MakeLayout();
    layout.records_per_raw_block = 4;
    layout.window_capacity_groups = 2;
    layout.window_bytes = 64;
    layout.compute_block_bytes = 32;
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(config, MakeSmallPipeline(), layout, &error),
           "small arena configures: " + error);
    std::vector<std::uint8_t> block;
    Append(&block, MakeRecord(11, 101, 0));
    Append(&block, MakeRecord(12, 101, 0));
    Append(&block, MakeRecord(10, 101, 0));
    Append(&block, MakeRecord(13, 101, 0));
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    Expect(engine.ConsumeRawBlock(block.data(), block.size(), 60,
                                  collector, &error),
           "full arena applies late/eviction policy without stopping: " + error);
    Expect(emitted.size() == 1U &&
           emitted[0].key.frame_number_within_second == 11U,
           "newer key evicts and zero-fills the active oldest group");
    const unpack::VdifUnpackStatistics& stats = engine.statistics();
    Expect(stats.accepted_packets == 3U, "older new key is not accepted");
    Expect(stats.late_packets == 1U, "older new key at full arena counts late");
    Expect(stats.window_evictions == 1U, "arena reuse counts one eviction");
    Expect(engine.Finish(collector, &error), "small arena EOD flush succeeds");
    Expect(emitted.size() == 3U, "all observed non-late groups emit by EOD");
}

void TestPartialBlockContract() {
    unpack::VdifUnpackEngine valid;
    std::string error;
    Expect(valid.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "partial-block engine configures");
    const std::vector<std::uint8_t> record = MakeRecord(10, 101, 0);
    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter collector = Collect(&emitted);
    Expect(valid.ConsumeRawBlock(record.data(), record.size(), 70,
                                 collector, &error),
           "partial raw block containing whole records is accepted");

    unpack::VdifUnpackEngine trailing;
    Expect(trailing.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "trailing-byte engine configures");
    std::vector<std::uint8_t> malformed = record;
    malformed.push_back(0xff);
    Expect(!trailing.ConsumeRawBlock(malformed.data(), malformed.size(), 70,
                                    collector, &error),
           "trailing partial record rejects the raw block");
    Expect(trailing.statistics().received_records == 0U,
           "trailing partial record is rejected before publishing input state");

    unpack::VdifUnpackEngine oversized;
    Expect(oversized.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "oversized-block engine configures");
    std::vector<std::uint8_t> too_many_records;
    for (int i = 0; i < 9; ++i)
        Append(&too_many_records, MakeRecord(10 + i, 101, 0));
    Expect(!oversized.ConsumeRawBlock(too_many_records.data(),
                                      too_many_records.size(), 70,
                                      collector, &error),
           "input larger than configured raw block is rejected");
    Expect(oversized.statistics().received_records == 0U,
           "oversized raw block is rejected before consuming records");
}

void TestConfigurationContract() {
    std::string error;
    unpack::VdifUnpackEngine engine;
    unpack::VdifUnpackConfig config = MakeConfig();
    config.window_blocks = 1;
    Expect(!engine.Configure(config, MakePipeline(), MakeLayout(), &error),
           "engine independently rejects a one-block reorder window");

    config = MakeConfig();
    config.max_window_bytes = 127;
    Expect(!engine.Configure(config, MakePipeline(), MakeLayout(), &error),
           "engine enforces configured window allocation cap");

    config = MakeConfig();
    unpack::VdifUnpackLayout inconsistent = MakeLayout();
    inconsistent.window_capacity_groups = 3;
    inconsistent.window_bytes = 96;
    Expect(!engine.Configure(config, MakePipeline(), inconsistent, &error),
           "engine rejects a layout not derived from window_blocks and raw geometry");
}

}  // namespace

int main() {
    const rdma_dada::PipelineConfig pipeline = MakePipeline();
    const unpack::VdifUnpackConfig config = MakeConfig();
    const unpack::VdifUnpackLayout layout = MakeLayout();
    unpack::VdifUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(config, pipeline, layout, &error),
           "engine configures: " + error);

    std::vector<std::uint8_t> block;
    const std::uint16_t station_order[] = {103, 101, 104, 102};
    const std::uint8_t antenna_order[] = {2, 0, 3, 1};
    for (std::uint32_t frame = 10; frame <= 11; ++frame) {
        for (std::size_t i = 0; i < 4; ++i) {
            Append(&block, MakeRecord(frame, station_order[i], antenna_order[i]));
        }
    }

    std::vector<EmittedGroup> emitted;
    const unpack::VdifGroupEmitter emitter =
        [&emitted](const unpack::VdifGroupKey& key,
                   const std::uint8_t* bytes, std::uint64_t size,
                   std::string*) {
            EmittedGroup group;
            group.key = key;
            group.bytes.assign(bytes, bytes + size);
            emitted.push_back(group);
            return true;
        };
    Expect(engine.ConsumeRawBlock(block.data(), block.size(), 7,
                                  emitter, &error),
           "in-order raw block is consumed: " + error);
    Expect(engine.Finish(emitter, &error), "engine finishes: " + error);
    Expect(emitted.size() == 2U, "two observed groups emitted exactly once");
    if (emitted.size() == 2U) {
        Expect(emitted[0].key.frame_number_within_second == 10U &&
               emitted[1].key.frame_number_within_second == 11U,
               "groups are emitted in key order");
        Expect(emitted[0].bytes == ExpectedGroup(10),
               "frame 10 is scattered from arbitrary Station order to TFPA");
        Expect(emitted[1].bytes == ExpectedGroup(11),
               "frame 11 is scattered from arbitrary Station order to TFPA");
    }
    const unpack::VdifUnpackStatistics& statistics = engine.statistics();
    Expect(statistics.received_records == 8U, "all records counted");
    Expect(statistics.accepted_packets == 8U, "all packets accepted");
    Expect(statistics.completed_groups == 2U, "complete groups counted");
    Expect(statistics.incomplete_groups == 0U, "no incomplete groups");
    Expect(statistics.expected_station_packets_for_observed_groups == 8U,
           "expected Station denominator counts observed groups only");

    TestCrossBlockCompletion();
    TestHorizonZeroFill();
    TestEodFlush();
    TestPacketRejectionsAndLateArrival();
    TestCompleteGroupWaitsForUnseenEarlierKey();
    TestFullArenaPolicy();
    TestPartialBlockContract();
    TestConfigurationContract();

    if (failures) return 1;
    std::cout << "vdif_unpack_engine_test passed\n";
    return 0;
}
