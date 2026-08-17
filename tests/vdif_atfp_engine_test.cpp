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

std::vector<std::uint8_t> MakeTimelineRecord(
    const unpack::VdifTimeline& timeline, std::uint64_t ordinal,
    std::uint16_t station, std::uint32_t antenna) {
    std::uint32_t seconds = 0;
    std::uint32_t frame = 0;
    std::string error;
    Expect(unpack::VdifOrdinalToTime(timeline, ordinal, &seconds, &frame,
                                     &error),
           "test timeline record time resolves: " + error);
    std::vector<std::uint8_t> result = MakeRecord(
        ordinal, station, antenna);
    unpack::ProjectVdifHeader header = {};
    Expect(unpack::DecodeProjectVdifV1(result.data(), 32, &header, &error),
           "test timeline record decodes: " + error);
    header.seconds_from_reference_epoch = seconds;
    header.frame_number_within_second = frame;
    Expect(unpack::EncodeProjectVdifV1(header, result.data(), 32, &error),
           "test timeline record re-encodes: " + error);
    return result;
}

std::vector<std::uint8_t> MakeTimestampRecord(
    std::uint32_t seconds, std::uint32_t frame, std::uint16_t station,
    std::uint32_t antenna) {
    std::vector<std::uint8_t> result = MakeRecord(0, station, antenna);
    unpack::ProjectVdifHeader header = {};
    std::string error;
    Expect(unpack::DecodeProjectVdifV1(result.data(), 32, &header, &error),
           "timestamp record decodes: " + error);
    header.seconds_from_reference_epoch = seconds;
    header.frame_number_within_second = frame;
    Expect(unpack::EncodeProjectVdifV1(header, result.data(), 32, &error),
           "timestamp record re-encodes: " + error);
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
    AppendCompleteGroup(&records, 6);
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
        for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
            present67.push_back(std::make_pair(6U, antenna));
        Expect(output[3].bytes == ExpectedBlock(6, 2, present67),
               "EOD preserves one far packet and zero-fills the trailing group");
    }
    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.fully_missing_groups == 2U,
           "groups three and seven are fully missing");
    Expect(stats.large_gap_advances == 1U,
           "far-future capacity advance is counted");
    Expect(stats.large_gap_advanced_groups == 2U,
           "capacity advance reports the number of emitted groups");
    Expect(stats.expected_station_packets == 24U,
           "loss denominator covers all EXPECTED_GROUPS");
}

void TestOneLeadingStationCannotEvictLaggingStations() {
    unpack::VdifAtfpUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(6), &error),
           "multi-Station watermark engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);

    std::vector<std::uint8_t> records;
    AppendCompleteGroup(&records, 0);
    Append(&records, MakeRecord(3, 205, 1));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 40, emit,
                                  &error),
           "leading Station block consumes: " + error);
    Expect(output.empty(),
           "one leading Station cannot finalize a lagging incomplete group");

    records.clear();
    AppendCompleteGroup(&records, 1);
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 41, emit,
                                  &error),
           "lagging Station records remain in-window: " + error);
    Expect(output.size() == 1U,
           "completed lagging group emits one compute block");
    if (output.size() == 1U) {
        std::vector<std::pair<std::uint64_t, std::uint32_t> > present;
        for (std::uint64_t group = 0; group < 2; ++group)
            for (std::uint32_t antenna = 0; antenna < 3; ++antenna)
                present.push_back(std::make_pair(group, antenna));
        Expect(output[0].bytes == ExpectedBlock(0, 2, present),
               "lagging Station payloads are preserved instead of zero-filled");
    }
    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.late_packets == 0U,
           "multi-Station watermark does not create artificial late packets");
}

void TestExplicitMissingWaitLeavesStationSkewReserve() {
    unpack::VdifAtfpUnpackEngine engine;
    unpack::VdifUnpackConfig config = MakeConfig();
    config.window_blocks = 3;
    config.max_window_bytes = 720;
    config.reorder_horizon_groups = 2;
    unpack::VdifUnpackLayout layout = MakeLayout();
    layout.window_capacity_groups = 6;
    layout.window_bytes = 720;
    std::string error;
    Expect(engine.Configure(config, MakePipeline(), layout, MakeTimeline(8),
                            &error),
           "explicit missing-wait engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);

    std::vector<std::uint8_t> records;
    Append(&records, MakeRecord(0, 101, 0));
    AppendCompleteGroup(&records, 1);
    Append(&records, MakeRecord(5, 101, 0));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 50, emit,
                                  &error),
           "leading Station fits in dedicated skew reserve: " + error);
    Expect(output.empty(), "incomplete group still waits two groups");

    records.clear();
    AppendCompleteGroup(&records, 2);
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 51, emit,
                                  &error),
           "missing wait expires independently of window capacity: " + error);
    Expect(output.size() == 1U,
           "explicit missing wait emits while skew reserve remains occupied");
}

