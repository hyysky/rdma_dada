// Adapted from libsrc/udp_rdma/src/ibv_utils.cpp
#include "ibv_utils.h"

void ibv_utils_info(const char *msg) { fprintf(stdout, "IBV-UTILS INFO: %s \n", msg); }
void ibv_utils_error(const char *msg) { fprintf(stderr, "IBV-UTILS ERROR: %s \n", msg); }
void ibv_utils_warn(const char *msg) { fprintf(stderr, "IBV-UTILS WARN: %s \n", msg); }

int open_ib_device(uint8_t device_id, struct ibv_utils_res *ib_res)
{
    struct ibv_device **ib_global_devs;
    int num_ib_devices;
    ib_global_devs = ibv_get_device_list(&num_ib_devices);
    if (!ib_global_devs) { ibv_utils_error("Failed to get IB devices list."); return -1; }
    if(device_id > num_ib_devices) { ibv_utils_error("Invalid device id."); return -1; }
    ib_res->dev = ib_global_devs[device_id];
    ib_res->context = ibv_open_device(ib_res->dev);
    if(!ib_res->context) { ibv_utils_error("Failed to open IB device."); return -1; }
    return 0;
}

int open_ib_device_by_name(const char *device_name) { (void)device_name; return -1; }

int create_ib_res(struct ibv_utils_res *ib_res, int send_wr_num, int recv_wr_num)
{
    if(ib_res->recv_nsge == 0) ib_res->recv_nsge = 4;
    if(ib_res->send_nsge == 0) ib_res->send_nsge = 4;
    int wr_num = send_wr_num > recv_wr_num ? send_wr_num : recv_wr_num;
    ib_res->send_wr_num = send_wr_num;
    ib_res->recv_wr_num = recv_wr_num;
    ib_res->pd = ibv_alloc_pd(ib_res->context);
    if (!ib_res->pd) { ibv_utils_error("Failed to allocate PD."); return -1; }
    ib_res->cq = ibv_create_cq(ib_res->context, wr_num, NULL, NULL, 0);
    if(!ib_res->cq){ ibv_utils_error("Couldn't create CQ."); return -2; }
    struct ibv_qp_init_attr qp_init_attr = { .qp_context = NULL, .send_cq = ib_res->cq, .recv_cq = ib_res->cq, .cap = { .max_send_wr = (uint32_t)send_wr_num, .max_recv_wr = (uint32_t)recv_wr_num, .max_send_sge = (uint32_t)ib_res->send_nsge, .max_recv_sge = (uint32_t)ib_res->recv_nsge, }, .qp_type = IBV_QPT_RAW_PACKET, };
    ib_res->qp = ibv_create_qp(ib_res->pd, &qp_init_attr);
    if(!ib_res->qp){ ibv_utils_error("Couldn't create QP."); return -4; }
    int max_sge = ib_res->send_nsge > ib_res->recv_nsge ? ib_res->send_nsge : ib_res->recv_nsge;
    ib_res->sge = (struct ibv_sge *)malloc(wr_num * sizeof(struct ibv_sge)*max_sge);
    if(!ib_res->sge) { ibv_utils_error("Failed to allocate memory for sge."); return -5; }
    if(ib_res->send_wr_num > 0) { ib_res->send_wr = (struct ibv_send_wr *)malloc(ib_res->send_wr_num * sizeof(struct ibv_send_wr)); if(!ib_res->send_wr){ ibv_utils_error("Failed to allocate memory for send_wr."); return -6; } }
    if(ib_res->recv_wr_num > 0) { ib_res->recv_wr = (struct ibv_recv_wr *)malloc(ib_res->recv_wr_num * sizeof(struct ibv_recv_wr)); if(!ib_res->recv_wr){ ibv_utils_error("Failed to allocate memory for recv_wr."); return -7; } }
    ib_res->wc = (struct ibv_wc *)malloc(wr_num * sizeof(struct ibv_wc)); if(!ib_res->wc){ ibv_utils_error("Failed to allocate memory for wc."); return -8; }
    ib_res->wc_tmp = (struct ibv_wc *)malloc(wr_num * sizeof(struct ibv_wc)); if(!ib_res->wc_tmp){ ibv_utils_error("Failed to allocate memory for wc_tmp."); return -8; }
    return 0;
}

