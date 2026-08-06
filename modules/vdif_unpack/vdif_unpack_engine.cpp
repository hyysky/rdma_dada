#include "rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h"

#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool EqualKey(const VdifGroupKey& left, const VdifGroupKey& right) {
    return !(left < right) && !(right < left);
}

}  // namespace

bool VdifGroupKey::operator<(const VdifGroupKey& other) const {
    if (reference_epoch != other.reference_epoch)
        return reference_epoch < other.reference_epoch;
    if (seconds_from_reference_epoch != other.seconds_from_reference_epoch)
        return seconds_from_reference_epoch < other.seconds_from_reference_epoch;
    if (frame_number_within_second != other.frame_number_within_second)
        return frame_number_within_second < other.frame_number_within_second;
    if (first_channel_id != other.first_channel_id)
        return first_channel_id < other.first_channel_id;
    if (nchan != other.nchan) return nchan < other.nchan;
    return npol < other.npol;
}

struct VdifUnpackEngine::Impl {
    struct Slot {
        VdifGroupKey key;
        std::uint64_t first_seen_block;
        std::uint32_t seen_count;
        bool active;
    };

    VdifUnpackConfig config;
    PipelineConfig pipeline;
    VdifUnpackLayout layout;
    ProjectVdifGeometry geometry;
    VdifUnpackStatistics statistics;
    std::vector<std::uint8_t> arena;
    std::vector<std::uint8_t> station_seen;
    std::vector<Slot> slots;
    std::vector<std::size_t> free_slots;
    std::map<std::uint16_t, std::size_t> station_to_antenna;
    std::map<VdifGroupKey, std::size_t> active;
    VdifGroupKey last_emitted;
    bool has_last_emitted;
    bool configured;
    bool finished;
    bool has_block_sequence;
    std::uint64_t last_block_sequence;
    std::uint64_t raw_block_bytes;

    Impl()
        : statistics(), has_last_emitted(false), configured(false),
          finished(false), has_block_sequence(false), last_block_sequence(0),
          raw_block_bytes(0) {}

    std::uint8_t* SlotBytes(std::size_t index) {
        return arena.data() + index * layout.group_bytes;
    }

    std::uint8_t* SlotSeen(std::size_t index) {
        return station_seen.data() + index * pipeline.nant;
    }

    void Release(std::map<VdifGroupKey, std::size_t>::iterator group) {
        const std::size_t slot = group->second;
        slots[slot].active = false;
        free_slots.push_back(slot);
        active.erase(group);
    }

    bool Emit(std::map<VdifGroupKey, std::size_t>::iterator group,
              const VdifGroupEmitter& emitter, std::string* error) {
        const std::size_t slot = group->second;
        if (!emitter(group->first, SlotBytes(slot), layout.group_bytes, error))
            return false;
        const std::uint64_t missing = pipeline.nant - slots[slot].seen_count;
        statistics.expected_station_packets_for_observed_groups += pipeline.nant;
        statistics.missing_station_packets += missing;
        if (missing == 0) ++statistics.completed_groups;
        else ++statistics.incomplete_groups;
        last_emitted = group->first;
        has_last_emitted = true;
        Release(group);
        return true;
    }

    bool EvictOldest(const VdifGroupEmitter& emitter, std::string* error) {
        if (active.empty()) return Fail("cannot evict an empty arena", error);
        std::map<VdifGroupKey, std::size_t>::iterator oldest = active.begin();
        if (!Emit(oldest, emitter, error)) return false;
        ++statistics.window_evictions;
        return true;
    }

    bool EmitStablePrefix(std::uint64_t current_block,
                          const VdifGroupEmitter& emitter,
                          std::string* error) {
        while (!active.empty()) {
            std::map<VdifGroupKey, std::size_t>::iterator oldest = active.begin();
            const Slot& slot = slots[oldest->second];
            const std::uint64_t age = current_block - slot.first_seen_block;
            if (age < static_cast<std::uint64_t>(config.window_blocks - 1U))
                break;
            const bool incomplete = slot.seen_count != pipeline.nant;
            if (!Emit(oldest, emitter, error)) return false;
            if (incomplete) ++statistics.window_evictions;
        }
        return true;
    }
};

VdifUnpackEngine::VdifUnpackEngine() : impl_(new Impl) {}
VdifUnpackEngine::~VdifUnpackEngine() {}

