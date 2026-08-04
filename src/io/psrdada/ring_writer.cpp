//定义 psrdada_ringbuf 类的成员函数，用于与 PSRDADA 环形缓冲区交互
#include "rdma_dada/io/psrdada/ring_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

extern "C" {
    #include <dada_hdu.h>
    #include <ascii_header.h>
    #include <futils.h>
    #include <ipcio.h>
    #include <multilog.h>
}

#include <infiniband/verbs.h>
#include "rdma_dada/io/psrdada/header_codec.h"
#include "dada_def.h"

// 注意：data_block和hdu改为成员变量，不再使用全局变量

PsrdadaRingBuf::PsrdadaRingBuf(): hdu(NULL), log(NULL), data_block(NULL), current_ptr(NULL), current_block(0), 
    is_initialized(0), buffer_key(0), 
    registered_pd(NULL), use_block_registration(false) {}

// 初始化 PSRDADA 环形缓冲区
int PsrdadaRingBuf::Init(key_t key, uint64_t block_bytes, uint64_t nbufs,
                        const char *header_template_path,
                        const dada_header_t &runtime_header)
{
    if (is_initialized) return -1;
    buffer_key = key;

    log = (void *)multilog_open("psrdada_ringbuf", 0);
    if (!log) return -1;
    multilog_add((multilog_t *)log, stderr);
    this->hdu = (void*)dada_hdu_create((multilog_t *)log);
    if (!this->hdu) { 
        multilog_close((multilog_t *)log); 
        log = NULL; 
        fprintf(stderr, "Failed to create DADA HDU\n");
        return -1; 
    }
    dada_hdu_set_key((dada_hdu_t*)this->hdu, key);
    if (dada_hdu_connect((dada_hdu_t*)this->hdu) < 0) { 
        dada_hdu_destroy((dada_hdu_t*)this->hdu); 
        multilog_close((multilog_t *)log); 
        log = NULL; 
        this->hdu = NULL; 
        fprintf(stderr, "Failed to connect to DADA HDU with key 0x%x\n", key);
        return -1; 
    } else {
        fprintf(stderr, "Connected to DADA HDU with key 0x%x\n", key);
    }
    if (dada_hdu_lock_write((dada_hdu_t*)this->hdu) < 0) { 
        fprintf(stderr, "Failed to lock DADA HDU for writing\n");
        ResetAfterInitFailure(false);
        return -1; }
    dada_hdu_t *hdu_ptr = (dada_hdu_t*)this->hdu;
    this->data_block = (ipcio_t *)(hdu_ptr->data_block);

    ipcbuf_t *data_buf = (ipcbuf_t *)this->data_block;
    const uint64_t actual_block_bytes = ipcbuf_get_bufsz(data_buf);
    const uint64_t actual_nbufs = ipcbuf_get_nbufs(data_buf);
    if (actual_block_bytes != block_bytes || actual_nbufs != nbufs) {
        fprintf(stderr,
                "DADA ring layout mismatch: expected %" PRIu64
                " blocks x %" PRIu64 " bytes, got %" PRIu64
                " blocks x %" PRIu64 " bytes\n",
                nbufs, block_bytes, actual_nbufs, actual_block_bytes);
        ResetAfterInitFailure(true);
        return -1;
    }

    ipcbuf_t *header_block = (ipcbuf_t *)(hdu_ptr->header_block);
    uint64_t header_size = ipcbuf_get_bufsz(header_block);
    char *hdrbuf = ipcbuf_get_next_write(header_block);
    if (!hdrbuf || header_size == 0) {
        fprintf(stderr, "Failed to acquire DADA header block\n");
        ResetAfterInitFailure(true);
        return -1;
    }
    memset(hdrbuf, 0, (size_t)header_size);

    // The template owns only site/observation fields unknown to this module.
    // All data-contract fields are overlaid from the validated runtime config.
    if (header_template_path) {
        // Preserve every field in the template, including fields unknown to
        // this module, and then overlay runtime values.
        if (fileread(header_template_path, hdrbuf, (unsigned)header_size) < 0) {
            fprintf(stderr, "Failed to read DADA header template %s\n", header_template_path);
            ResetAfterInitFailure(true);
            return -1;
        }
    }
    if (ascii_header_set(hdrbuf, "HDR_VERSION", "1.0") < 0 ||
        ascii_header_set(hdrbuf, "HDR_SIZE", "%" PRIu64, header_size) < 0 ||
        write_dada_header(runtime_header, hdrbuf) < 0) {
        fprintf(stderr, "Failed to populate runtime DADA header\n");
        ResetAfterInitFailure(true);
        return -1;
    }
    if (ipcbuf_enable_eod(header_block) < 0) {
        fprintf(stderr, "Failed to enable EOD on DADA header block\n");
        ResetAfterInitFailure(true);
        return -1;
    }
    if (ipcbuf_mark_filled(header_block, header_size) < 0) {
        fprintf(stderr, "Failed to publish DADA header block\n");
        ResetAfterInitFailure(true);
        return -1;
    }
    printf("Published runtime DADA header%s%s\n",
           header_template_path ? " over template " : "",
           header_template_path ? header_template_path : "");
    is_initialized = 1;
    printf("PsrdadaRingBuf initialized with key=0x%x, blocks=%lu, block_size=%lu\n", 
           key, (unsigned long)nbufs, (unsigned long)block_bytes);
    return 0;
}