int init_ib_res(struct ibv_utils_res *ib_res)
{
    struct ibv_qp_attr qp_attr;
    int qp_flags;
    
    // Check port status before QP initialization
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ib_res->context, 1, &port_attr) == 0) {
        printf("[init_ib_res] Port 1 status: %s\n", 
               port_attr.state == IBV_PORT_ACTIVE ? "ACTIVE" :
               port_attr.state == IBV_PORT_DOWN ? "DOWN" : "UNKNOWN");
        if (port_attr.state != IBV_PORT_ACTIVE) {
            fprintf(stderr, "WARNING: Port 1 is not ACTIVE! Flow steering will likely fail.\n");
        }
    }
    
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_flags = IBV_QP_STATE | IBV_QP_PORT;
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1;
    if(ibv_modify_qp(ib_res->qp, &qp_attr, qp_flags) < 0) { 
        ibv_utils_error("Failed to init qp."); 
        return -1; 
    }
    printf("[init_ib_res] QP state: RESET -> INIT\n");
    
    memset(&qp_attr, 0, sizeof(qp_attr)); 
    qp_flags = IBV_QP_STATE; 
    qp_attr.qp_state = IBV_QPS_RTR; 
    if(ibv_modify_qp(ib_res->qp, &qp_attr, qp_flags) < 0) { 
        ibv_utils_error("Failed to modify qp to RTR."); 
        return -2; 
    }
    printf("[init_ib_res] QP state: INIT -> RTR\n");
    
    memset(&qp_attr, 0, sizeof(qp_attr)); 
    qp_flags = IBV_QP_STATE; 
    qp_attr.qp_state = IBV_QPS_RTS; 
    if(ibv_modify_qp(ib_res->qp, &qp_attr, qp_flags) < 0) { 
        ibv_utils_error("Failed to modify qp to RTS."); 
        return -3; 
    }
    printf("[init_ib_res] QP state: RTR -> RTS ✓\n");
    
    // Verify QP is in RTS state
    struct ibv_qp_attr qp_attr_query;
    struct ibv_qp_init_attr qp_init_attr_query;
    if (ibv_query_qp(ib_res->qp, &qp_attr_query, IBV_QP_STATE, &qp_init_attr_query) == 0) {
        printf("[init_ib_res] Verified QP state: %s\n",
               qp_attr_query.qp_state == IBV_QPS_RTS ? "RTS (Ready)" : "NOT RTS!");
    }
    
    return 0;
}

int register_memory(struct ibv_utils_res *ib_res, void *addr, size_t total_length, size_t chunck_size)
{
    ib_res->mr = ibv_reg_mr(ib_res->pd, addr, total_length, IBV_ACCESS_LOCAL_WRITE);
    if(!ib_res->mr){ ibv_utils_error("Failed to register memory."); return -1; }
    int max_sge = ib_res->send_nsge > ib_res->recv_nsge ? ib_res->send_nsge : ib_res->recv_nsge;
    int wr_num = total_length / chunck_size / max_sge;
    if((wr_num != ib_res->send_wr_num) && (wr_num != ib_res->recv_wr_num)){ ibv_utils_error("The number of wr is not equal to the number of sge."); return -2; }
    char *addr_char = (char*)addr;  // Cast to char* for pointer arithmetic
    for(int i=0; i< wr_num * max_sge; i++){ ib_res->sge[i].addr = (uint64_t)(addr_char + i*chunck_size); ib_res->sge[i].length = chunck_size ; ib_res->sge[i].lkey = ib_res->mr->lkey; }
    for(int i = 0; i < wr_num; i++){ ib_res->wc[i].wr_id = i; }
    return 0;
}