bool VdifUnpackEngine::Configure(const VdifUnpackConfig& config,
                                 const PipelineConfig& pipeline,
                                 const VdifUnpackLayout& layout,
                                 std::string* error) {
    PipelineLayout pipeline_layout = {};
    if (!ComputePipelineLayout(pipeline, &pipeline_layout, error)) return false;
    if (config.window_blocks < 2)
        return Fail("window_blocks must be at least two", error);
    if (pipeline.nant == 0 || config.antenna_map.size() != pipeline.nant)
        return Fail("antenna_map length must equal positive NANT", error);
    if (static_cast<std::uint32_t>(config.first_channel_id) + pipeline.nchan > 65536U)
        return Fail("selected channel range exceeds uint16 range", error);
    if (pipeline_layout.packets_per_antenna_per_block >
            std::numeric_limits<std::uint64_t>::max() / config.window_blocks ||
        pipeline_layout.compute_block_bytes >
            std::numeric_limits<std::uint64_t>::max() / config.window_blocks)
        return Fail("window geometry exceeds uint64 range", error);
    const std::uint64_t expected_window_groups =
        pipeline_layout.packets_per_antenna_per_block * config.window_blocks;
    const std::uint64_t expected_window_bytes =
        pipeline_layout.compute_block_bytes * config.window_blocks;
    if (layout.raw_record_bytes != pipeline_layout.raw_record_bytes ||
        layout.records_per_raw_block != pipeline.records_per_block ||
        layout.group_bytes != pipeline.packet_payload_bytes * pipeline.nant ||
        layout.window_capacity_groups != expected_window_groups ||
        layout.window_bytes != expected_window_bytes ||
        layout.compute_block_bytes != pipeline_layout.compute_block_bytes)
        return Fail("unpack layout conflicts with pipeline geometry", error);
    if (layout.window_bytes > config.max_window_bytes)
        return Fail("window allocation exceeds configured maximum", error);
    if (layout.window_bytes > std::numeric_limits<std::size_t>::max() ||
        layout.window_capacity_groups > std::numeric_limits<std::size_t>::max() ||
        (pipeline.nant != 0 && layout.window_capacity_groups >
            std::numeric_limits<std::size_t>::max() / pipeline.nant))
        return Fail("window allocation exceeds host size_t range", error);

    std::unique_ptr<Impl> next(new Impl);
    next->config = config;
    next->pipeline = pipeline;
    next->layout = layout;
    next->raw_block_bytes = pipeline_layout.raw_block_bytes;
    next->geometry.first_channel_id = config.first_channel_id;
    next->geometry.nchan = static_cast<std::uint8_t>(pipeline.nchan);
    next->geometry.npol = static_cast<std::uint8_t>(pipeline.npol);
    next->geometry.nsamp_per_packet =
        static_cast<std::uint32_t>(pipeline.packet_samples);
    next->geometry.component_bits =
        static_cast<std::uint8_t>(pipeline.packet_nbit / 2U);
    next->geometry.payload_bytes = pipeline.packet_payload_bytes;
    next->arena.resize(static_cast<std::size_t>(layout.window_bytes));
    next->station_seen.resize(static_cast<std::size_t>(
        layout.window_capacity_groups * pipeline.nant));
    next->slots.resize(static_cast<std::size_t>(layout.window_capacity_groups));
    for (std::size_t i = 0; i < next->slots.size(); ++i) {
        next->slots[i].active = false;
        next->free_slots.push_back(next->slots.size() - 1U - i);
    }
    for (std::size_t i = 0; i < config.antenna_map.size(); ++i) {
        if (!next->station_to_antenna.insert(
                std::make_pair(config.antenna_map[i], i)).second)
            return Fail("antenna_map contains duplicate Station IDs", error);
    }
    next->configured = true;
    impl_.swap(next);
    return true;
}

