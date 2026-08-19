#include "rdma_dada/io/rdma/receiver.h"

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
#include <deque>
#include <vector>

#include "rdma_dada/io/rdma/receive_policy.h"
#include "rdma_dada/io/rdma/verbs_context.h"

#define RDMA_OK 0
#define RDMA_ERROR -1
#define RDMA_NULL_POINTER -2

namespace {

struct DirectWrTarget {
    DirectWrTarget() : token(0), slot(0), addr(NULL), active(false) {}
    std::uint64_t token;
    std::size_t slot;
    unsigned char *addr;
    bool active;
};

struct DirectBlockState {
    DirectBlockState(const RoCEv2Dada::DirectRawBlockLease& lease_value,
                     std::size_t record_bytes)
        : lease(lease_value), progress(lease_value.bytes / record_bytes),
          next_slot(0) {}

    RoCEv2Dada::DirectRawBlockLease lease;
    rdma_dada::io::rdma::DirectRawBlockProgress progress;
    std::size_t next_slot;
};

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
    if (count == 0) return 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t wr_id = wr_ids[index];
        if (wr_id >= static_cast<std::uint64_t>(resource->recv_wr_num)) {
            fprintf(stderr, "[ERROR] Invalid receive WR id=%" PRIu64 ".\n",
                    wr_id);
            return -1;
        }
        struct ibv_recv_wr *wr = &resource->recv_wr[wr_id];
        memset(wr, 0, sizeof(*wr));
        wr->wr_id = wr_id;
        wr->sg_list = &resource->sge[wr_id * resource->recv_nsge];
        wr->num_sge = resource->recv_nsge;
        wr->next = index + 1 < count
            ? &resource->recv_wr[wr_ids[index + 1]] : NULL;
    }
    struct ibv_recv_wr *first = &resource->recv_wr[wr_ids[0]];
    const int result = ibv_post_recv(resource->qp, first,
                                     &resource->bad_recv_wr);
    if (result != 0) {
        fprintf(stderr,
                "[ERROR] Failed to post receive WR batch: count=%zu, "
                "ret=%d, errno=%d (%s)\n",
                count, result, errno, strerror(errno));
    }
    return result;
}

void LogFatalCompletion(const struct ibv_wc& completion,
                        const struct ibv_utils_res *resource) {
    fprintf(stderr,
            "[ERROR] Fatal receive completion: wr_id=%" PRIu64
            ", status=%s (%d), opcode=%d, byte_len=%u, vendor_err=%u, "
            "wr_limit=%d\n",
            completion.wr_id, ibv_wc_status_str(completion.status),
            completion.status, completion.opcode, completion.byte_len,
            completion.vendor_err, resource->recv_wr_num);
}

std::uint64_t MonotonicRawNs() {
    struct timespec value = {};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * UINT64_C(1000000000) +
           static_cast<std::uint64_t>(value.tv_nsec);
}

}  // namespace

struct RoCEv2Dada::DirectReceiveState {
    DirectReceiveState(std::size_t wr_depth)
        : header_scratch(NULL), header_mr(NULL), ring_prepared(false),
          failed(false), thread_started(false), poll_calls(0), empty_polls(0),
          full_polls(0), completions(0), reposted_wrs(0), repost_failures(0),
          repost_batches(0), posted_wrs(wr_depth), min_posted_wrs(wr_depth),
          poll_batch_high_watermark(0), completion_to_repost_ns_total(0),
          completion_to_repost_ns_max(0), consecutive_wrong_length(0),
          targets(wr_depth) {}

