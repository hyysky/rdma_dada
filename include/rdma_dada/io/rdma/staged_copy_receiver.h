#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

// Receive-only ablation reference. One RAW_PACKET QP receives complete frames
// into registered staging slots; the same CQ thread copies Project VDIF
// records into the PSRDADA raw ring.
class StagedCopyReceiver {
  public:
    enum class ReceiveExitReason {
        kNotStopped,
        kDrainDeadline,
        kError
    };

    struct ReceiveStats {
        std::uint64_t accepted_packets;
        std::uint64_t wrong_length_packets;
        std::uint64_t zeroed_packets;
        std::uint64_t published_packets;
        std::uint64_t published_blocks;
        std::uint64_t partial_blocks;
        std::uint64_t cq_tail_records;
        std::uint64_t poll_calls;
        std::uint64_t empty_polls;
        std::uint64_t full_polls;
        std::uint64_t reposted_wrs;
        std::uint64_t repost_failures;
        std::uint64_t repost_batches;
        std::uint64_t min_posted_wrs;
        std::uint64_t poll_batch_high_watermark;
        std::uint64_t completion_to_repost_ns_total;
        std::uint64_t completion_to_repost_ns_max;
        std::uint64_t drain_duration_ns;
        std::uint64_t completions_after_stop;
        std::uint64_t staged_copy_bytes;
        std::uint64_t staged_copy_ns_total;
        std::uint64_t receive_publication_ns_p50;
        std::uint64_t receive_publication_ns_p95;
        std::uint64_t receive_thread_cpu_ns;
        std::uint64_t raw_ring_used_bytes_high_watermark;
        std::uint64_t raw_block_acquire_wait_ns_total;
        std::uint64_t raw_block_acquire_wait_ns_max;
        ReceiveExitReason exit_reason;
    };

    struct HostRawBlockLease {
        unsigned char *addr;
        std::uint64_t bytes;
        std::uint64_t token;
    };

    typedef std::function<int(HostRawBlockLease *)> AcquireRawBlock;
    typedef std::function<int(std::uint64_t, std::uint64_t)> CommitRawBlock;
    typedef std::function<std::uint64_t()> RawRingUsedBytes;

    struct RdmaParam {
        unsigned char device_id;
        unsigned int pkt_size;
        unsigned int recv_wr_num;
        unsigned int poll_batch;
        int poll_cpu_id;
        char DAddr[64];
        char DMacAddr[64];
        char dst_port[64];
        AcquireRawBlock AcquireRawBlockPtr;
        CommitRawBlock CommitRawBlockPtr;
        RawRingUsedBytes RawRingUsedBytesPtr;
    };

    explicit StagedCopyReceiver(const RdmaParam& param);
    ~StagedCopyReceiver();

    int Start();
    int Stop();
    ReceiveStats GetReceiveStats() const;

  private:
    struct State;
    StagedCopyReceiver(const StagedCopyReceiver&);
    const StagedCopyReceiver& operator=(const StagedCopyReceiver&);
    static void *ReceiveThread(void *arg);
    bool AcquireBlock();
    bool CommitFullBlock();
    bool CommitTail();

    RdmaParam param_;
    void *ibv_res_;
    State *state_;
    std::atomic<bool> stop_requested_;
    bool thread_started_;
};