int create_flow(struct ibv_utils_res *ib_res, struct ibv_pkt_info *pkt_info)
{
    struct ibv_qp *qp = ib_res->qp;
    // Remove __attribute__((packed)) as ibverbs structures are already properly aligned
    struct raw_eth_flow_attr { 
        struct ibv_flow_attr attr; 
        struct ibv_flow_spec_eth spec_eth; 
        struct ibv_flow_spec_ipv4 spec_ipv4; 
        struct ibv_flow_spec_tcp_udp spec_udp; 
    } flow_attr = { 
        .attr = { 
            .comp_mask = 0, 
            .type = IBV_FLOW_ATTR_NORMAL, 
            .size = sizeof(flow_attr), 
            .priority = 0, 
            .num_of_specs = 3, 
            .port = 1, 
            .flags = 0, 
        }, 
        .spec_eth = { 
            .type = IBV_FLOW_SPEC_ETH, 
            .size = sizeof(struct ibv_flow_spec_eth), 
        }, 
        .spec_ipv4 = { 
            .type = IBV_FLOW_SPEC_IPV4, 
            .size = sizeof(struct ibv_flow_spec_ipv4), 
        }, 
        .spec_udp = { 
            .type = IBV_FLOW_SPEC_UDP, 
            .size = sizeof(struct ibv_flow_spec_tcp_udp), 
        } 
    };
    // Set Ethernet header matching (eth_type REQUIRED for RAW_PACKET)
    flow_attr.spec_eth.val.ether_type = htons(0x0800);  // IPv4
    flow_attr.spec_eth.mask.ether_type = 0xFFFF;        // Must match exactly
    memcpy(flow_attr.spec_eth.val.dst_mac, pkt_info->dst_mac, 6);
    memcpy(flow_attr.spec_eth.val.src_mac, pkt_info->src_mac, 6);
    memset(flow_attr.spec_eth.mask.dst_mac, 0xFF, 6);  // Match all MAC bits
    memset(flow_attr.spec_eth.mask.src_mac, 0xFF, 6);
    
    // Set IPv4 header matching
    flow_attr.spec_ipv4.val.dst_ip = pkt_info->dst_ip;
    flow_attr.spec_ipv4.val.src_ip = pkt_info->src_ip;
    flow_attr.spec_ipv4.mask.dst_ip = 0xFFFFFFFF;       // Match full IP
    flow_attr.spec_ipv4.mask.src_ip = 0xFFFFFFFF;
    
    // Set UDP port matching
    flow_attr.spec_udp.val.dst_port = htons(pkt_info->dst_port);
    flow_attr.spec_udp.val.src_port = htons(pkt_info->src_port);
    flow_attr.spec_udp.mask.dst_port = 0xFFFF;          // Match full port
    flow_attr.spec_udp.mask.src_port = 0xFFFF;
    
    // Debug: print flow rule details before creation
    printf("[create_flow] Attempting to create flow steering rule:\n");
    printf("  Port: %d, Priority: %d, Num specs: %d\n", 
           flow_attr.attr.port, flow_attr.attr.priority, flow_attr.attr.num_of_specs);
    printf("  ETH: ether_type=0x%04x (IPv4=%s)\n", 
           ntohs(flow_attr.spec_eth.val.ether_type),
           ntohs(flow_attr.spec_eth.val.ether_type) == 0x0800 ? "Yes" : "No");
    printf("       dst_mac=%02x:%02x:%02x:%02x:%02x:%02x src_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           pkt_info->dst_mac[0], pkt_info->dst_mac[1], pkt_info->dst_mac[2],
           pkt_info->dst_mac[3], pkt_info->dst_mac[4], pkt_info->dst_mac[5],
           pkt_info->src_mac[0], pkt_info->src_mac[1], pkt_info->src_mac[2],
           pkt_info->src_mac[3], pkt_info->src_mac[4], pkt_info->src_mac[5]);
    uint8_t *sip = (uint8_t*)&pkt_info->src_ip;
    uint8_t *dip = (uint8_t*)&pkt_info->dst_ip;
    printf("  IPv4: %d.%d.%d.%d -> %d.%d.%d.%d\n",
           sip[0], sip[1], sip[2], sip[3],
           dip[0], dip[1], dip[2], dip[3]);
    printf("  UDP: port %d -> %d\n", pkt_info->src_port, pkt_info->dst_port);
    
    struct ibv_flow *flow = ibv_create_flow(qp, &flow_attr.attr);
    if(!flow){ 
        fprintf(stderr, "\n[create_flow] ❌ ERROR: ibv_create_flow failed\n");
        fprintf(stderr, "  errno=%d (%s)\n", errno, strerror(errno));
        fprintf(stderr, "\n  Common errno=22 causes for RAW_PACKET QP:\n");
        if (errno == 22) {
            fprintf(stderr, "    1. eth_type not set in flow spec (must be 0x0800 for IPv4)\n");
            fprintf(stderr, "    2. MAC mask missing (RAW_PACKET requires explicit masks)\n");
            fprintf(stderr, "    3. Port not in ACTIVE state\n");
            fprintf(stderr, "    4. Flow specs order incorrect (must be: ETH -> IP -> UDP)\n");
        }
        fprintf(stderr, "\n  Debug info:\n");
        fprintf(stderr, "    Port: %d, Priority: %d, Specs: %d\n", 
                flow_attr.attr.port, flow_attr.attr.priority, flow_attr.attr.num_of_specs);
        ibv_utils_error("Couldn't attach steering flow.");
        return -1;
    }
    printf("[create_flow] ✓ Flow steering rule created successfully\n");
    return 0;
}

