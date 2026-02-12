// Simplified RDMA + PSRDADA receiver demo for standalone rdma_dada module
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#include "RoCEv2Dada.h"
#include "psrdada_ringbuf.h"
#include "ibv_utils.h"

#define PSRDADA_BUFFER_KEY 0xdada
#define PKT_DATA_SIZE 8192

PsrdadaRingBuf *g_ringbuf = NULL;
volatile int g_thread_exit = 0;
static bool g_debug_mode = false;  // Debug mode flag
static uint32_t g_pkt_size = PKT_DATA_SIZE;
static uint32_t g_send_n = 64;
static uint64_t g_current_block_remaining_writes = 0;  // 当前block剩余可写入次数
static uint64_t g_bytes_per_write = 0;  // 每次写入的字节数
static uint64_t g_block_size = 0;  // 完整block的大小（固定值）

void signal_handler(int sig) {
    printf("\nReceived signal %d, exiting gracefully...\n", sig);
    g_thread_exit = 1;
}

char* GetBuffPtr(long int& buf_size) {
    if (g_debug_mode) {
        printf("[GetBuffPtr] Entry: buf_size=%ld\n", buf_size);
        fflush(stdout);
    }
    
    if (!g_ringbuf) return NULL;
    
    // 获取完整block的大小（固定值，不是剩余空间）
    if (g_block_size == 0) {
        g_block_size = g_ringbuf->GetBlockSize();
        if (g_debug_mode) {
            printf("[GetBuffPtr] Initialized g_block_size = %lu bytes (%.2f MB)\n",
                   g_block_size, g_block_size / 1024.0 / 1024.0);
        }
    }
    
    // 计算每次写入的字节数
    // g_pkt_size 已经包含包头（从命令行传入的是完整包大小）
    g_bytes_per_write = (uint64_t)g_pkt_size * g_send_n;
    
    // 计算这个block可以接收多少次：N = block_size / (pkt_size * send_n)
    g_current_block_remaining_writes = g_block_size / g_bytes_per_write;
    uint64_t remainder = g_block_size % g_bytes_per_write;
    
    if (g_debug_mode) {
        printf("[GetBuffPtr] Block calculation: block_size=%lu, bytes_per_write=%lu\n",
               g_block_size, g_bytes_per_write);
        printf("[GetBuffPtr] This block can receive %lu times", g_current_block_remaining_writes);
        if (remainder > 0) {
            printf(" (with %lu bytes remainder)\n", remainder);
            fprintf(stderr, "[GetBuffPtr] WARNING: block_size is not exact multiple!\n");
        } else {
            printf(" (exact fit)\n");
        }
        fflush(stdout);
    } else if (remainder > 0) {
        fprintf(stderr, "[WARN] Block size not exact multiple, %lu bytes wasted per block\n", remainder);
    }
    
    // 直接调用 GetWriteBuffer 获取下一个可写的block
    // GetWriteBuffer 内部会调用 ipcbuf_get_next_write()
    if (g_debug_mode) {
        printf("[GetBuffPtr] Calling GetWriteBuffer to get next writable block...\n");
        fflush(stdout);
    }
    
    char *ptr = g_ringbuf->GetWriteBuffer(g_block_size);
    if (!ptr) {
        fprintf(stderr, "[ERROR] Failed to get next block!\n");
        return NULL;
    }
    
    buf_size = (long int)g_block_size;
    if (g_debug_mode) {
        printf("[GetBuffPtr] ✓ Got next block: ptr=%p, size=%ld bytes (%.2f MB)\n", 
               ptr, buf_size, buf_size / 1024.0 / 1024.0);
        printf("[GetBuffPtr] ✓ Ready to receive %lu times (%lu bytes each)\n",
               g_current_block_remaining_writes, g_bytes_per_write);
        fflush(stdout);
    }
    return ptr;
}

