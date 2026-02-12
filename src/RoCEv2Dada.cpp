//定义 RoCEv2Dada 类的成员函数，用于通过 RoCEv2 协议进行数据发送和接收
#include <infiniband/verbs.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#include "RoCEv2Dada.h"
#include "ibv_utils.h"
#include "pkt_gen.h"

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

// Constants from original
#define PKT_DATA_SIZE 6414

static int check_send_recv_info(struct ibv_utils_res * ibv_res_ptr, RoCEv2Dada::RdmaParam * Param_ptr)
{
    printf("**********************************************\n");
    printf("Recv Config Information:\n");
    printf("    device_id: %d\n", Param_ptr->device_id);
    printf("    src_mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                ibv_res_ptr->pkt_info.src_mac[0], ibv_res_ptr->pkt_info.src_mac[1],
                ibv_res_ptr->pkt_info.src_mac[2], ibv_res_ptr->pkt_info.src_mac[3],
                ibv_res_ptr->pkt_info.src_mac[4], ibv_res_ptr->pkt_info.src_mac[5]);
    printf("    dst_mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                ibv_res_ptr->pkt_info.dst_mac[0], ibv_res_ptr->pkt_info.dst_mac[1],
                ibv_res_ptr->pkt_info.dst_mac[2], ibv_res_ptr->pkt_info.dst_mac[3],
                ibv_res_ptr->pkt_info.dst_mac[4], ibv_res_ptr->pkt_info.dst_mac[5]);
    uint8_t tmp[4];
    tmp[3] = (ibv_res_ptr->pkt_info.src_ip >> 24) & 0xff;
    tmp[2] = (ibv_res_ptr->pkt_info.src_ip >> 16) & 0xff;
    tmp[1] = (ibv_res_ptr->pkt_info.src_ip >> 8) & 0xff;
    tmp[0] = ibv_res_ptr->pkt_info.src_ip & 0xff;
    printf("    src_ip: %d.%d.%d.%d\n", tmp[0], tmp[1], tmp[2], tmp[3]);
    tmp[3] = (ibv_res_ptr->pkt_info.dst_ip >> 24) & 0xff;
    tmp[2] = (ibv_res_ptr->pkt_info.dst_ip >> 16) & 0xff;
    tmp[1] = (ibv_res_ptr->pkt_info.dst_ip >> 8) & 0xff;
    tmp[0] = ibv_res_ptr->pkt_info.dst_ip & 0xff;
    printf("    dst_ip: %d.%d.%d.%d\n", tmp[0], tmp[1], tmp[2], tmp[3]);
    printf("    src_port: %d\n", ibv_res_ptr->pkt_info.src_port);
    printf("    dst_port: %d\n", ibv_res_ptr->pkt_info.dst_port);
    printf("    RdmaDirectGpu: %d, gpu_id: %d\n", Param_ptr->RdmaDirectGpu, Param_ptr->gpu_id);
    printf("    recv_wr_num: %d send_wr_num: %d send_nsge: %d recv_nsge: %d \n",
        ibv_res_ptr->recv_wr_num , ibv_res_ptr->send_wr_num,
        ibv_res_ptr->send_nsge, ibv_res_ptr->recv_nsge);
    printf("    pkt_size: %d send_n: %d\n", Param_ptr->pkt_size, Param_ptr->send_n);
    printf("**********************************************\n");
    
    // Debug: print validation check values
    printf("[DEBUG] Validation checks:\n");
    printf("  pkt_size=%d (must be > 64): %s\n", Param_ptr->pkt_size, Param_ptr->pkt_size > 64 ? "PASS" : "FAIL");
    printf("  send_n=%d (must be >= 8): %s\n", Param_ptr->send_n, Param_ptr->send_n >= 8 ? "PASS" : "FAIL");
    printf("  gpu_id=%d (must be < 6): %s\n", Param_ptr->gpu_id, Param_ptr->gpu_id < 6 ? "PASS" : "FAIL");
    printf("  device_id=%d (must be < 4): %s\n", Param_ptr->device_id, Param_ptr->device_id < 4 ? "PASS" : "FAIL");
    printf("  bind_cpu_id=%d (must be < 384): %s\n", Param_ptr->bind_cpu_id, Param_ptr->bind_cpu_id < 384 ? "PASS" : "FAIL");
    
    if(Param_ptr->pkt_size <= 64 || Param_ptr->send_n < 8 || Param_ptr->gpu_id >= 6
        || Param_ptr->device_id >= 4 || Param_ptr->bind_cpu_id >= 384) return -1;
    return 0;
}

