#include "rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h"

#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0U &&
         right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

}  // namespace

struct VdifAtfpUnpackEngine::Impl {
    struct GroupSlot {
        std::uint64_t owned_ordinal;
        std::uint32_t seen_count;
        bool active;
    };

    enum DescriptorFlags {
        kRecordPresent = 1U,
        kHeaderValid = 2U,
        kStationKnown = 4U,
        kOrdinalValid = 8U,
        kInvalidData = 16U,
        kAccepted = 32U
    };

    enum WorkerPhase { kWorkerIdle, kWorkerParse, kWorkerCopy, kWorkerStop };

    VdifUnpackConfig config;
    PipelineConfig pipeline;
    VdifUnpackLayout layout;
    VdifTimeline timeline;
    ProjectVdifGeometry geometry;
    VdifAtfpStatistics statistics;
    std::vector<std::uint8_t> payload_window;
    std::vector<std::uint8_t> station_seen;
    std::vector<GroupSlot> slots;
    std::vector<std::int32_t> station_to_antenna;
    std::vector<std::uint8_t> station_has_ordinal;
    std::vector<std::uint64_t> block_station_counts;
    std::vector<ParsedRecordDescriptor> parsed_records;
    std::vector<int> thread_cpus;
    std::vector<std::thread> workers;
    std::size_t worker_count;
    std::mutex worker_mutex;
    std::condition_variable worker_start;
    std::condition_variable worker_done;
    WorkerPhase worker_phase;
    std::uint64_t worker_generation;
    std::size_t workers_completed;
    std::size_t workers_ready;
    bool worker_affinity_failed;
    const std::uint8_t* worker_raw_data;
    std::uint64_t worker_record_count;
    std::mutex lease_mutex;
    std::condition_variable lease_released;
    std::vector<std::uint64_t> slot_leases;
    std::uint64_t next_lease_id;
    std::uint64_t groups_per_compute_block;
    std::uint64_t reorder_horizon_groups;
    std::uint64_t next_emit_ordinal;
    std::uint64_t raw_block_bytes;
    std::uint64_t last_raw_block_sequence;
    bool has_raw_block_sequence;
    bool prepared;
    bool configured;
    bool finished;
    bool discard_before_timeline_start;

    Impl()
        : statistics(), groups_per_compute_block(0),
          reorder_horizon_groups(0), next_emit_ordinal(0),
          raw_block_bytes(0), last_raw_block_sequence(0),
          has_raw_block_sequence(false), prepared(false), configured(false),
          finished(false), discard_before_timeline_start(false),
          worker_count(0U), worker_phase(kWorkerIdle), worker_generation(0U),
          workers_completed(0U), workers_ready(0U),
          worker_affinity_failed(false), worker_raw_data(NULL),
          worker_record_count(0U), next_lease_id(1U) {}

    ~Impl() { StopWorkers(); }

    bool BindCurrentThread(int cpu) {
#if defined(__linux__)
        if (cpu >= 0) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(cpu, &mask);
            return pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) ==
                   0;
        }
#else
        (void)cpu;
