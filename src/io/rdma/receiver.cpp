//定义 RoCEv2Dada 类的成员函数，用于通过 RoCEv2 协议进行数据发送和接收
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <stdio.h>

#include <vector>
#include <algorithm>

#include "rdma_dada/io/rdma/receiver.h"
#include "rdma_dada/io/rdma/receive_policy.h"
#include "rdma_dada/io/rdma/verbs_context.h"
#include "rdma_dada/io/rdma/packet_builder.h"

#ifndef NO_CUDA
#include <cuda_runtime.h>
#endif

#define RDMA_OK 0
#define RDMA_ERROR -1
#define RDMA_NULL_POINTER -2

#ifndef NO_CUDA
#define CUDA_CALL(x) do { cudaError_t err = (x); if (err != cudaSuccess) { fprintf(stderr, "CUDA error at %s:%d - %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); } } while(0)
#else
#define CUDA_CALL(x) do {} while(0)
#endif

#define ELAPSED_US(start,stop) (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000+(stop.tv_nsec-start.tv_nsec)/1000)
#define MEASURE_BANDWIDTH(size, t) ((double)size * 8.0 / t / 1000)
#define RX_STRIP_HEADER_BYTES 42

static bool validate_send_completion(
    const struct ibv_wc &wc,
    const struct ibv_utils_res *ibv_res_ptr)
{
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr,
                "[ERROR] CQ completion failed: wr_id=%" PRIu64
                ", status=%s (%d), vendor_err=%u\n",
                wc.wr_id, ibv_wc_status_str(wc.status), wc.status,
                wc.vendor_err);
        return false;
    }

    const uint64_t wr_limit = (uint64_t)ibv_res_ptr->send_wr_num;
    if (wc.wr_id >= wr_limit) {
        fprintf(stderr,
                "[ERROR] CQ completion has invalid wr_id=%" PRIu64
                " (limit=%" PRIu64 ").\n",
                wc.wr_id, wr_limit);
        return false;
    }

    if (wc.opcode != IBV_WC_SEND) {
        fprintf(stderr,
                "[ERROR] Send CQ returned unexpected opcode=%d for "
                "wr_id=%" PRIu64 ".\n",
                wc.opcode, wc.wr_id);
        return false;
    }

    return true;
}

static void log_fatal_receive_completion(
    const struct ibv_wc &wc,
    const struct ibv_utils_res *ibv_res_ptr)
{
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr,
                "[ERROR] Receive CQ completion failed: wr_id=%" PRIu64
                ", status=%s (%d), vendor_err=%u\n",
                wc.wr_id, ibv_wc_status_str(wc.status), wc.status,
                wc.vendor_err);
    } else if (wc.wr_id >= (uint64_t)ibv_res_ptr->recv_wr_num) {
        fprintf(stderr,
                "[ERROR] Receive CQ completion has invalid wr_id=%" PRIu64
                " (limit=%d).\n",
                wc.wr_id, ibv_res_ptr->recv_wr_num);
    } else {
        fprintf(stderr,
                "[ERROR] Receive CQ returned unexpected opcode=%d for "
                "wr_id=%" PRIu64 ".\n",
                wc.opcode, wc.wr_id);
    }
}

static int post_receive_batch(
    struct ibv_utils_res *ibv_res_ptr,
    const rdma_dada::io::rdma::ReceiveWorkItem *items,
    size_t count)
{
    if (count == 0) return 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t wr_id = items[i].wr_id;
        if (wr_id >= static_cast<uint64_t>(ibv_res_ptr->recv_wr_num)) {
            fprintf(stderr, "[ERROR] Invalid receive WR id=%" PRIu64 ".\n",
                    wr_id);
            return -1;
        }
        struct ibv_recv_wr *wr = &ibv_res_ptr->recv_wr[wr_id];
        memset(wr, 0, sizeof(*wr));
        wr->wr_id = wr_id;
        wr->sg_list = &ibv_res_ptr->sge[wr_id * ibv_res_ptr->recv_nsge];
        wr->num_sge = ibv_res_ptr->recv_nsge;
        wr->next = i + 1 < count
            ? &ibv_res_ptr->recv_wr[items[i + 1].wr_id]
            : NULL;
    }
    struct ibv_recv_wr *first =
        &ibv_res_ptr->recv_wr[items[0].wr_id];
    const int ret = ibv_post_recv(ibv_res_ptr->qp,
                                  first,
                                  &ibv_res_ptr->bad_recv_wr);
    if (ret != 0) {
        fprintf(stderr,
                "[ERROR] Failed to post receive WR batch: count=%zu, "
                "ret=%d, errno=%d (%s)\n",
                count, ret, errno, strerror(errno));
    }
    return ret;
}

struct ReceiveShardState {
    ReceiveShardState(RoCEv2Dada *owner_value,
                      struct ibv_utils_res *resource_value,
                      std::size_t wr_depth, std::size_t index_value)
        : owner(owner_value), resource(resource_value), index(index_value),
          completed(wr_depth), recycle(wr_depth), poll_done(false),
          poll_thread_started(false), poll_calls(0), empty_polls(0),
          completions(0), full_polls(0), reposted(0), repost_failures(0),
          posted_wrs(wr_depth), min_posted_wrs(wr_depth) {}

    RoCEv2Dada *owner;
    struct ibv_utils_res *resource;
    std::size_t index;
    rdma_dada::io::rdma::ReceiveSpscQueue completed;
    rdma_dada::io::rdma::ReceiveSpscQueue recycle;
    pthread_t poll_tid;
    std::atomic<bool> poll_done;
    bool poll_thread_started;
    std::atomic<std::uint64_t> poll_calls;
    std::atomic<std::uint64_t> empty_polls;
    std::atomic<std::uint64_t> completions;
    std::atomic<std::uint64_t> full_polls;
    std::atomic<std::uint64_t> reposted;
    std::atomic<std::uint64_t> repost_failures;
    std::atomic<std::uint64_t> posted_wrs;
    std::atomic<std::uint64_t> min_posted_wrs;
};

struct RoCEv2Dada::ReceivePipelineState {
    ReceivePipelineState()
        : copy_done(false), failed(false), copy_thread_started(false),
          copy_batches(0), completion_to_recycle_ns_total(0),
          completion_to_recycle_ns_max(0) {}

    std::vector<ReceiveShardState *> shards;
    pthread_t copy_tid;
    std::atomic<bool> copy_done;
    std::atomic<bool> failed;
    bool copy_thread_started;
    std::atomic<std::uint64_t> copy_batches;
    std::atomic<std::uint64_t> completion_to_recycle_ns_total;
    std::atomic<std::uint64_t> completion_to_recycle_ns_max;
};

