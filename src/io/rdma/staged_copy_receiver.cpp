#include "rdma_dada/io/rdma/staged_copy_receiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include "rdma_dada/io/rdma/receive_policy.h"
#include "rdma_dada/io/rdma/verbs_context.h"

namespace {

std::uint64_t ClockNs(clockid_t clock_id) {
    struct timespec value = {};
    clock_gettime(clock_id, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * UINT64_C(1000000000) +
           static_cast<std::uint64_t>(value.tv_nsec);
}

void UpdateAtomicMin(std::atomic<std::uint64_t> *target,
                     std::uint64_t value) {
    std::uint64_t observed = target->load(std::memory_order_relaxed);
    while (value < observed &&
           !target->compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {}
}

void UpdateAtomicMax(std::atomic<std::uint64_t> *target,
                     std::uint64_t value) {
    std::uint64_t observed = target->load(std::memory_order_relaxed);
    while (value > observed &&
           !target->compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {}
}

int PostReceiveBatch(struct ibv_utils_res *resource,
                     const std::uint64_t *wr_ids,
                     std::size_t count) {
    if (count == 0U) return 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t wr_id = wr_ids[index];
        if (wr_id >= static_cast<std::uint64_t>(resource->recv_wr_num))
            return -1;
        struct ibv_recv_wr *wr = &resource->recv_wr[wr_id];
        memset(wr, 0, sizeof(*wr));
        wr->wr_id = wr_id;
        wr->sg_list = &resource->sge[wr_id];
        wr->num_sge = 1;
        wr->next = index + 1U < count
            ? &resource->recv_wr[wr_ids[index + 1U]] : NULL;
    }
    return ibv_post_recv(resource->qp, &resource->recv_wr[wr_ids[0]],
                         &resource->bad_recv_wr);
}

}  // namespace

struct StagedCopyReceiver::State {
    explicit State(std::size_t wr_depth)
        : staging(NULL), staging_mr(NULL), frame_bytes(0), block(),
          block_offset(0), failed(false), thread_started(false), poll_calls(0),
          empty_polls(0), full_polls(0), reposted_wrs(0), repost_failures(0),
          repost_batches(0), posted_wrs(wr_depth), min_posted_wrs(wr_depth),
          poll_batch_high_watermark(0), completion_to_repost_ns_total(0),
          completion_to_repost_ns_max(0), drain_duration_ns(0),
          completions_after_stop(0), accepted_packets(0),
          wrong_length_packets(0), zeroed_packets(0), published_packets(0),
          published_blocks(0), partial_blocks(0), cq_tail_records(0),
          staged_copy_bytes(0), staged_copy_ns_total(0),
          receive_thread_cpu_ns(0), raw_ring_used_bytes_high_watermark(0),
          raw_block_acquire_wait_ns_total(0),
          raw_block_acquire_wait_ns_max(0),
          exit_reason(ReceiveExitReason::kNotStopped),
          consecutive_wrong_length(0) {}

    unsigned char *staging;
    struct ibv_mr *staging_mr;
    std::size_t frame_bytes;
    HostRawBlockLease block;
    std::uint64_t block_offset;
    pthread_t tid;
    std::atomic<bool> failed;
    bool thread_started;
    std::atomic<std::uint64_t> poll_calls;
    std::atomic<std::uint64_t> empty_polls;
    std::atomic<std::uint64_t> full_polls;
    std::atomic<std::uint64_t> reposted_wrs;
    std::atomic<std::uint64_t> repost_failures;
    std::atomic<std::uint64_t> repost_batches;
    std::atomic<std::uint64_t> posted_wrs;
    std::atomic<std::uint64_t> min_posted_wrs;
    std::atomic<std::uint64_t> poll_batch_high_watermark;
    std::atomic<std::uint64_t> completion_to_repost_ns_total;
    std::atomic<std::uint64_t> completion_to_repost_ns_max;
    std::atomic<std::uint64_t> drain_duration_ns;
    std::atomic<std::uint64_t> completions_after_stop;
    std::atomic<std::uint64_t> accepted_packets;
    std::atomic<std::uint64_t> wrong_length_packets;
    std::atomic<std::uint64_t> zeroed_packets;
    std::atomic<std::uint64_t> published_packets;
    std::atomic<std::uint64_t> published_blocks;
    std::atomic<std::uint64_t> partial_blocks;
    std::atomic<std::uint64_t> cq_tail_records;
    std::atomic<std::uint64_t> staged_copy_bytes;
    std::atomic<std::uint64_t> staged_copy_ns_total;
    std::atomic<std::uint64_t> receive_thread_cpu_ns;
    std::atomic<std::uint64_t> raw_ring_used_bytes_high_watermark;
    std::atomic<std::uint64_t> raw_block_acquire_wait_ns_total;
    std::atomic<std::uint64_t> raw_block_acquire_wait_ns_max;
    std::atomic<ReceiveExitReason> exit_reason;
    std::uint32_t consecutive_wrong_length;
    rdma_dada::io::rdma::ReceiveLatencyHistogram batch_latency;
};

StagedCopyReceiver::StagedCopyReceiver(const RdmaParam& param)
    : param_(param), ibv_res_(NULL), state_(NULL), stop_requested_(false),
      thread_started_(false) {
    if (param_.pkt_size == 0U || param_.recv_wr_num == 0U ||
        param_.poll_batch == 0U || param_.poll_batch > param_.recv_wr_num ||
        param_.poll_cpu_id < -1 || !param_.AcquireRawBlockPtr ||
        !param_.CommitRawBlockPtr || !param_.RawRingUsedBytesPtr) {
        fprintf(stderr, "[ERROR] Invalid staged-copy receive parameters.\n");
        return;
    }
    struct ibv_utils_res *resource = static_cast<struct ibv_utils_res *>(
        calloc(1, sizeof(struct ibv_utils_res)));
    if (!resource) return;
    ibv_res_ = resource;
    resource->pkt_size =
        param_.pkt_size + rdma_dada::io::rdma::kDirectRawHeaderBytes;
    resource->poll_n = param_.poll_batch;
    resource->recv_nsge = 1;

    struct in_addr destination_address = {};
    if (inet_pton(AF_INET, param_.DAddr, &destination_address) != 1 ||
        sscanf(param_.DMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &resource->pkt_info.dst_mac[0],
               &resource->pkt_info.dst_mac[1],
               &resource->pkt_info.dst_mac[2],
               &resource->pkt_info.dst_mac[3],
               &resource->pkt_info.dst_mac[4],
               &resource->pkt_info.dst_mac[5]) != 6 ||
        sscanf(param_.dst_port, "%hu", &resource->pkt_info.dst_port) != 1) {
        fprintf(stderr, "[ERROR] Invalid staged-copy destination.\n");
        return;
    }
    resource->pkt_info.dst_ip = destination_address.s_addr;
    if (open_ib_device(param_.device_id, resource) < 0 ||
        create_ib_res(resource, static_cast<int>(param_.recv_wr_num)) < 0 ||
        init_ib_res(resource) < 0) {
        fprintf(stderr, "[ERROR] Failed to initialize staged-copy QP/CQ.\n");
        return;
    }

    state_ = new State(param_.recv_wr_num);
    state_->frame_bytes = resource->pkt_size;
    const std::size_t staging_bytes =
        state_->frame_bytes * static_cast<std::size_t>(param_.recv_wr_num);
    state_->staging = static_cast<unsigned char *>(calloc(1, staging_bytes));
    if (!state_->staging) return;
    state_->staging_mr = ibv_reg_mr(
        resource->pd, state_->staging, staging_bytes, IBV_ACCESS_LOCAL_WRITE);
    if (!state_->staging_mr) {
        fprintf(stderr, "[ERROR] Failed to register staged receive memory.\n");
        return;
    }
    for (std::uint64_t wr_id = 0; wr_id < param_.recv_wr_num; ++wr_id) {
        struct ibv_sge& sge = resource->sge[wr_id];
        sge.addr = reinterpret_cast<std::uint64_t>(
            state_->staging + wr_id * state_->frame_bytes);
        sge.length = static_cast<std::uint32_t>(state_->frame_bytes);
        sge.lkey = state_->staging_mr->lkey;
    }
    if (!AcquireBlock()) return;
    const std::size_t records_per_block = state_->block.bytes / param_.pkt_size;
    if (!rdma_dada::io::rdma::ValidateDirectRawConfiguration(
            param_.recv_wr_num, records_per_block, 2U)) {
        fprintf(stderr, "[ERROR] Invalid staged-copy block/WR geometry.\n");
        return;
    }
    std::vector<std::uint64_t> wr_ids(param_.recv_wr_num);
    for (std::uint64_t index = 0; index < param_.recv_wr_num; ++index)
        wr_ids[index] = index;
    if (create_flow(resource, &resource->pkt_info) < 0 ||
        PostReceiveBatch(resource, wr_ids.data(), wr_ids.size()) != 0) {
        fprintf(stderr, "[ERROR] Failed to post staged-copy receive WRs.\n");
        return;
    }
    resource->init_flag = true;
    printf("[RDMA] Staged copy initialized: flow=destination-only, qp=1, "
           "cq=1, threads=1, nsge=1, frame_bytes=%zu, record_bytes=%u, "
           "wr_depth=%u, poll_batch=%u\n",
           state_->frame_bytes, param_.pkt_size, param_.recv_wr_num,
           param_.poll_batch);
    fflush(stdout);
}

bool StagedCopyReceiver::AcquireBlock() {
    if (!state_) return false;
    const std::uint64_t started = ClockNs(CLOCK_MONOTONIC_RAW);
    HostRawBlockLease lease = {};
    const int result = param_.AcquireRawBlockPtr(&lease);
    const std::uint64_t elapsed = ClockNs(CLOCK_MONOTONIC_RAW) - started;
    state_->raw_block_acquire_wait_ns_total.fetch_add(elapsed);
    UpdateAtomicMax(&state_->raw_block_acquire_wait_ns_max, elapsed);
    if (result != 0 || !lease.addr || lease.bytes == 0U ||
        lease.bytes % param_.pkt_size != 0U) {
        state_->failed.store(true);
        return false;
    }
    state_->block = lease;
    state_->block_offset = 0;
    return true;
}

bool StagedCopyReceiver::CommitFullBlock() {
    if (!state_ || state_->block_offset != state_->block.bytes) return false;
    if (param_.CommitRawBlockPtr(state_->block.token, state_->block.bytes) != 0)
        return false;
    state_->published_packets.fetch_add(
        state_->block.bytes / param_.pkt_size);
    state_->published_blocks.fetch_add(1);
    UpdateAtomicMax(&state_->raw_ring_used_bytes_high_watermark,
                    param_.RawRingUsedBytesPtr());
    return AcquireBlock();
}

bool StagedCopyReceiver::CommitTail() {
    if (!state_ || state_->block_offset == 0U) return true;
    if (param_.CommitRawBlockPtr(
            state_->block.token, state_->block_offset) != 0) return false;
    const std::uint64_t records = state_->block_offset / param_.pkt_size;
    state_->published_packets.fetch_add(records);
    state_->published_blocks.fetch_add(1);
    state_->partial_blocks.fetch_add(1);
    state_->cq_tail_records.store(records);
    UpdateAtomicMax(&state_->raw_ring_used_bytes_high_watermark,
                    param_.RawRingUsedBytesPtr());
    state_->block_offset = 0;
    return true;
}

void *StagedCopyReceiver::ReceiveThread(void *arg) {
    StagedCopyReceiver *receiver = static_cast<StagedCopyReceiver *>(arg);
    State *state = receiver->state_;
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(receiver->ibv_res_);
    std::vector<std::uint64_t> repost_ids(receiver->param_.poll_batch);
    const std::uint64_t cpu_started = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    std::uint64_t drain_empty_polls = 0;
    std::uint64_t drain_started = 0;
    std::uint64_t drain_deadline = 0;
    bool draining = false;
    bool deadline_reached = false;

    while (!state->failed.load()) {
        if (receiver->stop_requested_.load() && !draining) {
            drain_started = ClockNs(CLOCK_MONOTONIC_RAW);
            drain_deadline = drain_started +
                rdma_dada::io::rdma::kDirectRawDrainDurationNs;
            draining = true;
        }
        const int polled = ibv_poll_cq(resource->cq, resource->poll_n,
                                       resource->wc);
        state->poll_calls.fetch_add(1);
        if (polled < 0) {
            state->failed.store(true);
            break;
        }
        if (polled == 0) {
            state->empty_polls.fetch_add(1);
            if (draining &&
                rdma_dada::io::rdma::ShouldCheckDirectRawDrainClock(
                    ++drain_empty_polls)) {
                drain_empty_polls = 0;
                const std::uint64_t now = ClockNs(CLOCK_MONOTONIC_RAW);
                if (now >= drain_deadline) {
                    state->drain_duration_ns.store(now - drain_started);
                    state->exit_reason.store(
                        ReceiveExitReason::kDrainDeadline);
                    break;
                }
            }
            continue;
        }
        drain_empty_polls = 0;
        if (draining) state->completions_after_stop.fetch_add(polled);
        if (polled == static_cast<int>(resource->poll_n))
            state->full_polls.fetch_add(1);
        UpdateAtomicMax(&state->poll_batch_high_watermark, polled);
        const std::uint64_t before = state->posted_wrs.fetch_sub(polled);
        UpdateAtomicMin(&state->min_posted_wrs,
                        before >= static_cast<std::uint64_t>(polled)
                            ? before - polled : 0U);
        const std::uint64_t batch_started = ClockNs(CLOCK_MONOTONIC_RAW);
        std::size_t repost_count = 0;
        for (int index = 0; index < polled; ++index) {
            const struct ibv_wc& completion = resource->wc[index];
            if (completion.status != IBV_WC_SUCCESS ||
                completion.opcode != IBV_WC_RECV ||
                completion.wr_id >= receiver->param_.recv_wr_num) {
                state->failed.store(true);
                break;
            }
            unsigned char *destination =
                state->block.addr + state->block_offset;
            const unsigned char *frame =
                state->staging + completion.wr_id * state->frame_bytes;
            const rdma_dada::io::rdma::StagedRawCopyAction action =
                rdma_dada::io::rdma::CopyStagedRawRecord(
                    frame, completion.byte_len, receiver->param_.pkt_size,
                    destination,
                    &state->consecutive_wrong_length);
            if (action ==
                rdma_dada::io::rdma::StagedRawCopyAction::kFatal) {
                state->failed.store(true);
                break;
            }
            if (action ==
                rdma_dada::io::rdma::StagedRawCopyAction::kCopied) {
                state->accepted_packets.fetch_add(1);
                state->staged_copy_bytes.fetch_add(
                    receiver->param_.pkt_size);
            } else {
                state->wrong_length_packets.fetch_add(1);
                state->zeroed_packets.fetch_add(1);
            }
            state->block_offset += receiver->param_.pkt_size;
            if (state->block_offset == state->block.bytes &&
                !receiver->CommitFullBlock()) {
                state->failed.store(true);
                break;
            }
            repost_ids[repost_count++] = completion.wr_id;
        }
        if (state->failed.load()) break;
        const std::uint64_t after_copy = ClockNs(CLOCK_MONOTONIC_RAW);
        state->staged_copy_ns_total.fetch_add(after_copy - batch_started);
        if (draining) {
            const std::uint64_t now = ClockNs(CLOCK_MONOTONIC_RAW);
            deadline_reached = now >= drain_deadline;
            if (deadline_reached) {
                state->drain_duration_ns.store(now - drain_started);
                state->exit_reason.store(ReceiveExitReason::kDrainDeadline);
            }
        }
        if (repost_count != 0U &&
            rdma_dada::io::rdma::ShouldRepostDirectRawWr(
                receiver->stop_requested_.load(), deadline_reached)) {
            if (PostReceiveBatch(resource, repost_ids.data(), repost_count) != 0) {
                state->repost_failures.fetch_add(1);
                state->failed.store(true);
                break;
            }
            const std::uint64_t reposted = ClockNs(CLOCK_MONOTONIC_RAW);
            const std::uint64_t delay = reposted - after_copy;
            state->completion_to_repost_ns_total.fetch_add(
                delay * repost_count);
            UpdateAtomicMax(&state->completion_to_repost_ns_max, delay);
            state->reposted_wrs.fetch_add(repost_count);
            state->posted_wrs.fetch_add(repost_count);
            state->repost_batches.fetch_add(1);
            state->batch_latency.Observe(reposted - batch_started);
        }
        if (deadline_reached) break;
    }
    state->receive_thread_cpu_ns.store(
        ClockNs(CLOCK_THREAD_CPUTIME_ID) - cpu_started);
    if (state->failed.load()) {
        state->exit_reason.store(ReceiveExitReason::kError);
    } else if (!receiver->CommitTail()) {
        state->failed.store(true);
        state->exit_reason.store(ReceiveExitReason::kError);
    }
    return NULL;
}

int StagedCopyReceiver::Start() {
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(ibv_res_);
    if (!resource || !state_ || !resource->init_flag || thread_started_)
        return -1;
    stop_requested_.store(false);
    const int result = pthread_create(&state_->tid, NULL, ReceiveThread, this);
    if (result != 0) return -1;
    state_->thread_started = true;
    thread_started_ = true;
    if (param_.poll_cpu_id >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(param_.poll_cpu_id, &mask);
        const int affinity_result = pthread_setaffinity_np(
            state_->tid, sizeof(mask), &mask);
        if (affinity_result != 0) {
            Stop();
            return -1;
        }
    }
    printf("[RDMA] Receive threads ready: staged_copy=1, poll_cpu=%d\n",
           param_.poll_cpu_id);
    fflush(stdout);
    return 0;
}

int StagedCopyReceiver::Stop() {
    if (!thread_started_ || !state_) return 0;
    stop_requested_.store(true);
    const int result = pthread_join(state_->tid, NULL);
    state_->thread_started = false;
    thread_started_ = false;
    const ReceiveStats stats = GetReceiveStats();
    printf("[RDMA] Receive summary: accepted=%" PRIu64
           ", wrong_length=%" PRIu64 ", zeroed=%" PRIu64
           ", published=%" PRIu64 ", blocks=%" PRIu64
           ", partial_blocks=%" PRIu64 ", cq_tail_records=%" PRIu64 "\n",
           stats.accepted_packets, stats.wrong_length_packets,
           stats.zeroed_packets, stats.published_packets,
           stats.published_blocks, stats.partial_blocks,
           stats.cq_tail_records);
    printf("[RDMA] Staged receive summary: placement=STAGED_COPY, "
           "poll_calls=%" PRIu64 ", empty_polls=%" PRIu64
           ", full_polls=%" PRIu64 ", reposted_wrs=%" PRIu64
           ", repost_failures=%" PRIu64 ", repost_batches=%" PRIu64
           ", min_posted_wrs=%" PRIu64
           ", poll_batch_high_watermark=%" PRIu64
           ", completion_to_repost_ns_total=%" PRIu64
           ", completion_to_repost_ns_max=%" PRIu64
           ", drain_duration_ns=%" PRIu64
           ", completions_after_stop=%" PRIu64
           ", staged_copy_bytes=%" PRIu64
           ", staged_copy_ns_total=%" PRIu64
           ", receive_publication_ns_p50=%" PRIu64
           ", receive_publication_ns_p95=%" PRIu64
           ", receive_thread_cpu_ns=%" PRIu64
           ", raw_ring_used_bytes_high_watermark=%" PRIu64
           ", raw_block_acquire_wait_ns_total=%" PRIu64
           ", raw_block_acquire_wait_ns_max=%" PRIu64
           ", exit_reason=%s\n",
           stats.poll_calls, stats.empty_polls, stats.full_polls,
           stats.reposted_wrs, stats.repost_failures, stats.repost_batches,
           stats.min_posted_wrs, stats.poll_batch_high_watermark,
           stats.completion_to_repost_ns_total,
           stats.completion_to_repost_ns_max, stats.drain_duration_ns,
           stats.completions_after_stop, stats.staged_copy_bytes,
           stats.staged_copy_ns_total, stats.receive_publication_ns_p50,
           stats.receive_publication_ns_p95, stats.receive_thread_cpu_ns,
           stats.raw_ring_used_bytes_high_watermark,
           stats.raw_block_acquire_wait_ns_total,
           stats.raw_block_acquire_wait_ns_max,
           stats.exit_reason == ReceiveExitReason::kDrainDeadline
               ? "DRAIN_DEADLINE"
               : stats.exit_reason == ReceiveExitReason::kError
                   ? "ERROR" : "NOT_STOPPED");
    fflush(stdout);
    return result == 0 && !state_->failed.load() ? 0 : -1;
}

StagedCopyReceiver::ReceiveStats StagedCopyReceiver::GetReceiveStats() const {
    ReceiveStats stats = {};
    if (!state_) return stats;
#define LOAD_STAT(name) stats.name = state_->name.load()
    LOAD_STAT(accepted_packets);
    LOAD_STAT(wrong_length_packets);
    LOAD_STAT(zeroed_packets);
    LOAD_STAT(published_packets);
    LOAD_STAT(published_blocks);
    LOAD_STAT(partial_blocks);
    LOAD_STAT(cq_tail_records);
    LOAD_STAT(poll_calls);
    LOAD_STAT(empty_polls);
    LOAD_STAT(full_polls);
    LOAD_STAT(reposted_wrs);
    LOAD_STAT(repost_failures);
    LOAD_STAT(repost_batches);
    LOAD_STAT(min_posted_wrs);
    LOAD_STAT(poll_batch_high_watermark);
    LOAD_STAT(completion_to_repost_ns_total);
    LOAD_STAT(completion_to_repost_ns_max);
    LOAD_STAT(drain_duration_ns);
    LOAD_STAT(completions_after_stop);
    LOAD_STAT(staged_copy_bytes);
    LOAD_STAT(staged_copy_ns_total);
    LOAD_STAT(receive_thread_cpu_ns);
    LOAD_STAT(raw_ring_used_bytes_high_watermark);
    LOAD_STAT(raw_block_acquire_wait_ns_total);
    LOAD_STAT(raw_block_acquire_wait_ns_max);
#undef LOAD_STAT
    stats.receive_publication_ns_p50 = state_->batch_latency.Percentile(50);
    stats.receive_publication_ns_p95 = state_->batch_latency.Percentile(95);
    stats.exit_reason = state_->exit_reason.load();
    return stats;
}

StagedCopyReceiver::~StagedCopyReceiver() {
    Stop();
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(ibv_res_);
    if (state_ && state_->staging_mr) {
        ibv_dereg_mr(state_->staging_mr);
        state_->staging_mr = NULL;
    }
    if (state_ && state_->staging) {
        free(state_->staging);
        state_->staging = NULL;
    }
    delete state_;
    state_ = NULL;
    if (resource) {
        destroy_ib_res(resource);
        close_ib_device(resource);
        free(resource);
    }
    ibv_res_ = NULL;
}