static int post_direct_recvs(struct ibv_utils_res *ibv_res_ptr, int recv_num)
{
    for (int i = 0; i < recv_num; i++) {
        ibv_res_ptr->recv_wr->wr_id = i;
        ibv_res_ptr->recv_wr->sg_list = &ibv_res_ptr->sge[i * ibv_res_ptr->recv_nsge];
        ibv_res_ptr->recv_wr->num_sge = ibv_res_ptr->recv_nsge;
        ibv_res_ptr->recv_wr->next = NULL;
        if (ibv_post_recv(ibv_res_ptr->qp, ibv_res_ptr->recv_wr, &ibv_res_ptr->bad_recv_wr) < 0) return -1;
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
{
    printf("[RoCEv2Dada] Constructor started\n");
    fflush(stdout);
    uint8_t tmp[4];
    int ret = 0;
    memcpy(&this->param, &Param, sizeof(Param));
    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)malloc(sizeof(struct ibv_utils_res));
    this->ibv_res = (void *)ibv_res_ptr;
    memset(ibv_res_ptr, 0, sizeof(struct ibv_utils_res));
    
    printf("[RoCEv2Dada] Parsing network parameters...\n");
    
    sscanf(this->param.SAddr, "%hhd.%hhd.%hhd.%hhd", &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
    ibv_res_ptr->pkt_info.src_ip = (tmp[3] << 24) | (tmp[2] << 16) | (tmp[1] << 8) | tmp[0];
    sscanf(this->param.SMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                               &ibv_res_ptr->pkt_info.src_mac[0], &ibv_res_ptr->pkt_info.src_mac[1],
                               &ibv_res_ptr->pkt_info.src_mac[2], &ibv_res_ptr->pkt_info.src_mac[3],
                               &ibv_res_ptr->pkt_info.src_mac[4], &ibv_res_ptr->pkt_info.src_mac[5]);
    sscanf(this->param.DAddr, "%hhd.%hhd.%hhd.%hhd", &tmp[0], &tmp[1], &tmp[2], &tmp[3]);
    ibv_res_ptr->pkt_info.dst_ip = (tmp[3] << 24) | (tmp[2] << 16) | (tmp[1] << 8) | tmp[0];
    sscanf(this->param.DMacAddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                               &ibv_res_ptr->pkt_info.dst_mac[0], &ibv_res_ptr->pkt_info.dst_mac[1],
                               &ibv_res_ptr->pkt_info.dst_mac[2], &ibv_res_ptr->pkt_info.dst_mac[3],
                               &ibv_res_ptr->pkt_info.dst_mac[4], &ibv_res_ptr->pkt_info.dst_mac[5]);
    sscanf(this->param.src_port, "%hd", &ibv_res_ptr->pkt_info.src_port);
    sscanf(this->param.dst_port, "%hd", &ibv_res_ptr->pkt_info.dst_port);
    
    printf("[RoCEv2Dada] Network params parsed: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d\n",
           tmp[0], tmp[1], tmp[2], tmp[3], ibv_res_ptr->pkt_info.src_port,
           tmp[0], tmp[1], tmp[2], tmp[3], ibv_res_ptr->pkt_info.dst_port);
    fflush(stdout);
    
    ibv_res_ptr->pkt_size = Param.pkt_size;
    ibv_res_ptr->poll_n = 8;
    ibv_res_ptr->recv_completed = 0;
    ibv_res_ptr->recv_sum_completed = 0;
    ibv_res_ptr->recv_sum = 0;
    
    // Calculate work queue depth
    // DirectToRing mode: use send_n (one batch worth of WRs)
    // Normal mode: use smaller queue depth (typical NIC limit is ~16K)
    //   We only need enough to keep pipeline full - use send_n or 8192, whichever is smaller
    int work_num;
    if (!this->param.SendOrRecv && this->param.DirectToRing) {
        work_num = this->param.send_n;
    } else if (!this->param.SendOrRecv) {
        // Receiver with internal buffer: limit queue depth to reasonable size
        work_num = (this->param.send_n < 2048) ? (this->param.send_n * 4) : 8192;
    } else {
        // Sender
        work_num = this->param.send_n * 4;
    }
    printf("[RoCEv2Dada] Configured work_num=%d (DirectToRing=%d, send_n=%d)\n", 
           work_num, this->param.DirectToRing, this->param.send_n);
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
    
    uint32_t buf_size = (ibv_res_ptr->pkt_size + PKT_HEAD_LEN) * work_num;

    if (!this->param.SendOrRecv && this->param.DirectToRing) {
        ibv_res_ptr->mem_buf = NULL;
    } else {
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

        ret = register_memory(ibv_res_ptr, ibv_res_ptr->mem_buf, buf_size, (ibv_res_ptr->pkt_size + PKT_HEAD_LEN));
        if (ret < 0) { printf("Failed to register memory.\n"); fflush(stdout); return; }
        printf("Register memory successfully (buf_size=%u bytes).\n", buf_size);
        fflush(stdout);
    }
    
    if(this->param.SendOrRecv) {
        for(int k = 0; k < work_num; k++) {
            struct udp_pkt *pkt = (struct udp_pkt *)((uint8_t *)ibv_res_ptr->mem_buf + k * (ibv_res_ptr->pkt_size + PKT_HEAD_LEN));
            set_dest_mac(pkt, ibv_res_ptr->pkt_info.dst_mac);
            set_src_mac(pkt, ibv_res_ptr->pkt_info.src_mac);
            set_eth_type(pkt, (uint8_t *)"\x08\x00");
            set_src_ip(pkt, (uint8_t *)(&ibv_res_ptr->pkt_info.src_ip));
            set_dst_ip(pkt, (uint8_t *)(&ibv_res_ptr->pkt_info.dst_ip));
            set_udp_src_port(pkt, ibv_res_ptr->pkt_info.src_port);
            set_udp_dst_port(pkt, ibv_res_ptr->pkt_info.dst_port);
            set_pkt_len(pkt, ibv_res_ptr->pkt_size + PKT_HEAD_LEN - 34);
            ret = this->param.WritSendBuff(pkt->payload, ibv_res_ptr->pkt_size);
            if (ret < 0) { printf("Failed to WritSendBuff.\n"); return; }
        }
        ret = ib_send_pkg(ibv_res_ptr, 0, work_num);
        if (ret < 0) { printf("Failed to send pkts.\n"); return; }
    } else {
        ret = create_flow(ibv_res_ptr, &ibv_res_ptr->pkt_info);
        if (ret < 0) { 
            printf("========================================\n");
            printf("⚠️  WARNING: Flow Steering Failed\n");
            printf("========================================\n");
            printf("Flow steering creation failed. This can happen when:\n");
            printf("  - NIC doesn't support steering (older hardware)\n");
            printf("  - Flow table is full (other processes using it)\n");
            printf("  - Driver limitations\n");
            printf("\n");
            printf("📌 CONTINUING WITHOUT FLOW STEERING\n");
            printf("   The NIC will receive ALL packets (promiscuous mode)\n");
            printf("   You must filter by MAC/IP/Port in software.\n");
            printf("   Performance may be lower but functionality preserved.\n");
            printf("========================================\n");
            // Continue without flow - will receive all packets on this QP
            // Application must filter unwanted packets in software
        } else {
            printf("Create flow successfully.\n");
        }
        
        printf("[RoCEv2Dada] Posting receive work requests... (work_num=%d)\n", work_num);
        if (!this->param.DirectToRing) {
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
                    break;
                }
                posted++;
                if (i % 1024 == 0 && i > 0) {
                    printf("  Posted %d/%d receive WRs...\r", i, work_num);
                    fflush(stdout);
                }
            }
            printf("  Posted %d/%d receive WRs... Done!\n", posted, work_num);
            fflush(stdout);
        } else {
            printf("  DirectToRing mode: skipping recv WR posting\n");
            fflush(stdout);
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
    char * cpu_data = NULL;
    // pkt_size already includes header (passed from run_demo.sh as PKT_HEADER+PKT_DATA)
    int pkt_len = ibv_res_ptr->pkt_size;
    int send_idx = 0;
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[80];
    
    // 初始化时间戳，避免第一次计算时使用未初始化的值
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
    
    if (this_ptr->param.debug_mode) {
        printf("[DEBUG] Entering main receive loop...\n");
        fflush(stdout);
    }
    
    while (1) {
        pthread_testcancel();
        if(this_ptr->param.SendOrRecv) {
            while(total_recv_pre < this_ptr->param.send_n) {
                total_recv_pre += ibv_poll_cq(ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
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
            // 接收模式
            if (this_ptr->param.DirectToRing && ibv_res_ptr->mr) {
                if (this_ptr->param.debug_mode) {
                    printf("[DEBUG] Using DirectToRing path\n");
                    fflush(stdout);
                }
                int recv_num = this_ptr->param.send_n;
                // pkt_size already includes header
                int pkt_len = ibv_res_ptr->pkt_size;
                long int direct_bufsz = 0;
                char *direct_base = NULL;

                if (!ibv_res_ptr->recv_ready) {
                    direct_base = this_ptr->param.GetBuffPtr(direct_bufsz);
                    if (!direct_base || direct_bufsz < (long int)(recv_num * pkt_len)) {
                        printf("ERROR: Direct recv buffer invalid.\n");
                        return NULL;
                    }
                    for (int i = 0; i < recv_num; i++) {
                        ibv_res_ptr->sge[i].addr = (uint64_t)(direct_base + i * pkt_len);
                        ibv_res_ptr->sge[i].length = pkt_len;
                        ibv_res_ptr->sge[i].lkey = ibv_res_ptr->mr->lkey;
                    }
                    if (post_direct_recvs(ibv_res_ptr, recv_num) < 0) {
                        printf("ERROR: failed to post direct recv WRs.\n");
                        return NULL;
                    }
                    ibv_res_ptr->recv_ready = true;
                }

                ibv_res_ptr->recv_completed = ibv_poll_cq(ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
                if (ibv_res_ptr->recv_completed > 0) {
                    ibv_res_ptr->recv_sum_completed += ibv_res_ptr->recv_completed;
                    if (ibv_res_ptr->recv_sum_completed >= recv_num) {
                        ret = this_ptr->param.DataSendBuff();
                        if (ret < 0) { printf("ERROR: Direct DataSendBuff failed.\n"); return NULL; }
                        ibv_res_ptr->recv_sum_completed = 0;
                        ibv_res_ptr->recv_ready = false;
                    }
                } else if (ibv_res_ptr->recv_completed < 0) {
                    printf("ERROR: SendRecvThread Failed to recv.\n");
                    return NULL;
                }
                continue;
            }

            // 正常接收模式（非DirectToRing）
            static bool first_normal_path_log = true;
            if (first_normal_path_log) {
                printf("[RDMA] Using normal receive mode (with copy to ring buffer)\n");
                fflush(stdout);
                first_normal_path_log = false;
            }
            
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts_now);
            ns_elapsed = ELAPSED_US(ts_start, ts_now);
            if((ns_elapsed) > 1000 * 1000 * 1) {
                ts_start = ts_now;
                if(total_recv != total_recv_pre) {
                    double bandwidth = MEASURE_BANDWIDTH(((total_recv - total_recv_pre) * pkt_len), ns_elapsed);
                    time(&rawtime);
                    timeinfo = localtime(&rawtime);
                    timeinfo->tm_hour += 8;
                    strftime(time_buffer, sizeof(time_buffer), "year:%Y,month:%m,day:%d,hours:%H,minites:%M,second:%S", timeinfo);
                    printf("NowTime:%s,gpu_id:%d,total_recv:%-8luKB,Process Bandwidth:%6.3f Gbps,cost time:%lums\n",
                           time_buffer, this_ptr->param.gpu_id, (unsigned long)(total_recv * pkt_len / 1024), bandwidth, (unsigned long)(ns_elapsed / 1000));
                    total_recv_pre = 0;
                    total_recv = 0;
                } else {
                    printf("total_recv: %-10lu Bandwidth: 0 Gbps, cost time us_elapsed: %lu \n", (unsigned long)total_recv, (unsigned long)ns_elapsed);
                }
            }
            
            // Calculate space needed for next batch
            long int bytes_needed = (long int)(this_ptr->param.send_n * pkt_len);
            
            // Get new buffer if current buffer is empty OR insufficient for next batch
            if(block_bufsz <= 0 || block_bufsz < bytes_needed) {
                if (this_ptr->param.debug_mode && block_bufsz > 0 && block_bufsz < bytes_needed) {
                    printf("[DEBUG] Insufficient space (%ld < %ld), getting new block\n", 
                           block_bufsz, bytes_needed);
                    fflush(stdout);
                }
                gpu_ibuf = this_ptr->param.GetBuffPtr(block_bufsz);
                cpu_data = gpu_ibuf;
                write_bufsz = block_bufsz;
                if(!gpu_ibuf || !block_bufsz) {
                    printf("ERROR: SendRecvThread Failed to GetBuffPtr.gpu_ibuf: %p, block_bufsz:%ld\n", (void*)gpu_ibuf, block_bufsz);
                    return NULL;
                }
            }
            ibv_res_ptr->recv_completed = ibv_poll_cq(ibv_res_ptr->cq, ibv_res_ptr->poll_n, ibv_res_ptr->wc);
            
            // Debug polling info (only in debug mode)
            if (this_ptr->param.debug_mode) {
                static int poll_count = 0;
                static int no_data_count = 0;
                poll_count++;
                
                if (ibv_res_ptr->recv_completed == 0) {
                    no_data_count++;
                } else {
                    no_data_count = 0;
                }
                
                if (poll_count % 100000 == 0) {
                    printf("[DEBUG] Polled %d times, last result: %d completions, sum=%d\n", 
                           poll_count, ibv_res_ptr->recv_completed, ibv_res_ptr->recv_sum_completed);
                    fflush(stdout);
                }
            }
            
            if(ibv_res_ptr->recv_completed > 0) {
                if (this_ptr->param.debug_mode) {
                    printf("[DEBUG] Received %d completions (sum=%d/%d)\n", 
                           ibv_res_ptr->recv_completed, ibv_res_ptr->recv_sum_completed, 
                           this_ptr->param.send_n);
                    fflush(stdout);
                }
                ibv_res_ptr->recv_sum_completed += ibv_res_ptr->recv_completed;
            if(ibv_res_ptr->recv_sum_completed >= this_ptr->param.send_n) {
                    // Batch complete, process data
                    if (this_ptr->param.debug_mode) {
                        printf("[DEBUG] Processing batch: %d completions\n", this_ptr->param.send_n);
                        fflush(stdout);
                    }
                    
                    if(this_ptr->param.RdmaDirectGpu != 0) {
                        CUDA_CALL(cudaMemcpy(gpu_ibuf,
                                           (void *)ibv_res_ptr->sge[ibv_res_ptr->wc_tmp[0].wr_id*ibv_res_ptr->recv_nsge].addr,
                                           this_ptr->param.send_n * pkt_len, cudaMemcpyDeviceToDevice));
                    } else {
                        memcpy(gpu_ibuf,
                               (void *)ibv_res_ptr->sge[ibv_res_ptr->wc_tmp[0].wr_id*ibv_res_ptr->recv_nsge].addr,
                               this_ptr->param.send_n * pkt_len);
                    }
                    
                    uint64_t bytes_written = this_ptr->param.send_n * pkt_len;
                    
                    gpu_ibuf += bytes_written;
                    block_bufsz -= (long int)bytes_written;
                    
                    // 递减写入计数
                    if (this_ptr->param.DecrementWriteCount) {
                        this_ptr->param.DecrementWriteCount();
                    }
                    
                    // 检查block是否已满
                    bool is_full = false;
                    if (this_ptr->param.IsBlockFull) {
                        is_full = this_ptr->param.IsBlockFull();
                    }
                    
                    if(is_full) {
                        ret = this_ptr->param.DataSendBuff();
                        if(ret < 0) { 
                            fprintf(stderr, "[ERROR] Failed to mark block as written\n"); 
                            return NULL; 
                        }
                        
                        // Reset block_bufsz to 0 to force getting a new block next iteration
                        block_bufsz = 0;
                    }
                    for(int i = 0; i < this_ptr->param.send_n; i++) {
                        ibv_res_ptr->recv_wr->wr_id = ibv_res_ptr->wc_tmp[i].wr_id;
                        ibv_res_ptr->recv_wr->sg_list = &ibv_res_ptr->sge[ibv_res_ptr->wc_tmp[i].wr_id*ibv_res_ptr->recv_nsge];
                        ibv_res_ptr->recv_wr->num_sge = ibv_res_ptr->recv_nsge;
                        ibv_res_ptr->recv_wr->next = NULL;
                        ibv_post_recv(ibv_res_ptr->qp, ibv_res_ptr->recv_wr, &ibv_res_ptr->bad_recv_wr);
                    }
                    ibv_res_ptr->recv_sum_completed -= this_ptr->param.send_n;
                    memcpy(ibv_res_ptr->wc, ibv_res_ptr->wc_tmp + this_ptr->param.send_n,
                           sizeof(struct ibv_wc) * (ibv_res_ptr->recv_sum_completed));
                    memcpy(ibv_res_ptr->wc_tmp, ibv_res_ptr->wc,
                           sizeof(struct ibv_wc) * (ibv_res_ptr->recv_sum_completed));
                    total_recv += this_ptr->param.send_n;
                }
            } else if (ibv_res_ptr->recv_completed < 0) {
                printf("ERROR: SendRecvThread Failed to recv.\n");
                return NULL;
            }
        }
    }
    return NULL;
}

RoCEv2Dada::~RoCEv2Dada()
{
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
        }
        destroy_ib_res(ibv_res_ptr);
        close_ib_device(ibv_res_ptr);
        free(ibv_res_ptr);
        this->ibv_res = NULL;
    }
}

void * RoCEv2Dada::GetIbvRes() const { return this->ibv_res; }

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
    
    if(this->param.DirectToRing && !ibv_res_ptr->mr) { 
        printf("RoCEv2Dada::Start error: direct MR not set.\n"); 
        fflush(stdout);
        return RDMA_ERROR; 
    }
    
    if(!ibv_res_ptr->init_flag) { 
        printf("RoCEv2Dada::Start error: init_flag not set!\n"); 
        fflush(stdout);
        return RDMA_ERROR; 
    }
    
    printf("[RoCEv2Dada::Start] Creating pthread...\n");
    fflush(stdout);
    
    cpu_set_t mask;
    int ret = pthread_create(&ibv_res_ptr->tid, NULL, SendRecvThread, (void *)this);
    
    printf("[RoCEv2Dada::Start] pthread_create returned: %d (tid=%lu)\n", ret, (unsigned long)ibv_res_ptr->tid);
    fflush(stdout);
    
    if (ret) { 
        fprintf(stderr, "pthread_create failed: %d\n", ret); 
        fflush(stderr);
        return RDMA_ERROR; 
    }
    
    if(this->param.bind_cpu_id >= 0) {
        printf("[RoCEv2Dada::Start] Setting CPU affinity to core %d...\n", this->param.bind_cpu_id);
        fflush(stdout);
        CPU_ZERO(&mask);
        CPU_SET(this->param.bind_cpu_id, &mask);
        pthread_setaffinity_np(ibv_res_ptr->tid, sizeof(mask), &mask);
    }
    
    printf("[RoCEv2Dada::Start] Detaching thread...\n");
    fflush(stdout);
    pthread_detach(ibv_res_ptr->tid);
    
    printf("[RoCEv2Dada::Start] Success, returning RDMA_OK\n");
    fflush(stdout);
    return RDMA_OK;
}

int RoCEv2Dada::SetDirectMr(struct ibv_mr *mr)
{
    if (!mr || !this->ibv_res) return RDMA_ERROR;
    struct ibv_utils_res * ibv_res_ptr = (struct ibv_utils_res *)this->ibv_res;
    ibv_res_ptr->mr = mr;
    ibv_res_ptr->mr_external = true;
    this->param.DirectToRing = 1;
    return RDMA_OK;
}