// 递减当前block的剩余写入次数
void DecrementWriteCount() {
    if (g_current_block_remaining_writes > 0) {
        g_current_block_remaining_writes--;
        
        if (g_debug_mode) {
            // Debug模式：每10次或最后几次打印
            static int print_counter = 0;
            print_counter++;
            if (print_counter % 10 == 0 || g_current_block_remaining_writes < 5) {
                printf("[DecrementWriteCount] Remaining writes: %lu / %lu\n", 
                       g_current_block_remaining_writes, g_block_size / g_bytes_per_write);
                fflush(stdout);
            }
        }
    } else {
        fprintf(stderr, "[WARN] Write counter already at 0!\n");
    }
}

// 检查当前block是否已满
bool IsBlockFull() {
    return g_current_block_remaining_writes == 0;
}

int SendBuffPtr(void) {
    if (!g_ringbuf) {
        fprintf(stderr, "[ERROR] g_ringbuf is NULL!\n");
        return -1;
    }
    
    if (g_block_size == 0) {
        fprintf(stderr, "[ERROR] g_block_size is 0!\n");
        return -1;
    }
    
    // 使用完整block的大小（固定值），不是剩余空间
    if (g_debug_mode) {
        printf("[SendBuffPtr] Marking block as written: %lu bytes (%.2f MB)\n",
               g_block_size, g_block_size / 1024.0 / 1024.0);
        fflush(stdout);
    }
    
    if (g_ringbuf->MarkWritten(g_block_size) < 0) {
        fprintf(stderr, "[ERROR] MarkWritten() failed!\n");
        return -1;
    }
    static time_t last_print = 0;
    static uint64_t total_blocks = 0;
    total_blocks++;
    time_t now = time(NULL);
    if (now - last_print >= 2) {  // 每2秒打印一次
        last_print = now;
        uint64_t used = g_ringbuf->GetUsedSpace();
        uint64_t free = g_ringbuf->GetFreeSpace();
        uint64_t total = used + free;
        double fill_percent = total > 0 ? (double)used * 100.0 / total : 0.0;
        printf("[Progress] Blocks written: %lu | Ring buffer: %.1f%% full (%lu/%lu MB)\n", 
               total_blocks, fill_percent, used / 1024 / 1024, total / 1024 / 1024);
    }
    return 0;
}

void print_helper() {
    printf("Usage:\n");
    printf("    ./Demo_psrdada_online [options]\n");
    printf("Options:\n");
    printf("    -d, NIC device number (default: 0)\n");
    printf("    --smac, source MAC address (required)\n");
    printf("    --dmac, destination MAC address (required)\n");
    printf("    --sip, source IP address (required)\n");
    printf("    --dip, destination IP address (required)\n");
    printf("    --sport, source port number (required)\n");
    printf("    --dport, destination port number (required)\n");
    printf("    --pkt_size, packet size including header (default: %d)\n", PKT_DATA_SIZE);
    printf("    --send_n, batch size (default: 64)\n");
    printf("    --nsge, scatter/gather entries per work request (default: 4)\n");
    printf("    --key, psrdada buffer key in hex (default: 0x%x)\n", PSRDADA_BUFFER_KEY);
    printf("    --gpu, GPU device ID (default: 0)\n");
    printf("    --cpu, CPU ID for thread affinity (default: -1)\n");
    printf("    --debug, enable debug mode with verbose logging\n");
    printf("    --help, -h\n");
    printf("    --dump-dir, directory for dada_dbdisk output (runs in background)\n");
    printf("    --dump-header, path to header template file (default: header/array_GZNU.header)\n");
    printf("    --nbufs, number of PSRDADA ring blocks (default: 8)\n");
    printf("    --file-bytes, output file size in bytes (for reference, not used internally)\n");
}