int ib_send(struct ibv_utils_res *ibv_res)
{
    memset(ibv_res->send_wr, 0, sizeof(struct ibv_send_wr));
    for(int i = 0; i < ibv_res->send_wr_num; i++){
        ibv_res->send_wr[i].wr_id = i;
        ibv_res->send_wr[i].sg_list = &ibv_res->sge[i*ibv_res->send_nsge];
        ibv_res->send_wr[i].num_sge = ibv_res->send_nsge;
        ibv_res->send_wr[i].next = (i == ibv_res->send_wr_num - 1) ? NULL : &ibv_res->send_wr[i+1];
        ibv_res->send_wr[i].opcode = IBV_WR_SEND;
        ibv_res->send_wr[i].send_flags |= IBV_SEND_SIGNALED;
    }
    int state = ibv_post_send(ibv_res->qp, ibv_res->send_wr, &ibv_res->bad_send_wr);
    if(state < 0){ ibv_utils_error("Failed to post send."); return -1; }
    return 0;
}

int ib_recv(struct ibv_utils_res *ibv_res)
{
    if(ibv_res->recv_completed > 0){ for(int i = 0; i < ibv_res->recv_completed; i++){ ibv_res->recv_wr->wr_id = ibv_res->wc[i].wr_id; ibv_res->recv_wr->sg_list = &ibv_res->sge[ibv_res->wc[i].wr_id*ibv_res->recv_nsge]; ibv_res->recv_wr->num_sge = ibv_res->recv_nsge; ibv_res->recv_wr->next = NULL; ibv_post_recv(ibv_res->qp, ibv_res->recv_wr, &ibv_res->bad_recv_wr); } }
    ibv_res->recv_completed = ibv_poll_cq(ibv_res->cq, ibv_res->poll_n, ibv_res->wc);
    return ibv_res->recv_completed;
}

int destroy_ib_res(struct ibv_utils_res *ib_res)
{
    int ret = 0;
    // 正确的销毁顺序：QP -> MR -> CQ -> PD
    
    // 1. 销毁QP（依赖CQ和PD）
    if (ib_res->qp) {
        if (ibv_destroy_qp(ib_res->qp) != 0) {
            ibv_utils_error("Failed to destroy QP");
            ret = -1;
        }
    }
    
    // 2. 注销MR（依赖PD）
    if (ib_res->mr && !ib_res->mr_external) {
        if (ibv_dereg_mr(ib_res->mr) != 0) {
            ibv_utils_error("Failed to deregister MR");
            ret = -1;
        }
    }
    
    // 3. 销毁CQ（依赖PD）
    if (ib_res->cq) {
        if (ibv_destroy_cq(ib_res->cq) != 0) {
            ibv_utils_error("Failed to destroy CQ");
            ret = -1;
        }
    }
    
    // 4. 释放PD（最后释放）
    if (ib_res->pd) {
        if (ibv_dealloc_pd(ib_res->pd) != 0) {
            ibv_utils_error("Failed to deallocate PD");
            ret = -1;
        }
    }
    
    // 释放内存
    free(ib_res->sge);
    if(ib_res->send_wr_num > 0) free(ib_res->send_wr);
    if(ib_res->recv_wr_num > 0) free(ib_res->recv_wr);
    free(ib_res->wc);
    free(ib_res->wc_tmp);
    
    return ret;
}

int close_ib_device(struct ibv_utils_res *ib_res)
{
    ibv_close_device(ib_res->context);
    return 0;
}