void PsrdadaRingBuf::ResetAfterInitFailure(bool write_locked)
{
    dada_hdu_t *hdu_ptr = (dada_hdu_t *)hdu;
    if (hdu_ptr) {
        if (write_locked) dada_hdu_unlock_write(hdu_ptr);
        dada_hdu_disconnect(hdu_ptr);
        dada_hdu_destroy(hdu_ptr);
    }
    if (log) multilog_close((multilog_t *)log);
    hdu = NULL;
    log = NULL;
    data_block = NULL;
    current_ptr = NULL;
    is_initialized = 0;
}

char* PsrdadaRingBuf::GetWriteBuffer(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (!is_initialized) return NULL;
    // 使用底层ipcbuf API获取下一个写入block
    // 这样RoCE可以直接写入到这个block
    ipcbuf_t *buf = (ipcbuf_t*)data_block;
    current_ptr = ipcbuf_get_next_write(buf);
    if (!current_ptr) {
        fprintf(stderr, "Failed to get next write block from ipcbuf\n");
        return NULL;
    }
    
    // 记录当前block索引（用于获取对应的MR）
    current_block = (uint64_t)ipcbuf_get_write_count(buf) % ipcbuf_get_nbufs(buf);
    
    // 验证请求的大小不超过block大小
    uint64_t bufsz = ipcbuf_get_bufsz(buf);
    if (bytes > bufsz) {
        fprintf(stderr, "Requested size %lu exceeds block size %lu\n", 
                (unsigned long)bytes, (unsigned long)bufsz);
        ipcbuf_mark_cleared(buf);  // 释放刚获取的block
        current_ptr = NULL;
        return NULL;
    }
    
    return current_ptr;
}

int PsrdadaRingBuf::MarkWritten(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (!is_initialized) return -1;
    if (!current_ptr) {
        fprintf(stderr, "MarkWritten called but no current block\n");
        return -1;
    }
    
    // 使用底层ipcbuf API标记block已填充
    // bytes参数指定实际写入的字节数
    ipcbuf_t *buf = (ipcbuf_t*)data_block;
    if (ipcbuf_mark_filled(buf, bytes) < 0) {
        fprintf(stderr, "Failed to mark block as filled\n");
        current_ptr = NULL;
        return -1;
    }
    
    current_ptr = NULL;  // 清空当前指针，准备下一次获取
    return 0;
}

int PsrdadaRingBuf::StartBlock()
{
    if (!is_initialized) return -1;
    uint64_t byte = ipcio_tell((ipcio_t*)data_block);
    if (ipcio_start((ipcio_t*)data_block, byte) < 0) return -1;
    return 0;
}

int PsrdadaRingBuf::StopBlock()
{
    if (!is_initialized) return -1;
    if (ipcio_stop((ipcio_t*)data_block) < 0) return -1;
    return 0;
}

void PsrdadaRingBuf::GetBufferStats(uint64_t &free_space, uint64_t &used_space)
{
    if (!is_initialized) {
        free_space = 0;
        used_space = 0;
        return;
    }
    ipcbuf_t *buf = (ipcbuf_t*)data_block;
    uint64_t total_bufs = ipcbuf_get_nbufs(buf);
    uint64_t bufsz = ipcbuf_get_bufsz(buf);

    uint64_t write_count = ipcbuf_get_write_count(buf);
    uint64_t read_count = ipcbuf_get_read_count(buf);
    uint64_t nbufs_used = (write_count >= read_count) ? (write_count - read_count) : 0;
    if (nbufs_used > total_bufs) nbufs_used = total_bufs;

    used_space = nbufs_used * bufsz;
    free_space = (total_bufs - nbufs_used) * bufsz;
}