static int parse_args(RoCEv2Dada::RdmaParam &param, key_t &psrdada_key,
                      uint64_t &nbufs, uint64_t &file_bytes,
                      char *dump_dir, size_t dump_dir_len,
                      char *header_path, size_t header_path_len,
                      int argc, char *argv[]) {
    int c;
    struct option long_options[] = {
        {.name = "smac", .has_arg = required_argument, .val = 256},
        {.name = "dmac", .has_arg = required_argument, .val = 257},
        {.name = "sip", .has_arg = required_argument, .val = 258},
        {.name = "dip", .has_arg = required_argument, .val = 259},
        {.name = "sport", .has_arg = required_argument, .val = 260},
        {.name = "dport", .has_arg = required_argument, .val = 261},
        {.name = "pkt_size", .has_arg = required_argument, .val = 264},
        {.name = "send_n", .has_arg = required_argument, .val = 265},
        {.name = "key", .has_arg = required_argument, .val = 266},
        {.name = "dump-dir", .has_arg = required_argument, .val = 267},
        {.name = "dump-header", .has_arg = required_argument, .val = 268},
        {.name = "nbufs", .has_arg = required_argument, .val = 269},
        {.name = "file-bytes", .has_arg = required_argument, .val = 270},
        {.name = "debug", .has_arg = no_argument, .val = 271},
        {.name = "nsge", .has_arg = required_argument, .val = 272},
        {.name = "gpu", .has_arg = required_argument, .val = 'g'},
        {.name = "cpu", .has_arg = required_argument, .val = 'c'},
        {.name = "device", .has_arg = required_argument, .val = 'd'},
        {.name = "help", .has_arg = no_argument, .val = 'h'},
        {0, 0, 0, 0}
    };
    param.bind_cpu_id = -1;
    param.gpu_id = 0;
    param.SendOrRecv = false;
    param.device_id = 0;
    param.pkt_size = PKT_DATA_SIZE;
    param.RdmaDirectGpu = 0;
    param.send_n = 64;
    param.DirectToRing = 0;  // Will be enabled by SetDirectMr() if single MR is available
    param.DirectMr = NULL;
    param.nsge = 4;
    psrdada_key = PSRDADA_BUFFER_KEY;
    nbufs = 8;
    while (1) {
        c = getopt_long(argc, argv, "d:g:c:h", long_options, NULL);
        switch (c) {
            case 'd': param.device_id = atoi(optarg); break;
            case 256: strcpy(param.SMacAddr, optarg); break;
            case 257: strcpy(param.DMacAddr, optarg); break;
            case 258: strcpy(param.SAddr, optarg); break;
            case 259: strcpy(param.DAddr, optarg); break;
            case 260: strcpy(param.src_port, optarg); break;
            case 261: strcpy(param.dst_port, optarg); break;
            case 264: param.pkt_size = atoi(optarg); break;
            case 265: param.send_n = atoi(optarg); break;
            case 266: sscanf(optarg, "%x", &psrdada_key); break;
            case 267: strncpy(dump_dir, optarg, dump_dir_len - 1); dump_dir[dump_dir_len - 1] = '\0'; break;
            case 268: strncpy(header_path, optarg, header_path_len - 1); header_path[header_path_len - 1] = '\0'; break;
            case 269: nbufs = (uint64_t)strtoull(optarg, NULL, 10); break;
            case 270: file_bytes = strtoull(optarg, NULL, 10); break;
            case 271: g_debug_mode = true; break;
            case 272: param.nsge = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 'g': param.gpu_id = atoi(optarg); break;
            case 'c': param.bind_cpu_id = atoi(optarg); break;
            case 'h': print_helper(); return -1;
            case -1: return 0;
            default: print_helper(); return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;
    key_t psrdada_key = PSRDADA_BUFFER_KEY;
    uint64_t nbufs = 8;
    uint64_t file_bytes = 0;  // Output file size (optional, for reference)
    char dump_dir[256] = "./data_out";
    char header_path[256] = "header/array_GZNU.header";
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    RoCEv2Dada::RdmaParam param;
    ret = parse_args(param, psrdada_key, nbufs, file_bytes, dump_dir, sizeof(dump_dir), header_path, sizeof(header_path), argc, argv);
    if (ret < 0) return -1;
    param.debug_mode = g_debug_mode;  // Set debug mode in RDMA param
    if (param.nsge == 0) {
        fprintf(stderr, "[WARN] Invalid --nsge value 0, falling back to 4\n");
        param.nsge = 4;
    }
    g_pkt_size = param.pkt_size;
    g_send_n = param.send_n;

    if (strlen(param.SMacAddr) == 0 || strlen(param.DMacAddr) == 0 ||
        strlen(param.SAddr) == 0 || strlen(param.DAddr) == 0 ||
        strlen(param.src_port) == 0 || strlen(param.dst_port) == 0) {
        fprintf(stderr, "Error: Missing required network parameters\n");
        print_helper();
        return -1;
    }
    g_ringbuf = new PsrdadaRingBuf();
    if (!g_ringbuf) { fprintf(stderr, "Error: Failed to create PsrdadaRingBuf\n"); return -1; }
    
    if (g_debug_mode) {
        printf("[Debug] Mode: ENABLED\n");
    }
    
    // pkt_size 已经包含包头（从 run_demo.sh 传入的是 PKT_HEADER+PKT_DATA）
    uint64_t receive_bytes_per_time = (uint64_t)(param.pkt_size) * param.send_n;
    printf("  Receive size per batch: %lu bytes (%.2f MB)\n", 
           receive_bytes_per_time, receive_bytes_per_time / 1024.0 / 1024.0);
    fflush(stdout);

    printf("\n[Main] Connecting to PSRDADA ring buffer (key=0x%x)...\n", psrdada_key);
    if (file_bytes > 0) {
        printf("  Output file size: %lu MB\n", file_bytes / 1024 / 1024);
    }
    ret = g_ringbuf->Init(psrdada_key, receive_bytes_per_time, nbufs, header_path, file_bytes);
    if (ret < 0) { 
        fprintf(stderr, "Error: Failed to initialize psrdada ring buffer\n"); delete g_ringbuf; return -1; 
    } else { 
        fprintf(stderr, "[Main] ✓ psrdada ring buffer initialized\n"); 
    }
    
    // 获取实际PSRDADA block大小（由dada_db创建时决定）
    uint64_t actual_block_size = g_ringbuf->GetBlockSize();
    printf("[Main] PSRDADA block size: %lu bytes (%lu MB)\n", 
           actual_block_size, actual_block_size / 1024 / 1024);
    fflush(stdout);
    if (actual_block_size % receive_bytes_per_time) {
        fprintf(stderr, "Warning: Actual block size (%lu) does not mattch with receive size (%lu)\n", 
                actual_block_size, receive_bytes_per_time);
    }
    param.DataSendBuff = &SendBuffPtr;
    param.GetBuffPtr = &GetBuffPtr;
    param.DecrementWriteCount = &DecrementWriteCount;
    param.IsBlockFull = &IsBlockFull;
    printf("[Main] Creating RDMA receiver...\n");
    printf("  Device: %d\n", param.device_id);
    printf("  GPU: %d\n", param.gpu_id);
    printf("  Packet Size: %d\n", param.pkt_size);
    printf("  Batch Size: %d\n", param.send_n);
    printf("  NSGE: %u\n", param.nsge);
    printf("  Source: %s:%s (%s)\n", param.SAddr, param.src_port, param.SMacAddr);
    printf("  Destination: %s:%s (%s)\n", param.DAddr, param.dst_port, param.DMacAddr);
    printf("[Main] Calling: new RoCEv2Dada(param)...\n");
    fflush(stdout);
    RoCEv2Dada *rdma_dada = new RoCEv2Dada(param);
    printf("[Main] RoCEv2Dada object created successfully\n");
    fflush(stdout);
    if (!rdma_dada) { fprintf(stderr, "Error: Failed to create RoCEv2Dada\n"); delete g_ringbuf; return -1; }
    printf("[Main] Getting IB resources...\n");
    fflush(stdout);
    void *ibv_res_void = rdma_dada->GetIbvRes();
    if (ibv_res_void) {
        struct ibv_utils_res *ibv_res_ptr = (struct ibv_utils_res *)ibv_res_void;
        if (ibv_res_ptr->pd) {
            printf("[Main] Attempting to register whole ring buffer...\n");
            fflush(stdout);
            struct ibv_mr *ring_mr = g_ringbuf->RegisterWholeRing(ibv_res_ptr->pd,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
            printf("[Main] RegisterWholeRing returned: %p\n", (void*)ring_mr);
            fflush(stdout);
            if (ring_mr) {
                // 连续内存模式：整个ring注册为单一MR，启用DirectToRing优化
                printf("[Demo] Registered ring MR: addr=%p rkey=0x%x\n", (void*)ring_mr->addr, ring_mr->rkey);
                rdma_dada->SetDirectMr(ring_mr);
                printf("[Demo] DirectToRing mode enabled (zero-copy RDMA writes)\n");
            } else {
                // 分块注册模式：每个block有自己的MR
                // DirectToRing模式不支持多MR场景，使用普通接收路径(内部buffer + memcpy)
                printf("[Demo] Using per-block MR registration mode\n");
                printf("[Demo] RDMA will use normal receive path (not DirectToRing)\n");
                printf("[Demo] Note: For best performance, use 'dada_db --contig' to enable single-MR mode\n");
                // 不调用SetDirectMr，让RDMA使用默认的接收路径
            }
        } else {
            fprintf(stderr, "[Demo] Warning: ibv_res_ptr->pd is NULL\n");
        }
    } else {
        fprintf(stderr, "[Demo] Warning: zjlab_rdma ibv_res not available\n");
    }
    if (mkdir(dump_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[Demo] Warning: failed to create dump dir %s\n", dump_dir);
    }
    
    // Note: dada_dbdisk is started externally by run_demo.sh
    // Do NOT start it here to avoid conflicts
    
    printf("[Main] Starting RDMA receiver thread...\n");
    fflush(stdout);
    ret = rdma_dada->Start();
    if (ret != 0) { fprintf(stderr, "Error: rdma_dada->Start failed: %d\n", ret); delete rdma_dada; delete g_ringbuf; return -1; }
    printf("\n========================================\n");
    printf("RDMA receiver running\n");
    printf("Listening on: %s:%s (%s)\n", param.DAddr, param.dst_port, param.DMacAddr);
    printf("Expecting packets from: %s:%s (%s)\n", param.SAddr, param.src_port, param.SMacAddr);
    printf("\n");
    printf("⚠️  WAITING FOR DATA PACKETS\n");
    printf("   Make sure the sender is running and sending to:\n");
    printf("   Destination: %s:%s\n", param.DAddr, param.dst_port);
    printf("\n");
    printf("Press Ctrl+C to exit gracefully\n");
    printf("========================================\n\n");
    while (!g_thread_exit) sleep(1);
    
    printf("\n[Main] Shutting down...\n");
    
    // Step 1: Stop RDMA receiver (stop new data coming in)
    printf("[Main] Stopping RDMA receiver...\n");
    delete rdma_dada;
    printf("[Main] ✓ RDMA receiver stopped\n");
    
    // Step 2: Send EOD signal and disconnect from ring buffer
    // Do NOT destroy the ring buffer - let run_demo.sh cleanup handle it
    if (g_ringbuf) {
        printf("[Main] Sending EOD signal and disconnecting from ring buffer...\n");
        if (g_ringbuf->SendEODAndDisconnect() == 0) {
            printf("[Main] ✓ EOD sent, disconnected from ring\n");
        }
        // Safe to delete now - SendEODAndDisconnect sets is_initialized=0
        // so destructor's Cleanup() will return immediately
        delete g_ringbuf;
        g_ringbuf = NULL;
    }
    
    // Give readers (dada_dbdisk) time to detect EOD and finish gracefully
    printf("[Main] Waiting for readers to detect EOD and finish...\n");
    sleep(2);
    
    printf("[Main] ✓ Writer shutdown complete. Ring buffer will be cleaned by script.\n");
    return 0;
}
