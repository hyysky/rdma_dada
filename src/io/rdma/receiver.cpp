//定义 RoCEv2Dada 类的成员函数，用于通过 RoCEv2 协议进行数据发送和接收
#include <infiniband/verbs.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

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

static int repost_receive_wr(struct ibv_utils_res *ibv_res_ptr,
                             uint64_t wr_id)
{
    ibv_res_ptr->recv_wr->wr_id = wr_id;
    ibv_res_ptr->recv_wr->sg_list =
        &ibv_res_ptr->sge[wr_id * ibv_res_ptr->recv_nsge];
    ibv_res_ptr->recv_wr->num_sge = ibv_res_ptr->recv_nsge;
    ibv_res_ptr->recv_wr->next = NULL;
    const int ret = ibv_post_recv(ibv_res_ptr->qp,
                                  ibv_res_ptr->recv_wr,
                                  &ibv_res_ptr->bad_recv_wr);
    if (ret != 0) {
        fprintf(stderr,
                "[ERROR] Failed to repost receive WR wr_id=%" PRIu64
                ": ret=%d, errno=%d (%s)\n",
                wr_id, ret, errno, strerror(errno));
    }
    return ret;
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
        Param_ptr->bind_cpu_id < -1) {
        fprintf(stderr,
                "[ERROR] Invalid RDMA geometry: pkt_size=%u, send_n=%u, "
                "bind_cpu_id=%d.\n",
                Param_ptr->pkt_size, Param_ptr->send_n,
                Param_ptr->bind_cpu_id);
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
    : param(Param), ibv_res(NULL), stop_requested(false),
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
    ibv_res_ptr->poll_n = 8;
    ibv_res_ptr->recv_completed = 0;
    ibv_res_ptr->recv_sum_completed = 0;
    ibv_res_ptr->recv_sum = 0;
    
    // Keep enough receive WRs posted to absorb bursts without exceeding typical
    // NIC queue limits. Data is copied from these buffers into the PSRDADA ring.
    int work_num;
    if (!this->param.SendOrRecv) {
        work_num = (this->param.send_n < 2048) ? (this->param.send_n * 4) : 8192;
    } else {
        work_num = this->param.send_n * 4;
    }
    printf("[RoCEv2Dada] Configured work_num=%d (send_n=%d)\n",
           work_num, this->param.send_n);
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
    unsigned int nsge = this->param.nsge ? this->param.nsge : 4;
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
        int posted = 0;
        for(int i = 0; i < work_num; i++) {
            ibv_res_ptr->recv_wr->wr_id = i;
            ibv_res_ptr->recv_wr->sg_list = &ibv_res_ptr->sge[i*ibv_res_ptr->recv_nsge];
            ibv_res_ptr->recv_wr->num_sge = ibv_res_ptr->recv_nsge;
            ibv_res_ptr->recv_wr->next = NULL;
            ret = ibv_post_recv(ibv_res_ptr->qp, ibv_res_ptr->recv_wr, &ibv_res_ptr->bad_recv_wr);
            if (ret != 0) {
                fprintf(stderr, "[ERROR] ibv_post_recv failed at i=%d, ret=%d, errno=%d (%s)\n",
                        i, ret, errno, strerror(errno));
                return;
            }
            posted++;
            if (i % 1024 == 0 && i > 0) {
                printf("  Posted %d/%d receive WRs...\r", i, work_num);
                fflush(stdout);
            }
        }
        printf("  Posted %d/%d receive WRs... Done!\n", posted, work_num);
        fflush(stdout);
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
    size_t total_recv = 0;
    size_t total_recv_pre = 0;
    int ret = 0;
    RoCEv2Dada * this_ptr = (RoCEv2Dada *)arg;
    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)this_ptr->ibv_res;
    
    if (this_ptr->param.debug_mode) {
        printf("[DEBUG] SendRecvThread started (tid=%lu)\n", (unsigned long)pthread_self());
        fflush(stdout);
    }
    struct timespec ts_start;
    struct timespec ts_now;
    uint64_t ns_elapsed;
    long int block_bufsz = 0;
    long int write_bufsz = 0;
    char * gpu_ibuf = NULL;
    // pkt_size already includes header (passed from run_demo.sh as PKT_HEADER+PKT_DATA)
    int pkt_len = ibv_res_ptr->pkt_size;
    int ring_pkt_len = this_ptr->param.pkt_size;
    int send_idx = 0;
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[80];

    if (!this_ptr->param.SendOrRecv && this_ptr->param.pkt_size <= 0) {
        fprintf(stderr, "ERROR: invalid payload pkt_size=%u.\n", this_ptr->param.pkt_size);
        return NULL;
    }
    
    // 初始化时间戳，避免第一次计算时使用未初始化的值
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
    
    if (this_ptr->param.debug_mode) {
        printf("[DEBUG] Entering main receive loop...\n");
        fflush(stdout);
    }
    
    while (!this_ptr->stop_requested.load()) {
        if(this_ptr->param.SendOrRecv) {
            while(total_recv_pre < this_ptr->param.send_n &&
                  !this_ptr->stop_requested.load()) {
                int polled = ibv_poll_cq(ibv_res_ptr->cq, ibv_res_ptr->poll_n,
                                         ibv_res_ptr->wc);
                if (polled < 0) {
                    fprintf(stderr, "ERROR: failed to poll send CQ.\n");
                    return NULL;
                }
                for (int i = 0; i < polled; ++i) {
                    if (!validate_send_completion(ibv_res_ptr->wc[i],
                                                  ibv_res_ptr)) {
                        return NULL;
                    }
                }
                total_recv_pre += (size_t)polled;
            }
            if (this_ptr->stop_requested.load()) {
                break;
            }
            for(int k = 0; k < this_ptr->param.send_n; k++) {
                struct udp_pkt *pkt = (struct udp_pkt *)((uint8_t *)ibv_res_ptr->mem_buf + (k + send_idx) * pkt_len);
                ret = this_ptr->param.WritSendBuff(pkt->payload, ibv_res_ptr->pkt_size);
                if (ret < 0) { printf("Failed to WritSendBuff.\n"); return NULL; }
            }
            total_recv_pre -= this_ptr->param.send_n;
            send_idx = (send_idx + this_ptr->param.send_n) % ibv_res_ptr->send_wr_num;
            ret = ib_send_pkg(ibv_res_ptr, send_idx, this_ptr->param.send_n);
            if (ret < 0) { printf("Failed to send pkts.\n"); return NULL; }
        } else {
            static bool first_normal_path_log = true;
            if (first_normal_path_log) {
                printf("[RDMA] Using normal receive mode (with copy to ring buffer)\n");
                fflush(stdout);
                first_normal_path_log = false;
            }

            clock_gettime(CLOCK_MONOTONIC_RAW, &ts_now);
            ns_elapsed = ELAPSED_US(ts_start, ts_now);
            if (ns_elapsed > 1000 * 1000) {
                ts_start = ts_now;
                if (total_recv != total_recv_pre) {
                    const double bandwidth = MEASURE_BANDWIDTH(
                        ((total_recv - total_recv_pre) * ring_pkt_len),
                        ns_elapsed);
                    time(&rawtime);
                    timeinfo = localtime(&rawtime);
                    timeinfo->tm_hour += 8;
                    strftime(time_buffer, sizeof(time_buffer),
                             "year:%Y,month:%m,day:%d,hours:%H,minites:%M,second:%S",
                             timeinfo);
                    printf("NowTime:%s,gpu_id:%d,total_recv:%-8luKB,Process Bandwidth:%6.3f Gbps,cost time:%lums\n",
                           time_buffer, this_ptr->param.gpu_id,
                           (unsigned long)(total_recv * ring_pkt_len / 1024),
                           bandwidth, (unsigned long)(ns_elapsed / 1000));
                    total_recv_pre = total_recv;
                }
            }

            ibv_res_ptr->recv_completed = ibv_poll_cq(
                ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
            if (ibv_res_ptr->recv_completed < 0) {
                fprintf(stderr, "ERROR: SendRecvThread failed to poll receive CQ.\n");
                return NULL;
            }
            if (ibv_res_ptr->recv_completed == 0) continue;

            int accepted_completed = 0;
            for (int i = 0; i < ibv_res_ptr->recv_completed; ++i) {
                const struct ibv_wc completion = ibv_res_ptr->wc[i];
                const rdma_dada::io::rdma::ReceiveCompletion policy_input = {
                    completion.status == IBV_WC_SUCCESS,
                    completion.opcode == IBV_WC_RECV,
                    completion.wr_id,
                    completion.byte_len
                };
                const rdma_dada::io::rdma::ReceiveDisposition disposition =
                    rdma_dada::io::rdma::ClassifyReceiveCompletion(
                        policy_input, (uint64_t)ibv_res_ptr->recv_wr_num,
                        ibv_res_ptr->pkt_size);
                if (disposition ==
                    rdma_dada::io::rdma::ReceiveDisposition::kFatal) {
                    log_fatal_receive_completion(completion, ibv_res_ptr);
                    return NULL;
                }
                if (disposition ==
                    rdma_dada::io::rdma::ReceiveDisposition::kDropWrongLength) {
                    const uint64_t dropped =
                        this_ptr->wrong_length_receive_packets.fetch_add(1) + 1;
                    if (rdma_dada::io::rdma::ShouldLogWrongLengthDrop(dropped)) {
                        fprintf(stderr,
                                "[WARN] Dropping wrong-length receive: "
                                "byte_len=%u, expected=%u, wr_id=%" PRIu64
                                ", total_wrong_length=%" PRIu64 ".\n",
                                completion.byte_len, ibv_res_ptr->pkt_size,
                                completion.wr_id, dropped);
                    }
                    if (repost_receive_wr(ibv_res_ptr, completion.wr_id) != 0)
                        return NULL;
                    continue;
                }
                ibv_res_ptr->wc[accepted_completed++] = completion;
                this_ptr->accepted_receive_packets.fetch_add(1);
            }
            if (ibv_res_ptr->recv_sum_completed + accepted_completed >
                ibv_res_ptr->recv_wr_num) {
                fprintf(stderr,
                        "[ERROR] CQ accumulation overflow: buffered=%d, "
                        "new=%d, capacity=%d.\n",
                        ibv_res_ptr->recv_sum_completed, accepted_completed,
                        ibv_res_ptr->recv_wr_num);
                return NULL;
            }
            memcpy(ibv_res_ptr->wc_tmp + ibv_res_ptr->recv_sum_completed,
                   ibv_res_ptr->wc,
                   sizeof(struct ibv_wc) * accepted_completed);
            ibv_res_ptr->recv_sum_completed += accepted_completed;

            while (ibv_res_ptr->recv_sum_completed >=
                   (int)this_ptr->param.send_n) {
                const int count = (int)this_ptr->param.send_n;
                const uint64_t bytes = (uint64_t)count * ring_pkt_len;
                if (!gpu_ibuf) {
                    gpu_ibuf = this_ptr->param.GetBuffPtr(block_bufsz);
                    write_bufsz = block_bufsz;
                    if (!gpu_ibuf || block_bufsz <= 0) {
                        fprintf(stderr, "ERROR: failed to acquire raw ring block.\n");
                        return NULL;
                    }
                }
                if ((uint64_t)block_bufsz < bytes) {
                    fprintf(stderr,
                            "ERROR: complete receive batch does not fit raw block.\n");
                    return NULL;
                }
                for (int i = 0; i < count; ++i) {
                    const uint64_t wr_id = ibv_res_ptr->wc_tmp[i].wr_id;
                    unsigned char *src = (unsigned char *)
                        ibv_res_ptr->sge[
                            wr_id * ibv_res_ptr->recv_nsge].addr;
                    if (this_ptr->param.RdmaDirectGpu != 0) {
                        CUDA_CALL(cudaMemcpy(
                            gpu_ibuf + ((size_t)i * ring_pkt_len),
                            src + RX_STRIP_HEADER_BYTES, ring_pkt_len,
                            cudaMemcpyDeviceToDevice));
                    } else {
                        memcpy(gpu_ibuf + ((size_t)i * ring_pkt_len),
                               src + RX_STRIP_HEADER_BYTES, ring_pkt_len);
                    }
                    if (repost_receive_wr(ibv_res_ptr, wr_id) != 0)
                        return NULL;
                }
                gpu_ibuf += bytes;
                block_bufsz -= (long int)bytes;
                total_recv += count;
                ibv_res_ptr->recv_sum_completed -= count;
                memmove(ibv_res_ptr->wc_tmp,
                        ibv_res_ptr->wc_tmp + count,
                        sizeof(struct ibv_wc) *
                            ibv_res_ptr->recv_sum_completed);
                if (block_bufsz == 0) {
                    if (this_ptr->param.DataSendBuff(
                            (uint64_t)write_bufsz) < 0) {
                        fprintf(stderr,
                                "[ERROR] Failed to publish full raw block.\n");
                        return NULL;
                    }
                    this_ptr->published_receive_packets.fetch_add(
                        (uint64_t)write_bufsz / ring_pkt_len);
                    this_ptr->published_receive_blocks.fetch_add(1);
                    gpu_ibuf = NULL;
                    write_bufsz = 0;
                }
            }
        }
    }

    if (!this_ptr->param.SendOrRecv) {
        const int tail_count = ibv_res_ptr->recv_sum_completed;
        if (tail_count > 0) {
            const uint64_t tail_bytes = (uint64_t)tail_count * ring_pkt_len;
            if (!gpu_ibuf) {
                gpu_ibuf = this_ptr->param.GetBuffPtr(block_bufsz);
                write_bufsz = block_bufsz;
                if (!gpu_ibuf || block_bufsz <= 0) {
                    fprintf(stderr,
                            "ERROR: failed to acquire raw ring block for CQ tail.\n");
                    return NULL;
                }
            }
            if ((uint64_t)block_bufsz < tail_bytes) {
                fprintf(stderr, "ERROR: CQ tail does not fit raw ring block.\n");
                return NULL;
            }
            for (int i = 0; i < tail_count; ++i) {
                const uint64_t wr_id = ibv_res_ptr->wc_tmp[i].wr_id;
                unsigned char *src = (unsigned char *)
                    ibv_res_ptr->sge[wr_id * ibv_res_ptr->recv_nsge].addr;
                if (this_ptr->param.RdmaDirectGpu != 0) {
                    CUDA_CALL(cudaMemcpy(
                        gpu_ibuf + ((size_t)i * ring_pkt_len),
                        src + RX_STRIP_HEADER_BYTES, ring_pkt_len,
                        cudaMemcpyDeviceToDevice));
                } else {
                    memcpy(gpu_ibuf + ((size_t)i * ring_pkt_len),
                           src + RX_STRIP_HEADER_BYTES, ring_pkt_len);
                }
            }
            gpu_ibuf += tail_bytes;
            block_bufsz -= (long int)tail_bytes;
            total_recv += tail_count;
            this_ptr->cq_tail_receive_records.fetch_add(tail_count);
            ibv_res_ptr->recv_sum_completed = 0;
        }

        if (gpu_ibuf && write_bufsz > block_bufsz) {
            const uint64_t valid_bytes =
                (uint64_t)(write_bufsz - block_bufsz);
            if (this_ptr->param.DataSendBuff(valid_bytes) < 0) {
                fprintf(stderr,
                        "[ERROR] Failed to publish final raw block tail.\n");
                return NULL;
            }
            this_ptr->published_receive_packets.fetch_add(
                valid_bytes / ring_pkt_len);
            this_ptr->published_receive_blocks.fetch_add(1);
            if (valid_bytes < (uint64_t)write_bufsz)
                this_ptr->partial_receive_blocks.fetch_add(1);
            printf("[RDMA] Published final raw block: bytes=%" PRIu64
                   ", records=%" PRIu64 "\n",
                   valid_bytes, valid_bytes / ring_pkt_len);
            fflush(stdout);
        }
    }
    return NULL;
}