#endif
        return true;
    }

    void WorkerLoop(std::size_t worker_index, int cpu) {
        const bool affinity_ok = BindCurrentThread(cpu);
        {
            std::lock_guard<std::mutex> lock(worker_mutex);
            if (!affinity_ok) worker_affinity_failed = true;
            ++workers_ready;
            worker_done.notify_one();
        }
        std::uint64_t observed_generation = 0U;
        for (;;) {
            WorkerPhase phase = kWorkerIdle;
            const std::uint8_t* data = NULL;
            std::uint64_t record_count = 0U;
            {
                std::unique_lock<std::mutex> lock(worker_mutex);
                worker_start.wait(lock, [this, &observed_generation] {
                    return worker_generation != observed_generation;
                });
                observed_generation = worker_generation;
                phase = worker_phase;
                data = worker_raw_data;
                record_count = worker_record_count;
            }
            if (phase == kWorkerStop) break;
            const std::uint64_t first =
                record_count * worker_index / worker_count;
            const std::uint64_t last =
                record_count * (worker_index + 1U) / worker_count;
            if (phase == kWorkerParse) ParseRange(data, first, last);
            if (phase == kWorkerCopy) CopyRange(data, first, last);
            {
                std::lock_guard<std::mutex> lock(worker_mutex);
                ++workers_completed;
                if (workers_completed == worker_count)
                    worker_done.notify_one();
            }
        }
    }

    bool StartWorkers(std::string* error) {
        if (!workers.empty()) return true;
        worker_count =
            thread_cpus.size() >= 3U ? thread_cpus.size() - 2U : 1U;
        workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            const int cpu = thread_cpus.size() >= 3U
                                ? thread_cpus[index + 1U]
                                : -1;
            workers.push_back(std::thread(&Impl::WorkerLoop, this, index, cpu));
        }
        std::unique_lock<std::mutex> lock(worker_mutex);
        worker_done.wait(lock, [this] { return workers_ready == worker_count; });
        if (worker_affinity_failed) {
            lock.unlock();
            StopWorkers();
            return Fail("cannot bind ATFP parse/copy worker CPU", error);
        }
        return true;
    }

    void StopWorkers() {
        if (workers.empty()) return;
        {
            std::lock_guard<std::mutex> lock(worker_mutex);
            worker_phase = kWorkerStop;
            ++worker_generation;
        }
        worker_start.notify_all();
        for (std::size_t index = 0; index < workers.size(); ++index)
            if (workers[index].joinable()) workers[index].join();
        workers.clear();
    }

    void RunWorkers(WorkerPhase phase, const std::uint8_t* data,
                    std::uint64_t record_count) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex);
            worker_phase = phase;
            worker_raw_data = data;
            worker_record_count = record_count;
            workers_completed = 0U;
            ++worker_generation;
        }
        worker_start.notify_all();
        std::unique_lock<std::mutex> lock(worker_mutex);
        worker_done.wait(lock, [this] {
            return workers_completed == worker_count;
        });
    }

    void WaitSlotWritable(std::uint64_t slot) {
        std::unique_lock<std::mutex> lock(lease_mutex);
        lease_released.wait(lock, [this, slot] {
            return slot_leases[static_cast<std::size_t>(slot)] == 0U;
        });
    }

    void WaitRangeWritable(std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t offset = 0; offset < count; ++offset)
            WaitSlotWritable(SlotIndex(first + offset));
    }

    void ParseRange(const std::uint8_t* data, std::uint64_t first,
                    std::uint64_t last) {
        for (std::uint64_t index = first; index < last; ++index) {
            ParsedRecordDescriptor descriptor = {};
            descriptor.record_index = static_cast<std::uint32_t>(index);
            descriptor.antenna = std::numeric_limits<std::uint16_t>::max();
            descriptor.flags = kRecordPresent;
            const std::uint8_t* record =
                data + index * layout.raw_record_bytes;
            ProjectVdifHeader header = {};
            std::string ignored;
            const bool decoded =
                DecodeProjectVdifV1(record, 32U, &header, &ignored);
            if (decoded && discard_before_timeline_start &&
                header.reference_epoch == timeline.start_reference_epoch &&
                (header.seconds_from_reference_epoch < timeline.start_seconds ||
                 (header.seconds_from_reference_epoch == timeline.start_seconds &&
                  header.frame_number_within_second < timeline.start_frame))) {
                descriptor.flags = 0U;
                parsed_records[static_cast<std::size_t>(index)] = descriptor;
                continue;
            }
            if (!decoded || !ValidateProjectVdifV1(
                    header, geometry, layout.raw_record_bytes, &ignored)) {
                parsed_records[static_cast<std::size_t>(index)] = descriptor;
                continue;
            }
            descriptor.flags |= kHeaderValid;
            const std::int32_t antenna = station_to_antenna[header.station_id];
            if (antenna < 0) {
                parsed_records[static_cast<std::size_t>(index)] = descriptor;
                continue;
            }
            descriptor.flags |= kStationKnown;
            descriptor.antenna = static_cast<std::uint16_t>(antenna);
            std::uint64_t ordinal = 0U;
            if (!VdifTimeToOrdinal(timeline, header.reference_epoch,
                                   header.seconds_from_reference_epoch,
                                   header.frame_number_within_second,
                                   &ordinal, &ignored)) {
                parsed_records[static_cast<std::size_t>(index)] = descriptor;
                continue;
            }
            descriptor.flags |= kOrdinalValid;
            if (header.invalid_data) descriptor.flags |= kInvalidData;
            descriptor.ordinal = ordinal;
            parsed_records[static_cast<std::size_t>(index)] = descriptor;
        }
    }

    void CopyRange(const std::uint8_t* data, std::uint64_t first,
                   std::uint64_t last) {
        for (std::uint64_t index = first; index < last; ++index) {
            const ParsedRecordDescriptor& parsed =
                parsed_records[static_cast<std::size_t>(index)];
            if ((parsed.flags & kAccepted) == 0U) continue;
            const std::uint8_t* record =
                data + parsed.record_index * layout.raw_record_bytes;
            std::memcpy(Payload(SlotIndex(parsed.ordinal), parsed.antenna),
                        record + pipeline.packet_header_bytes,
                        static_cast<std::size_t>(pipeline.packet_payload_bytes));
        }
    }

    std::uint64_t SlotIndex(std::uint64_t ordinal) const {
        return ordinal % layout.window_capacity_groups;
    }

    std::uint8_t* Seen(std::uint64_t slot, std::uint32_t antenna) {
        return &station_seen[static_cast<std::size_t>(
            slot * pipeline.nant + antenna)];
    }

    std::uint8_t* Payload(std::uint64_t slot, std::uint32_t antenna) {
        const std::uint64_t element =
            static_cast<std::uint64_t>(antenna) *
                layout.window_capacity_groups + slot;
        return payload_window.data() + static_cast<std::size_t>(
            element * pipeline.packet_payload_bytes);
    }

    const GroupSlot* ActiveSlot(std::uint64_t ordinal) const {
        const GroupSlot& slot = slots[static_cast<std::size_t>(
            ordinal % layout.window_capacity_groups)];
        return slot.active && slot.owned_ordinal == ordinal ? &slot : NULL;
    }

    bool StationWatermark(std::uint64_t* watermark) const {
        if (!watermark ||
            std::find(station_has_ordinal.begin(), station_has_ordinal.end(),
                      0U) != station_has_ordinal.end()) {
            return false;
        }
        *watermark = *std::min_element(
            statistics.station_highest_ordinals.begin(),
            statistics.station_highest_ordinals.end());
        return true;
    }

    bool IsFinal(std::uint64_t ordinal, bool has_station_watermark,
                 std::uint64_t station_watermark) const {
        const GroupSlot* slot = ActiveSlot(ordinal);
        if (slot && slot->seen_count == pipeline.nant) return true;
        return has_station_watermark && station_watermark >= ordinal &&
               station_watermark - ordinal >= reorder_horizon_groups;
    }

    bool RangeIsFinal(std::uint64_t first, std::uint64_t count) const {
        std::uint64_t station_watermark = 0;
        const bool has_station_watermark =
            StationWatermark(&station_watermark);
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            if (!IsFinal(first + offset, has_station_watermark,
                         station_watermark)) {
                return false;
            }
        }
        return true;
    }

    void PrepareMissing(std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const std::uint64_t ordinal = first + offset;
            const std::uint64_t slot_index = SlotIndex(ordinal);
            const GroupSlot* active = ActiveSlot(ordinal);
            for (std::uint32_t antenna = 0; antenna < pipeline.nant;
                 ++antenna) {
                const bool present = active && *Seen(slot_index, antenna) != 0U;
                if (!present) {
                    std::memset(Payload(slot_index, antenna), 0,
                    static_cast<std::size_t>(
                                    pipeline.packet_payload_bytes));
                }
            }
        }
    }

    bool CountPublishedGroups(std::uint64_t first, std::uint64_t count,
                              std::string* error) {
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const std::uint64_t ordinal = first + offset;
            const GroupSlot* active = ActiveSlot(ordinal);
            const std::uint64_t seen_count = active ? active->seen_count : 0U;
            const std::uint64_t missing = pipeline.nant - seen_count;
            statistics.expected_station_packets += pipeline.nant;
            statistics.missing_station_packets += missing;
            if (missing != 0U &&
                !statistics.missing_station_packets_per_second.empty()) {
                std::uint32_t seconds = 0;
                std::uint32_t frame = 0;
                if (!VdifOrdinalToTime(timeline, ordinal, &seconds, &frame,
                                       error)) {
                    return false;
                }
                const std::uint64_t second_index =
                    static_cast<std::uint64_t>(seconds - timeline.start_seconds);
                if (second_index >=
                    statistics.missing_station_packets_per_second.size()) {
                    return Fail("missing-packet second exceeds diagnostic range",
                                error);
                }
                statistics.missing_station_packets_per_second[
                    static_cast<std::size_t>(second_index)] += missing;
            }
            if (seen_count == pipeline.nant) {
                ++statistics.completed_groups;
            } else {
                ++statistics.incomplete_groups;
                if (seen_count == 0U) ++statistics.fully_missing_groups;
            }
        }
        return true;
    }

    bool Publish(std::uint64_t count, const VdifAtfpBlockEmitter& emit,
                 std::string* error) {
        if (count == 0U || count > groups_per_compute_block ||
            count > timeline.expected_groups - next_emit_ordinal) {
            return Fail("invalid ATFP publication range", error);
        }
        WaitRangeWritable(next_emit_ordinal, count);
        PrepareMissing(next_emit_ordinal, count);
        const std::uint64_t lease_id = next_lease_id++;
        {
            std::lock_guard<std::mutex> lock(lease_mutex);
            for (std::uint64_t offset = 0; offset < count; ++offset)
                slot_leases[static_cast<std::size_t>(
                    SlotIndex(next_emit_ordinal + offset))] = lease_id;
        }
        AtfpBlockView view = {};
        view.window_data = payload_window.data();
        view.window_capacity_groups = layout.window_capacity_groups;
        view.first_group_ordinal = next_emit_ordinal;
        view.first_slot = SlotIndex(next_emit_ordinal);
        view.group_count = count;
        view.nant = pipeline.nant;
        view.packet_payload_bytes = pipeline.packet_payload_bytes;
        view.lease_id = lease_id;
        if (!emit(view, error)) {
            std::string ignored;
            ReleaseLease(lease_id, &ignored);
            return false;
        }

        if (!CountPublishedGroups(next_emit_ordinal, count, error)) return false;
        std::uint64_t emitted_bytes = 0;
        if (!CheckedMultiply(count, layout.group_bytes, &emitted_bytes))
            return Fail("emitted ATFP byte count overflows", error);
        statistics.emitted_bytes += emitted_bytes;
        ++statistics.emitted_blocks;
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const std::uint64_t ordinal = next_emit_ordinal + offset;
            GroupSlot& slot = slots[static_cast<std::size_t>(SlotIndex(ordinal))];
            if (slot.active && slot.owned_ordinal == ordinal) {
                slot.active = false;
                slot.seen_count = 0;
            }
        }
        next_emit_ordinal += count;
        return true;
    }

    bool ReleaseLease(std::uint64_t lease_id, std::string* error) {
        if (lease_id == 0U) return Fail("ATFP lease id must be non-zero", error);
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(lease_mutex);
            for (std::size_t index = 0; index < slot_leases.size(); ++index) {
                if (slot_leases[index] == lease_id) {
                    slot_leases[index] = 0U;
                    found = true;
                }
            }
        }
        if (!found) return Fail("ATFP lease is unknown or already released", error);
        lease_released.notify_all();
        return true;
    }

    bool EmitReadyFullBlocks(const VdifAtfpBlockEmitter& emit,
                             std::string* error) {
        while (timeline.expected_groups - next_emit_ordinal >=
                   groups_per_compute_block &&
               RangeIsFinal(next_emit_ordinal,
                            groups_per_compute_block)) {
            if (!Publish(groups_per_compute_block, emit, error)) return false;
        }
        return true;
    }

    bool EnsureFits(std::uint64_t ordinal,
                    const VdifAtfpBlockEmitter& emit,
                    std::string* error) {
        while (ordinal >= next_emit_ordinal &&
               ordinal - next_emit_ordinal >=
                   layout.window_capacity_groups) {
            if (timeline.expected_groups - next_emit_ordinal <
                groups_per_compute_block) {
                return Fail("far-future packet cannot fit before final partial "
                            "range", error);
            }
            if (!RangeIsFinal(next_emit_ordinal,
                              groups_per_compute_block)) {
                return Fail("window pressure reached a non-final output range",
                            error);
            }
            if (!Publish(groups_per_compute_block, emit, error)) return false;
            ++statistics.large_gap_advances;
            statistics.large_gap_advanced_groups += groups_per_compute_block;
        }
        return true;
    }
};

