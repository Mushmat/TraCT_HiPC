#include "verbs_common.h"

int ib_init(struct ib_resources *res, int ib_port, int gid_idx, size_t buf_size) {
    memset(res, 0, sizeof(*res));
    res->ib_port = ib_port;
    res->gid_idx = gid_idx;
    res->buf_size = buf_size;

    /* Find the first available RDMA device (soft-RoCE shows up here as
     * "rxe0" once you've done `rdma link add rxe0 type rxe netdev <eth>`) */
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "no RDMA devices found (did you modprobe rdma_rxe and add an rxe link?)\n");
        return -1;
    }
    res->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!res->ctx) { fprintf(stderr, "ibv_open_device failed\n"); return -1; }

    res->pd = ibv_alloc_pd(res->ctx);
    if (!res->pd) { fprintf(stderr, "ibv_alloc_pd failed\n"); return -1; }

    res->cq = ibv_create_cq(res->ctx, 128, NULL, NULL, 0);
    if (!res->cq) { fprintf(stderr, "ibv_create_cq failed\n"); return -1; }

    /* Data buffer: registered once, reused across all sizes/modes. Access
     * flags grant remote peers write AND read access, since we don't know
     * in advance which of the four modes will target this buffer. */
    res->buf = malloc(buf_size);
    if (!res->buf) { fprintf(stderr, "malloc failed\n"); return -1; }
    memset(res->buf, 0xAB, buf_size);

    res->mr = ibv_reg_mr(res->pd, res->buf, buf_size,
                          IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (!res->mr) { fprintf(stderr, "ibv_reg_mr failed\n"); return -1; }

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;   /* Reliable Connection: required for RDMA read,
                                        and gives us in-order reliable delivery like TCP */
    qp_attr.cap.max_send_wr  = 64;
    qp_attr.cap.max_recv_wr  = 64;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    if (!res->qp) { fprintf(stderr, "ibv_create_qp failed\n"); return -1; }

    if (ibv_query_gid(res->ctx, ib_port, gid_idx, &res->local_gid)) {
        fprintf(stderr, "ibv_query_gid failed\n");
        return -1;
    }

    /* Query the port itself to find out: (a) its LID, needed for native
     * InfiniBand addressing, (b) its active MTU, since path_mtu in RTR
     * must not exceed this or the state transition is rejected, and
     * (c) link_layer, which tells us whether we're on RoCE (Ethernet) or
     * native InfiniBand -- these use different addressing schemes. */
    struct ibv_port_attr port_attr;
    if (ibv_query_port(res->ctx, ib_port, &port_attr)) {
        fprintf(stderr, "ibv_query_port failed\n");
        return -1;
    }
    res->local_lid = port_attr.lid;
    res->active_mtu = port_attr.active_mtu;
    res->is_roce = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);
    printf("[ib_init] link_layer=%s lid=%u active_mtu=%d\n",
           res->is_roce ? "Ethernet(RoCE)" : "InfiniBand",
           res->local_lid, res->active_mtu);

    return 0;
}

void ib_destroy(struct ib_resources *res) {
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->buf) free(res->buf);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
}

/* RESET -> INIT: assign port, enable local access flags for incoming
 * remote operations (a peer can only RDMA-write/read this QP's memory
 * if we grant that here). */
int qp_to_init(struct ib_resources *res) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index       = 0;
    attr.port_num        = res->ib_port;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(res->qp, &attr, mask)) {
        perror("ibv_modify_qp(INIT)");
        return -1;
    }
    return 0;
}

/* INIT -> RTR (Ready To Receive): tell the QP who the peer is and the
 * starting sequence number, so incoming packets are recognized as
 * belonging to this connection. Addressing differs by fabric type:
 *   - Native InfiniBand: peers are addressed by LID (a 16-bit address
 *     assigned by the subnet manager). No GRH needed.
 *   - RoCE: runs over Ethernet, which has no LID concept, so peers are
 *     addressed by GID (an IPv6-like address) instead, via the AH's GRH. */
