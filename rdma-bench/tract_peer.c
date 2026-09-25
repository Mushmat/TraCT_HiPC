#include "tract_peer.h"

void tract_rdma_write_blocking(tract_peer_t *p, void *local_buf, uint32_t lkey,
                                size_t len, uint64_t remote_off) {
    struct ibv_wc wc;
    ib_post_write(&p->qp, local_buf, lkey, len, p->remote_base + remote_off, p->remote_rkey, 1, 1);
    ib_poll_one(&p->dev, &wc);
}

void tract_rdma_read_blocking(tract_peer_t *p, void *local_buf, uint32_t lkey,
                               size_t len, uint64_t remote_off) {
    struct ibv_wc wc;
    ib_post_read(&p->qp, local_buf, lkey, len, p->remote_base + remote_off, p->remote_rkey, 2);
    ib_poll_one(&p->dev, &wc);
}

/* these two are what tract_lock.h calls through remote_mem_t -- they just
 * bounce a single byte over RDMA using the peer's tiny scratch buffer.
 * one round trip per call, which is exactly the busy-poll cost the paper
 * pays too for the lock array */
static uint8_t peer_read_byte(remote_mem_t *self, uint64_t off) {
    tract_peer_t *p = self->ctx;
    tract_rdma_read_blocking(p, p->lock_scratch, p->lock_scratch_mr->lkey, 1, off);
    return p->lock_scratch[0];
}

static void peer_write_byte(remote_mem_t *self, uint64_t off, uint8_t val) {
    tract_peer_t *p = self->ctx;
    p->lock_scratch[0] = val;
    tract_rdma_write_blocking(p, p->lock_scratch, p->lock_scratch_mr->lkey, 1, off);
}

int tract_peer_connect(tract_peer_t *p, const char *host_ip, int host_port,
                        int ib_port, int gid_idx) {
    memset(p, 0, sizeof(*p));

    if (ib_dev_open(&p->dev, ib_port, gid_idx) != 0) return -1;
    if (ib_qp_create(&p->dev, &p->qp) != 0) return -1;
    if (ib_qp_to_init(&p->dev, &p->qp) != 0) return -1;

    p->lock_scratch = malloc(64); /* only need 1 byte but keep it cacheline-ish sized */
    p->lock_scratch_mr = ibv_reg_mr(p->dev.pd, p->lock_scratch, 64,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!p->lock_scratch_mr) { fprintf(stderr, "reg lock_scratch mr failed\n"); return -1; }

    int fd = tcp_connect_to(host_ip, host_port);
    if (fd < 0) { fprintf(stderr, "could not connect to host at %s:%d\n", host_ip, host_port); return -1; }

    struct qp_handshake local, remote;
    memset(&local, 0, sizeof(local));
    local.qp_num = p->qp.qp->qp_num;
    local.psn = (uint32_t)(lrand48() & 0xffffff);
    local.lid = p->dev.local_lid;
    local.gid = p->dev.local_gid;

    if (send_all_bytes(fd, &local, sizeof(local)) < 0) return -1;
    if (recv_all_bytes(fd, &remote, sizeof(remote)) < 0) return -1;
    close(fd);

    p->remote_base = remote.buf_addr;
    p->remote_rkey = remote.rkey;
    p->node_id = remote.node_id;

    if (ib_qp_to_rtr(&p->dev, &p->qp, &remote) != 0) return -1;
    if (ib_qp_to_rts(&p->dev, &p->qp, local.psn) != 0) return -1;

    p->rm.ctx = p;
    p->rm.read_byte = peer_read_byte;
    p->rm.write_byte = peer_write_byte;
    tract_local_locks_init(&p->local_locks);

    printf("[peer] connected, node_id=%u remote_base=0x%lx rkey=%u\n",
           p->node_id, (unsigned long)p->remote_base, p->remote_rkey);
    return 0;
}