VdifAtfpUnpackEngine::VdifAtfpUnpackEngine() : impl_(new Impl) {}
VdifAtfpUnpackEngine::~VdifAtfpUnpackEngine() {}

bool VdifAtfpUnpackEngine::ConfigureThreadCpus(
    const std::vector<int>& cpus, std::string* error) {
    if (impl_->prepared)
        return Fail("thread CPUs must be configured before Prepare", error);
    if (cpus.size() < 3U)
        return Fail("thread CPUs require coordinator, worker and writer", error);
    std::set<int> unique;
    for (std::size_t index = 0; index < cpus.size(); ++index) {
        if (cpus[index] < 0 || !unique.insert(cpus[index]).second)
            return Fail("thread CPUs must be distinct nonnegative values", error);
    }
    impl_->thread_cpus = cpus;
    return true;
}

bool VdifAtfpUnpackEngine::Prepare(const VdifUnpackConfig& config,
                                   const PipelineConfig& pipeline,
                                   const VdifUnpackLayout& layout,
                                   std::string* error) {
    PipelineLayout pipeline_layout = {};
    if (!ComputePipelineLayout(pipeline, &pipeline_layout, error)) return false;
    if (config.window_blocks < 2U)
        return Fail("window_blocks must be at least two", error);
    if (pipeline.nant == 0U || config.antenna_map.size() != pipeline.nant)
        return Fail("antenna_map length must equal positive NANT", error);
    std::uint64_t expected_group_bytes = 0;
    std::uint64_t expected_window_groups = 0;
    std::uint64_t expected_window_bytes = 0;
    if (!CheckedMultiply(pipeline.packet_payload_bytes, pipeline.nant,
                         &expected_group_bytes) ||
        !CheckedMultiply(pipeline_layout.packets_per_antenna_per_block,
                         config.window_blocks, &expected_window_groups) ||
        !CheckedMultiply(pipeline_layout.compute_block_bytes,
                         config.window_blocks, &expected_window_bytes)) {
        return Fail("ATFP layout arithmetic overflows uint64", error);
    }
    if (layout.raw_record_bytes != pipeline_layout.raw_record_bytes ||
        layout.records_per_raw_block != pipeline.records_per_block ||
        layout.group_bytes != expected_group_bytes ||
        layout.compute_block_bytes != pipeline_layout.compute_block_bytes ||
        layout.window_capacity_groups != expected_window_groups ||
        layout.window_bytes != expected_window_bytes) {
        return Fail("ATFP layout conflicts with pipeline geometry", error);
    }
    if (layout.window_capacity_groups == 0U || layout.group_bytes == 0U ||
        layout.compute_block_bytes % layout.group_bytes != 0U)
        return Fail("ATFP compute block must contain complete groups", error);
    const std::uint64_t groups_per_block =
        layout.compute_block_bytes / layout.group_bytes;
    std::uint64_t minimum_window_groups = 0;
    if (!CheckedMultiply(groups_per_block, 2U, &minimum_window_groups) ||
        layout.window_capacity_groups < minimum_window_groups)
        return Fail("ATFP window must hold at least two compute blocks", error);
    const std::uint64_t maximum_reorder_horizon =
        layout.window_capacity_groups - groups_per_block;
    const std::uint64_t reorder_horizon_groups =
        config.reorder_horizon_groups == 0U
            ? maximum_reorder_horizon
            : config.reorder_horizon_groups;
    if (reorder_horizon_groups > maximum_reorder_horizon)
        return Fail("ATFP reorder horizon exceeds window lookahead", error);
    if (layout.window_bytes > config.max_window_bytes ||
        layout.window_bytes > std::numeric_limits<std::size_t>::max() ||
        layout.window_capacity_groups > std::numeric_limits<std::size_t>::max() ||
        layout.window_capacity_groups >
            std::numeric_limits<std::size_t>::max() / pipeline.nant) {
        return Fail("ATFP window exceeds configured host memory limits", error);
    }
    std::unique_ptr<Impl> next(new Impl);
    next->config = config;
    next->pipeline = pipeline;
    next->layout = layout;
    next->raw_block_bytes = pipeline_layout.raw_block_bytes;
    next->groups_per_compute_block = groups_per_block;
    next->reorder_horizon_groups = reorder_horizon_groups;
    next->geometry.first_channel_id = config.first_channel_id;
    next->geometry.nchan = static_cast<std::uint8_t>(pipeline.nchan);
    next->geometry.npol = static_cast<std::uint8_t>(pipeline.npol);
    next->geometry.nsamp_per_packet =
        static_cast<std::uint32_t>(pipeline.packet_samples);
    next->geometry.component_bits =
        static_cast<std::uint8_t>(pipeline.packet_nbit / 2U);
    next->geometry.payload_bytes = pipeline.packet_payload_bytes;
    next->payload_window.resize(static_cast<std::size_t>(layout.window_bytes));
    next->station_seen.resize(static_cast<std::size_t>(
        layout.window_capacity_groups * pipeline.nant));
    next->slots.resize(static_cast<std::size_t>(layout.window_capacity_groups));
    next->station_to_antenna.assign(65536U, -1);
    next->station_has_ordinal.assign(pipeline.nant, 0U);
    next->block_station_counts.assign(pipeline.nant, 0U);
    next->parsed_records.resize(
        static_cast<std::size_t>(layout.records_per_raw_block));
    next->thread_cpus = impl_->thread_cpus;
    next->slot_leases.assign(
        static_cast<std::size_t>(layout.window_capacity_groups), 0U);
    next->statistics.station_observed_packets.assign(pipeline.nant, 0U);
    next->statistics.station_accepted_packets.assign(pipeline.nant, 0U);
    next->statistics.station_late_packets.assign(pipeline.nant, 0U);
    next->statistics.station_highest_ordinals.assign(pipeline.nant, 0U);
    for (std::size_t antenna = 0; antenna < config.antenna_map.size();
         ++antenna) {
        const std::uint16_t station = config.antenna_map[antenna];
        if (next->station_to_antenna[station] >= 0)
            return Fail("antenna_map contains duplicate Station IDs", error);
        next->station_to_antenna[station] =
            static_cast<std::int32_t>(antenna);
    }
    // Force physical page allocation before the producer can start.
    std::fill(next->payload_window.begin(), next->payload_window.end(), 0U);
    std::fill(next->station_seen.begin(), next->station_seen.end(), 0U);
    std::fill(next->slots.begin(), next->slots.end(), Impl::GroupSlot());
    std::fill(next->parsed_records.begin(), next->parsed_records.end(),
              ParsedRecordDescriptor());
    if (!next->StartWorkers(error)) return false;
    next->prepared = true;
    impl_.swap(next);
    return true;
}

