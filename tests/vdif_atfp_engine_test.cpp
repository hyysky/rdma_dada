#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"
#include "rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
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
    result.nant = 3;
    result.nchan = 2;
    result.npol = 2;
    result.payload_order = "TFP";
    result.packet_header_bytes = 32;
    result.packet_payload_bytes = 40;
    result.packet_samples = 5;
    result.packet_nbit = 16;
    result.sample_interval_us = 1.0;
    result.records_per_block = 6;
    result.raw_ring_blocks = 4;
    result.compute_ring_blocks = 4;
    result.file_blocks = 0;
    result.disk_enabled = false;
    result.direct_io = false;
    result.utc_start = "2026-08-08-00:00:00";
    return result;
}

unpack::VdifUnpackConfig MakeConfig() {
    unpack::VdifUnpackConfig result = {};
    result.first_channel_id = 20;
    result.antenna_map = {101, 205, 409};
    result.window_blocks = 2;
    result.max_window_bytes = 480;
    result.output_memory = "HOST";
    return result;
}

unpack::VdifUnpackLayout MakeLayout() {
    unpack::VdifUnpackLayout result = {};
    result.raw_record_bytes = 72;
    result.records_per_raw_block = 6;
    result.group_bytes = 120;
    result.window_capacity_groups = 4;
    result.window_bytes = 480;
    result.compute_block_bytes = 240;
    return result;
}

unpack::VdifTimeline MakeTimeline(std::uint64_t expected_groups) {
    unpack::VdifTimeline result = {};
    result.group_period_ps = 5000000;
    result.start_reference_epoch = 10;
    result.start_seconds = 1000;
    result.start_frame = 0;
    result.expected_groups = expected_groups;
    return result;
}

std::vector<std::uint8_t> MakePayload(std::uint64_t ordinal,
                                      std::uint32_t antenna) {
    std::vector<std::uint8_t> result(40);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            1U + ordinal * 53U + antenna * 41U + index);
    }
    return result;
}

std::vector<std::uint8_t> MakeRecord(std::uint64_t ordinal,
                                     std::uint16_t station,
                                     std::uint32_t antenna,
                                     bool invalid_data = false) {
    std::vector<std::uint8_t> result(72, 0);
    unpack::ProjectVdifHeader header = {};
    header.invalid_data = invalid_data;
    header.seconds_from_reference_epoch = 1000;
    header.reference_epoch = 10;
    header.frame_number_within_second = static_cast<std::uint32_t>(ordinal);
    header.station_id = station;
    header.first_channel_id = 20;
    header.nchan = 2;
    header.npol = 2;
    header.nsamp_per_packet = 5;
    header.component_bits = 8;
    header.frame_length_units_8_bytes = 9;
    std::string error;
    Expect(unpack::EncodeProjectVdifV1(header, result.data(), 32, &error),
           "test record encodes: " + error);
    const std::vector<std::uint8_t> payload = MakePayload(ordinal, antenna);
    std::copy(payload.begin(), payload.end(), result.begin() + 32);
    return result;
}

void Append(std::vector<std::uint8_t>* block,
            const std::vector<std::uint8_t>& record) {
    block->insert(block->end(), record.begin(), record.end());
}

struct CollectedBlock {
    std::uint64_t first_ordinal;
    std::uint64_t group_count;
    std::vector<std::uint8_t> bytes;
};