uint64_t PsrdadaRingBuf::GetFreeSpace()
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    uint64_t free_space, used_space;
    GetBufferStats(free_space, used_space);
    return free_space;
}

uint64_t PsrdadaRingBuf::GetUsedSpace()
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    uint64_t free_space, used_space;
    GetBufferStats(free_space, used_space);
    return used_space;
}

uint64_t PsrdadaRingBuf::GetBlockSize()
{
    if (!is_initialized) return 0;
    ipcbuf_t *buf = (ipcbuf_t*)data_block;
    return ipcbuf_get_bufsz(buf);
}

void PsrdadaRingBuf::Cleanup()
{
    if (!is_initialized) return;
    
    printf("[Cleanup] Starting cleanup sequence...\n");
    
    // Step 1: Send EOD (End of Data) signal to readers
    dada_hdu_t *hdu_ptr = (dada_hdu_t*)hdu;
    if (hdu_ptr) {
        if (hdu_ptr->data_block) {
            printf("[Cleanup] Sending EOD signal to readers...\n");
            if (ipcio_stop((ipcio_t*)data_block) < 0) {
                fprintf(stderr, "[Cleanup] Warning: ipcio_stop failed\n");
            } else {
                printf("[Cleanup] \u2713 EOD signal sent\n");
            }
        }
    }
    
    // Step 2: Wait for external readers (like dada_dbdisk) to detect EOD and exit
    printf("[Cleanup] Waiting for readers to detect EOD and exit gracefully...\n");
    sleep(5);
    
    // Step 3: Cleanup RDMA registrations BEFORE disconnecting
    if (use_block_registration && !block_mrs.empty()) {
        printf("[Cleanup] Unregistering RDMA blocks...\n");
        UnregisterAllBlocks();
    }
    
    // Step 4: Unlock write, disconnect, then destroy HDU
    // CRITICAL: Must complete unlock and disconnect BEFORE destroying
    if (hdu_ptr) {
        
        // 4a. Unlock write first
        printf("[Cleanup] Unlocking write lock...\n");
        if (dada_hdu_unlock_write(hdu_ptr) < 0) {
            fprintf(stderr, "[Cleanup] Warning: dada_hdu_unlock_write failed\n");
        } else {
            printf("[Cleanup] \u2713 Write lock released\n");
        }
        
        // 4b. Disconnect from ring buffer
        printf("[Cleanup] Disconnecting from ring buffer...\n");
        if (dada_hdu_disconnect(hdu_ptr) < 0) {
            fprintf(stderr, "[Cleanup] Warning: dada_hdu_disconnect failed\n");
        } else {
            printf("[Cleanup] \u2713 Disconnected from ring buffer\n");
        }
        
        // Wait a bit to ensure all disconnect operations complete
        usleep(100000);  // 100ms
        
        // 4c. Destroy HDU (local handle only, not the shared memory)
        printf("[Cleanup] Destroying HDU handle...\n");
        dada_hdu_destroy(hdu_ptr);
        hdu = NULL;
        printf("[Cleanup] \u2713 HDU handle destroyed\n");
    }
    
    if (log) { 
        multilog_close((multilog_t *)log); 
        log = NULL; 
    }
    
    is_initialized = 0;
    printf("[Cleanup] \u2713 Cleanup complete - ring buffer can now be safely destroyed\n");
}

PsrdadaRingBuf::~PsrdadaRingBuf() { Cleanup(); }

struct ibv_mr* PsrdadaRingBuf::RegisterMemoryFromPointer(struct ibv_pd *pd, void *addr, uint64_t size, int access)
{
    if (!pd || !addr || size == 0) return NULL;
    struct ibv_mr *mr = ibv_reg_mr(pd, addr, size, access);
    if (!mr) return NULL;
    return mr;
}

int PsrdadaRingBuf::UnregisterMemory(struct ibv_mr *mr)
{
    if (!mr) return 0;
    if (ibv_dereg_mr(mr) != 0) return -1;
    return 0;
}