bool VdifAtfpUnpackEngine::BeginTransfer(const VdifTimeline& timeline,
                                         std::string* error) {
    return BeginTransfer(timeline, false, false, error);
}

bool VdifAtfpUnpackEngine::BeginTransfer(
    const VdifTimeline& timeline, bool collect_missing_per_second,
    std::string* error) {
    return BeginTransfer(timeline, collect_missing_per_second, false, error);
}

bool VdifAtfpUnpackEngine::BeginTransfer(
    const VdifTimeline& timeline, bool collect_missing_per_second,
    bool discard_before_timeline_start, std::string* error) {
    if (!impl_->prepared)
        return Fail("ATFP engine static resources are not prepared", error);
    if (impl_->configured && !impl_->finished)
        return Fail("ATFP engine transfer is already active", error);
    if (timeline.group_period_ps == 0U || timeline.start_frame != 0U ||
        timeline.expected_groups == 0U)
        return Fail("ATFP timeline is invalid", error);
    std::uint64_t expected_transfer_bytes = 0;
    std::uint64_t expected_station_packets = 0;
    if (!CheckedMultiply(timeline.expected_groups, impl_->layout.group_bytes,
                         &expected_transfer_bytes) ||
        !CheckedMultiply(timeline.expected_groups, impl_->pipeline.nant,
                         &expected_station_packets)) {
        return Fail("ATFP expected transfer geometry overflows uint64", error);
    }
    impl_->timeline = timeline;
    impl_->statistics = VdifAtfpStatistics();
    impl_->statistics.station_observed_packets.assign(impl_->pipeline.nant, 0U);
    impl_->statistics.station_accepted_packets.assign(impl_->pipeline.nant, 0U);
    impl_->statistics.station_late_packets.assign(impl_->pipeline.nant, 0U);
    impl_->statistics.station_highest_ordinals.assign(impl_->pipeline.nant, 0U);
    if (collect_missing_per_second) {
        std::uint32_t last_seconds = 0;
        std::uint32_t last_frame = 0;
        if (!VdifOrdinalToTime(timeline, timeline.expected_groups - 1U,
                               &last_seconds, &last_frame, error)) {
            return false;
        }
        const std::uint64_t second_count =
            static_cast<std::uint64_t>(last_seconds - timeline.start_seconds) +
            1U;
        if (second_count > std::numeric_limits<std::size_t>::max()) {
            return Fail("missing-packet diagnostic range exceeds size_t",
                        error);
        }
        impl_->statistics.missing_station_packets_per_second.assign(
            static_cast<std::size_t>(second_count), 0U);
    }
    std::fill(impl_->station_seen.begin(), impl_->station_seen.end(), 0U);
    std::fill(impl_->slots.begin(), impl_->slots.end(), Impl::GroupSlot());
    std::fill(impl_->station_has_ordinal.begin(),
              impl_->station_has_ordinal.end(), 0U);
    std::fill(impl_->parsed_records.begin(), impl_->parsed_records.end(),
              ParsedRecordDescriptor());
    {
        std::lock_guard<std::mutex> lock(impl_->lease_mutex);
        if (std::find_if(impl_->slot_leases.begin(), impl_->slot_leases.end(),
                         [](std::uint64_t value) { return value != 0U; }) !=
            impl_->slot_leases.end()) {
            return Fail("previous ATFP writer leases are still active", error);
        }
    }
    impl_->next_emit_ordinal = 0U;
    impl_->last_raw_block_sequence = 0U;
    impl_->has_raw_block_sequence = false;
    impl_->discard_before_timeline_start = discard_before_timeline_start;
    impl_->configured = true;
    impl_->finished = false;
    return true;
}