static void update_atomic_min(std::atomic<std::uint64_t> *target,
                              std::uint64_t value) {
    std::uint64_t observed = target->load(std::memory_order_relaxed);
    while (value < observed &&
           !target->compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {}
}

static void update_atomic_max(std::atomic<std::uint64_t> *target,
                              std::uint64_t value) {
    std::uint64_t observed = target->load(std::memory_order_relaxed);
    while (value > observed &&
           !target->compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {}
}

static int check_send_recv_info(struct ibv_utils_res * ibv_res_ptr, RoCEv2Dada::RdmaParam * Param_ptr)
{
    printf("**********************************************\n");
    printf("Recv Config Information:\n");
    printf("    device_id: %d\n", Param_ptr->device_id);
    if (Param_ptr->SendOrRecv) {
        printf("    src_mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    ibv_res_ptr->pkt_info.src_mac[0], ibv_res_ptr->pkt_info.src_mac[1],
                    ibv_res_ptr->pkt_info.src_mac[2], ibv_res_ptr->pkt_info.src_mac[3],
                    ibv_res_ptr->pkt_info.src_mac[4], ibv_res_ptr->pkt_info.src_mac[5]);
    } else {
        printf("    source_filter: ANY\n");
    }
    printf("    dst_mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                ibv_res_ptr->pkt_info.dst_mac[0], ibv_res_ptr->pkt_info.dst_mac[1],
                ibv_res_ptr->pkt_info.dst_mac[2], ibv_res_ptr->pkt_info.dst_mac[3],
                ibv_res_ptr->pkt_info.dst_mac[4], ibv_res_ptr->pkt_info.dst_mac[5]);
    uint8_t tmp[4];
    if (Param_ptr->SendOrRecv) {
        tmp[3] = (ibv_res_ptr->pkt_info.src_ip >> 24) & 0xff;
        tmp[2] = (ibv_res_ptr->pkt_info.src_ip >> 16) & 0xff;
        tmp[1] = (ibv_res_ptr->pkt_info.src_ip >> 8) & 0xff;
        tmp[0] = ibv_res_ptr->pkt_info.src_ip & 0xff;
        printf("    src_ip: %d.%d.%d.%d\n", tmp[0], tmp[1], tmp[2], tmp[3]);
    }
    tmp[3] = (ibv_res_ptr->pkt_info.dst_ip >> 24) & 0xff;
    tmp[2] = (ibv_res_ptr->pkt_info.dst_ip >> 16) & 0xff;
    tmp[1] = (ibv_res_ptr->pkt_info.dst_ip >> 8) & 0xff;
    tmp[0] = ibv_res_ptr->pkt_info.dst_ip & 0xff;
    printf("    dst_ip: %d.%d.%d.%d\n", tmp[0], tmp[1], tmp[2], tmp[3]);
    if (Param_ptr->SendOrRecv) {
        printf("    src_port: %d\n", ibv_res_ptr->pkt_info.src_port);
    }
    printf("    dst_port: %d\n", ibv_res_ptr->pkt_info.dst_port);
    printf("    RdmaDirectGpu: %d, gpu_id: %d\n", Param_ptr->RdmaDirectGpu, Param_ptr->gpu_id);
    printf("    recv_wr_num: %d send_wr_num: %d send_nsge: %d recv_nsge: %d \n",
        ibv_res_ptr->recv_wr_num , ibv_res_ptr->send_wr_num,
        ibv_res_ptr->send_nsge, ibv_res_ptr->recv_nsge);
    printf("    pkt_size: %d send_n: %d\n", Param_ptr->pkt_size, Param_ptr->send_n);
    printf("**********************************************\n");
    
    if (Param_ptr->pkt_size <= PKT_HEAD_LEN || Param_ptr->send_n == 0 ||
        Param_ptr->poll_cpu_id < -1 || Param_ptr->copy_cpu_id < -1 ||
        (Param_ptr->poll_cpu_id >= 0 &&
         Param_ptr->poll_cpu_id == Param_ptr->copy_cpu_id)) {
        fprintf(stderr,
                "[ERROR] Invalid RDMA geometry: pkt_size=%u, send_n=%u, "
                "poll_cpu_id=%d, copy_cpu_id=%d.\n",
                Param_ptr->pkt_size, Param_ptr->send_n,
                Param_ptr->poll_cpu_id, Param_ptr->copy_cpu_id);
        return -1;
    }
    return 0;
}

static int ib_send_pkg(struct ibv_utils_res * ibv_res, int send_idx, int send_num)
{
    int i = 0, state = 0;
    memset(ibv_res->send_wr, 0, sizeof(struct ibv_send_wr));
    for(int i = 0; i < send_num; i++) {
        ibv_res->send_wr[i].wr_id = i;
        ibv_res->send_wr[i].sg_list = &ibv_res->sge[i*ibv_res->send_nsge + send_idx];
        ibv_res->send_wr[i].num_sge = ibv_res->send_nsge;
        ibv_res->send_wr[i].next = (i == send_num - 1) ? NULL : &ibv_res->send_wr[i+1];
        ibv_res->send_wr[i].opcode = IBV_WR_SEND;
        ibv_res->send_wr[i].send_flags |= IBV_SEND_SIGNALED;
    }
    state = ibv_post_send(ibv_res->qp, ibv_res->send_wr, &ibv_res->bad_send_wr);
    if(state < 0) { ibv_utils_error("Failed to post send."); return -1; }
    return 0;
}

RoCEv2Dada::RoCEv2Dada(const RdmaParam & Param)
    : param(Param), ibv_res(NULL), receive_pipeline(NULL),
      stop_requested(false),
      accepted_receive_packets(0), wrong_length_receive_packets(0),
      published_receive_packets(0), published_receive_blocks(0),
      partial_receive_blocks(0), cq_tail_receive_records(0),
      thread_started(false)
{
    printf("[RoCEv2Dada] Constructor started\n");
    fflush(stdout);
    uint8_t tmp[4];
    int ret = 0;
    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)malloc(sizeof(struct ibv_utils_res));
    if (!ibv_res_ptr) {
        fprintf(stderr, "[ERROR] Failed to allocate RDMA resource state.\n");
        return;
    }
    this->ibv_res = (void *)ibv_res_ptr;
    memset(ibv_res_ptr, 0, sizeof(struct ibv_utils_res));
    
    printf("[RoCEv2Dada] Parsing network parameters...\n");

    if (this->param.pkt_size <= PKT_HEAD_LEN || this->param.send_n == 0 ||
        (this->param.SendOrRecv && this->param.send_n > INT_MAX / 4)) {
        fprintf(stderr,
                "[ERROR] Invalid packet/batch geometry: pkt_size=%u, "
                "send_n=%u.\n",
                this->param.pkt_size, this->param.send_n);
        return;
    }
    if (!this->param.SendOrRecv) {
        if (this->param.receive_shards == 0) this->param.receive_shards = 1;
        if (this->param.poll_cpu_ids.empty() &&
            this->param.poll_cpu_id >= 0) {
            this->param.poll_cpu_ids.push_back(this->param.poll_cpu_id);
        }
        if (((this->param.receive_shards > 1 ||
              !this->param.receive_flows.empty()) &&
             this->param.receive_flows.size() !=
                 this->param.receive_shards) ||
            (!this->param.poll_cpu_ids.empty() &&
             this->param.poll_cpu_ids.size() !=
                 this->param.receive_shards)) {
            fprintf(stderr,
                    "[ERROR] Receive flow/CPU counts must match "
                    "receive_shards=%u.\n", this->param.receive_shards);
            return;
        }
    }
    
    if (this->param.SendOrRecv) {
        sscanf(this->param.SAddr, "%hhd.%hhd.%hhd.%hhd",
               &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
        ibv_res_ptr->pkt_info.src_ip =
            (tmp[3] << 24) | (tmp[2] << 16) | (tmp[1] << 8) | tmp[0];
        sscanf(this->param.SMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &ibv_res_ptr->pkt_info.src_mac[0],
               &ibv_res_ptr->pkt_info.src_mac[1],
               &ibv_res_ptr->pkt_info.src_mac[2],
               &ibv_res_ptr->pkt_info.src_mac[3],
               &ibv_res_ptr->pkt_info.src_mac[4],
               &ibv_res_ptr->pkt_info.src_mac[5]);
        sscanf(this->param.src_port, "%hd", &ibv_res_ptr->pkt_info.src_port);
    }
    sscanf(this->param.DAddr, "%hhd.%hhd.%hhd.%hhd", &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
    ibv_res_ptr->pkt_info.dst_ip = (tmp[3] << 24) | (tmp[2] << 16) | (tmp[1] << 8) | tmp[0];
    sscanf(this->param.DMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                               &ibv_res_ptr->pkt_info.dst_mac[0], &ibv_res_ptr->pkt_info.dst_mac[1],
                               &ibv_res_ptr->pkt_info.dst_mac[2], &ibv_res_ptr->pkt_info.dst_mac[3],
                               &ibv_res_ptr->pkt_info.dst_mac[4], &ibv_res_ptr->pkt_info.dst_mac[5]);
    sscanf(this->param.dst_port, "%hd", &ibv_res_ptr->pkt_info.dst_port);
    if (!this->param.SendOrRecv && !this->param.receive_flows.empty()) {
        const rdma_dada::io::rdma::ReceiveFlowSpec& flow =
            this->param.receive_flows.front();
        struct in_addr source_address = {};
        if (inet_pton(AF_INET, flow.source_ip.c_str(), &source_address) != 1) {
            fprintf(stderr, "[ERROR] Invalid receive flow source IP.\n");
            return;
        }
        ibv_res_ptr->pkt_info.src_ip = source_address.s_addr;
        ibv_res_ptr->pkt_info.src_port = flow.source_port;
    }

    if (this->param.SendOrRecv) {
        printf("[RoCEv2Dada] Network params parsed: source -> "
               "%d.%d.%d.%d:%d\n",
               tmp[0], tmp[1], tmp[2], tmp[3],
               ibv_res_ptr->pkt_info.dst_port);
    } else {
        printf("[RoCEv2Dada] Receive filter parsed: ANY source -> "
               "%d.%d.%d.%d:%d\n",
               tmp[0], tmp[1], tmp[2], tmp[3],
               ibv_res_ptr->pkt_info.dst_port);
    }
    fflush(stdout);
    
    if (!this->param.SendOrRecv) {
        ibv_res_ptr->pkt_size = Param.pkt_size + RX_STRIP_HEADER_BYTES;
    } else {
        ibv_res_ptr->pkt_size = Param.pkt_size;
    }
    if (ibv_res_ptr->pkt_size <= PKT_HEAD_LEN) {
        fprintf(stderr, "[ERROR] pkt_size (%u) must exceed PKT_HEAD_LEN (%u).\n", ibv_res_ptr->pkt_size, PKT_HEAD_LEN);
        return;
    }
    ibv_res_ptr->poll_n = this->param.poll_batch;
    ibv_res_ptr->recv_completed = 0;
    ibv_res_ptr->recv_sum_completed = 0;
    ibv_res_ptr->recv_sum = 0;
    
    // Keep enough receive WRs posted to absorb bursts without exceeding typical
    // NIC queue limits. Data is copied from these buffers into the PSRDADA ring.
    int work_num;
    if (!this->param.SendOrRecv) {
        work_num = this->param.recv_wr_num != 0U
            ? static_cast<int>(this->param.recv_wr_num)
            : ((this->param.send_n < 2048) ? (this->param.send_n * 4) : 8192);
    } else {
        work_num = this->param.send_n * 4;
    }
    printf("[RoCEv2Dada] Configured work_num=%d (send_n=%d, poll_batch=%u)\n",
           work_num, this->param.send_n, this->param.poll_batch);
    fflush(stdout);
    
#ifndef NO_CUDA
    printf("[RoCEv2Dada] Setting CUDA device %d...\n", this->param.gpu_id);
    fflush(stdout);
    cudaSetDevice(this->param.gpu_id);
    printf("[RoCEv2Dada] CUDA device set\n");
    fflush(stdout);
#endif
    
    printf("[RoCEv2Dada] Opening IB device %d...\n", this->param.device_id);
    fflush(stdout);
    ret = open_ib_device(this->param.device_id, ibv_res_ptr);
    if (ret < 0) { printf("Failed to open IB device.\n"); fflush(stdout); return; }
    printf("Open IB device successfully.\n");
    fflush(stdout);
    
    printf("[RoCEv2Dada] Creating IB resources... (SendOrRecv=%d)\n", this->param.SendOrRecv);
    fflush(stdout);
    unsigned int nsge = this->param.nsge
        ? this->param.nsge
        : rdma_dada::io::rdma::kDefaultReceiveNsge;
    ibv_res_ptr->recv_nsge = nsge;
    ibv_res_ptr->send_nsge = nsge;
    if(this->param.SendOrRecv) {
        ret = create_ib_res(ibv_res_ptr, work_num, 0);
        if (ret < 0) { printf("Failed to create ib resources.\n"); return; }
        printf("Create IB resources successfully.\n");
    } else {
        ret = create_ib_res(ibv_res_ptr, 0, work_num);
        if (ret < 0) { printf("Failed to create ib resources.\n"); fflush(stdout); return; }
        printf("Create IB resources successfully (recv_wr_num=%d).\n", work_num);
        fflush(stdout);
    }
    
    printf("[RoCEv2Dada] Initializing IB resources (QP state transitions)...\n");
    fflush(stdout);
    ret = init_ib_res(ibv_res_ptr);
    if (ret < 0) { printf("Failed to init ib resources.\n"); fflush(stdout); return; }
    printf("Init IB resources successfully.\n");
    fflush(stdout);
    
    printf("[RoCEv2Dada] Allocating memory buffers...\n");
    fflush(stdout);
    
    if ((size_t)work_num > SIZE_MAX / ibv_res_ptr->pkt_size) {
        fprintf(stderr, "[ERROR] RDMA buffer size calculation overflow.\n");
        return;
    }
    size_t buf_size = (size_t)ibv_res_ptr->pkt_size * (size_t)work_num;

    if (this->param.RdmaDirectGpu > 0 && !this->param.SendOrRecv) {
#ifndef NO_CUDA
        CUDA_CALL(cudaMalloc((void **) &ibv_res_ptr->mem_buf, buf_size));
#else
        fprintf(stderr, "Warning: RdmaDirectGpu requested but build has NO_CUDA; using host malloc instead.\n");
        ibv_res_ptr->mem_buf = (unsigned char *)malloc(buf_size);
#endif
    } else if(this->param.RdmaDirectGpu < 0 && !this->param.SendOrRecv) {
#ifndef NO_CUDA
        CUDA_CALL(cudaMallocHost((void **)&ibv_res_ptr->mem_buf, buf_size));
#else
        ibv_res_ptr->mem_buf = (unsigned char *)malloc(buf_size);
#endif
    } else {
        ibv_res_ptr->mem_buf = (unsigned char *)malloc(buf_size);
    }

    if (!ibv_res_ptr->mem_buf) {
        fprintf(stderr, "[ERROR] Failed to allocate %zu-byte RDMA buffer.\n", buf_size);
        return;
    }
    ret = register_memory(ibv_res_ptr, ibv_res_ptr->mem_buf, buf_size, ibv_res_ptr->pkt_size);
    if (ret < 0) { printf("Failed to register memory.\n"); fflush(stdout); return; }
    printf("Register memory successfully (buf_size=%zu bytes).\n", buf_size);
    fflush(stdout);
    
    if(this->param.SendOrRecv) {
        const unsigned int udp_header_len = 8;
        for(int k = 0; k < work_num; k++) {
            struct udp_pkt *pkt = (struct udp_pkt *)((uint8_t *)ibv_res_ptr->mem_buf + k * ibv_res_ptr->pkt_size);
            set_dest_mac(pkt, ibv_res_ptr->pkt_info.dst_mac);
            set_src_mac(pkt, ibv_res_ptr->pkt_info.src_mac);
            set_eth_type(pkt, (uint8_t *)"\x08\x00");
            set_src_ip(pkt, (uint8_t *)(&ibv_res_ptr->pkt_info.src_ip));
            set_dst_ip(pkt, (uint8_t *)(&ibv_res_ptr->pkt_info.dst_ip));
            set_udp_src_port(pkt, ibv_res_ptr->pkt_info.src_port);
            set_udp_dst_port(pkt, ibv_res_ptr->pkt_info.dst_port);
            set_pkt_len(pkt, ibv_res_ptr->pkt_size + udp_header_len);
            ret = this->param.WritSendBuff(pkt->payload, ibv_res_ptr->pkt_size);
            if (ret < 0) { printf("Failed to WritSendBuff.\n"); return; }
        }
        ret = ib_send_pkg(ibv_res_ptr, 0, work_num);
        if (ret < 0) { printf("Failed to send pkts.\n"); return; }
    } else {
        ret = create_flow(ibv_res_ptr, &ibv_res_ptr->pkt_info);
        if (ret < 0) {
            fprintf(stderr,
                    "[ERROR] Flow steering is required because no software "
                    "packet filter is implemented.\n");
            return;
        }
        printf("Create flow successfully.\n");
        
        printf("[RoCEv2Dada] Posting receive work requests... (work_num=%d)\n", work_num);
        std::vector<rdma_dada::io::rdma::ReceiveWorkItem> initial_work(
            static_cast<std::size_t>(work_num));
        for (int i = 0; i < work_num; ++i) {
            initial_work[static_cast<std::size_t>(i)].wr_id =
                static_cast<std::uint64_t>(i);
            initial_work[static_cast<std::size_t>(i)].completion_ns = 0;
        }
        ret = post_receive_batch(ibv_res_ptr, initial_work.data(),
                                 initial_work.size());
        if (ret != 0) return;
        printf("  Posted %d/%d receive WRs... Done!\n", work_num, work_num);
        fflush(stdout);

        receive_resources.push_back(ibv_res_ptr);
        for (unsigned int shard_index = 1;
             shard_index < this->param.receive_shards; ++shard_index) {
            struct ibv_utils_res *shard =
                static_cast<struct ibv_utils_res *>(
                    calloc(1, sizeof(struct ibv_utils_res)));
            if (!shard) {
                fprintf(stderr, "[ERROR] Failed to allocate receive shard.\n");
                return;
            }
            shard->pkt_size = ibv_res_ptr->pkt_size;
            shard->poll_n = ibv_res_ptr->poll_n;
            shard->recv_nsge = ibv_res_ptr->recv_nsge;
            shard->send_nsge = ibv_res_ptr->send_nsge;
            shard->pkt_info = ibv_res_ptr->pkt_info;
            if (!this->param.receive_flows.empty()) {
                const rdma_dada::io::rdma::ReceiveFlowSpec& flow =
                    this->param.receive_flows[shard_index];
                struct in_addr source_address = {};
                if (inet_pton(AF_INET, flow.source_ip.c_str(),
                              &source_address) != 1) {
                    fprintf(stderr, "[ERROR] Invalid receive shard IP.\n");
                    free(shard);
                    return;
                }
                shard->pkt_info.src_ip = source_address.s_addr;
                shard->pkt_info.src_port = flow.source_port;
            }
            if (open_ib_device(this->param.device_id, shard) < 0 ||
                create_ib_res(shard, 0, work_num) < 0 ||
                init_ib_res(shard) < 0) {
                fprintf(stderr,
                        "[ERROR] Failed to initialize receive shard %u.\n",
                        shard_index);
                receive_resources.push_back(shard);
                return;
            }
            shard->mem_buf = static_cast<unsigned char *>(malloc(buf_size));
            if (!shard->mem_buf ||
                register_memory(shard, shard->mem_buf, buf_size,
                                shard->pkt_size) < 0 ||
                create_flow(shard, &shard->pkt_info) < 0) {
                fprintf(stderr,
                        "[ERROR] Failed to prepare receive shard %u.\n",
                        shard_index);
                receive_resources.push_back(shard);
                return;
            }
            std::vector<rdma_dada::io::rdma::ReceiveWorkItem> shard_work(
                static_cast<std::size_t>(work_num));
            for (int wr = 0; wr < work_num; ++wr) {
                shard_work[static_cast<std::size_t>(wr)] = {
                    static_cast<std::uint64_t>(wr), 0};
            }
            if (post_receive_batch(shard, shard_work.data(),
                                   shard_work.size()) != 0) {
                receive_resources.push_back(shard);
                return;
            }
            shard->init_flag = true;
            receive_resources.push_back(shard);
            printf("[RDMA] Receive shard %u initialized with %d WRs.\n",
                   shard_index, work_num);
        }
        receive_pipeline = new ReceivePipelineState();
        if (!receive_pipeline) return;
        for (std::size_t shard_index = 0;
             shard_index < receive_resources.size(); ++shard_index) {
            receive_pipeline->shards.push_back(new ReceiveShardState(
                this,
                static_cast<struct ibv_utils_res *>(
                    receive_resources[shard_index]),
                static_cast<std::size_t>(work_num), shard_index));
        }
    }
    
    printf("[RoCEv2Dada] Checking send/recv info...\n");
    fflush(stdout);
    ret = check_send_recv_info(ibv_res_ptr, &this->param);
    if(ret >= 0) {
        printf("[RoCEv2Dada] ✓ Initialization complete, ready to start\n");
        ibv_res_ptr->init_flag = true;
        printf("INIT successfully.\n");
    } else {
        printf("RoCEv2Dada ERROE: check_send_recv_info is failed!\n");
    }
}

void * RoCEv2Dada::SendRecvThread(void *arg)
{
    RoCEv2Dada *this_ptr = static_cast<RoCEv2Dada *>(arg);
    struct ibv_utils_res *ibv_res_ptr =
        static_cast<struct ibv_utils_res *>(this_ptr->ibv_res);
    std::size_t completed = 0;
    int send_idx = 0;
    while (!this_ptr->stop_requested.load()) {
        while (completed < this_ptr->param.send_n &&
               !this_ptr->stop_requested.load()) {
            const int polled = ibv_poll_cq(
                ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
            if (polled < 0) {
                fprintf(stderr, "ERROR: failed to poll send CQ.\n");
                return NULL;
            }
            for (int i = 0; i < polled; ++i) {
                if (!validate_send_completion(ibv_res_ptr->wc[i],
                                              ibv_res_ptr)) return NULL;
            }
            completed += static_cast<std::size_t>(polled);
        }
        if (this_ptr->stop_requested.load()) break;
        for (unsigned int k = 0; k < this_ptr->param.send_n; ++k) {
            struct udp_pkt *pkt = reinterpret_cast<struct udp_pkt *>(
                ibv_res_ptr->mem_buf +
                static_cast<std::size_t>(k + send_idx) *
                    ibv_res_ptr->pkt_size);
            if (this_ptr->param.WritSendBuff(
                    pkt->payload, ibv_res_ptr->pkt_size) < 0) return NULL;
        }
        completed -= this_ptr->param.send_n;
        send_idx = (send_idx + this_ptr->param.send_n) %
            ibv_res_ptr->send_wr_num;
        if (ib_send_pkg(ibv_res_ptr, send_idx,
                        this_ptr->param.send_n) < 0) return NULL;
    }
    return NULL;
}

void * RoCEv2Dada::ReceivePollThread(void *arg)
{
    ReceiveShardState *shard = static_cast<ReceiveShardState *>(arg);
    RoCEv2Dada *this_ptr = shard->owner;
    struct ibv_utils_res *ibv_res_ptr = shard->resource;
    ReceivePipelineState *pipeline = this_ptr->receive_pipeline;
    const std::size_t maximum_batch = this_ptr->param.send_n;
    std::vector<rdma_dada::io::rdma::ReceiveWorkItem> repost_batch(
        maximum_batch);
    std::uint64_t stop_empty_polls = 0;
    const std::uint64_t kStopEmptyPolls = 4096;

    while (!pipeline->failed.load()) {
        std::size_t repost_count = 0;
        while (repost_count < maximum_batch &&
               shard->recycle.TryPop(&repost_batch[repost_count])) {
            ++repost_count;
        }
        if (repost_count != 0) {
            if (post_receive_batch(ibv_res_ptr, repost_batch.data(),
                                   repost_count) != 0) {
                shard->repost_failures.fetch_add(1);
                pipeline->failed.store(true);
                break;
            }
            shard->reposted.fetch_add(repost_count);
            shard->posted_wrs.fetch_add(repost_count);
        }

        const int polled = ibv_poll_cq(
            ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
        shard->poll_calls.fetch_add(1);
        if (polled < 0) {
            fprintf(stderr, "ERROR: receive poll thread failed to poll CQ.\n");
            pipeline->failed.store(true);
            break;
        }
        if (polled == 0) {
            shard->empty_polls.fetch_add(1);
            if (this_ptr->stop_requested.load()) {
                if (++stop_empty_polls >= kStopEmptyPolls) break;
            }
            continue;
        }
        stop_empty_polls = 0;
        shard->completions.fetch_add(static_cast<std::uint64_t>(polled));
        if (polled == static_cast<int>(ibv_res_ptr->poll_n))
            shard->full_polls.fetch_add(1);
        const std::uint64_t before = shard->posted_wrs.fetch_sub(
            static_cast<std::uint64_t>(polled));
        const std::uint64_t after = before >= static_cast<std::uint64_t>(polled)
            ? before - static_cast<std::uint64_t>(polled) : 0;
        update_atomic_min(&shard->min_posted_wrs, after);
        struct timespec completion_time = {};
        clock_gettime(CLOCK_MONOTONIC_RAW, &completion_time);
        const std::uint64_t completion_ns =
            static_cast<std::uint64_t>(completion_time.tv_sec) *
                UINT64_C(1000000000) +
            static_cast<std::uint64_t>(completion_time.tv_nsec);

        for (int i = 0; i < polled; ++i) {
            const struct ibv_wc completion = ibv_res_ptr->wc[i];
            const rdma_dada::io::rdma::ReceiveCompletion policy_input = {
                completion.status == IBV_WC_SUCCESS,
                completion.opcode == IBV_WC_RECV,
                completion.wr_id,
                completion.byte_len
            };
            const rdma_dada::io::rdma::ReceiveDisposition disposition =
                rdma_dada::io::rdma::ClassifyReceiveCompletion(
                    policy_input,
                    static_cast<std::uint64_t>(ibv_res_ptr->recv_wr_num),
                    ibv_res_ptr->pkt_size);
            if (disposition ==
                rdma_dada::io::rdma::ReceiveDisposition::kFatal) {
                log_fatal_receive_completion(completion, ibv_res_ptr);
                pipeline->failed.store(true);
                break;
            }
            const rdma_dada::io::rdma::ReceiveWorkItem item = {
                completion.wr_id, completion_ns
            };
            if (disposition ==
                rdma_dada::io::rdma::ReceiveDisposition::kDropWrongLength) {
                this_ptr->wrong_length_receive_packets.fetch_add(1);
                if (post_receive_batch(ibv_res_ptr, &item, 1) != 0) {
                    shard->repost_failures.fetch_add(1);
                    pipeline->failed.store(true);
                    break;
                }
                shard->reposted.fetch_add(1);
                shard->posted_wrs.fetch_add(1);
                continue;
            }
            while (!shard->completed.TryPush(item) &&
                   !pipeline->failed.load()) sched_yield();
            if (pipeline->failed.load()) break;
            this_ptr->accepted_receive_packets.fetch_add(1);
        }
    }
    shard->poll_done.store(true);

    while (!pipeline->failed.load()) {
        std::size_t repost_count = 0;
        while (repost_count < maximum_batch &&
               shard->recycle.TryPop(&repost_batch[repost_count])) {
            ++repost_count;
        }
        if (repost_count == 0) {
            if (pipeline->copy_done.load() && shard->recycle.empty()) break;
            sched_yield();
            continue;
        }
        if (post_receive_batch(ibv_res_ptr, repost_batch.data(),
                               repost_count) != 0) {
            shard->repost_failures.fetch_add(1);
            pipeline->failed.store(true);
            break;
        }
        shard->reposted.fetch_add(repost_count);
        shard->posted_wrs.fetch_add(repost_count);
    }
    return NULL;
}

void * RoCEv2Dada::ReceiveCopyThread(void *arg)
{
    RoCEv2Dada *this_ptr = static_cast<RoCEv2Dada *>(arg);
    ReceivePipelineState *pipeline = this_ptr->receive_pipeline;
    const std::size_t maximum_batch = this_ptr->param.send_n;
    const std::uint64_t ring_record_bytes = this_ptr->param.pkt_size;
    std::vector<rdma_dada::io::rdma::ReceiveWorkItem> batch(maximum_batch);
    long int remaining_bytes = 0;
    long int block_bytes = 0;
    char *write_ptr = NULL;
    std::uint64_t recycle_delay_total = 0;
    std::uint64_t recycle_delay_max = 0;

    printf("[RDMA] Using sharded receive mode (%zu poll/repost + "
           "one copy/ring thread)\n", pipeline->shards.size());
    fflush(stdout);

    while (!pipeline->failed.load()) {
        bool copied_any = false;
        bool all_pollers_done = true;
        for (std::size_t shard_index = 0;
             shard_index < pipeline->shards.size(); ++shard_index) {
            ReceiveShardState *shard = pipeline->shards[shard_index];
            if (!shard->poll_done.load() || !shard->completed.empty())
                all_pollers_done = false;
            std::size_t count = 0;
            while (count < maximum_batch &&
                   shard->completed.TryPop(&batch[count])) ++count;
            if (count == 0) continue;
            copied_any = true;
            pipeline->copy_batches.fetch_add(1);
            for (std::size_t i = 0; i < count; ++i) {
                if (!write_ptr) {
                    write_ptr = this_ptr->param.GetBuffPtr(remaining_bytes);
                    block_bytes = remaining_bytes;
                    if (!write_ptr || remaining_bytes <= 0 ||
                        static_cast<std::uint64_t>(remaining_bytes) <
                            ring_record_bytes) {
                        fprintf(stderr,
                                "ERROR: failed to acquire valid raw ring block.\n");
                        pipeline->failed.store(true);
                        break;
                    }
                }
                const std::uint64_t wr_id = batch[i].wr_id;
                struct ibv_utils_res *resource = shard->resource;
                unsigned char *src = reinterpret_cast<unsigned char *>(
                    resource->sge[wr_id * resource->recv_nsge].addr);
                if (this_ptr->param.RdmaDirectGpu != 0) {
                    CUDA_CALL(cudaMemcpy(write_ptr,
                                         src + RX_STRIP_HEADER_BYTES,
                                         ring_record_bytes,
                                         cudaMemcpyDeviceToDevice));
                } else {
                    memcpy(write_ptr, src + RX_STRIP_HEADER_BYTES,
                           ring_record_bytes);
                }
                write_ptr += ring_record_bytes;
                remaining_bytes -= static_cast<long int>(ring_record_bytes);
                if (remaining_bytes == 0) {
                    if (this_ptr->param.DataSendBuff(
                            static_cast<std::uint64_t>(block_bytes)) < 0) {
                        fprintf(stderr, "[ERROR] Failed to publish raw block.\n");
                        pipeline->failed.store(true);
                        break;
                    }
                    this_ptr->published_receive_packets.fetch_add(
                        static_cast<std::uint64_t>(block_bytes) /
                            ring_record_bytes);
                    this_ptr->published_receive_blocks.fetch_add(1);
                    write_ptr = NULL;
                    block_bytes = 0;
                }
            }
            if (pipeline->failed.load()) break;
            struct timespec recycled_at = {};
            clock_gettime(CLOCK_MONOTONIC_RAW, &recycled_at);
            const std::uint64_t recycled_ns =
                static_cast<std::uint64_t>(recycled_at.tv_sec) *
                    UINT64_C(1000000000) +
                static_cast<std::uint64_t>(recycled_at.tv_nsec);
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t recycle_delay =
                    recycled_ns >= batch[i].completion_ns
                    ? recycled_ns - batch[i].completion_ns : 0;
                recycle_delay_total += recycle_delay;
                if (recycle_delay > recycle_delay_max)
                    recycle_delay_max = recycle_delay;
                while (!shard->recycle.TryPush(batch[i]) &&
                       !pipeline->failed.load()) sched_yield();
            }
        }
        if (pipeline->failed.load()) break;
        if (!copied_any) {
            if (all_pollers_done) break;
            sched_yield();
        }
    }

    if (!pipeline->failed.load() && write_ptr && block_bytes > remaining_bytes) {
        const std::uint64_t valid_bytes =
            static_cast<std::uint64_t>(block_bytes - remaining_bytes);
        if (this_ptr->param.DataSendBuff(valid_bytes) < 0) {
            fprintf(stderr, "[ERROR] Failed to publish final raw block.\n");
            pipeline->failed.store(true);
        } else {
            this_ptr->published_receive_packets.fetch_add(
                valid_bytes / ring_record_bytes);
            this_ptr->published_receive_blocks.fetch_add(1);
            if (valid_bytes < static_cast<std::uint64_t>(block_bytes))
                this_ptr->partial_receive_blocks.fetch_add(1);
            printf("[RDMA] Published final raw block: bytes=%" PRIu64
                   ", records=%" PRIu64 "\n",
                   valid_bytes, valid_bytes / ring_record_bytes);
            fflush(stdout);
        }
    }
    pipeline->completion_to_recycle_ns_total.store(recycle_delay_total);
    pipeline->completion_to_recycle_ns_max.store(recycle_delay_max);
    pipeline->copy_done.store(true);
    return NULL;
}

RoCEv2Dada::~RoCEv2Dada()
{
    Stop();
    const auto release_resource = [&](struct ibv_utils_res *ibv_res_ptr) {
        if (!ibv_res_ptr) return;
        if (ibv_res_ptr->mem_buf) {
            if(this->param.RdmaDirectGpu > 0 && !this->param.SendOrRecv) {
                CUDA_CALL(cudaFree(ibv_res_ptr->mem_buf));
            } else if(this->param.RdmaDirectGpu < 0 && !this->param.SendOrRecv) {
                CUDA_CALL(cudaFreeHost(ibv_res_ptr->mem_buf));
            } else {
                free(ibv_res_ptr->mem_buf);
            }
            ibv_res_ptr->mem_buf = NULL;
        }
        destroy_ib_res(ibv_res_ptr);
        close_ib_device(ibv_res_ptr);
        free(ibv_res_ptr);
    };
    if (!receive_resources.empty()) {
        for (std::size_t index = 0; index < receive_resources.size(); ++index)
            release_resource(static_cast<struct ibv_utils_res *>(
                receive_resources[index]));
        receive_resources.clear();
    } else if (this->ibv_res) {
        release_resource(static_cast<struct ibv_utils_res *>(this->ibv_res));
    }
    this->ibv_res = NULL;
    if (receive_pipeline) {
        for (std::size_t index = 0;
             index < receive_pipeline->shards.size(); ++index)
            delete receive_pipeline->shards[index];
        receive_pipeline->shards.clear();
    }
    delete receive_pipeline;
    receive_pipeline = NULL;
}

int RoCEv2Dada::Start()
{
    printf("[RoCEv2Dada::Start] Entry\n");
    fflush(stdout);
    
    if(NULL == this->ibv_res) { 
        printf("[RoCEv2Dada::Start] Error: ibv_res is NULL\n");
        fflush(stdout);
        return RDMA_NULL_POINTER; 
    }
    
    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)this->ibv_res;
    printf("[RoCEv2Dada::Start] ibv_res_ptr=%p\n", (void*)ibv_res_ptr);
    fflush(stdout);
    
    if (thread_started) {
        fprintf(stderr, "RoCEv2Dada::Start error: receiver already started.\n");
        return RDMA_ERROR;
    }
    
    if(!ibv_res_ptr->init_flag) { 
        printf("RoCEv2Dada::Start error: init_flag not set!\n"); 
        fflush(stdout);
        return RDMA_ERROR; 
    }

    if ((!this->param.SendOrRecv &&
         (!this->param.GetBuffPtr || !this->param.DataSendBuff)) ||
        (this->param.SendOrRecv && !this->param.WritSendBuff)) {
        fprintf(stderr,
                "RoCEv2Dada::Start error: required data callback is missing.\n");
        return RDMA_ERROR;
    }
    
    stop_requested.store(false);
    cpu_set_t mask;
    int ret = 0;
    if (this->param.SendOrRecv) {
        ret = pthread_create(&ibv_res_ptr->tid, NULL, SendRecvThread,
                             static_cast<void *>(this));
        if (ret != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", ret);
            return RDMA_ERROR;
        }
    } else {
        if (!receive_pipeline) {
            fprintf(stderr, "RoCEv2Dada::Start error: receive pipeline missing.\n");
            return RDMA_ERROR;
        }
        receive_pipeline->copy_done.store(false);
        receive_pipeline->failed.store(false);
        for (std::size_t index = 0;
             index < receive_pipeline->shards.size(); ++index)
            receive_pipeline->shards[index]->poll_done.store(false);
        ret = pthread_create(&receive_pipeline->copy_tid, NULL,
                             ReceiveCopyThread, static_cast<void *>(this));
        if (ret != 0) {
            fprintf(stderr, "copy pthread_create failed: %d\n", ret);
            return RDMA_ERROR;
        }
        receive_pipeline->copy_thread_started = true;
        for (std::size_t index = 0;
             index < receive_pipeline->shards.size(); ++index) {
            ReceiveShardState *shard = receive_pipeline->shards[index];
            ret = pthread_create(&shard->poll_tid, NULL,
                                 ReceivePollThread,
                                 static_cast<void *>(shard));
            if (ret != 0) {
                fprintf(stderr, "poll pthread_create failed for shard %zu: %d\n",
                        index, ret);
                receive_pipeline->failed.store(true);
                for (std::size_t pending = index;
                     pending < receive_pipeline->shards.size(); ++pending)
                    receive_pipeline->shards[pending]->poll_done.store(true);
                for (std::size_t started = 0; started < index; ++started) {
                    pthread_join(receive_pipeline->shards[started]->poll_tid,
                                 NULL);
                    receive_pipeline->shards[started]->poll_thread_started = false;
                }
                pthread_join(receive_pipeline->copy_tid, NULL);
                receive_pipeline->copy_thread_started = false;
                return RDMA_ERROR;
            }
            shard->poll_thread_started = true;
        }
    }
    thread_started = true;

    const auto bind_thread = [&](pthread_t tid, int cpu,
                                 const char *role) -> bool {
        if (cpu < 0) return true;
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
        ret = pthread_setaffinity_np(tid, sizeof(mask), &mask);
        if (ret != 0) {
            fprintf(stderr,
                    "[ERROR] Failed to bind RDMA %s thread to CPU %d: %s\n",
                    role, cpu, strerror(ret));
            return false;
        }
        cpu_set_t actual;
        CPU_ZERO(&actual);
        ret = pthread_getaffinity_np(tid, sizeof(actual), &actual);
        if (ret != 0 || !CPU_ISSET(cpu, &actual)) {
            fprintf(stderr,
                    "[ERROR] RDMA %s thread affinity verification failed "
                    "for CPU %d.\n", role, cpu);
            return false;
        }
        return true;
    };
    bool affinity_ok = true;
    if (this->param.SendOrRecv) {
        affinity_ok = bind_thread(ibv_res_ptr->tid,
                                  this->param.poll_cpu_id, "send");
    } else {
        affinity_ok = bind_thread(receive_pipeline->copy_tid,
                                  this->param.copy_cpu_id, "copy");
        for (std::size_t index = 0;
             affinity_ok && index < receive_pipeline->shards.size(); ++index) {
            const int cpu = this->param.poll_cpu_ids.empty()
                ? -1 : this->param.poll_cpu_ids[index];
            char role[64];
            snprintf(role, sizeof(role), "poll[%zu]", index);
            affinity_ok = bind_thread(
                receive_pipeline->shards[index]->poll_tid, cpu, role);
        }
    }
    if (!affinity_ok) {
        Stop();
        return RDMA_ERROR;
    }
    if (!this->param.SendOrRecv) {
        printf("[RDMA] Receive threads ready: shards=%zu, poll_cpus=",
               receive_pipeline->shards.size());
        if (this->param.poll_cpu_ids.empty()) {
            printf("unbound");
        } else {
            for (std::size_t index = 0;
                 index < this->param.poll_cpu_ids.size(); ++index)
                printf("%s%d", index == 0 ? "" : ",",
                       this->param.poll_cpu_ids[index]);
        }
        printf(", copy_cpu=%d\n", this->param.copy_cpu_id);
        fflush(stdout);
    }
    
    printf("[RoCEv2Dada::Start] Success, returning RDMA_OK\n");
    fflush(stdout);
    return RDMA_OK;
}

int RoCEv2Dada::Stop()
{
    if (!thread_started || !this->ibv_res) {
        return RDMA_OK;
    }

    stop_requested.store(true);
    int ret = 0;
    if (this->param.SendOrRecv) {
        struct ibv_utils_res *ibv_res_ptr =
            static_cast<struct ibv_utils_res *>(this->ibv_res);
        ret = pthread_join(ibv_res_ptr->tid, NULL);
        if (ret != 0) {
            fprintf(stderr, "pthread_join failed: %d (%s)\n", ret,
                    strerror(ret));
            return RDMA_ERROR;
        }
    } else if (receive_pipeline) {
        for (std::size_t index = 0;
             index < receive_pipeline->shards.size(); ++index) {
            ReceiveShardState *shard = receive_pipeline->shards[index];
            if (!shard->poll_thread_started) continue;
            ret = pthread_join(shard->poll_tid, NULL);
            shard->poll_thread_started = false;
            if (ret != 0) {
                fprintf(stderr,
                        "poll[%zu] pthread_join failed: %d (%s)\n",
                        index, ret, strerror(ret));
                return RDMA_ERROR;
            }
        }
        if (receive_pipeline->copy_thread_started) {
            ret = pthread_join(receive_pipeline->copy_tid, NULL);
            receive_pipeline->copy_thread_started = false;
            if (ret != 0) {
                fprintf(stderr, "copy pthread_join failed: %d (%s)\n", ret,
                        strerror(ret));
                return RDMA_ERROR;
            }
        }
    }
    thread_started = false;
    if (!this->param.SendOrRecv) {
        const ReceiveStats stats = GetReceiveStats();
        const std::uint64_t total =
            stats.accepted_packets + stats.wrong_length_packets;
        const double wrong_length_ratio = total == 0 ? 0.0 :
            static_cast<double>(stats.wrong_length_packets) /
                static_cast<double>(total);
        printf("[RDMA] Receive summary: accepted=%" PRIu64
               ", wrong_length=%" PRIu64 ", published=%" PRIu64
               ", blocks=%" PRIu64 ", partial_blocks=%" PRIu64
               ", cq_tail_records=%" PRIu64
               ", wrong_length_ratio=%.9f\n",
               stats.accepted_packets, stats.wrong_length_packets,
               stats.published_packets, stats.published_blocks,
               stats.partial_blocks, stats.cq_tail_records,
               wrong_length_ratio);
        fflush(stdout);
        printf("[RDMA] Receive pipeline summary: poll_calls=%" PRIu64
               ", empty_polls=%" PRIu64 ", full_polls=%" PRIu64
               ", reposted_wrs=%" PRIu64
               ", repost_failures=%" PRIu64
               ", copy_batches=%" PRIu64
               ", min_posted_wrs=%" PRIu64
               ", completion_queue_high_watermark=%" PRIu64
               ", recycle_queue_high_watermark=%" PRIu64
               ", completion_to_recycle_ns_total=%" PRIu64
               ", completion_to_recycle_ns_max=%" PRIu64 "\n",
               stats.poll_calls, stats.empty_polls, stats.full_polls,
               stats.reposted_wrs, stats.repost_failures,
               stats.copy_batches, stats.min_posted_wrs,
               stats.completion_queue_high_watermark,
               stats.recycle_queue_high_watermark,
               stats.completion_to_recycle_ns_total,
               stats.completion_to_recycle_ns_max);
        fflush(stdout);
        if (receive_pipeline) {
            for (std::size_t index = 0;
                 index < receive_pipeline->shards.size(); ++index) {
                const ReceiveShardState *shard =
                    receive_pipeline->shards[index];
                printf("[RDMA] Receive shard summary: shard=%zu"
                       ", poll_calls=%" PRIu64
                       ", empty_polls=%" PRIu64
                       ", full_polls=%" PRIu64
                       ", completions=%" PRIu64
                       ", reposted_wrs=%" PRIu64
                       ", repost_failures=%" PRIu64
                       ", min_posted_wrs=%" PRIu64
                       ", completion_queue_high_watermark=%zu"
                       ", recycle_queue_high_watermark=%zu\n",
                       index, shard->poll_calls.load(),
                       shard->empty_polls.load(), shard->full_polls.load(),
                       shard->completions.load(), shard->reposted.load(),
                       shard->repost_failures.load(),
                       shard->min_posted_wrs.load(),
                       shard->completed.high_watermark(),
                       shard->recycle.high_watermark());
            }
            fflush(stdout);
        }
        if (receive_pipeline && receive_pipeline->failed.load()) {
            return RDMA_ERROR;
        }
    }
    return RDMA_OK;
}

RoCEv2Dada::ReceiveStats RoCEv2Dada::GetReceiveStats() const
{
    ReceiveStats stats;
    stats.accepted_packets = accepted_receive_packets.load();
    stats.wrong_length_packets = wrong_length_receive_packets.load();
    stats.published_packets = published_receive_packets.load();
    stats.published_blocks = published_receive_blocks.load();
    stats.partial_blocks = partial_receive_blocks.load();
    stats.cq_tail_records = cq_tail_receive_records.load();
    stats.poll_calls = 0;
    stats.empty_polls = 0;
    stats.full_polls = 0;
    stats.reposted_wrs = 0;
    stats.repost_failures = 0;
    stats.copy_batches = receive_pipeline
        ? receive_pipeline->copy_batches.load() : 0;
    stats.min_posted_wrs = 0;
    stats.completion_queue_high_watermark = 0;
    stats.recycle_queue_high_watermark = 0;
    if (receive_pipeline && !receive_pipeline->shards.empty()) {
        stats.min_posted_wrs = UINT64_MAX;
        for (std::size_t index = 0;
             index < receive_pipeline->shards.size(); ++index) {
            const ReceiveShardState *shard = receive_pipeline->shards[index];
            stats.poll_calls += shard->poll_calls.load();
            stats.empty_polls += shard->empty_polls.load();
            stats.full_polls += shard->full_polls.load();
            stats.reposted_wrs += shard->reposted.load();
            stats.repost_failures += shard->repost_failures.load();
            stats.min_posted_wrs = std::min(
                stats.min_posted_wrs, shard->min_posted_wrs.load());
            stats.completion_queue_high_watermark = std::max(
                stats.completion_queue_high_watermark,
                static_cast<std::uint64_t>(
                    shard->completed.high_watermark()));
            stats.recycle_queue_high_watermark = std::max(
                stats.recycle_queue_high_watermark,
                static_cast<std::uint64_t>(
                    shard->recycle.high_watermark()));
        }
    }
    stats.completion_to_recycle_ns_total = receive_pipeline
        ? receive_pipeline->completion_to_recycle_ns_total.load() : 0;
    stats.completion_to_recycle_ns_max = receive_pipeline
        ? receive_pipeline->completion_to_recycle_ns_max.load() : 0;
    return stats;
}