unpack::VdifAtfpBlockEmitter Collect(std::vector<CollectedBlock>* output) {
    return [output](const unpack::AtfpBlockView& view, std::string* error) {
        if (!view.window_data || view.group_count == 0U || view.nant == 0U ||
            view.packet_payload_bytes == 0U ||
            view.first_slot >= view.window_capacity_groups ||
            view.group_count > view.window_capacity_groups) {
            if (error) *error = "invalid ATFP view in test collector";
            return false;
        }
        CollectedBlock block;
        block.first_ordinal = view.first_group_ordinal;
        block.group_count = view.group_count;
        block.bytes.resize(static_cast<std::size_t>(
            view.nant * view.group_count * view.packet_payload_bytes));
        const std::uint64_t first_groups = std::min(
            view.group_count, view.window_capacity_groups - view.first_slot);
        const std::uint64_t second_groups = view.group_count - first_groups;
        for (std::uint32_t antenna = 0; antenna < view.nant; ++antenna) {
            const std::uint8_t* plane =
                view.window_data +
                antenna * view.window_capacity_groups *
                    view.packet_payload_bytes;
            std::uint8_t* destination =
                block.bytes.data() +
                antenna * view.group_count * view.packet_payload_bytes;
            std::memcpy(destination,
                        plane + view.first_slot * view.packet_payload_bytes,
                        static_cast<std::size_t>(
                            first_groups * view.packet_payload_bytes));
            if (second_groups != 0U) {
                std::memcpy(
                    destination + first_groups * view.packet_payload_bytes,
                    plane,
                    static_cast<std::size_t>(
                        second_groups * view.packet_payload_bytes));
            }
        }
        output->push_back(block);
        return true;
    };
}

unpack::VdifGroupEmitter CollectLegacy(
    std::vector<std::vector<std::uint8_t> >* output) {
    return [output](const unpack::VdifGroupKey&, const std::uint8_t* bytes,
                    std::uint64_t size, std::string*) {
        output->push_back(std::vector<std::uint8_t>(bytes, bytes + size));
        return true;
    };
}

std::vector<std::uint8_t> AtfpToTfpa(const CollectedBlock& block) {
    const std::uint64_t sample_bytes = 2U;
    const std::uint64_t samples_per_payload = 40U / sample_bytes;
    std::vector<std::uint8_t> result(block.bytes.size());
    for (std::uint64_t group = 0; group < block.group_count; ++group) {
        for (std::uint64_t sample = 0; sample < samples_per_payload; ++sample) {
            for (std::uint64_t antenna = 0; antenna < 3U; ++antenna) {
                const std::uint64_t source =
                    (antenna * block.group_count * 40U) + group * 40U +
                    sample * sample_bytes;
                const std::uint64_t destination =
                    ((group * samples_per_payload + sample) * 3U + antenna) *
                    sample_bytes;
                std::memcpy(result.data() + destination,
                            block.bytes.data() + source,
                            static_cast<std::size_t>(sample_bytes));
            }
        }
    }
    return result;
}

std::vector<std::uint8_t> ExpectedBlock(
    std::uint64_t first_ordinal, std::uint64_t group_count,
    const std::vector<std::pair<std::uint64_t, std::uint32_t> >& present) {
    std::vector<std::uint8_t> expected(
        static_cast<std::size_t>(3U * group_count * 40U), 0);
    for (std::size_t item = 0; item < present.size(); ++item) {
        const std::uint64_t ordinal = present[item].first;
        const std::uint32_t antenna = present[item].second;
        if (ordinal < first_ordinal ||
            ordinal >= first_ordinal + group_count || antenna >= 3U) {
            continue;
        }
        const std::vector<std::uint8_t> payload =
            MakePayload(ordinal, antenna);
        const std::uint64_t local_group = ordinal - first_ordinal;
        std::copy(payload.begin(), payload.end(),
                  expected.begin() + static_cast<std::ptrdiff_t>(
                      (antenna * group_count + local_group) * 40U));
    }
    return expected;
}

void AppendCompleteGroup(std::vector<std::uint8_t>* block,
                         std::uint64_t ordinal) {
    Append(block, MakeRecord(ordinal, 409, 2));
    Append(block, MakeRecord(ordinal, 101, 0));
    Append(block, MakeRecord(ordinal, 205, 1));
}