int qp_to_rtr(struct ib_resources *res, const struct qp_info *remote) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = res->active_mtu;   /* must not exceed the port's active MTU */
    attr.dest_qp_num        = remote->qp_num;
    attr.rq_psn             = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;

    if (res->is_roce) {
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = remote->gid;
        attr.ah_attr.grh.sgid_index = res->gid_idx;
        attr.ah_attr.grh.hop_limit  = 1;
        attr.ah_attr.dlid           = 0;   /* unused on RoCE */
    } else {
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote->lid;   /* native IB: address by LID */
    }
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = res->ib_port;

    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
               IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(res->qp, &attr, mask)) {
        perror("ibv_modify_qp(RTR)");
        return -1;
    }
    return 0;
}

/* RTR -> RTS (Ready To Send): now the QP can send too. Sets retry/timeout
 * behavior for the reliable-delivery guarantees RC provides. */
int qp_to_rts(struct ib_resources *res, uint32_t local_psn) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout        = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 7;
    attr.sq_psn         = local_psn;
    attr.max_rd_atomic  = 1;

    int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(res->qp, &attr, mask)) {
        perror("ibv_modify_qp(RTS)");
        return -1;
    }
    return 0;
}

/* Post a RECV work request. Needed before: (a) the peer SENDs to us
 * (two-sided send/recv), and (b) the peer does RDMA WRITE WITH IMMEDIATE
 * to us -- unlike a plain RDMA WRITE, the "with immediate" variant
 * consumes a receive WR on the target side to deliver the immediate data,
 * even though the payload itself is written directly via RDMA. */
int post_recv(struct ib_resources *res, uint64_t wr_id) {
    struct ibv_sge sge = {
        .addr   = (uintptr_t)res->buf,
        .length = (uint32_t)res->buf_size,
        .lkey   = res->mr->lkey
    };
    struct ibv_recv_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id   = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(res->qp, &wr, &bad_wr)) {
        perror("ibv_post_recv");
        return -1;
    }
    return 0;
}

/* Post a SEND work request, opcode depending on which of the four modes
 * we're benchmarking. Always signaled (IBV_SEND_SIGNALED) because we time
 * every operation by polling for its completion. */
int post_send(struct ib_resources *res, enum bench_mode mode, size_t size,
              const struct qp_info *remote, uint32_t imm_data, uint64_t wr_id) {
    struct ibv_sge sge = {
        .addr   = (uintptr_t)res->buf,
        .length = (uint32_t)size,
        .lkey   = res->mr->lkey
    };
    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = wr_id;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    switch (mode) {
        case MODE_SEND_RECV:
            wr.opcode = IBV_WR_SEND;
            break;
        case MODE_RDMA_WRITE:
            wr.opcode = IBV_WR_RDMA_WRITE;
            wr.wr.rdma.remote_addr = remote->addr;
            wr.wr.rdma.rkey        = remote->rkey;
            break;
        case MODE_RDMA_WRITE_IMM:
            wr.opcode  = IBV_WR_RDMA_WRITE_WITH_IMM;
            wr.imm_data = htonl(imm_data);
            wr.wr.rdma.remote_addr = remote->addr;
            wr.wr.rdma.rkey        = remote->rkey;
            break;
        case MODE_RDMA_READ:
            wr.opcode = IBV_WR_RDMA_READ;
            wr.wr.rdma.remote_addr = remote->addr;
            wr.wr.rdma.rkey        = remote->rkey;
            break;
        default:
            return -1;
    }

    if (ibv_post_send(res->qp, &wr, &bad_wr)) {
        perror("ibv_post_send");
        return -1;
    }
    return 0;
}

/* Busy-poll the completion queue until exactly one completion shows up.
 * Busy-polling (vs. blocking on an event channel) minimizes the latency
 * we measure being inflated by our own wakeup overhead -- this is the
 * same reason perftest (ib_send_lat etc.) busy-polls by default. */
int poll_one_completion(struct ib_resources *res, struct ibv_wc *wc) {
    int n;
    do {
        n = ibv_poll_cq(res->cq, 1, wc);
    } while (n == 0);

    if (n < 0) {
        fprintf(stderr, "ibv_poll_cq failed\n");
        return -1;
    }
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "completion error: %s (status %d)\n",
                ibv_wc_status_str(wc->status), wc->status);
        return -1;
    }
    return 0;
}