struct ibv_mr* PsrdadaRingBuf::RegisterWholeRing(struct ibv_pd *pd, int access)
{
    if (!is_initialized) return NULL;
    dada_hdu_t *hdu_ptr = (dada_hdu_t *)hdu;
    ipcio_t *ipc = (ipcio_t *)hdu_ptr->data_block;
    if (!ipc) return NULL;
    ipcbuf_t *buf = &ipc->buf;
    if (!buf->shm_addr) return NULL;
    if (!buf->sync) return NULL;
    uint64_t nbufs = buf->sync->nbufs;
    uint64_t bufsz = buf->sync->bufsz;
    if (nbufs == 0 || bufsz == 0) return NULL;
    void *base = buf->shm_addr[0];
    if (!base) return NULL;
    
    // 验证所有blocks是否在内存中连续
    // 这对于RDMA注册整个ring是必需的
    bool is_contiguous = true;
    for (uint64_t i = 1; i < nbufs; i++) {
        void *expected_addr = (char*)base + i * bufsz;
        void *actual_addr = buf->shm_addr[i];
        if (expected_addr != actual_addr) {
            fprintf(stderr, " Warning : Ring buffer blocks are NOT contiguous!\n");
            fprintf(stderr, "  Block %" PRIu64 ": expected %p, actual %p\n",
                    i, expected_addr, actual_addr);
            is_contiguous = false;
            break;
        }
    }
    
    if (!is_contiguous) {
        fprintf(stderr, "\n[RegisterWholeRing] Ring buffer blocks are NOT contiguous.\n");
        fprintf(stderr, "Current config: %" PRIu64 " blocks × %" PRIu64
                        " bytes = %" PRIu64 " MB\n",
                nbufs, bufsz, (nbufs * bufsz) / 1024 / 1024);
        fprintf(stderr, "Automatically switching to per-block registration mode...\n\n");
        
        // 自动切换到分块注册模式
        if (RegisterRingBlocks(pd, access) == 0) {
            printf("[RegisterWholeRing] Successfully registered %" PRIu64
                   " blocks individually\n", nbufs);
            use_block_registration = true;
            registered_pd = pd;
            // 注意：分块注册模式下返回NULL，因为没有单一的MR代表整个ring
            // 调用方应该使用GetCurrentBlockMr()来获取每个block的MR
            return NULL;
        } else {
            fprintf(stderr, "[RegisterWholeRing] Failed to register blocks individually\n");
            return NULL;
        }
    }
    
    uint64_t total_size = nbufs * bufsz;
    printf("[RegisterWholeRing] Memory is contiguous: %" PRIu64
           " blocks × %" PRIu64 " bytes\n",
           nbufs, bufsz);
    
    struct ibv_mr *mr = RegisterMemoryFromPointer(pd, base, total_size, access);
    if (!mr) {
        fprintf(stderr, "Failed to register memory region with RDMA\n");
        return NULL;
    }
    
    // 记录使用整块注册模式
    use_block_registration = false;
    registered_pd = pd;
    
    printf("[RegisterWholeRing] Success: base=%p size=%" PRIu64
           " MB rkey=0x%x lkey=0x%x\n",
           base, total_size / 1024 / 1024, mr->rkey, mr->lkey);
    return mr;
}

int PsrdadaRingBuf::DumpToDada(const char *out_path, const char *header_template_path)
{
    if (!is_initialized || !out_path) return -1;
    dada_hdu_t *hdu_ptr = (dada_hdu_t *)hdu;
    ipcio_t *ipc = (ipcio_t *)hdu_ptr->data_block;
    if (!ipc) return -1;
    ipcbuf_t *buf = &ipc->buf;
    if (!buf->shm_addr || !buf->sync) return -1;
    uint64_t nbufs = buf->sync->nbufs;
    uint64_t bufsz = buf->sync->bufsz;
    if (nbufs == 0 || bufsz == 0) return -1;

    const size_t HDR_SIZE = 4096;
    char hdr[HDR_SIZE];
    // 读取并构建header
    dada_header_t dada_header;
    if (read_dada_header_from_file(header_template_path, &dada_header) < 0) return -1;
    if (write_dada_header(dada_header, hdr) < 0) return -1;

    FILE *out = fopen(out_path, "wb");
    if (!out) return -1;

    // Write header
    if (fwrite(hdr, 1, HDR_SIZE, out) != HDR_SIZE) {
        fclose(out);
        return -1;
    }

    // Write ring buffers sequentially
    for (uint64_t i = 0; i < nbufs; ++i) {
        void *base = buf->shm_addr[i];
        if (!base) continue;
        size_t written = fwrite(base, 1, (size_t)bufsz, out);
        if (written != (size_t)bufsz) {
            fclose(out);
            return -1;
        }
    }
    fclose(out);
    printf("PsrdadaRingBuf: dumped %" PRIu64 " buffers of %" PRIu64
           " bytes to %s\n", nbufs, bufsz, out_path);
    return 0;
}

