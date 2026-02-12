#ifndef PSRDADA_RINGBUF_H
#define PSRDADA_RINGBUF_H

#include <stdint.h>
#include <sys/types.h>
#include <vector>

struct ibv_pd;
struct ibv_mr;

// 存储每个block的MR信息
struct BlockMrInfo {
    void *addr;           // block地址
    uint64_t size;        // block大小
    struct ibv_mr *mr;    // 对应的MR
    uint64_t block_idx;   // block索引
};

class PsrdadaRingBuf {
public:
    PsrdadaRingBuf();
    int Init(key_t key, uint64_t block_bytes, uint64_t nbufs, const char *header_template_path, uint64_t file_bytes = 0);
    char* GetWriteBuffer(uint64_t bytes);
    int MarkWritten(uint64_t bytes);
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
    
    // 获取当前写入block的MR
    struct ibv_mr* GetCurrentBlockMr();
    
    // 清理所有已注册的block MRs
    void UnregisterAllBlocks();
    
    int DumpToDada(const char *out_path, const char *header_template_path);

    ~PsrdadaRingBuf();
private:
    void *hdu;
    void *log;
    void *data_block;  // ipcio_t* (ת����void*����)
    char *current_ptr;
    uint64_t current_block;
    int is_initialized;
    uint32_t buffer_key;
    
    // RDMA相关：存储每个block的MR
    std::vector<BlockMrInfo> block_mrs;
    struct ibv_pd *registered_pd;
    bool use_block_registration;  // 是否使用分块注册模式
};

#endif // PSRDADA_RINGBUF_H
