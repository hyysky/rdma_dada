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
#include <limits>
#include <climits>

#include <infiniband/verbs.h>

#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/io/psrdada/ring_writer.h"
#include "rdma_dada/io/rdma/receive_policy.h"
#include "rdma_dada/io/rdma/receiver.h"

PsrdadaRingBuf *g_ringbuf = NULL;
volatile sig_atomic_t g_thread_exit = 0;
static bool g_debug_mode = false;  // Debug mode flag
static uint32_t g_pkt_size = PKT_DATA_SIZE;
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
        fflush(stdout);
    }
    return ptr;
}

int SendBuffPtr(std::uint64_t valid_bytes) {
    if (!g_ringbuf) {
        fprintf(stderr, "[ERROR] g_ringbuf is NULL!\n");
        return -1;
    }
    
    if (g_block_size == 0) {
        fprintf(stderr, "[ERROR] g_block_size is 0!\n");
        return -1;
    }
    
    const rdma_dada::io::rdma::RawBlockTail tail =
        rdma_dada::io::rdma::ClassifyRawBlockTail(
            g_block_size, g_pkt_size, valid_bytes);
    if (tail.disposition !=
        rdma_dada::io::rdma::RawBlockTailDisposition::kPublish) {
        fprintf(stderr, "[ERROR] Invalid raw block publication: %" PRIu64
                        " bytes\n", valid_bytes);
        return -1;
    }
    if (g_debug_mode) {
        printf("[SendBuffPtr] Marking block as written: %" PRIu64
               " bytes (%" PRIu64 " records)\n",
               valid_bytes, tail.valid_records);
        fflush(stdout);
    }
    
    if (g_ringbuf->MarkWritten(valid_bytes) < 0) {
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
        fflush(stdout);
    }
    return 0;
}

void print_helper() {
    printf("Usage:\n");
    printf("    ./rdma2dada [options]\n");
    printf("Options:\n");
    printf("    --plan, compiler-generated resolved_observation.json (required)\n");
    printf("    --send_n, batch size (default: 64)\n");
    printf("    --recv-wr-num, receive WR depth (default: send_n * 4)\n");
    printf("    --poll-batch, maximum CQ completions per poll (default: 8)\n");
    printf("    --nsge, scatter/gather entries per work request (default: 4)\n");
    printf("    --cpu, CPU ID for thread affinity (default: -1)\n");
    printf("    --debug, enable debug mode with verbose logging\n");
    printf("    --help, -h\n");
    printf("    --preflight-only, validate plan/device and exit before ring access\n");
}