    unsigned char *header_scratch;
    struct ibv_mr *header_mr;
    bool ring_prepared;
    std::deque<DirectBlockState> blocks;
    std::vector<DirectWrTarget> targets;
    pthread_t tid;
    std::atomic<bool> failed;
    bool thread_started;
    std::atomic<std::uint64_t> poll_calls;
    std::atomic<std::uint64_t> empty_polls;
    std::atomic<std::uint64_t> full_polls;
    std::atomic<std::uint64_t> completions;
    std::atomic<std::uint64_t> reposted_wrs;
    std::atomic<std::uint64_t> repost_failures;
    std::atomic<std::uint64_t> repost_batches;
    std::atomic<std::uint64_t> posted_wrs;
    std::atomic<std::uint64_t> min_posted_wrs;
    std::atomic<std::uint64_t> poll_batch_high_watermark;
    std::atomic<std::uint64_t> completion_to_repost_ns_total;
    std::atomic<std::uint64_t> completion_to_repost_ns_max;
    std::uint32_t consecutive_wrong_length;
};

RoCEv2Dada::RoCEv2Dada(const RdmaParam& value)
    : param(value), ibv_res(NULL), receive_state(NULL), stop_requested(false),
      accepted_receive_packets(0), wrong_length_receive_packets(0),
      zeroed_receive_packets(0), published_receive_packets(0),
      published_receive_blocks(0), partial_receive_blocks(0),
      cq_tail_receive_records(0), thread_started(false) {
    if (param.pkt_size == 0U || param.recv_wr_num == 0U ||
        param.poll_batch == 0U || param.poll_batch > param.recv_wr_num ||
        param.poll_cpu_id < -1 || !param.PrepareRawRingMemory ||
        !param.AcquireRawBlockPtr || !param.CommitRawBlockPtr ||
        !param.ReleaseRawRingMemory) {
        fprintf(stderr, "[ERROR] Invalid direct raw receive parameters.\n");
        return;
    }

    struct ibv_utils_res *resource = static_cast<struct ibv_utils_res *>(
        calloc(1, sizeof(struct ibv_utils_res)));
    if (!resource) return;
    ibv_res = resource;
    resource->pkt_size =
        param.pkt_size + rdma_dada::io::rdma::kDirectRawHeaderBytes;
    resource->poll_n = param.poll_batch;
    resource->recv_nsge = rdma_dada::io::rdma::kDirectRawNsge;

    struct in_addr destination_address = {};
    if (inet_pton(AF_INET, param.DAddr, &destination_address) != 1 ||
        sscanf(param.DMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &resource->pkt_info.dst_mac[0],
               &resource->pkt_info.dst_mac[1],
               &resource->pkt_info.dst_mac[2],
               &resource->pkt_info.dst_mac[3],
               &resource->pkt_info.dst_mac[4],
               &resource->pkt_info.dst_mac[5]) != 6 ||
        sscanf(param.dst_port, "%hu", &resource->pkt_info.dst_port) != 1) {
        fprintf(stderr, "[ERROR] Invalid destination flow parameters.\n");
        return;
    }
    resource->pkt_info.dst_ip = destination_address.s_addr;

    if (open_ib_device(param.device_id, resource) < 0 ||
        create_ib_res(resource, static_cast<int>(param.recv_wr_num)) < 0 ||
        init_ib_res(resource) < 0) {
        fprintf(stderr, "[ERROR] Failed to initialize direct raw QP/CQ.\n");
        return;
    }

    receive_state = new DirectReceiveState(param.recv_wr_num);
    if (!receive_state) return;
    if (param.PrepareRawRingMemory(resource->pd) != 0) {
        fprintf(stderr, "[ERROR] Failed to register PSRDADA ring blocks.\n");
        return;
    }
    receive_state->ring_prepared = true;

    const std::size_t scratch_bytes =
        static_cast<std::size_t>(param.recv_wr_num) *
        rdma_dada::io::rdma::kDirectRawHeaderBytes;
    receive_state->header_scratch =
        static_cast<unsigned char *>(calloc(1, scratch_bytes));
    if (!receive_state->header_scratch) return;
    receive_state->header_mr = ibv_reg_mr(
        resource->pd, receive_state->header_scratch, scratch_bytes,
        IBV_ACCESS_LOCAL_WRITE);
    if (!receive_state->header_mr) {
        fprintf(stderr, "[ERROR] Failed to register header scratch MR.\n");
        return;
    }

    if (!AcquireDirectBlock() || !AcquireDirectBlock()) return;
    const std::size_t records_per_block =
        receive_state->blocks.front().lease.bytes / param.pkt_size;
    if (!rdma_dada::io::rdma::ValidateDirectRawConfiguration(
            param.recv_wr_num, records_per_block, 2)) {
        fprintf(stderr, "[ERROR] Invalid direct raw block/WR geometry.\n");
        return;
    }

    std::vector<std::uint64_t> initial_wrs(param.recv_wr_num);
    for (std::uint64_t wr_id = 0; wr_id < param.recv_wr_num; ++wr_id) {
        if (!AssignDirectSlot(wr_id)) return;
        initial_wrs[wr_id] = wr_id;
    }
    if (create_flow(resource, &resource->pkt_info) < 0 ||
        PostReceiveBatch(resource, initial_wrs.data(), initial_wrs.size()) != 0) {
        fprintf(stderr, "[ERROR] Failed to create flow/post initial WRs.\n");
        return;
    }
    resource->init_flag = true;
    printf("[RDMA] Direct raw initialized: flow=destination-only, qp=1, cq=1, "
           "nsge=2, header_bytes=%u, record_bytes=%u, wr_depth=%u, "
           "poll_batch=%u, outstanding_blocks=2\n",
           rdma_dada::io::rdma::kDirectRawHeaderBytes, param.pkt_size,
           param.recv_wr_num, param.poll_batch);
    fflush(stdout);
}