bool VdifUnpackEngine::ConsumeRawBlock(const std::uint8_t* data,
                                       std::uint64_t size,
                                       std::uint64_t raw_block_sequence,
                                       const VdifGroupEmitter& emit,
                                       std::string* error) {
    if (!impl_->configured || impl_->finished)
        return Fail("engine is not configured for an active transfer", error);
    if (!emit) return Fail("group emitter is empty", error);
    if (!data && size != 0) return Fail("raw block data pointer is null", error);
    if (size == 0) return Fail("raw block must contain at least one record", error);
    if (size > impl_->raw_block_bytes)
        return Fail("input exceeds configured raw block size", error);
    if (size % impl_->layout.raw_record_bytes != 0)
        return Fail("raw block ends with a partial Project VDIF record", error);
    if (impl_->has_block_sequence &&
        raw_block_sequence <= impl_->last_block_sequence)
        return Fail("raw block sequence must increase", error);
    impl_->has_block_sequence = true;
    impl_->last_block_sequence = raw_block_sequence;

    for (std::uint64_t offset = 0; offset < size;
         offset += impl_->layout.raw_record_bytes) {
        ++impl_->statistics.received_records;
        const std::uint8_t* record = data + offset;
        ProjectVdifHeader header = {};
        std::string ignored;
        if (!DecodeProjectVdifV1(record, 32, &header, &ignored) ||
            !ValidateProjectVdifV1(header, impl_->geometry,
                                   impl_->layout.raw_record_bytes, &ignored)) {
            ++impl_->statistics.invalid_header_packets;
            continue;
        }
        if (header.invalid_data) {
            ++impl_->statistics.invalid_data_packets;
            continue;
        }
        const std::map<std::uint16_t, std::size_t>::const_iterator antenna =
            impl_->station_to_antenna.find(header.station_id);
        if (antenna == impl_->station_to_antenna.end()) {
            ++impl_->statistics.unknown_station_packets;
            continue;
        }
        VdifGroupKey key = {};
        key.reference_epoch = header.reference_epoch;
        key.seconds_from_reference_epoch = header.seconds_from_reference_epoch;
        key.frame_number_within_second = header.frame_number_within_second;
        key.first_channel_id = header.first_channel_id;
        key.nchan = header.nchan;
        key.npol = header.npol;
        if (impl_->has_last_emitted &&
            (key < impl_->last_emitted || EqualKey(key, impl_->last_emitted))) {
            ++impl_->statistics.late_packets;
            continue;
        }

        std::map<VdifGroupKey, std::size_t>::iterator group = impl_->active.find(key);
        if (group == impl_->active.end()) {
            if (impl_->free_slots.empty()) {
                const VdifGroupKey& oldest = impl_->active.begin()->first;
                if (key < oldest) {
                    ++impl_->statistics.late_packets;
                    continue;
                }
                if (!impl_->EvictOldest(emit, error)) return false;
            }
            const std::size_t slot = impl_->free_slots.back();
            impl_->free_slots.pop_back();
            std::memset(impl_->SlotBytes(slot), 0,
                        static_cast<std::size_t>(impl_->layout.group_bytes));
            std::memset(impl_->SlotSeen(slot), 0,
                        static_cast<std::size_t>(impl_->pipeline.nant));
            impl_->slots[slot].key = key;
            impl_->slots[slot].first_seen_block = raw_block_sequence;
            impl_->slots[slot].seen_count = 0;
            impl_->slots[slot].active = true;
            group = impl_->active.insert(std::make_pair(key, slot)).first;
        }
        const std::size_t slot = group->second;
        if (impl_->SlotSeen(slot)[antenna->second]) {
            ++impl_->statistics.duplicate_packets;
            continue;
        }
        const std::uint64_t sample_bytes = impl_->pipeline.packet_nbit / 8U;
        const std::uint64_t elements =
            impl_->pipeline.packet_payload_bytes / sample_bytes;
        const std::uint8_t* payload = record + impl_->pipeline.packet_header_bytes;
        std::uint8_t* destination = impl_->SlotBytes(slot);
        for (std::uint64_t element = 0; element < elements; ++element) {
            std::memcpy(destination +
                            (element * impl_->pipeline.nant + antenna->second) *
                                sample_bytes,
                        payload + element * sample_bytes,
                        static_cast<std::size_t>(sample_bytes));
        }
        impl_->SlotSeen(slot)[antenna->second] = 1;
        ++impl_->slots[slot].seen_count;
        ++impl_->statistics.accepted_packets;
    }
    return impl_->EmitStablePrefix(raw_block_sequence, emit, error);
}

bool VdifUnpackEngine::Finish(const VdifGroupEmitter& emit,
                              std::string* error) {
    if (!impl_->configured || impl_->finished)
        return Fail("engine is not configured for an active transfer", error);
    if (!emit) return Fail("group emitter is empty", error);
    while (!impl_->active.empty()) {
        if (!impl_->Emit(impl_->active.begin(), emit, error)) return false;
    }
    impl_->finished = true;
    return true;
}

const VdifUnpackStatistics& VdifUnpackEngine::statistics() const {
    return impl_->statistics;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