// 发送EOD信号并断开writer连接，但不销毁ring buffer
// 用于外部管理ring buffer生命周期的场景
int PsrdadaRingBuf::SendEODAndDisconnect()
{
    if (!is_initialized) return -1;
    
    printf("[SendEODAndDisconnect] Sending EOD and disconnecting...\n");
    
    dada_hdu_t *hdu_ptr = (dada_hdu_t*)hdu;
    if (hdu_ptr) {
        
        // 1. Send EOD signal to readers
        if (hdu_ptr->data_block) {
            printf("[SendEODAndDisconnect] Sending EOD signal...\n");
            if (ipcio_stop((ipcio_t*)data_block) < 0) {
                fprintf(stderr, "[SendEODAndDisconnect] Warning: ipcio_stop failed\n");
            } else {
                printf("[SendEODAndDisconnect] ✓ EOD signal sent\n");
            }
            // Give readers time to detect EOD and start their cleanup
            printf("[SendEODAndDisconnect] Waiting for readers to detect EOD...\n");
            sleep(2);
        }
        
        // 2. Cleanup RDMA registrations (safe to do before disconnecting)
        if (use_block_registration && !block_mrs.empty()) {
            printf("[SendEODAndDisconnect] Unregistering RDMA blocks...\n");
            UnregisterAllBlocks();
        }
        
        // 3. Unlock write (release writer lock) - MUST be done before disconnect
        printf("[SendEODAndDisconnect] Unlocking write...\n");
        if (dada_hdu_unlock_write(hdu_ptr) < 0) {
            fprintf(stderr, "[SendEODAndDisconnect] Warning: unlock_write failed\n");
        } else {
            printf("[SendEODAndDisconnect] ✓ Write lock released\n");
        }
        
        // 4. Disconnect from HDU (but don't destroy it) - MUST be done before destroy
        printf("[SendEODAndDisconnect] Disconnecting from HDU...\n");
        if (dada_hdu_disconnect(hdu_ptr) < 0) {
            fprintf(stderr, "[SendEODAndDisconnect] Warning: disconnect failed\n");
        } else {
            printf("[SendEODAndDisconnect] ✓ Disconnected from HDU\n");
        }
        
        // Wait a bit to ensure disconnect completes
        usleep(100000);  // 100ms
        
        // 5. Destroy local HDU handle (ring buffer itself remains in shared memory)
        printf("[SendEODAndDisconnect] Destroying HDU handle...\n");
        dada_hdu_destroy(hdu_ptr);
        hdu = NULL;
        printf("[SendEODAndDisconnect] ✓ Disconnected (ring buffer remains active)\n");
    }
    
    if (log) {
        multilog_close((multilog_t *)log);
        log = NULL;
    }
    
    is_initialized = 0;
    printf("[SendEODAndDisconnect] ✓ Complete\n");
    return 0;
}

// ==================== 新增：支持非连续内存的RDMA注册 ====================