bool RoCEv2Dada::AcquireDirectBlock() {
    if (!receive_state || receive_state->blocks.size() >= 2U) return false;
    DirectRawBlockLease lease = {};
    if (param.AcquireRawBlockPtr(&lease) != 0 || !lease.addr ||
        lease.bytes == 0U || lease.bytes % param.pkt_size != 0U) {
        fprintf(stderr, "[ERROR] Failed to acquire aligned raw ring block.\n");
        if (receive_state) receive_state->failed.store(true);
        return false;
    }
    if (!receive_state->blocks.empty() &&
        receive_state->blocks.front().lease.bytes != lease.bytes) {
        fprintf(stderr, "[ERROR] Raw ring block size changed.\n");
        receive_state->failed.store(true);
        return false;
    }
    receive_state->blocks.emplace_back(lease, param.pkt_size);
    return true;
}

bool RoCEv2Dada::AssignDirectSlot(std::uint64_t wr_id) {
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(ibv_res);
    if (!receive_state || !resource || wr_id >= receive_state->targets.size())
        return false;
    DirectBlockState *selected = NULL;
    for (std::deque<DirectBlockState>::iterator it =
             receive_state->blocks.begin();
         it != receive_state->blocks.end(); ++it) {
        const std::size_t slots = it->lease.bytes / param.pkt_size;
        if (it->next_slot < slots) {
            selected = &(*it);
            break;
        }
    }
    if (!selected) {
        fprintf(stderr, "[ERROR] Direct raw two-block window has no free slot.\n");
        receive_state->failed.store(true);
        return false;
    }
    const std::size_t slot = selected->next_slot++;
    if (!selected->progress.AssignSlot(slot)) {
        receive_state->failed.store(true);
        return false;
    }
    DirectWrTarget& target = receive_state->targets[wr_id];
    target.token = selected->lease.token;
    target.slot = slot;
    target.addr = selected->lease.addr + slot * param.pkt_size;
    target.active = true;

    struct ibv_sge *header =
        &resource->sge[wr_id * rdma_dada::io::rdma::kDirectRawNsge];
    header[0].addr = reinterpret_cast<std::uint64_t>(
        receive_state->header_scratch +
        wr_id * rdma_dada::io::rdma::kDirectRawHeaderBytes);
    header[0].length = rdma_dada::io::rdma::kDirectRawHeaderBytes;
    header[0].lkey = receive_state->header_mr->lkey;
    header[1].addr = reinterpret_cast<std::uint64_t>(target.addr);
    header[1].length = param.pkt_size;
    header[1].lkey = selected->lease.lkey;
    return true;
}

