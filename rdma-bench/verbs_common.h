#ifndef VERBS_COMMON_H
#define VERBS_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* ---- Benchmark parameters (must match on client AND server) ---- */
#define MIN_SIZE        1
#define MAX_SIZE        4194304      /* 4 MiB */
#define WARMUP_ITERS    20
#define TIMED_ITERS     200
#define TCP_PORT        6001         /* out-of-band handshake port, separate from RDMA */

/* Which of the four operation types we're benchmarking */
enum bench_mode {
    MODE_SEND_RECV = 0,
    MODE_RDMA_WRITE,
    MODE_RDMA_WRITE_IMM,
    MODE_RDMA_READ,
    MODE_COUNT
};

static const char *mode_name(enum bench_mode m) {
    switch (m) {
        case MODE_SEND_RECV:      return "SEND/RECV";
        case MODE_RDMA_WRITE:     return "RDMA WRITE";
        case MODE_RDMA_WRITE_IMM: return "RDMA WRITE w/ IMM";
        case MODE_RDMA_READ:      return "RDMA READ";
        default: return "?";
    }
}

/* Everything one side needs to know about the other side in order to
 * transition its QP to RTR/RTS, and (for one-sided ops) to target the
 * peer's memory region directly. Exchanged once, up front, over a plain
 * TCP socket -- RDMA has no built-in handshake like TCP's SYN/ACK.
 *
 * Both `lid` and `gid` are included so the same code works on native
 * InfiniBand (LID-based addressing) and on RoCE (GID-based addressing,
 * since RoCE runs over Ethernet and has no LID concept). We pick which
 * one to actually use for the address handle at RTR time, based on the
 * port's link_layer. */
struct qp_info {
    uint32_t qp_num;
    uint32_t psn;         /* starting packet sequence number */
    uint16_t lid;          /* used for native InfiniBand addressing */
    union ibv_gid gid;    /* used for RoCE addressing */
    uint64_t addr;        /* remote buffer address (for RDMA read/write) */
    uint32_t rkey;        /* remote memory region key (for RDMA read/write) */
};

/* All the verbs objects one side needs, bundled together */
struct ib_resources {
    struct ibv_context *ctx;
    struct ibv_pd       *pd;   /* protection domain */
    struct ibv_cq       *cq;   /* completion queue */
    struct ibv_qp       *qp;   /* queue pair */
    struct ibv_mr       *mr;   /* registered memory region */
    char                *buf;  /* the actual data buffer, registered as the MR */
    size_t               buf_size;
    int                  ib_port;
    int                  gid_idx;
    union ibv_gid        local_gid;
    uint16_t             local_lid;
    enum ibv_mtu          active_mtu;   /* queried from the port -- must not
                                          exceed this or RTR transition fails */
    int                   is_roce;      /* 0 = native InfiniBand (LID-based),
                                          1 = RoCE (GID-based) */
};

/* ---------------- out-of-band TCP exchange (reused pattern from task 1) --------------- */

static inline int send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

static inline int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    char *p = buf;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    return 0;
}

static inline int tcp_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    /* Server may not be listening yet -- retry briefly instead of failing immediately */
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        usleep(100000);
    }
    close(fd);
    return -1;
}

static inline int tcp_listen_accept(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 1) < 0) { perror("listen"); exit(1); }
    int cfd = accept(lfd, NULL, NULL);
    close(lfd);
    return cfd;
}

/* Exchange qp_info structs: both sides send theirs, then both receive the
 * peer's. Order doesn't matter for correctness here since it's full-duplex,
 * but this ordering (send-then-recv) avoids both sides blocking on recv(). */
static inline int exchange_qp_info(int sock, struct qp_info *local, struct qp_info *remote) {
    if (send_all(sock, local, sizeof(*local)) < 0) return -1;
    if (recv_all(sock, remote, sizeof(*remote)) < 0) return -1;
    return 0;
}

/* Simple rendezvous point: both sides block here until both have arrived.
 * Used between benchmark modes so client and server never race -- e.g. so
 * the server doesn't still expect SEND/RECV echoes while the client has
 * already moved on to RDMA WRITE. */
static inline void tcp_barrier(int sock) {
    char b = 'x';
    send_all(sock, &b, 1);
    recv_all(sock, &b, 1);
}

/* ---------------- timing ---------------- */

static inline long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------------- verbs setup ---------------- */

int  ib_init(struct ib_resources *res, int ib_port, int gid_idx, size_t buf_size);
void ib_destroy(struct ib_resources *res);
int  qp_to_init(struct ib_resources *res);
int  qp_to_rtr(struct ib_resources *res, const struct qp_info *remote);
int  qp_to_rts(struct ib_resources *res, uint32_t local_psn);
int  post_recv(struct ib_resources *res, uint64_t wr_id);
int  post_send(struct ib_resources *res, enum bench_mode mode, size_t size,
               const struct qp_info *remote, uint32_t imm_data, uint64_t wr_id);
int  poll_one_completion(struct ib_resources *res, struct ibv_wc *wc);

#endif /* VERBS_COMMON_H */