void TestStaticPrepareThenBeginTransfer() {
    unpack::VdifAtfpUnpackEngine engine;
    unpack::VdifUnpackConfig config = MakeConfig();
    unpack::VdifUnpackLayout layout = MakeLayout();
    std::string error;
    Expect(engine.Prepare(config, MakePipeline(), layout, &error),
           "ATFP static resources prepare before timeline: " + error);
    Expect(engine.prepared(), "ATFP engine reports prepared state");
    Expect(engine.prepared_window_bytes() == layout.window_bytes,
           "prepared window byte count is exposed for readiness");
    Expect(engine.BeginTransfer(MakeTimeline(8), &error),
           "ATFP timeline begins without reallocating static resources: " + error);
}

void TestOptionalMissingPacketsPerSecondStatistics() {
    unpack::VdifTimeline timeline = MakeTimeline(4);
    timeline.group_period_ps = UINT64_C(500000000000);

    unpack::VdifAtfpUnpackEngine disabled;
    std::string error;
    Expect(disabled.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                              timeline, &error),
           "default diagnostics engine configures: " + error);
    std::vector<CollectedBlock> disabled_output;
    Expect(disabled.Finish(Collect(&disabled_output), &error),
           "default diagnostics engine flushes: " + error);
    Expect(disabled.statistics().missing_station_packets_per_second.empty(),
           "per-second missing diagnostics are disabled by default");

    unpack::VdifAtfpUnpackEngine enabled;
    Expect(enabled.Prepare(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "per-second diagnostics engine prepares: " + error);
    Expect(enabled.BeginTransfer(timeline, true, &error),
           "per-second diagnostics transfer begins: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);

    std::vector<std::uint8_t> records;
    for (std::uint32_t antenna = 0; antenna < 3; ++antenna) {
        Append(&records, MakeTimelineRecord(
            timeline, 0, MakeConfig().antenna_map[antenna], antenna));
    }
    Append(&records, MakeTimelineRecord(timeline, 1, 101, 0));
    Append(&records, MakeTimelineRecord(timeline, 1, 205, 1));
    Expect(enabled.ConsumeRawBlock(records.data(), records.size(), 60, emit,
                                   &error),
           "first diagnostic second consumes: " + error);

    records.clear();
    for (std::uint32_t antenna = 0; antenna < 3; ++antenna) {
        Append(&records, MakeTimelineRecord(
            timeline, 3, MakeConfig().antenna_map[antenna], antenna));
    }
    Expect(enabled.ConsumeRawBlock(records.data(), records.size(), 61, emit,
                                   &error),
           "second diagnostic second consumes: " + error);
    Expect(enabled.Finish(emit, &error),
           "per-second diagnostics transfer flushes: " + error);

    const unpack::VdifAtfpStatistics& statistics = enabled.statistics();
    Expect(statistics.missing_station_packets == 4U,
           "one incomplete and one fully missing group count four packets");
    Expect(statistics.missing_station_packets_per_second.size() == 2U &&
               statistics.missing_station_packets_per_second[0] == 1U &&
               statistics.missing_station_packets_per_second[1] == 3U,
           "missing packets are counted in their expected VDIF second");
}

void TestPreTimelinePacketsAreExcludedFromFormalStatistics() {
    unpack::VdifAtfpUnpackEngine engine;
    unpack::VdifTimeline timeline = MakeTimeline(2);
    timeline.groups_per_second = 2U;
    std::string error;
    Expect(engine.Prepare(MakeConfig(), MakePipeline(), MakeLayout(), &error),
           "pre-timeline discard engine prepares: " + error);
    Expect(engine.BeginTransfer(timeline, false, true, &error),
           "pre-timeline discard policy begins: " + error);

    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);
    std::vector<std::uint8_t> records;
    Append(&records, MakeTimestampRecord(999, 7, 205, 1));
    Append(&records, MakeTimestampRecord(999, 8, 101, 0));
    Append(&records, MakeTimelineRecord(timeline, 0, 409, 2));
    Append(&records, MakeTimelineRecord(timeline, 0, 101, 0));
    Append(&records, MakeTimelineRecord(timeline, 0, 205, 1));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 62, emit,
                                  &error),
           "preparation and formal records consume together: " + error);
    records.clear();
    Append(&records, MakeTimelineRecord(timeline, 1, 205, 1));
    Append(&records, MakeTimelineRecord(timeline, 1, 409, 2));
    Append(&records, MakeTimelineRecord(timeline, 1, 101, 0));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 63, emit,
                                  &error),
           "formal Station order remains unrestricted: " + error);
    Expect(engine.Finish(emit, &error),
           "formal records flush after preparation discard: " + error);

    const unpack::VdifAtfpStatistics& statistics = engine.statistics();
    Expect(statistics.received_records == 6U,
           "pre-timeline records are excluded from formal record count");
    Expect(statistics.accepted_packets == 6U,
           "all formal Station records are accepted");
    Expect(statistics.completed_groups == 2U &&
               statistics.missing_station_packets == 0U &&
               statistics.out_of_range_packets == 0U,
           "formal interval starts complete at group zero");
}