RoCEv2Dada::~RoCEv2Dada()
{
    Stop();
    if(this->ibv_res) {
        struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)this->ibv_res;
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
        this->ibv_res = NULL;
    }
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
    
    printf("[RoCEv2Dada::Start] Creating pthread...\n");
    fflush(stdout);
    
    stop_requested.store(false);
    cpu_set_t mask;
    int ret = pthread_create(&ibv_res_ptr->tid, NULL, SendRecvThread, (void *)this);
    
    printf("[RoCEv2Dada::Start] pthread_create returned: %d (tid=%lu)\n", ret, (unsigned long)ibv_res_ptr->tid);
    fflush(stdout);
    
    if (ret) { 
        fprintf(stderr, "pthread_create failed: %d\n", ret); 
        fflush(stderr);
        return RDMA_ERROR; 
    }
    thread_started = true;
    
    if(this->param.bind_cpu_id >= 0) {
        printf("[RoCEv2Dada::Start] Setting CPU affinity to core %d...\n", this->param.bind_cpu_id);
        fflush(stdout);
        CPU_ZERO(&mask);
        CPU_SET(this->param.bind_cpu_id, &mask);
        ret = pthread_setaffinity_np(ibv_res_ptr->tid, sizeof(mask), &mask);
        if (ret != 0) {
            fprintf(stderr,
                    "[WARN] Failed to bind RDMA thread to CPU %d: %s\n",
                    this->param.bind_cpu_id, strerror(ret));
        }
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

    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)this->ibv_res;
    stop_requested.store(true);
    int ret = pthread_join(ibv_res_ptr->tid, NULL);
    if (ret != 0) {
        fprintf(stderr, "pthread_join failed: %d (%s)\n", ret, strerror(ret));
        return RDMA_ERROR;
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
    return stats;
}
