// Adapted from libsrc/udp_rdma/src/ibv_utils.cpp
#include "rdma_dada/io/rdma/verbs_context.h"
#include "rdma_dada/io/rdma/receive_policy.h"

void ibv_utils_info(const char *msg) { fprintf(stdout, "IBV-UTILS INFO: %s \n", msg); }
void ibv_utils_error(const char *msg) { fprintf(stderr, "IBV-UTILS ERROR: %s \n", msg); }
void ibv_utils_warn(const char *msg) { fprintf(stderr, "IBV-UTILS WARN: %s \n", msg); }

int open_ib_device(uint8_t device_id, struct ibv_utils_res *ib_res)
{
    struct ibv_device **ib_global_devs;
    int num_ib_devices;
    ib_global_devs = ibv_get_device_list(&num_ib_devices);
    if (!ib_global_devs) { ibv_utils_error("Failed to get IB devices list."); return -1; }
    if(device_id >= num_ib_devices) {
        ibv_utils_error("Invalid device id.");
        ibv_free_device_list(ib_global_devs);
        return -1;
    }
    ib_res->dev = ib_global_devs[device_id];
    ib_res->context = ibv_open_device(ib_res->dev);
    ibv_free_device_list(ib_global_devs);
    if(!ib_res->context) { ibv_utils_error("Failed to open IB device."); return -1; }
    return 0;
}

int open_ib_device_by_name(const char *device_name) { (void)device_name; return -1; }

int create_ib_res(struct ibv_utils_res *ib_res, int recv_wr_num)
{
    if(ib_res->recv_nsge == 0) return -1;
    ib_res->recv_wr_num = recv_wr_num;
    ib_res->pd = ibv_alloc_pd(ib_res->context);
    if (!ib_res->pd) { ibv_utils_error("Failed to allocate PD."); return -1; }
    ib_res->cq = ibv_create_cq(ib_res->context, recv_wr_num, NULL, NULL, 0);
    if(!ib_res->cq){ ibv_utils_error("Couldn't create CQ."); return -2; }
    struct ibv_qp_init_attr qp_init_attr = { .qp_context = NULL, .send_cq = ib_res->cq, .recv_cq = ib_res->cq, .cap = { .max_send_wr = 0, .max_recv_wr = (uint32_t)recv_wr_num, .max_send_sge = 0, .max_recv_sge = (uint32_t)ib_res->recv_nsge, }, .qp_type = IBV_QPT_RAW_PACKET, };
    ib_res->qp = ibv_create_qp(ib_res->pd, &qp_init_attr);
    if(!ib_res->qp){ ibv_utils_error("Couldn't create QP."); return -4; }
    const size_t sge_size = recv_wr_num * sizeof(struct ibv_sge) *
                            ib_res->recv_nsge;
    const size_t recv_wr_size = recv_wr_num * sizeof(struct ibv_recv_wr);
    const size_t wc_size = recv_wr_num * sizeof(struct ibv_wc);
    const size_t total_size = sge_size + recv_wr_size + wc_size;
    
    void *pool = malloc(total_size);
    if (!pool) { ibv_utils_error("Failed to allocate memory pool."); return -9; }
    
    char *ptr = (char *)pool;
    ib_res->sge = (struct ibv_sge *)ptr;
    ptr += sge_size;
    ib_res->recv_wr = (struct ibv_recv_wr *)ptr;
    ptr += recv_wr_size;
    ib_res->wc = (struct ibv_wc *)ptr;
    ib_res->pool_ptr = pool;
    
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
    const rdma_dada::io::rdma::DestinationUdpFilter filter =
        rdma_dada::io::rdma::BuildDestinationUdpFilter(
            pkt_info->dst_mac, pkt_info->dst_ip, pkt_info->dst_port);

    // Match IPv4 UDP traffic for this destination. All source values and masks
    // stay zero so every FPGA/Station source is accepted by the same QP.
    flow_attr.spec_eth.val.ether_type = htons(0x0800);  // IPv4
    flow_attr.spec_eth.mask.ether_type = 0xFFFF;        // Must match exactly
    memcpy(flow_attr.spec_eth.val.dst_mac, filter.destination_mac, 6);
    memcpy(flow_attr.spec_eth.val.src_mac, filter.source_mac, 6);
    memcpy(flow_attr.spec_eth.mask.dst_mac,
           filter.destination_mac_mask, 6);
    memcpy(flow_attr.spec_eth.mask.src_mac, filter.source_mac_mask, 6);
    
    // Set IPv4 header matching
    flow_attr.spec_ipv4.val.dst_ip = filter.destination_ip;
    flow_attr.spec_ipv4.val.src_ip = filter.source_ip;
    flow_attr.spec_ipv4.mask.dst_ip = filter.destination_ip_mask;
    flow_attr.spec_ipv4.mask.src_ip = filter.source_ip_mask;
    
    // Set UDP port matching
    flow_attr.spec_udp.val.dst_port = htons(filter.destination_port);
    flow_attr.spec_udp.val.src_port = htons(filter.source_port);
    flow_attr.spec_udp.mask.dst_port = filter.destination_port_mask;
    flow_attr.spec_udp.mask.src_port = filter.source_port_mask;
    
    // Debug: print flow rule details before creation
    printf("[create_flow] Attempting to create flow steering rule:\n");
    printf("  Port: %d, Priority: %d, Num specs: %d\n", 
           flow_attr.attr.port, flow_attr.attr.priority, flow_attr.attr.num_of_specs);
    printf("  ETH: ether_type=0x%04x (IPv4=%s)\n", 
           ntohs(flow_attr.spec_eth.val.ether_type),
           ntohs(flow_attr.spec_eth.val.ether_type) == 0x0800 ? "Yes" : "No");
    printf("       dst_mac=%02x:%02x:%02x:%02x:%02x:%02x src_mac=ANY\n",
           pkt_info->dst_mac[0], pkt_info->dst_mac[1], pkt_info->dst_mac[2],
           pkt_info->dst_mac[3], pkt_info->dst_mac[4], pkt_info->dst_mac[5]);
    uint8_t *dip = (uint8_t*)&pkt_info->dst_ip;
    printf("  IPv4: ANY -> %d.%d.%d.%d\n",
           dip[0], dip[1], dip[2], dip[3]);
    printf("  UDP: port ANY -> %d\n", pkt_info->dst_port);
    
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
    
    // 2. 销毁CQ（依赖PD）
    if (ib_res->cq) {
        if (ibv_destroy_cq(ib_res->cq) != 0) {
            ibv_utils_error("Failed to destroy CQ");
            ret = -1;
        }
    }
    
    // 3. 释放PD（最后释放）
    if (ib_res->pd) {
        if (ibv_dealloc_pd(ib_res->pd) != 0) {
            ibv_utils_error("Failed to deallocate PD");
            ret = -1;
        }
    }
    
    // 释放 WR/SGE/WC 内存池
    if (ib_res->pool_ptr) {
        free(ib_res->pool_ptr);
        ib_res->pool_ptr = NULL;
    }
    // 清空指针避免悬空
    ib_res->sge = NULL;
    ib_res->recv_wr = NULL;
    ib_res->wc = NULL;
    
    return ret;
}

int close_ib_device(struct ibv_utils_res *ib_res)
{
    if (ib_res && ib_res->context) {
        ibv_close_device(ib_res->context);
        ib_res->context = NULL;
    }
    return 0;
}