void TestArbitraryArrivalProducesAtfpBlock() {
    unpack::VdifAtfpUnpackEngine engine;
    unpack::VdifUnpackEngine legacy;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(2), &error),
           "ATFP engine configures: " + error);
    Expect(legacy.Configure(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "legacy reference engine configures: " + error);
    std::vector<CollectedBlock> output;
    std::vector<std::vector<std::uint8_t> > legacy_groups;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);
    const unpack::VdifGroupEmitter legacy_emit = CollectLegacy(&legacy_groups);
    std::vector<std::uint8_t> first;
    Append(&first, MakeRecord(0, 205, 1));
    Append(&first, MakeRecord(1, 409, 2));
    Append(&first, MakeRecord(0, 101, 0));
    Expect(engine.ConsumeRawBlock(first.data(), first.size(), 0, emit, &error),
           "first arbitrary block consumes: " + error);
    Expect(legacy.ConsumeRawBlock(first.data(), first.size(), 0, legacy_emit,
                                  &error),
           "legacy first arbitrary block consumes: " + error);
    Expect(output.empty(), "incomplete output block stays private");

    std::vector<std::uint8_t> second;
    Append(&second, MakeRecord(1, 101, 0));
    Append(&second, MakeRecord(0, 409, 2));
    Append(&second, MakeRecord(1, 205, 1));
    Expect(engine.ConsumeRawBlock(second.data(), second.size(), 1, emit,
                                  &error),
           "second arbitrary block consumes: " + error);
    Expect(legacy.ConsumeRawBlock(second.data(), second.size(), 1, legacy_emit,
                                  &error),
           "legacy second arbitrary block consumes: " + error);
    Expect(output.size() == 1U, "one complete compute block emits");
    if (output.size() == 1U) {
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present;
        for (std::uint64_t group = 0; group < 2; ++group)
            for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
                present.push_back(std::make_pair(group, antenna));
        Expect(output[0].first_ordinal == 0U && output[0].group_count == 2U,
               "first ATFP block covers groups zero and one");
        Expect(output[0].bytes == ExpectedBlock(0, 2, present),
               "arbitrary Station arrival is physically ATFP");
        std::vector<std::uint8_t> legacy_tfpa;
        for (std::size_t index = 0; index < legacy_groups.size(); ++index) {
            legacy_tfpa.insert(legacy_tfpa.end(), legacy_groups[index].begin(),
                               legacy_groups[index].end());
        }
        Expect(AtfpToTfpa(output[0]) == legacy_tfpa,
               "ATFP plus independent transpose is byte-identical to legacy TFPA");
    }
    Expect(engine.Finish(emit, &error), "completed engine finishes: " + error);
    Expect(legacy.Finish(legacy_emit, &error),
           "legacy reference engine finishes: " + error);
    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.accepted_packets == 6U, "all six unique packets accepted");
    Expect(stats.payload_copy_calls == 6U,
           "each accepted packet performs one payload placement copy");
    Expect(stats.payload_copy_bytes == 240U,
           "payload placement bytes equal accepted payload bytes");
}

void TestWatermarkZeroFillAndSlotReuse() {
    unpack::VdifAtfpUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(8), &error),
           "watermark engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);

    std::vector<std::uint8_t> records;
    Append(&records, MakeRecord(0, 101, 0));
    AppendCompleteGroup(&records, 1);
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 10, emit,
                                  &error),
           "watermark prefix consumes: " + error);
    Expect(output.empty(), "group zero still waits inside reorder horizon");
    records.clear();
    AppendCompleteGroup(&records, 2);
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 11, emit,
                                  &error),
           "watermark advance consumes: " + error);
    Expect(output.size() == 1U, "group two stabilizes incomplete group zero");
    if (output.size() == 1U) {
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present;
        present.push_back(std::make_pair(0U, 0U));
        for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
            present.push_back(std::make_pair(1U, antenna));
        Expect(output[0].bytes == ExpectedBlock(0, 2, present),
               "missing Station slices are zero in ATFP positions");
    }

    records.clear();
    Append(&records, MakeRecord(6, 101, 0));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 12, emit,
                                  &error),
           "far-future packet advances safe window: " + error);
    Expect(output.size() == 2U, "fully missing groups two/three publish once");
    if (output.size() >= 2U) {
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present23;
        for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
            present23.push_back(std::make_pair(2U, antenna));
        Expect(output[1].first_ordinal == 2U &&
                   output[1].bytes == ExpectedBlock(2, 2, present23),
               "capacity advance preserves group two and zero-fills group three");
    }

    records.clear();
    AppendCompleteGroup(&records, 4);
    AppendCompleteGroup(&records, 5);
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 13, emit,
                                  &error),
           "reused circular slots accept later groups: " + error);
    Expect(engine.Finish(emit, &error), "watermark engine EOD flushes: " + error);
    Expect(output.size() == 4U, "all four compute ranges publish exactly once");
    if (output.size() == 4U) {
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present45;
        for (std::uint64_t group = 4; group < 6; ++group)
            for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
                present45.push_back(std::make_pair(group, antenna));
        Expect(output[2].bytes == ExpectedBlock(4, 2, present45),
               "slot reuse does not leak stale group bytes");
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present67;
        present67.push_back(std::make_pair(6U, 0U));
        Expect(output[3].bytes == ExpectedBlock(6, 2, present67),
               "EOD preserves one far packet and zero-fills the trailing group");
    }
    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.fully_missing_groups == 2U,
           "groups three and seven are fully missing");
    Expect(stats.large_gap_advances == 1U,
           "far-future capacity advance is counted");
    Expect(stats.expected_station_packets == 24U,
           "loss denominator covers all EXPECTED_GROUPS");
}

