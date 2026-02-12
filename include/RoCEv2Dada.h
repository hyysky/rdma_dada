#pragma once

#include <functional>

#ifdef __cplusplus
extern "C" {
#endif

#define PKT_HEAD_LEN 64

struct ibv_mr;

class RoCEv2Dada
{
    public:
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
            int DirectToRing;
            struct ibv_mr *DirectMr;
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
        void * GetIbvRes() const;
        int SetDirectMr(struct ibv_mr *mr);
    private:
        RoCEv2Dada(const RoCEv2Dada &);
        const RoCEv2Dada &operator=(const RoCEv2Dada &);
        static void * SendRecvThread(void * arg);
        RdmaParam param;
        void * ibv_res;
};

#ifdef __cplusplus
}
#endif