bool RoCEv2Dada::PublishReadyDirectBlocks(bool replenish) {
    if (!receive_state) return false;
    while (!receive_state->blocks.empty() &&
           receive_state->blocks.front().progress.ready_to_publish()) {
        const DirectRawBlockLease lease = receive_state->blocks.front().lease;
        if (param.CommitRawBlockPtr(lease.token, lease.bytes) != 0) {
            fprintf(stderr, "[ERROR] Failed to commit full raw ring block.\n");
            receive_state->failed.store(true);
            return false;
        }
        published_receive_packets.fetch_add(lease.bytes / param.pkt_size);
        published_receive_blocks.fetch_add(1);
        receive_state->blocks.pop_front();
        if (replenish && !AcquireDirectBlock()) return false;
    }
    return true;
}

bool RoCEv2Dada::PublishDirectTail() {
    if (!receive_state) return false;
    if (!PublishReadyDirectBlocks(false)) return false;
    if (receive_state->blocks.empty()) return true;
    DirectBlockState& block = receive_state->blocks.front();
    const std::size_t records = block.progress.ContiguousCompletedSlots();
    if (records == 0U) return true;
    const std::uint64_t valid_bytes = records * param.pkt_size;
    if (param.CommitRawBlockPtr(block.lease.token, valid_bytes) != 0) {
        fprintf(stderr, "[ERROR] Failed to commit direct raw tail block.\n");
        receive_state->failed.store(true);
        return false;
    }
    published_receive_packets.fetch_add(records);
    published_receive_blocks.fetch_add(1);
    if (valid_bytes < block.lease.bytes) {
        partial_receive_blocks.fetch_add(1);
        cq_tail_receive_records.store(records);
    }
    receive_state->blocks.pop_front();
    return true;
}

void *RoCEv2Dada::ReceiveDirectThread(void *arg) {
    RoCEv2Dada *receiver = static_cast<RoCEv2Dada *>(arg);
    DirectReceiveState *state = receiver->receive_state;
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(receiver->ibv_res);
    std::vector<std::uint64_t> repost_ids(receiver->param.poll_batch);
    std::uint64_t stop_empty_polls = 0;
    const std::uint64_t kStopEmptyPolls = 4096;

    while (!state->failed.load()) {
        const int polled = ibv_poll_cq(resource->cq, resource->poll_n,
                                       resource->wc);
        state->poll_calls.fetch_add(1);
        if (polled < 0) {
            fprintf(stderr, "[ERROR] Failed to poll direct raw CQ.\n");
            state->failed.store(true);
            break;
        }
        if (polled == 0) {
            state->empty_polls.fetch_add(1);
            if (receiver->stop_requested.load() &&
                ++stop_empty_polls >= kStopEmptyPolls) break;
            continue;
        }
        stop_empty_polls = 0;
        state->completions.fetch_add(polled);
        if (polled == static_cast<int>(resource->poll_n))
            state->full_polls.fetch_add(1);
        UpdateAtomicMax(&state->poll_batch_high_watermark,
                        static_cast<std::uint64_t>(polled));
        const std::uint64_t before = state->posted_wrs.fetch_sub(polled);
        UpdateAtomicMin(&state->min_posted_wrs,
                        before >= static_cast<std::uint64_t>(polled)
                            ? before - polled : 0U);
        const std::uint64_t completed_ns = MonotonicRawNs();
        std::size_t repost_count = 0;

        for (int index = 0; index < polled; ++index) {
            const struct ibv_wc& completion = resource->wc[index];
            const bool valid_wr =
                completion.wr_id < state->targets.size() &&
                state->targets[completion.wr_id].active;
            const rdma_dada::io::rdma::DirectRawCompletionAction action =
                rdma_dada::io::rdma::ClassifyDirectRawCompletion(
                    completion.status == IBV_WC_SUCCESS,
                    completion.opcode == IBV_WC_RECV, valid_wr,
                    completion.byte_len, resource->pkt_size,
                    &state->consecutive_wrong_length);
            if (action ==
                rdma_dada::io::rdma::DirectRawCompletionAction::kFatal) {
                LogFatalCompletion(completion, resource);
                state->failed.store(true);
                break;
            }
            DirectWrTarget& target = state->targets[completion.wr_id];
            DirectBlockState *block = NULL;
            for (std::deque<DirectBlockState>::iterator it =
                     state->blocks.begin(); it != state->blocks.end(); ++it) {
                if (it->lease.token == target.token) {
                    block = &(*it);
                    break;
                }
            }
            if (!block || !block->progress.CompleteSlot(target.slot)) {
                fprintf(stderr, "[ERROR] Invalid direct raw slot completion.\n");
                state->failed.store(true);
                break;
            }
            if (action ==
                rdma_dada::io::rdma::DirectRawCompletionAction::kZeroSlot) {
                memset(target.addr, 0, receiver->param.pkt_size);
                receiver->wrong_length_receive_packets.fetch_add(1);
                receiver->zeroed_receive_packets.fetch_add(1);
            } else {
                receiver->accepted_receive_packets.fetch_add(1);
            }
            target.active = false;
            if (!receiver->stop_requested.load())
                repost_ids[repost_count++] = completion.wr_id;
        }
        if (state->failed.load()) break;
        if (!receiver->PublishReadyDirectBlocks(
                !receiver->stop_requested.load())) break;

        if (repost_count != 0U) {
            for (std::size_t index = 0; index < repost_count; ++index) {
                if (!receiver->AssignDirectSlot(repost_ids[index])) break;
            }
            if (state->failed.load() ||
                PostReceiveBatch(resource, repost_ids.data(), repost_count) != 0) {
                state->repost_failures.fetch_add(1);
                state->failed.store(true);
                break;
            }
            const std::uint64_t reposted_ns = MonotonicRawNs();
            const std::uint64_t delay = reposted_ns >= completed_ns
                ? reposted_ns - completed_ns : 0U;
            state->completion_to_repost_ns_total.fetch_add(
                delay * repost_count);
            UpdateAtomicMax(&state->completion_to_repost_ns_max, delay);
            state->reposted_wrs.fetch_add(repost_count);
            state->posted_wrs.fetch_add(repost_count);
            state->repost_batches.fetch_add(1);
        }
    }
    if (!state->failed.load()) receiver->PublishDirectTail();
    return NULL;
}

