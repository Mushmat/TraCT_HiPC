#include "tract_rdma.h"

int ib_dev_open(struct ib_dev *dev, int ib_port, int gid_idx) {
    memset(dev, 0, sizeof(*dev));
    dev->ib_port = ib_port;
    dev->gid_idx = gid_idx;

    int num_devs = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devs);
    if (!dev_list || num_devs == 0) { fprintf(stderr, "no IB devices found\n"); return -1; }

    dev->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!dev->ctx) { fprintf(stderr, "ibv_open_device failed\n"); return -1; }

    dev->pd = ibv_alloc_pd(dev->ctx);
    if (!dev->pd) { fprintf(stderr, "ibv_alloc_pd failed\n"); return -1; }

    /* one CQ shared by every QP on this side is fine, we're not trying to
     * squeeze out max IOPS, just get a correct multi-node prototype working */
    dev->cq = ibv_create_cq(dev->ctx, 256, NULL, NULL, 0);
    if (!dev->cq) { fprintf(stderr, "ibv_create_cq failed\n"); return -1; }

    struct ibv_port_attr port_attr;
    if (ibv_query_port(dev->ctx, ib_port, &port_attr)) {
        fprintf(stderr, "ibv_query_port failed\n"); return -1;
    }
    dev->local_lid = port_attr.lid;
    dev->active_mtu = port_attr.active_mtu;
    dev->is_roce = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);
    printf("[ib_dev] link_layer=%s lid=%u active_mtu=%d\n",
           dev->is_roce ? "Ethernet(RoCE)" : "InfiniBand", dev->local_lid, dev->active_mtu);

    if (dev->is_roce) {
        if (ibv_query_gid(dev->ctx, ib_port, gid_idx, &dev->local_gid)) {
            fprintf(stderr, "ibv_query_gid failed\n"); return -1;
        }
    }
    return 0;
}

void ib_dev_close(struct ib_dev *dev) {
    if (dev->cq) ibv_destroy_cq(dev->cq);
    if (dev->pd) ibv_dealloc_pd(dev->pd);
    if (dev->ctx) ibv_close_device(dev->ctx);
}

int ib_qp_create(struct ib_dev *dev, struct ib_qp *q) {
    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = dev->cq;
    attr.recv_cq = dev->cq;
    attr.qp_type = IBV_QPT_RC; /* reliable connection, we want ordered delivery for the lock bytes */
    attr.cap.max_send_wr = 64;
    attr.cap.max_recv_wr = 16;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    q->qp = ibv_create_qp(dev->pd, &attr);
    if (!q->qp) { fprintf(stderr, "ibv_create_qp failed\n"); return -1; }
    return 0;
}

int ib_qp_to_init(struct ib_dev *dev, struct ib_qp *q) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = dev->ib_port;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(q->qp, &attr, flags)) { perror("ibv_modify_qp INIT"); return -1; }
    return 0;
}

int ib_qp_to_rtr(struct ib_dev *dev, struct ib_qp *q, const struct qp_handshake *remote) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = dev->active_mtu;
    attr.dest_qp_num = remote->qp_num;
    attr.rq_psn = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;

    attr.ah_attr.port_num = dev->ib_port;
    if (dev->is_roce) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = remote->gid;
        attr.ah_attr.grh.sgid_index = dev->gid_idx;
        attr.ah_attr.grh.hop_limit = 1;
    } else {
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid = remote->lid;
        attr.ah_attr.sl = 0;
        attr.ah_attr.src_path_bits = 0;
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(q->qp, &attr, flags)) { perror("ibv_modify_qp RTR"); return -1; }
    return 0;
}

int ib_qp_to_rts(struct ib_dev *dev, struct ib_qp *q, uint32_t local_psn) {
    (void)dev;
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = local_psn;
    attr.max_rd_atomic = 1;

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(q->qp, &attr, flags)) { perror("ibv_modify_qp RTS"); return -1; }
    return 0;
}

int ib_post_write(struct ib_qp *q, void *local_buf, uint32_t lkey, size_t len,
                   uint64_t remote_addr, uint32_t rkey, uint64_t wr_id, int signaled) {
    struct ibv_sge sge = { .addr = (uintptr_t)local_buf, .length = (uint32_t)len, .lkey = lkey };
    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;
    return ibv_post_send(q->qp, &wr, &bad_wr);
}

int ib_post_read(struct ib_qp *q, void *local_buf, uint32_t lkey, size_t len,
                  uint64_t remote_addr, uint32_t rkey, uint64_t wr_id) {
    struct ibv_sge sge = { .addr = (uintptr_t)local_buf, .length = (uint32_t)len, .lkey = lkey };
    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED; /* reads always need to be signaled, we always wait on them */
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;
    return ibv_post_send(q->qp, &wr, &bad_wr);
}

int ib_poll_one(struct ib_dev *dev, struct ibv_wc *wc) {
    int n;
    do { n = ibv_poll_cq(dev->cq, 1, wc); } while (n == 0);
    if (n < 0) return -1;
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "completion error: %s (wr_id=%llu)\n",
                ibv_wc_status_str(wc->status), (unsigned long long)wc->wr_id);
        return -1;
    }
    return 0;
}

/* ---- plain TCP handshake helpers, same pattern as the rest of the project ---- */

int send_all_bytes(int fd, const void *buf, size_t len) {
    size_t sent = 0; const char *p = buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

int recv_all_bytes(int fd, void *buf, size_t len) {
    size_t got = 0; char *p = buf;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    return 0;
}

int tcp_connect_to(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    for (int i = 0; i < 100; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        usleep(100000);
    }
    close(fd);
    return -1;
}

int tcp_listen_on(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, TRACT_MAX_NODES) < 0) { perror("listen"); exit(1); }
    return lfd;
}

int tcp_accept_one(int lfd) {
    int cfd = accept(lfd, NULL, NULL);
    if (cfd >= 0) {
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return cfd;
}