static int parse_args(RoCEv2Dada::RdmaParam &param,
                      char *plan_path, size_t plan_path_len,
                      bool *preflight_only,
                      int argc, char *argv[]) {
    int c;
    struct option long_options[] = {
        {"send_n", required_argument, NULL, 265},
        {"recv-wr-num", required_argument, NULL, 275},
        {"poll-batch", required_argument, NULL, 276},
        {"debug", no_argument, NULL, 271},
        {"nsge", required_argument, NULL, 272},
        {"plan", required_argument, NULL, 273},
        {"preflight-only", no_argument, NULL, 274},
        {"cpu", required_argument, NULL, 'c'},
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
    param.recv_wr_num = 0;
    param.poll_batch = 8;
    param.nsge = 4;
    while (1) {
        c = getopt_long(argc, argv, "c:h", long_options, NULL);
        switch (c) {
            case 265: param.send_n = atoi(optarg); break;
            case 275: param.recv_wr_num = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 276: param.poll_batch = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 271: g_debug_mode = true; break;
            case 272: param.nsge = (unsigned int)strtoul(optarg, NULL, 10); break;
            case 273: strncpy(plan_path, optarg, plan_path_len - 1); plan_path[plan_path_len - 1] = '\0'; break;
            case 274: *preflight_only = true; break;
            case 'c': param.bind_cpu_id = atoi(optarg); break;
            case 'h': print_helper(); return -1;
            case -1: return optind == argc ? 0 : -1;
            default: print_helper(); return -1;
        }
    }
    return 0;
}

static bool resolve_device_index(const std::string& name,
                                 unsigned char *device_id,
                                 std::string *error) {
    int count = 0;
    ibv_device **devices = ibv_get_device_list(&count);
    if (!devices) {
        *error = "cannot enumerate ibverbs devices";
        return false;
    }
    bool found = false;
    for (int index = 0; index < count; ++index) {
        const char *candidate = ibv_get_device_name(devices[index]);
        if (candidate && name == candidate && index <= UCHAR_MAX) {
            *device_id = static_cast<unsigned char>(index);
            found = true;
            break;
        }
    }
    ibv_free_device_list(devices);
    if (!found) *error = "configured ibverbs device was not found: " + name;
    return found;
}

int main(int argc, char *argv[]) {
    int ret = 0;
    key_t psrdada_key = 0;
    char plan_path[1024] = "";
    bool preflight_only = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    RoCEv2Dada::RdmaParam param = {};
    ret = parse_args(param, plan_path, sizeof(plan_path), &preflight_only,
                     argc, argv);
    if (ret < 0) return -1;
    if (plan_path[0] == '\0') {
        fprintf(stderr, "Error: --plan is required\n");
        return -1;
    }

    rdma_dada::ResolvedObservationPlan resolved_plan;
    rdma_dada::ObservationArtifacts artifacts;
    rdma_dada::PipelineConfig pipeline_config;
    rdma_dada::PipelineLayout pipeline_layout;
    std::string config_error;
    if (!rdma_dada::LoadResolvedObservationPlan(
            plan_path, &resolved_plan, &config_error) ||
        !rdma_dada::BuildPipelineRuntimeFromResolvedPlan(
            resolved_plan, &pipeline_config, &pipeline_layout,
            &config_error) ||
        !rdma_dada::BuildObservationArtifactsFromResolvedPlan(
            resolved_plan, &artifacts, &config_error)) {
        fprintf(stderr, "Error: Invalid resolved plan %s: %s\n",
                plan_path, config_error.c_str());
        return -1;
    }
    psrdada_key = static_cast<key_t>(resolved_plan.source.raw_key);
    snprintf(param.DMacAddr, sizeof(param.DMacAddr), "%s",
             resolved_plan.source.destination_mac.c_str());
    snprintf(param.DAddr, sizeof(param.DAddr), "%s",
             resolved_plan.source.destination_ip.c_str());
    snprintf(param.dst_port, sizeof(param.dst_port), "%u",
             resolved_plan.source.destination_port);
    if (!resolve_device_index(resolved_plan.source.receiver_device,
                              &param.device_id, &config_error)) {
        fprintf(stderr, "Error: %s\n", config_error.c_str());
        return -1;
    }
    if (resolved_plan.source.cuda_device < 0 ||
        resolved_plan.source.cuda_device > UCHAR_MAX) {
        fprintf(stderr, "Error: CUDA device index exceeds runtime range\n");
        return -1;
    }
    param.gpu_id = static_cast<unsigned char>(
        resolved_plan.source.cuda_device);
    if (pipeline_layout.raw_record_bytes > UINT32_MAX) {
        fprintf(stderr, "Error: Raw record size exceeds RDMA uint32 limit\n");
        return -1;
    }
    if (param.send_n == 0 || param.poll_batch == 0 ||
        pipeline_config.records_per_block % param.send_n != 0) {
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

    if (preflight_only) {
        printf("PLAN %s\nCONFIG_ID %s\nGEOMETRY_ID %s\n"
               "RAW_KEY 0x%x\nRAW_BLOCK_BYTES %lu\nDEVICE %s\n",
               plan_path, resolved_plan.config_id.c_str(),
               resolved_plan.geometry_id.c_str(), resolved_plan.source.raw_key,
               (unsigned long)resolved_plan.raw_block_bytes,
               resolved_plan.source.receiver_device.c_str());
        return 0;
    }
    g_ringbuf = new PsrdadaRingBuf();
    if (!g_ringbuf) { fprintf(stderr, "Error: Failed to create PsrdadaRingBuf\n"); return -1; }
    
    if (g_debug_mode) {
        printf("[Debug] Mode: ENABLED\n");
    }
    
    uint64_t receive_bytes_per_time = (uint64_t)param.pkt_size * param.send_n;
    printf("  Resolved plan: %s\n", plan_path);
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
                          pipeline_config.raw_ring_blocks,
                          pipeline_layout.raw_record_bytes,
                          artifacts.raw_header);
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
    printf("[Main] Creating RDMA receiver...\n");
    printf("  Device: %d\n", param.device_id);
    printf("  GPU: %d\n", param.gpu_id);
    printf("  Packet Size: %d\n", param.pkt_size);
    printf("  Batch Size: %d\n", param.send_n);
    printf("  Receive WR Depth: %u\n", param.recv_wr_num);
    printf("  CQ Poll Batch: %u\n", param.poll_batch);
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