int RoCEv2Dada::Start() {
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(ibv_res);
    if (!resource || !receive_state || !resource->init_flag)
        return RDMA_NULL_POINTER;
    if (thread_started) return RDMA_ERROR;
    stop_requested.store(false);
    const int result = pthread_create(&receive_state->tid, NULL,
                                      ReceiveDirectThread, this);
    if (result != 0) {
        fprintf(stderr, "[ERROR] Direct raw pthread_create failed: %d\n",
                result);
        return RDMA_ERROR;
    }
    receive_state->thread_started = true;
    thread_started = true;
    if (param.poll_cpu_id >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(param.poll_cpu_id, &mask);
        const int affinity_result = pthread_setaffinity_np(
            receive_state->tid, sizeof(mask), &mask);
        if (affinity_result != 0) {
            fprintf(stderr, "[ERROR] Failed to bind direct raw poll CPU %d: %s\n",
                    param.poll_cpu_id, strerror(affinity_result));
            Stop();
            return RDMA_ERROR;
        }
    }
    printf("[RDMA] Receive threads ready: direct_raw=1, poll_cpu=%d\n",
           param.poll_cpu_id);
    fflush(stdout);
    return RDMA_OK;
}

int RoCEv2Dada::Stop() {
    if (!thread_started || !receive_state) return RDMA_OK;
    stop_requested.store(true);
    const int result = pthread_join(receive_state->tid, NULL);
    receive_state->thread_started = false;
    thread_started = false;
    if (result != 0) {
        fprintf(stderr, "[ERROR] Direct raw pthread_join failed: %d (%s)\n",
                result, strerror(result));
        return RDMA_ERROR;
    }
    const ReceiveStats stats = GetReceiveStats();
    const std::uint64_t total =
        stats.accepted_packets + stats.wrong_length_packets;
    const double wrong_ratio = total == 0U ? 0.0 :
        static_cast<double>(stats.wrong_length_packets) /
        static_cast<double>(total);
    printf("[RDMA] Receive summary: accepted=%" PRIu64
           ", wrong_length=%" PRIu64 ", zeroed=%" PRIu64
           ", published=%" PRIu64 ", blocks=%" PRIu64
           ", partial_blocks=%" PRIu64 ", cq_tail_records=%" PRIu64
           ", wrong_length_ratio=%.9f\n",
           stats.accepted_packets, stats.wrong_length_packets,
           stats.zeroed_packets, stats.published_packets,
           stats.published_blocks, stats.partial_blocks,
           stats.cq_tail_records, wrong_ratio);
    printf("[RDMA] Direct receive summary: poll_calls=%" PRIu64
           ", empty_polls=%" PRIu64 ", full_polls=%" PRIu64
           ", reposted_wrs=%" PRIu64 ", repost_failures=%" PRIu64
           ", repost_batches=%" PRIu64 ", min_posted_wrs=%" PRIu64
           ", poll_batch_high_watermark=%" PRIu64
           ", completion_to_repost_ns_total=%" PRIu64
           ", completion_to_repost_ns_max=%" PRIu64 "\n",
           stats.poll_calls, stats.empty_polls, stats.full_polls,
           stats.reposted_wrs, stats.repost_failures,
           stats.repost_batches, stats.min_posted_wrs,
           stats.poll_batch_high_watermark,
           stats.completion_to_repost_ns_total,
           stats.completion_to_repost_ns_max);
    fflush(stdout);
    return receive_state->failed.load() ? RDMA_ERROR : RDMA_OK;
}

