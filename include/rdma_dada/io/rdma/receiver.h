#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "rdma_dada/io/rdma/receive_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

// Linux RoCE transport adapter. It knows nothing about PSRDADA ownership;
// callers provide block callbacks at the application boundary.
#define PKT_HEAD_LEN 64
#define PKT_DATA_SIZE 8192

class RoCEv2Dada
{
    public:
        struct ReceiveStats
        {
            std::uint64_t accepted_packets;
            std::uint64_t wrong_length_packets;
            std::uint64_t published_packets;
            std::uint64_t published_blocks;
            std::uint64_t partial_blocks;
            std::uint64_t cq_tail_records;
            std::uint64_t poll_calls;
            std::uint64_t empty_polls;
            std::uint64_t full_polls;
            std::uint64_t reposted_wrs;
            std::uint64_t repost_failures;
            std::uint64_t copy_batches;
            std::uint64_t min_posted_wrs;
            std::uint64_t completion_queue_high_watermark;
            std::uint64_t recycle_queue_high_watermark;
            std::uint64_t completion_to_recycle_ns_total;
            std::uint64_t completion_to_recycle_ns_max;
        };

        typedef std::function<int(std::uint64_t)> DataSend;
        typedef std::function<char*(long int &)> GetBuff;
        typedef std::function<int(unsigned char *, long int )> WriteBuff;

        struct RdmaParam
        {
            unsigned char gpu_id;
            unsigned char device_id;
            unsigned int pkt_size;
            unsigned int send_n;
            unsigned int recv_wr_num;
            unsigned int poll_batch;
            int poll_cpu_id;
            std::vector<int> poll_cpu_ids;
            int copy_cpu_id;
            unsigned int receive_shards;
            std::vector<rdma_dada::io::rdma::ReceiveFlowSpec> receive_flows;
            int RdmaDirectGpu;
            bool SendOrRecv;
            bool debug_mode;  // Debug mode flag
            unsigned int nsge;
            char SAddr[64];
            char DAddr[64];
            char SMacAddr[64];
            char DMacAddr[64];
            char src_port[64];
            char dst_port[64];
            DataSend DataSendBuff;
            GetBuff GetBuffPtr;
            WriteBuff WritSendBuff;
        };

        explicit RoCEv2Dada(const RdmaParam & Param);
        ~RoCEv2Dada();
        int Start();
        int Stop();
        ReceiveStats GetReceiveStats() const;
    private:
        struct ReceivePipelineState;
        RoCEv2Dada(const RoCEv2Dada &);
        const RoCEv2Dada &operator=(const RoCEv2Dada &);
        static void * SendRecvThread(void * arg);
        static void * ReceivePollThread(void * arg);
        static void * ReceiveCopyThread(void * arg);
        RdmaParam param;
        void * ibv_res;
        std::vector<void *> receive_resources;
        ReceivePipelineState * receive_pipeline;
        std::atomic<bool> stop_requested;
        std::atomic<std::uint64_t> accepted_receive_packets;
        std::atomic<std::uint64_t> wrong_length_receive_packets;
        std::atomic<std::uint64_t> published_receive_packets;
        std::atomic<std::uint64_t> published_receive_blocks;
        std::atomic<std::uint64_t> partial_receive_blocks;
        std::atomic<std::uint64_t> cq_tail_receive_records;
        bool thread_started;
};

#ifdef __cplusplus
}
#endif
