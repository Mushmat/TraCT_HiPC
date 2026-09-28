#ifndef TRACT_RDMA_H
#define TRACT_RDMA_H

/* generic RDMA connection setup, basically the same bones as
 * verbs_common.c from the earlier benchmark but stripped down and made
 * to support many QPs sharing one big registered buffer (the host has
 * one MR and a separate QP per connected peer, everyone else has just
 * one QP talking to the host) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "tract_common.h"

struct qp_handshake {
    uint32_t qp_num;
    uint32_t psn;
    uint16_t lid;
    union ibv_gid gid;
    uint64_t buf_addr;   /* only meaningful when the host is telling a peer where the shared region lives */
    uint32_t rkey;
    uint32_t node_id;    /* host assigns this, peer just echoes it back so both sides agree */
};

/* device-level stuff shared by every QP: one context, one PD, one CQ is
 * plenty since we're not chasing peak completion throughput here */
struct ib_dev {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    int ib_port;
    int gid_idx;
    union ibv_gid local_gid;
    uint16_t local_lid;
    enum ibv_mtu active_mtu;
    int is_roce;
};

/* one queue pair, pointed at a peer */
struct ib_qp {
    struct ibv_qp *qp;
};

int  ib_dev_open(struct ib_dev *dev, int ib_port, int gid_idx);
void ib_dev_close(struct ib_dev *dev);

int  ib_qp_create(struct ib_dev *dev, struct ib_qp *q);
int  ib_qp_to_init(struct ib_dev *dev, struct ib_qp *q);
int  ib_qp_to_rtr(struct ib_dev *dev, struct ib_qp *q, const struct qp_handshake *remote);
int  ib_qp_to_rts(struct ib_dev *dev, struct ib_qp *q, uint32_t local_psn);

/* raw one-sided ops against an arbitrary remote addr/rkey -- these are
 * what the lock and the block transfers actually ride on */
int  ib_post_write(struct ib_qp *q, void *local_buf, uint32_t lkey, size_t len,
                    uint64_t remote_addr, uint32_t rkey, uint64_t wr_id, int signaled);
int  ib_post_read(struct ib_qp *q, void *local_buf, uint32_t lkey, size_t len,
                   uint64_t remote_addr, uint32_t rkey, uint64_t wr_id);
int  ib_poll_one(struct ib_dev *dev, struct ibv_wc *wc);

/* plain TCP helpers for the out-of-band handshake, same pattern used
 * everywhere else in this project */
int tcp_connect_to(const char *ip, int port);
int tcp_listen_on(int port);
int tcp_accept_one(int lfd);
int send_all_bytes(int fd, const void *buf, size_t len);
int recv_all_bytes(int fd, void *buf, size_t len);

#endif