bool VdifAtfpUnpackEngine::Configure(const VdifUnpackConfig& config,
                                     const PipelineConfig& pipeline,
                                     const VdifUnpackLayout& layout,
                                     const VdifTimeline& timeline,
                                     std::string* error) {
    return Prepare(config, pipeline, layout, error) &&
           BeginTransfer(timeline, error);
}

bool VdifAtfpUnpackEngine::ConsumeRawBlock(
    const std::uint8_t* data, std::uint64_t size,
    std::uint64_t raw_block_sequence, const VdifAtfpBlockEmitter& emit,
    std::string* error) {
    const VdifAtfpBlockEmitter synchronous =
        [this, &emit](const AtfpBlockView& view, std::string* emit_error) {
            if (!emit(view, emit_error)) return false;
            return ReleasePublishedBlock(view.lease_id, emit_error);
        };
    return ConsumeRawBlockAsync(data, size, raw_block_sequence, synchronous,
                                error);
}

bool VdifAtfpUnpackEngine::ConsumeRawBlockAsync(
    const std::uint8_t* data, std::uint64_t size,
    std::uint64_t raw_block_sequence, const VdifAtfpBlockEmitter& emit,
    std::string* error) {
    if (!impl_->configured || impl_->finished)
        return Fail("ATFP engine is not configured for an active transfer",
                    error);
    if (!emit) return Fail("ATFP block emitter is empty", error);
    if (!data && size != 0U) return Fail("raw block data pointer is null", error);
    if (size == 0U) return Fail("raw block must contain at least one record", error);
    if (size > impl_->raw_block_bytes)
        return Fail("input exceeds configured raw block size", error);
    if (size % impl_->layout.raw_record_bytes != 0U)
        return Fail("raw block ends with a partial Project VDIF record", error);
    if (impl_->has_raw_block_sequence &&
        raw_block_sequence <= impl_->last_raw_block_sequence)
        return Fail("raw block sequence must increase", error);
    impl_->has_raw_block_sequence = true;
    impl_->last_raw_block_sequence = raw_block_sequence;

    const std::uint64_t record_count =
        size / impl_->layout.raw_record_bytes;
    if (record_count > impl_->parsed_records.size())
        return Fail("raw block record count exceeds prepared descriptors", error);
    impl_->RunWorkers(Impl::kWorkerParse, data, record_count);

    std::fill(impl_->block_station_counts.begin(),
              impl_->block_station_counts.end(), 0U);
    std::uint64_t distinct_stations = 0U;
    std::int32_t last_antenna = -1;
    std::uint64_t consecutive_station_records = 0U;
    std::uint64_t max_consecutive_station_records = 0U;

    for (std::uint64_t index = 0; index < record_count; ++index) {
        ParsedRecordDescriptor& parsed =
            impl_->parsed_records[static_cast<std::size_t>(index)];
        if ((parsed.flags & Impl::kRecordPresent) == 0U) continue;
        ++impl_->statistics.received_records;
        if ((parsed.flags & Impl::kHeaderValid) == 0U) {
            ++impl_->statistics.invalid_header_packets;
            continue;
        }
        if ((parsed.flags & Impl::kStationKnown) == 0U) {
            ++impl_->statistics.unknown_station_packets;
            continue;
        }
        if ((parsed.flags & Impl::kOrdinalValid) == 0U) {
            ++impl_->statistics.out_of_range_packets;
            continue;
        }
        const std::uint32_t antenna_index = parsed.antenna;
        ++impl_->statistics.station_observed_packets[antenna_index];
        if (impl_->block_station_counts[antenna_index]++ == 0U)
            ++distinct_stations;
        if (last_antenna == static_cast<std::int32_t>(antenna_index)) {
            ++consecutive_station_records;
        } else {
            last_antenna = static_cast<std::int32_t>(antenna_index);
            consecutive_station_records = 1U;
        }
        max_consecutive_station_records = std::max(
            max_consecutive_station_records, consecutive_station_records);
        if (impl_->station_has_ordinal[antenna_index] == 0U ||
            parsed.ordinal >
                impl_->statistics.station_highest_ordinals[antenna_index]) {
            impl_->station_has_ordinal[antenna_index] = 1U;
            impl_->statistics.station_highest_ordinals[antenna_index] =
                parsed.ordinal;
        }
    }
    if (distinct_stations == 1U) {
        ++impl_->statistics.single_station_raw_blocks;
    } else if (distinct_stations > 1U) {
        ++impl_->statistics.mixed_station_raw_blocks;
    }
    for (std::size_t antenna = 0;
         antenna < impl_->block_station_counts.size(); ++antenna) {
        impl_->statistics.max_station_records_per_raw_block = std::max(
            impl_->statistics.max_station_records_per_raw_block,
            impl_->block_station_counts[antenna]);
    }
    impl_->statistics.max_consecutive_station_records = std::max(
        impl_->statistics.max_consecutive_station_records,
        max_consecutive_station_records);
    if (std::find(impl_->station_has_ordinal.begin(),
                  impl_->station_has_ordinal.end(), 0U) ==
        impl_->station_has_ordinal.end()) {
        const std::pair<std::vector<std::uint64_t>::const_iterator,
                        std::vector<std::uint64_t>::const_iterator> bounds =
            std::minmax_element(
                impl_->statistics.station_highest_ordinals.begin(),
                impl_->statistics.station_highest_ordinals.end());
        impl_->statistics.max_station_ordinal_skew = std::max(
            impl_->statistics.max_station_ordinal_skew,
            *bounds.second - *bounds.first);
    }

    for (std::uint64_t index = 0; index < record_count; ++index) {
        ParsedRecordDescriptor& parsed =
            impl_->parsed_records[static_cast<std::size_t>(index)];
        if ((parsed.flags & (Impl::kHeaderValid | Impl::kStationKnown |
                             Impl::kOrdinalValid)) !=
            (Impl::kHeaderValid | Impl::kStationKnown |
             Impl::kOrdinalValid)) {
            continue;
        }
        if (parsed.ordinal < impl_->next_emit_ordinal) {
            ++impl_->statistics.late_packets;
            ++impl_->statistics.station_late_packets[parsed.antenna];
            continue;
        }
        if (!impl_->EnsureFits(parsed.ordinal, emit, error)) return false;
        if ((parsed.flags & Impl::kInvalidData) != 0U) {
            ++impl_->statistics.invalid_data_packets;
            continue;
        }

        const std::uint64_t slot_index = impl_->SlotIndex(parsed.ordinal);
        impl_->WaitSlotWritable(slot_index);
        Impl::GroupSlot& slot =
            impl_->slots[static_cast<std::size_t>(slot_index)];
        if (slot.active && slot.owned_ordinal != parsed.ordinal) {
            return Fail("circular slot alias would overwrite a live ordinal",
                        error);
        }
        if (!slot.active) {
            slot.active = true;
            slot.owned_ordinal = parsed.ordinal;
            slot.seen_count = 0;
            std::memset(&impl_->station_seen[static_cast<std::size_t>(
                            slot_index * impl_->pipeline.nant)],
                        0, static_cast<std::size_t>(impl_->pipeline.nant));
        }
        if (*impl_->Seen(slot_index, parsed.antenna) != 0U) {
            ++impl_->statistics.duplicate_packets;
            continue;
        }
        parsed.flags |= Impl::kAccepted;
        *impl_->Seen(slot_index, parsed.antenna) = 1U;
        ++slot.seen_count;
        ++impl_->statistics.accepted_packets;
        ++impl_->statistics.station_accepted_packets[parsed.antenna];
        ++impl_->statistics.payload_copy_calls;
        impl_->statistics.payload_copy_bytes +=
            impl_->pipeline.packet_payload_bytes;
    }
    impl_->RunWorkers(Impl::kWorkerCopy, data, record_count);
    return impl_->EmitReadyFullBlocks(emit, error);
}

