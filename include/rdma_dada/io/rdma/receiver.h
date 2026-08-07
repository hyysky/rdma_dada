#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

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
        };

        typedef std::function<int(void)> DataSend;
        typedef std::function<char*(long int &)> GetBuff;
        typedef std::function<int(unsigned char *, long int )> WriteBuff;
        typedef std::function<void(void)> DecrementWriteCount;  // 递减写入计数
        typedef std::function<bool(void)> IsBlockFull;  // 检查block是否已满

        struct RdmaParam
        {
            unsigned char gpu_id;
            unsigned char device_id;
            unsigned int pkt_size;
            unsigned int send_n;
            int bind_cpu_id;
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
            DecrementWriteCount DecrementWriteCount;
            IsBlockFull IsBlockFull;
        };

        explicit RoCEv2Dada(const RdmaParam & Param);
        ~RoCEv2Dada();
        int Start();
        int Stop();
        ReceiveStats GetReceiveStats() const;
    private:
        RoCEv2Dada(const RoCEv2Dada &);
        const RoCEv2Dada &operator=(const RoCEv2Dada &);
        static void * SendRecvThread(void * arg);
        RdmaParam param;
        void * ibv_res;
        std::atomic<bool> stop_requested;
        std::atomic<std::uint64_t> accepted_receive_packets;
        std::atomic<std::uint64_t> wrong_length_receive_packets;
        bool thread_started;
};

#ifdef __cplusplus
}
#endif