void TestPacketClassificationAndPartialEod() {
    unpack::VdifAtfpUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(3), &error),
           "classification engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);
    std::vector<std::uint8_t> records;
    Append(&records, MakeRecord(1, 101, 0));
    Append(&records, MakeRecord(1, 101, 0));
    Append(&records, MakeRecord(1, 999, 0));
    Append(&records, MakeRecord(1, 205, 1, true));
    Append(&records, MakeRecord(1, 409, 2));
    Append(&records, MakeRecord(3, 101, 0));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 20, emit,
                                  &error),
           "recoverable packet classes consume: " + error);
    records.clear();
    Append(&records, MakeRecord(2, 101, 0));
    Append(&records, MakeRecord(1, 205, 1));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 21, emit,
                                  &error),
           "classification watermark consumes: " + error);
    Expect(output.size() == 1U, "completed first range emits before EOD");

    records.clear();
    Append(&records, MakeRecord(0, 101, 0));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 22, emit,
                                  &error),
           "late packet is recoverable after output: " + error);
    Expect(engine.Finish(emit, &error), "classification EOD flushes: " + error);
    Expect(output.size() == 2U, "one full and one compact partial block emit");
    if (output.size() == 2U) {
        Expect(output[0].group_count == 2U && output[1].group_count == 1U,
               "final block uses actual one-group extent");
        Expect(output[1].bytes.size() == 120U,
               "partial ATFP block has no antenna-plane gaps");
    }
    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.duplicate_packets == 1U, "duplicate is counted");
    Expect(stats.unknown_station_packets == 1U, "unknown Station is counted");
    Expect(stats.invalid_data_packets == 1U, "invalid-data packet is counted");
    Expect(stats.out_of_range_packets == 1U, "exclusive-boundary packet counted");
    Expect(stats.late_packets == 1U, "already emitted packet counted late");
}

void TestMalformedRawBlockDoesNotPublish() {
    unpack::VdifAtfpUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(2), &error),
           "malformed-input engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);
    std::vector<std::uint8_t> partial = MakeRecord(0, 101, 0);
    partial.pop_back();
    Expect(!engine.ConsumeRawBlock(partial.data(), partial.size(), 0, emit,
                                   &error),
           "partial Project VDIF record is rejected");
    Expect(output.empty(), "malformed raw block publishes no output");
}

void TestExpectedTransferOverflowIsRejected() {
    unpack::VdifAtfpUnpackEngine engine;
    unpack::VdifTimeline timeline =
        MakeTimeline(std::numeric_limits<std::uint64_t>::max());
    std::string error;
    Expect(!engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                             timeline, &error),
           "EXPECTED_GROUPS times group bytes overflow is rejected");
}

}  // namespace

int main() {
    TestArbitraryArrivalProducesAtfpBlock();
    TestWatermarkZeroFillAndSlotReuse();
    TestPacketClassificationAndPartialEod();
    TestMalformedRawBlockDoesNotPublish();
    TestExpectedTransferOverflowIsRejected();
    if (failures != 0) return 1;
    std::cout << "vdif_atfp_engine_test passed\n";
    return 0;
}