bool VdifAtfpUnpackEngine::Finish(const VdifAtfpBlockEmitter& emit,
                                  std::string* error) {
    const VdifAtfpBlockEmitter synchronous =
        [this, &emit](const AtfpBlockView& view, std::string* emit_error) {
            if (!emit(view, emit_error)) return false;
            return ReleasePublishedBlock(view.lease_id, emit_error);
        };
    return FinishAsync(synchronous, error);
}

bool VdifAtfpUnpackEngine::FinishAsync(const VdifAtfpBlockEmitter& emit,
                                       std::string* error) {
    if (!impl_->configured || impl_->finished)
        return Fail("ATFP engine is not configured for an active transfer",
                    error);
    if (!emit) return Fail("ATFP block emitter is empty", error);
    while (impl_->next_emit_ordinal < impl_->timeline.expected_groups) {
        const std::uint64_t remaining =
            impl_->timeline.expected_groups - impl_->next_emit_ordinal;
        const std::uint64_t count = std::min(
            remaining, impl_->groups_per_compute_block);
        if (!impl_->Publish(count, emit, error)) return false;
    }
    impl_->finished = true;
    return true;
}

bool VdifAtfpUnpackEngine::ReleasePublishedBlock(
    std::uint64_t lease_id, std::string* error) {
    return impl_->ReleaseLease(lease_id, error);
}

const VdifAtfpStatistics& VdifAtfpUnpackEngine::statistics() const {
    return impl_->statistics;
}

bool VdifAtfpUnpackEngine::prepared() const { return impl_->prepared; }

std::uint64_t VdifAtfpUnpackEngine::prepared_window_bytes() const {
    return impl_->prepared ? impl_->layout.window_bytes : 0U;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