// 为每个block分别注册MR（支持非连续内存）
int PsrdadaRingBuf::RegisterRingBlocks(struct ibv_pd *pd, int access)
{
    if (!is_initialized) {
        fprintf(stderr, "[RegisterRingBlocks] Ring not initialized\n");
        return -1;
    }
    if (!pd) {
        fprintf(stderr, "[RegisterRingBlocks] Invalid PD\n");
        return -1;
    }
    
    dada_hdu_t *hdu_ptr = (dada_hdu_t *)hdu;
    ipcio_t *ipc = (ipcio_t *)hdu_ptr->data_block;
    if (!ipc) return -1;
    
    ipcbuf_t *buf = &ipc->buf;
    if (!buf->shm_addr || !buf->sync) return -1;
    
    uint64_t nbufs = buf->sync->nbufs;
    uint64_t bufsz = buf->sync->bufsz;
    if (nbufs == 0 || bufsz == 0) return -1;
    
    printf("[RegisterRingBlocks] Registering %" PRIu64
           " blocks individually...\n", nbufs);
    fflush(stderr);
    
    // 清理旧的注册（如果有）
    printf("[RegisterRingBlocks] Calling UnregisterAllBlocks()...\n");
    fflush(stderr);
    UnregisterAllBlocks();
    printf("[RegisterRingBlocks] UnregisterAllBlocks() completed\n");
    fflush(stderr);
    block_mrs.clear();
    
    printf("[RegisterRingBlocks] Starting to register %" PRIu64
           " blocks...\n", nbufs);
    fflush(stderr);
    
    // 为每个block注册MR
    for (uint64_t i = 0; i < nbufs; i++) {
        printf("[RegisterRingBlocks] Registering block %" PRIu64
               "/%" PRIu64 "...\n", i, nbufs);
        fflush(stderr);
        void *block_addr = buf->shm_addr[i];
        if (!block_addr) {
            fprintf(stderr, "[RegisterRingBlocks] Block %" PRIu64
                            " has NULL address\n", i);
            UnregisterAllBlocks();
            return -1;
        }
        
        printf("[RegisterRingBlocks] Calling ibv_reg_mr for block %" PRIu64
               " (addr=%p, size=%" PRIu64 ")...\n",
               i, block_addr, bufsz);
        fflush(stderr);
        struct ibv_mr *mr = ibv_reg_mr(pd, block_addr, bufsz, access);
        printf("[RegisterRingBlocks] ibv_reg_mr returned: %p\n", (void*)mr);
        fflush(stderr);
        if (!mr) {
            fprintf(stderr, "[RegisterRingBlocks] Failed to register block %" PRIu64
                            " at %p\n",
                    i, block_addr);
            UnregisterAllBlocks();
            return -1;
        }
        
        BlockMrInfo info;
        info.addr = block_addr;
        info.size = bufsz;
        info.mr = mr;
        info.block_idx = i;
        block_mrs.push_back(info);
        
        printf("[RegisterRingBlocks] Block %2" PRIu64
               ": addr=%p size=%" PRIu64 " KB rkey=0x%08x lkey=0x%08x\n",
               i, block_addr, bufsz / 1024, mr->rkey, mr->lkey);
    }
    
    registered_pd = pd;
    use_block_registration = true;
    
    printf("[RegisterRingBlocks] Successfully registered all %" PRIu64
           " blocks\n", nbufs);
    printf("[RegisterRingBlocks] Total size: %" PRIu64 " MB\n",
           (nbufs * bufsz) / 1024 / 1024);
    
    return 0;
}

// 获取当前写入block的MR
struct ibv_mr* PsrdadaRingBuf::GetCurrentBlockMr()
{
    if (!use_block_registration) {
        // 使用整块注册模式，不需要单独的block MR
        return NULL;
    }
    
    if (block_mrs.empty()) {
        fprintf(stderr, "[GetCurrentBlockMr] No blocks registered\n");
        return NULL;
    }
    
    if (!current_ptr) {
        fprintf(stderr, "[GetCurrentBlockMr] No current block\n");
        return NULL;
    }
    
    // 优先使用 current_block 索引做 O(1) 查找
    if (current_block < block_mrs.size() && block_mrs[current_block].addr == current_ptr) {
        return block_mrs[current_block].mr;
    }

    // 索引与地址不匹配（异常情况），回退到线性查找
    for (size_t i = 0; i < block_mrs.size(); i++) {
        if (block_mrs[i].addr == current_ptr) {
            fprintf(stderr, "[GetCurrentBlockMr] WARNING: current_block=%" PRIu64 " does not "
                    "match current_ptr=%p (found at index %zu)\n",
                    current_block, current_ptr, i);
            return block_mrs[i].mr;
        }
    }

    fprintf(stderr, "[GetCurrentBlockMr] Block MR not found for ptr=%p idx=%" PRIu64 "\n",
            current_ptr, current_block);
    return NULL;
}

// 清理所有已注册的block MRs
void PsrdadaRingBuf::UnregisterAllBlocks()
{
    if (block_mrs.empty()) return;
    
    printf("[UnregisterAllBlocks] Unregistering %lu blocks...\n", block_mrs.size());
    
    for (size_t i = 0; i < block_mrs.size(); i++) {
        if (block_mrs[i].mr) {
            if (ibv_dereg_mr(block_mrs[i].mr) != 0) {
                fprintf(stderr, "[UnregisterAllBlocks] Failed to unregister block %lu\n", i);
            }
        }
    }
    
    block_mrs.clear();
    use_block_registration = false;
    registered_pd = NULL;
    
    printf("[UnregisterAllBlocks] All blocks unregistered\n");
}
