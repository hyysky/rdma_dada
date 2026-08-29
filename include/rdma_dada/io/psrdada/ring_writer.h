#ifndef PSRDADA_RINGBUF_H
#define PSRDADA_RINGBUF_H

#include <stdint.h>
#include <sys/types.h>
#include <deque>
#include <vector>
#include <mutex>

#include "rdma_dada/pipeline/metadata.h"

struct ibv_pd;
struct ibv_mr;

// 存储每个block的MR信息
struct BlockMrInfo {
    void *addr;           // block地址
    uint64_t size;        // block大小
    struct ibv_mr *mr;    // 对应的MR
    uint64_t block_idx;   // block索引
};

struct WriteBlockLease {
    char *addr;
    uint64_t bytes;
    uint64_t token;
    uint64_t block_idx;
    struct ibv_mr *mr;
};

// PSRDADA write-side adapter. Algorithm modules must not depend on this type.
class PsrdadaRingBuf {
public:
    PsrdadaRingBuf();
    int Init(key_t key, uint64_t block_bytes, uint64_t nbufs,
             uint64_t record_bytes,
             const rdma_dada::pipeline::Metadata& runtime_header);
    int AcquireWriteBlock(WriteBlockLease *lease);
    int AcquireHostWriteBlock(WriteBlockLease *lease);
    int CommitWriteBlock(uint64_t token, uint64_t bytes);
    int StartBlock();
    int StopBlock();
    uint64_t GetFreeSpace();
    uint64_t GetUsedSpace();
    uint64_t GetBlockSize();  // 获取单个block的大小
    int SendEODAndDisconnect();  // 发送EOD信号并断开连接（不销毁ring）
    void Cleanup();
    struct ibv_mr* RegisterMemoryFromPointer(struct ibv_pd *pd, void *addr, uint64_t size, int access);
    int UnregisterMemory(struct ibv_mr *mr);
    
    // 旧方法：尝试注册整个连续ring（如果失败则自动切换到分块注册）
    struct ibv_mr* RegisterWholeRing(struct ibv_pd *pd, int access);
    
    // 新方法：为每个block分别注册MR（支持非连续内存）
    int RegisterRingBlocks(struct ibv_pd *pd, int access);
    
    // 清理所有已注册的block MRs
    void UnregisterAllBlocks();
    
    int DumpToDada(const char *out_path, const char *header_template_path);

    ~PsrdadaRingBuf();
private:
    void *hdu;
    void *log;
    void *data_block;  // ipcio_t* (ת����void*����)
    uint64_t block_bytes;
    uint64_t record_bytes;
    int is_initialized;
    uint32_t buffer_key;
    
    // RDMA相关：存储每个block的MR
    std::vector<BlockMrInfo> block_mrs;
    struct ibv_pd *registered_pd;
    bool use_block_registration;  // 是否使用分块注册模式
    struct OutstandingWriteBlock {
        char *addr;
        uint64_t token;
        uint64_t block_idx;
        struct ibv_mr *mr;
    };
    std::deque<OutstandingWriteBlock> outstanding_blocks_;
    uint64_t next_write_token_;
    
    void GetBufferStats(uint64_t &free_space, uint64_t &used_space);
    void ResetAfterInitFailure(bool write_locked);
    int CloseOutstandingBlocks();
    int AcquireWriteBlockInternal(WriteBlockLease *lease,
                                  bool require_registered_mr);

    // 线程安全：互斥锁保护关键方法
    std::mutex ring_mutex_;
};

#endif // PSRDADA_RINGBUF_H
