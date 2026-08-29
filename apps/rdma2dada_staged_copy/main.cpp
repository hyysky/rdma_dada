// Receive-only placement ablation: full frame staging -> raw PSRDADA ring.
#include <getopt.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <climits>
#include <string>

#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/io/psrdada/ring_writer.h"
#include "rdma_dada/io/rdma/receive_policy.h"
#include "rdma_dada/io/rdma/staged_copy_receiver.h"

namespace {

PsrdadaRingBuf *g_ringbuf = NULL;
volatile sig_atomic_t g_thread_exit = 0;
std::uint64_t g_record_bytes = 0;

void SignalHandler(int) { g_thread_exit = 1; }

int AcquireRawBlock(StagedCopyReceiver::HostRawBlockLease *lease) {
    if (!g_ringbuf || !lease) return -1;
    WriteBlockLease ring_lease = {};
    if (g_ringbuf->AcquireHostWriteBlock(&ring_lease) != 0) return -1;
    lease->addr = reinterpret_cast<unsigned char *>(ring_lease.addr);
    lease->bytes = ring_lease.bytes;
    lease->token = ring_lease.token;
    return 0;
}

int CommitRawBlock(std::uint64_t token, std::uint64_t valid_bytes) {
    if (!g_ringbuf) return -1;
    const rdma_dada::io::rdma::RawBlockTail tail =
        rdma_dada::io::rdma::ClassifyRawBlockTail(
            g_ringbuf->GetBlockSize(), g_record_bytes, valid_bytes);
    if (tail.disposition !=
        rdma_dada::io::rdma::RawBlockTailDisposition::kPublish) {
        fprintf(stderr, "[ERROR] Invalid staged raw publication: %" PRIu64
                        " bytes\n", valid_bytes);
        return -1;
    }
    return g_ringbuf->CommitWriteBlock(token, valid_bytes);
}

std::uint64_t RawRingUsedBytes() {
    return g_ringbuf ? g_ringbuf->GetUsedSpace() : 0U;
}

void PrintHelp() {
    printf("Usage: rdma2dada_staged_copy --plan resolved_observation.json "
           "[options]\n"
           "  --recv-wr-num N   receive WR depth (default: 1024)\n"
           "  --poll-batch N    maximum CQ completions per poll (default: 32)\n"
           "  --poll-cpu CPU    receive/copy thread CPU (default: unbound)\n"
           "  --preflight-only  validate staged-copy geometry only\n"
           "  --help, -h\n");
}

int ParseArgs(StagedCopyReceiver::RdmaParam *param, char *plan_path,
              std::size_t plan_path_size, bool *preflight_only,
              int argc, char **argv) {
    const struct option options[] = {
        {"recv-wr-num", required_argument, NULL, 275},
        {"poll-batch", required_argument, NULL, 276},
        {"poll-cpu", required_argument, NULL, 277},
        {"plan", required_argument, NULL, 273},
        {"preflight-only", no_argument, NULL, 274},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };
    param->device_id = 0;
    param->pkt_size = 0;
    param->recv_wr_num = rdma_dada::io::rdma::kDefaultReceiveWrDepth;
    param->poll_batch = rdma_dada::io::rdma::kDefaultReceivePollBatch;
    param->poll_cpu_id = -1;
    while (true) {
        const int option = getopt_long(argc, argv, "h", options, NULL);
        switch (option) {
            case 275:
                param->recv_wr_num = static_cast<unsigned int>(
                    strtoul(optarg, NULL, 10));
                break;
            case 276:
                param->poll_batch = static_cast<unsigned int>(
                    strtoul(optarg, NULL, 10));
                break;
            case 277: param->poll_cpu_id = atoi(optarg); break;
            case 273:
                strncpy(plan_path, optarg, plan_path_size - 1U);
                plan_path[plan_path_size - 1U] = '\0';
                break;
            case 274: *preflight_only = true; break;
            case 'h': PrintHelp(); return -1;
            case -1:
                return optind == argc && param->poll_cpu_id >= -1 ? 0 : -1;
            default: return -1;
        }
    }
}

bool ResolveDeviceIndex(const std::string& name, unsigned char *device_id,
                        std::string *error) {
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
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

}  // namespace

int main(int argc, char **argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    StagedCopyReceiver::RdmaParam param = {};
    char plan_path[1024] = "";
    bool preflight_only = false;
    if (ParseArgs(&param, plan_path, sizeof(plan_path), &preflight_only,
                  argc, argv) != 0 || plan_path[0] == '\0') {
        fprintf(stderr, "Error: valid --plan is required\n");
        return -1;
    }

    rdma_dada::ResolvedObservationPlan resolved_plan;
    rdma_dada::ObservationArtifacts artifacts;
    rdma_dada::PipelineConfig pipeline_config;
    rdma_dada::PipelineLayout pipeline_layout;
    std::string error;
    if (!rdma_dada::LoadResolvedObservationPlan(
            plan_path, &resolved_plan, &error) ||
        !rdma_dada::BuildPipelineRuntimeFromResolvedPlan(
            resolved_plan, &pipeline_config, &pipeline_layout, &error) ||
        !rdma_dada::BuildObservationArtifactsFromResolvedPlan(
            resolved_plan, &artifacts, &error)) {
        fprintf(stderr, "Error: Invalid resolved plan %s: %s\n",
                plan_path, error.c_str());
        return -1;
    }
    if (pipeline_layout.raw_record_bytes == 0U ||
        pipeline_layout.raw_record_bytes > UINT32_MAX ||
        pipeline_layout.raw_block_bytes % pipeline_layout.raw_record_bytes !=
            0U ||
        !rdma_dada::io::rdma::ValidateDirectRawConfiguration(
            param.recv_wr_num,
            pipeline_layout.raw_block_bytes /
                pipeline_layout.raw_record_bytes,
            pipeline_config.raw_ring_blocks) ||
        param.poll_batch == 0U || param.poll_batch > param.recv_wr_num) {
        fprintf(stderr, "Error: invalid staged-copy plan/queue geometry\n");
        return -1;
    }
    if (!ResolveDeviceIndex(resolved_plan.source.receiver_device,
                            &param.device_id, &error)) {
        fprintf(stderr, "Error: %s\n", error.c_str());
        return -1;
    }
    param.pkt_size = static_cast<unsigned int>(
        pipeline_layout.raw_record_bytes);
    snprintf(param.DMacAddr, sizeof(param.DMacAddr), "%s",
             resolved_plan.source.destination_mac.c_str());
    snprintf(param.DAddr, sizeof(param.DAddr), "%s",
             resolved_plan.source.destination_ip.c_str());
    snprintf(param.dst_port, sizeof(param.dst_port), "%u",
             resolved_plan.source.destination_port);
    g_record_bytes = pipeline_layout.raw_record_bytes;

    if (preflight_only) {
        printf("PLAN %s\nCONFIG_ID %s\nGEOMETRY_ID %s\n"
               "RAW_KEY 0x%x\nRAW_BLOCK_BYTES %" PRIu64 "\n"
               "RAW_RING_BLOCKS %" PRIu64 "\nDEVICE %s\n"
               "RECEIVER_PLACEMENT_MODE STAGED_COPY\nDIRECT_RAW 0\n"
               "RECEIVER_FLOWS 1\nRECEIVER_QPS 1\nRECEIVER_CQS 1\n"
               "RECEIVER_THREADS 1\nNSGE 1\nHEADER_STRIP_BYTES 42\n"
               "RECV_WR_NUM %u\nPOLL_BATCH %u\n",
               plan_path, resolved_plan.config_id.c_str(),
               resolved_plan.geometry_id.c_str(),
               resolved_plan.source.raw_key,
               pipeline_layout.raw_block_bytes,
               pipeline_config.raw_ring_blocks,
               resolved_plan.source.receiver_device.c_str(),
               param.recv_wr_num, param.poll_batch);
        return 0;
    }

    g_ringbuf = new PsrdadaRingBuf();
    if (!g_ringbuf ||
        g_ringbuf->Init(static_cast<key_t>(resolved_plan.source.raw_key),
                        pipeline_layout.raw_block_bytes,
                        pipeline_config.raw_ring_blocks,
                        pipeline_layout.raw_record_bytes,
                        artifacts.raw_header) != 0) {
        fprintf(stderr, "Error: failed to initialize PSRDADA raw ring\n");
        delete g_ringbuf;
        g_ringbuf = NULL;
        return -1;
    }
    param.AcquireRawBlockPtr = &AcquireRawBlock;
    param.CommitRawBlockPtr = &CommitRawBlock;
    param.RawRingUsedBytesPtr = &RawRingUsedBytes;

    StagedCopyReceiver *receiver = new StagedCopyReceiver(param);
    if (!receiver || receiver->Start() != 0) {
        fprintf(stderr, "Error: failed to start staged-copy receiver\n");
        delete receiver;
        g_ringbuf->SendEODAndDisconnect();
        delete g_ringbuf;
        g_ringbuf = NULL;
        return -1;
    }
    printf("RDMA receiver running\nListening on: %s:%s (%s)\n",
           param.DAddr, param.dst_port, param.DMacAddr);
    fflush(stdout);
    while (!g_thread_exit) sleep(1);

    const int stop_result = receiver->Stop();
    delete receiver;
    const int eod_result = g_ringbuf->SendEODAndDisconnect();
    delete g_ringbuf;
    g_ringbuf = NULL;
    if (stop_result != 0 || eod_result != 0) return -1;
    printf("[Main] Writer shutdown complete\n");
    return 0;
}