void TestStationSkewAndRawBlockCompositionStatistics() {
    unpack::VdifAtfpUnpackEngine engine;
    std::string error;
    Expect(engine.Configure(MakeConfig(), MakePipeline(), MakeLayout(),
                            MakeTimeline(8), &error),
           "station statistics engine configures: " + error);
    std::vector<CollectedBlock> output;
    const unpack::VdifAtfpBlockEmitter emit = Collect(&output);
    std::vector<std::uint8_t> records;
    Append(&records, MakeRecord(0, 101, 0));
    Append(&records, MakeRecord(1, 101, 0));
    Append(&records, MakeRecord(2, 101, 0));
    Append(&records, MakeRecord(0, 205, 1));
    Append(&records, MakeRecord(0, 409, 2));
    Expect(engine.ConsumeRawBlock(records.data(), records.size(), 30, emit,
                                  &error),
           "station statistics block consumes: " + error);

    const unpack::VdifAtfpStatistics& stats = engine.statistics();
    Expect(stats.station_observed_packets.size() == 3U &&
               stats.station_observed_packets[0] == 3U &&
               stats.station_observed_packets[1] == 1U &&
               stats.station_observed_packets[2] == 1U,
           "observed records are counted by antenna index");
    Expect(stats.station_accepted_packets == stats.station_observed_packets,
           "in-window station records are accepted");
    Expect(stats.station_late_packets.size() == 3U &&
               stats.station_late_packets[0] == 0U &&
               stats.station_late_packets[1] == 0U &&
               stats.station_late_packets[2] == 0U,
           "station late counters start at zero");
    Expect(stats.station_highest_ordinals.size() == 3U &&
               stats.station_highest_ordinals[0] == 2U &&
               stats.station_highest_ordinals[1] == 0U &&
               stats.station_highest_ordinals[2] == 0U,
           "highest ordinal is retained per Station");
    Expect(stats.max_station_ordinal_skew == 2U,
           "maximum Station ordinal skew is measured at block boundaries");
    Expect(stats.mixed_station_raw_blocks == 1U &&
               stats.single_station_raw_blocks == 0U,
           "raw block Station diversity is classified");
    Expect(stats.max_station_records_per_raw_block == 3U,
           "maximum records from one Station in a raw block is measured");
    Expect(stats.max_consecutive_station_records == 3U,
           "maximum consecutive Station run is measured");
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
    Append(&records, MakeRecord(2, 205, 1));
    Append(&records, MakeRecord(2, 409, 2));
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
    TestStationSkewAndRawBlockCompositionStatistics();
    TestOneLeadingStationCannotEvictLaggingStations();
    TestExplicitMissingWaitLeavesStationSkewReserve();
    TestStaticPrepareThenBeginTransfer();
    TestOptionalMissingPacketsPerSecondStatistics();
    TestPreTimelinePacketsAreExcludedFromFormalStatistics();
    TestPacketClassificationAndPartialEod();
    TestMalformedRawBlockDoesNotPublish();
    TestExpectedTransferOverflowIsRejected();
    if (failures != 0) return 1;
    std::cout << "vdif_atfp_engine_test passed\n";
    return 0;
}
