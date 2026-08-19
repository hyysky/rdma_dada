#pragma once

#include <infiniband/verbs.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

// Internal libibverbs resource helpers used by the RoCE adapter.
#define POLL_N 8

struct ibv_pkt_info { 
    uint8_t src_mac[6]; 
    uint8_t dst_mac[6]; 
    uint32_t src_ip; 
    uint32_t dst_ip; 
    uint16_t src_port; 
    uint16_t dst_port; 
};

struct ibv_utils_res {
    struct ibv_device *dev;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *recv_cc;
    struct ibv_qp *qp;
    struct ibv_sge *sge;
    struct ibv_recv_wr *recv_wr;
    struct ibv_wc *wc;
    struct ibv_recv_wr *bad_recv_wr;
    int recv_wr_num;
    int recv_nsge;
    unsigned int poll_n;
    unsigned int pkt_size;
    void *pool_ptr;
    struct ibv_pkt_info pkt_info;
    bool init_flag;
};

void ibv_utils_info(const char *msg);
void ibv_utils_error(const char *msg);
void ibv_utils_warn(const char *msg);
int open_ib_device(uint8_t device_id, struct ibv_utils_res *ib_res);
int open_ib_device_by_name(const char *device_name);
int create_ib_res(struct ibv_utils_res *ib_res, int recv_wr_num);
int init_ib_res(struct ibv_utils_res *ib_res);
int create_flow(struct ibv_utils_res *ib_res, struct ibv_pkt_info *pkt_info);
int destroy_ib_res(struct ibv_utils_res *ib_res);
int close_ib_device(struct ibv_utils_res *ib_res);