RoCEv2Dada::ReceiveStats RoCEv2Dada::GetReceiveStats() const {
    ReceiveStats stats = {};
    stats.accepted_packets = accepted_receive_packets.load();
    stats.wrong_length_packets = wrong_length_receive_packets.load();
    stats.zeroed_packets = zeroed_receive_packets.load();
    stats.published_packets = published_receive_packets.load();
    stats.published_blocks = published_receive_blocks.load();
    stats.partial_blocks = partial_receive_blocks.load();
    stats.cq_tail_records = cq_tail_receive_records.load();
    if (receive_state) {
        stats.poll_calls = receive_state->poll_calls.load();
        stats.empty_polls = receive_state->empty_polls.load();
        stats.full_polls = receive_state->full_polls.load();
        stats.reposted_wrs = receive_state->reposted_wrs.load();
        stats.repost_failures = receive_state->repost_failures.load();
        stats.repost_batches = receive_state->repost_batches.load();
        stats.min_posted_wrs = receive_state->min_posted_wrs.load();
        stats.poll_batch_high_watermark =
            receive_state->poll_batch_high_watermark.load();
        stats.completion_to_repost_ns_total =
            receive_state->completion_to_repost_ns_total.load();
        stats.completion_to_repost_ns_max =
            receive_state->completion_to_repost_ns_max.load();
    }
    return stats;
}

RoCEv2Dada::~RoCEv2Dada() {
    Stop();
    struct ibv_utils_res *resource =
        static_cast<struct ibv_utils_res *>(ibv_res);
    if (resource && resource->qp) {
        ibv_destroy_qp(resource->qp);
        resource->qp = NULL;
    }
    if (receive_state && receive_state->header_mr) {
        ibv_dereg_mr(receive_state->header_mr);
        receive_state->header_mr = NULL;
    }
    if (receive_state && receive_state->ring_prepared) {
        param.ReleaseRawRingMemory();
        receive_state->ring_prepared = false;
    }
    if (receive_state && receive_state->header_scratch) {
        free(receive_state->header_scratch);
        receive_state->header_scratch = NULL;
    }
    delete receive_state;
    receive_state = NULL;
    if (resource) {
        destroy_ib_res(resource);
        close_ib_device(resource);
        free(resource);
    }
    ibv_res = NULL;
}
