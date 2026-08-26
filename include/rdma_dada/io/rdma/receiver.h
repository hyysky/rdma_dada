#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

struct ibv_pd;

// Linux RAW_PACKET receiver. The NIC scatters each matching frame into a
// per-WR protocol-header scratch area and one directly owned PSRDADA slot.
class RoCEv2Dada {
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
        ReceiveExitReason exit_reason;
    };

    struct DirectRawBlockLease {
        unsigned char *addr;
        std::uint64_t bytes;
        std::uint64_t token;
        std::uint32_t lkey;
    };

    typedef std::function<int(struct ibv_pd *)> PrepareRawRing;
    typedef std::function<int(DirectRawBlockLease *)> AcquireRawBlock;
    typedef std::function<int(std::uint64_t, std::uint64_t)> CommitRawBlock;
    typedef std::function<void()> ReleaseRawRing;

    struct RdmaParam {
        unsigned char device_id;
        unsigned int pkt_size;
        unsigned int recv_wr_num;
        unsigned int poll_batch;
        int poll_cpu_id;
        bool debug_mode;
        char DAddr[64];
        char DMacAddr[64];
        char dst_port[64];
        PrepareRawRing PrepareRawRingMemory;
        AcquireRawBlock AcquireRawBlockPtr;
        CommitRawBlock CommitRawBlockPtr;
        ReleaseRawRing ReleaseRawRingMemory;
    };

    explicit RoCEv2Dada(const RdmaParam& param);
    ~RoCEv2Dada();

    int Start();
    int Stop();
    ReceiveStats GetReceiveStats() const;

  private:
    struct DirectReceiveState;
    RoCEv2Dada(const RoCEv2Dada&);
    const RoCEv2Dada& operator=(const RoCEv2Dada&);
    static void *ReceiveDirectThread(void *arg);
    bool AcquireDirectBlock();
    bool AssignDirectSlot(std::uint64_t wr_id);
    bool PublishReadyDirectBlocks(bool replenish);
    bool PublishDirectTail();

    RdmaParam param;
    void *ibv_res;
    DirectReceiveState *receive_state;
    std::atomic<bool> stop_requested;
    std::atomic<std::uint64_t> accepted_receive_packets;
    std::atomic<std::uint64_t> wrong_length_receive_packets;
    std::atomic<std::uint64_t> zeroed_receive_packets;
    std::atomic<std::uint64_t> published_receive_packets;
    std::atomic<std::uint64_t> published_receive_blocks;
    std::atomic<std::uint64_t> partial_receive_blocks;
    std::atomic<std::uint64_t> cq_tail_receive_records;
    bool thread_started;
};
