// rdma2dada composition root: RoCE receiver -> raw PSRDADA ring.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <string>

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/io/psrdada/ring_writer.h"
#include "rdma_dada/io/rdma/receiver.h"
#include "rdma_dada/pipeline/dada_header_builder.h"

#define PSRDADA_BUFFER_KEY 0xdada

PsrdadaRingBuf *g_ringbuf = NULL;
volatile sig_atomic_t g_thread_exit = 0;
static bool g_debug_mode = false;  // Debug mode flag
static uint32_t g_pkt_size = PKT_DATA_SIZE;
static uint32_t g_send_n = 64;
static uint64_t g_current_block_remaining_writes = 0;  // 当前block剩余可写入次数
static uint64_t g_bytes_per_write = 0;  // 每次写入的字节数
static uint64_t g_block_size = 0;  // 完整block的大小（固定值）

void signal_handler(int sig) {
    (void)sig;
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
            printf("[GetBuffPtr] Initialized g_block_size = %" PRIu64 " bytes (%.2f MB)\n",
                   g_block_size, g_block_size / 1024.0 / 1024.0);
        }
    }
    
    // g_pkt_size is payload bytes written into ring buffer per packet.
    g_bytes_per_write = (uint64_t)g_pkt_size * g_send_n;
    
    // 计算这个block可以接收多少次：N = block_size / (pkt_size * send_n)
    g_current_block_remaining_writes = g_block_size / g_bytes_per_write;
    uint64_t remainder = g_block_size % g_bytes_per_write;
    
    if (g_debug_mode) {
        printf("[GetBuffPtr] Block calculation: block_size=%" PRIu64
               ", bytes_per_write=%" PRIu64 "\n",
               g_block_size, g_bytes_per_write);
        printf("[GetBuffPtr] This block can receive %" PRIu64 " times",
               g_current_block_remaining_writes);
        if (remainder > 0) {
            printf(" (with %" PRIu64 " bytes remainder)\n", remainder);
            fprintf(stderr, "[GetBuffPtr] WARNING: block_size is not exact multiple!\n");
        } else {
            printf(" (exact fit)\n");
        }
        fflush(stdout);
    } else if (remainder > 0) {
        fprintf(stderr, "[WARN] Block size not exact multiple, %" PRIu64
                        " bytes wasted per block\n", remainder);
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
        printf("[GetBuffPtr] ✓ Ready to receive %" PRIu64
               " times (%" PRIu64 " bytes each)\n",
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
                printf("[DecrementWriteCount] Remaining writes: %" PRIu64
                       " / %" PRIu64 "\n",
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
        printf("[SendBuffPtr] Marking block as written: %" PRIu64
               " bytes (%.2f MB)\n",
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
        printf("[Progress] Blocks written: %" PRIu64
               " | Ring buffer: %.1f%% full (%" PRIu64 "/%" PRIu64 " MB)\n",
               total_blocks, fill_percent, used / 1024 / 1024, total / 1024 / 1024);
    }
    return 0;
}

void print_helper() {
    printf("Usage:\n");
    printf("    ./rdma2dada [options]\n");
    printf("Options:\n");
    printf("    -d, NIC device number (default: 0)\n");
    printf("    --smac, deprecated source MAC option (ignored by receiver)\n");
    printf("    --dmac, destination MAC address (required)\n");
    printf("    --sip, deprecated source IP option (ignored by receiver)\n");
    printf("    --dip, destination IP address (required)\n");
    printf("    --sport, deprecated source port option (ignored by receiver)\n");
    printf("    --dport, destination port number (required)\n");
    printf("    --config, pipeline JSON config (default: config/pipeline.example.json)\n");
    printf("    --send_n, batch size (default: 64)\n");
    printf("    --nsge, scatter/gather entries per work request (default: 4)\n");
    printf("    --key, psrdada buffer key in hex (default: 0x%x)\n", PSRDADA_BUFFER_KEY);
    printf("    --gpu, GPU device ID (default: 0)\n");
    printf("    --cpu, CPU ID for thread affinity (default: -1)\n");
    printf("    --debug, enable debug mode with verbose logging\n");
    printf("    --help, -h\n");
    printf("    --dump-dir, directory for dada_dbdisk output (runs in background)\n");
    printf("    --dump-header, path to header template file (default: header/array_GZNU.header)\n");
}

static int parse_args(RoCEv2Dada::RdmaParam &param, key_t &psrdada_key,
                      char *dump_dir, size_t dump_dir_len,
                      char *header_path, size_t header_path_len,
                      char *config_path, size_t config_path_len,
                      int argc, char *argv[]) {
    int c;
    struct option long_options[] = {
        {"smac", required_argument, NULL, 256},
        {"dmac", required_argument, NULL, 257},
        {"sip", required_argument, NULL, 258},
        {"dip", required_argument, NULL, 259},
        {"sport", required_argument, NULL, 260},
        {"dport", required_argument, NULL, 261},
        {"send_n", required_argument, NULL, 265},
        {"key", required_argument, NULL, 266},
        {"dump-dir", required_argument, NULL, 267},
        {"dump-header", required_argument, NULL, 268},
        {"debug", no_argument, NULL, 271},
        {"nsge", required_argument, NULL, 272},
        {"config", required_argument, NULL, 273},
        {"gpu", required_argument, NULL, 'g'},
        {"cpu", required_argument, NULL, 'c'},
        {"device", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };
    param.bind_cpu_id = -1;
    param.gpu_id = 0;
    param.SendOrRecv = false;
    param.device_id = 0;
    param.pkt_size = 0;  // Loaded from the validated pipeline config.
    param.RdmaDirectGpu = 0;
    param.send_n = 64;
    param.nsge = 4;
    psrdada_key = PSRDADA_BUFFER_KEY;
    while (1) {
        c = getopt_long(argc, argv, "d:g:c:h", long_options, NULL);
        switch (c) {
            case 'd': param.device_id = atoi(optarg); break;
            case 256: snprintf(param.SMacAddr, sizeof(param.SMacAddr), "%s", optarg); break;
            case 257: snprintf(param.DMacAddr, sizeof(param.DMacAddr), "%s", optarg); break;
            case 258: snprintf(param.SAddr, sizeof(param.SAddr), "%s", optarg); break;
            case 259: snprintf(param.DAddr, sizeof(param.DAddr), "%s", optarg); break;
            case 260: snprintf(param.src_port, sizeof(param.src_port), "%s", optarg); break;
            case 261: snprintf(param.dst_port, sizeof(param.dst_port), "%s", optarg); break;
            case 265: param.send_n = atoi(optarg); break;
            case 266: sscanf(optarg, "%x", &psrdada_key); break;
            case 267: strncpy(dump_dir, optarg, dump_dir_len - 1); dump_dir[dump_dir_len - 1] = '\0'; break;
            case 268: strncpy(header_path, optarg, header_path_len - 1); header_path[header_path_len - 1] = '\0'; break;
            case 271: g_debug_mode = true; break;
            case 272: param.nsge = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 273: strncpy(config_path, optarg, config_path_len - 1); config_path[config_path_len - 1] = '\0'; break;
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
    char dump_dir[256] = "./data_out";
    char header_path[256] = "header/array_GZNU.header";
    char config_path[256] = "config/pipeline.example.json";
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    RoCEv2Dada::RdmaParam param = {};
    ret = parse_args(param, psrdada_key, dump_dir, sizeof(dump_dir),
                     header_path, sizeof(header_path), config_path,
                     sizeof(config_path), argc, argv);
    if (ret < 0) return -1;

    rdma_dada::PipelineConfig pipeline_config;
    rdma_dada::PipelineLayout pipeline_layout;
    dada_header_t raw_header;
    std::string config_error;
    if (!rdma_dada::LoadPipelineConfig(config_path, &pipeline_config,
                                       &config_error) ||
        !rdma_dada::ComputePipelineLayout(pipeline_config, &pipeline_layout,
                                          &config_error) ||
        !rdma_dada::BuildPipelineDadaHeader(
            pipeline_config, pipeline_layout, rdma_dada::DataStage::kRaw,
            &raw_header, &config_error)) {
        fprintf(stderr, "Error: Invalid pipeline config %s: %s\n",
                config_path, config_error.c_str());
        return -1;
    }
    if (pipeline_layout.raw_record_bytes > UINT32_MAX) {
        fprintf(stderr, "Error: Raw record size exceeds RDMA uint32 limit\n");
        return -1;
    }
    if (param.send_n == 0 || pipeline_config.records_per_block % param.send_n != 0) {
        fprintf(stderr,
                "Error: RECORDS_PER_BLOCK (%lu) must be an exact multiple of "
                "--send_n (%u)\n",
                (unsigned long)pipeline_config.records_per_block, param.send_n);
        return -1;
    }
    param.pkt_size = static_cast<uint32_t>(pipeline_layout.raw_record_bytes);
    param.debug_mode = g_debug_mode;  // Set debug mode in RDMA param
    if (param.nsge == 0) {
        fprintf(stderr, "[WARN] Invalid --nsge value 0, falling back to 4\n");
        param.nsge = 4;
    }
    g_pkt_size = param.pkt_size;
    g_send_n = param.send_n;

    if (strlen(param.DMacAddr) == 0 || strlen(param.DAddr) == 0 ||
        strlen(param.dst_port) == 0) {
        fprintf(stderr,
                "Error: Missing required destination network parameters\n");
        print_helper();
        return -1;
    }
    g_ringbuf = new PsrdadaRingBuf();
    if (!g_ringbuf) { fprintf(stderr, "Error: Failed to create PsrdadaRingBuf\n"); return -1; }
    
    if (g_debug_mode) {
        printf("[Debug] Mode: ENABLED\n");
    }
    
    uint64_t receive_bytes_per_time = (uint64_t)param.pkt_size * param.send_n;
    printf("  Pipeline config: %s\n", config_path);
    printf("  Raw record: %lu bytes (%lu-byte app header + %lu-byte payload)\n",
           (unsigned long)pipeline_layout.raw_record_bytes,
           (unsigned long)pipeline_config.packet_header_bytes,
           (unsigned long)pipeline_config.packet_payload_bytes);
    printf("  Receive raw data per batch: %" PRIu64 " bytes (%.2f MB)\n",
           receive_bytes_per_time, receive_bytes_per_time / 1024.0 / 1024.0);
    fflush(stdout);

    printf("\n[Main] Connecting to PSRDADA ring buffer (key=0x%x)...\n", psrdada_key);
    printf("  Expected ring: %lu blocks x %lu bytes\n",
           (unsigned long)pipeline_config.raw_ring_blocks,
           (unsigned long)pipeline_layout.raw_block_bytes);
    printf("  Output file size: %lu bytes\n",
           (unsigned long)pipeline_layout.raw_file_bytes);
    ret = g_ringbuf->Init(psrdada_key, pipeline_layout.raw_block_bytes,
                          pipeline_config.raw_ring_blocks, header_path,
                          raw_header);
    if (ret < 0) { 
        fprintf(stderr, "Error: Failed to initialize psrdada ring buffer\n"); delete g_ringbuf; return -1; 
    } else { 
        fprintf(stderr, "[Main] ✓ psrdada ring buffer initialized\n"); 
    }
    
    // 获取实际PSRDADA block大小（由dada_db创建时决定）
    uint64_t actual_block_size = g_ringbuf->GetBlockSize();
    printf("[Main] PSRDADA block size: %" PRIu64
           " bytes (%" PRIu64 " MB)\n",
           actual_block_size, actual_block_size / 1024 / 1024);
    fflush(stdout);
    // Exact divisibility was validated before connecting to the ring.
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
    printf("  Source filter: ANY MAC/IP/UDP port\n");
    printf("  Destination: %s:%s (%s)\n", param.DAddr, param.dst_port, param.DMacAddr);
    printf("[Main] Calling: new RoCEv2Dada(param)...\n");
    fflush(stdout);
    RoCEv2Dada *rdma_dada = new RoCEv2Dada(param);
    printf("[Main] RoCEv2Dada object created successfully\n");
    fflush(stdout);
    if (!rdma_dada) { fprintf(stderr, "Error: Failed to create RoCEv2Dada\n"); delete g_ringbuf; return -1; }
    printf("[Main] RDMA uses registered receive buffers and copies records into the ring\n");
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
    printf("Accepting packets from: ANY source MAC/IP/UDP port\n");
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
    if (rdma_dada->Stop() != 0) {
        fprintf(stderr, "[Main] Warning: failed to join RDMA receiver thread\n");
    }
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
