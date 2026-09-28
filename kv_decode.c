/*
 * kv_decode.c -- "decode worker" role of the KV-transfer baseline.
 *
 * Passive side: receives the run's config and exact workload plan from
 * the prefill worker over the control channel (so there's zero risk of
 * the two sides disagreeing about sizes or request counts), sizes its
 * registered "KV cache" buffer accordingly, then pre-posts a RECV before
 * every expected block so each incoming RDMA_WRITE_WITH_IMM has
 * somewhere to land its completion notification.
 *
 * This side does not need to time anything for the characterization --
 * all latency/throughput numbers are captured on the prefill side (see
 * kv_prefill.c) via its own local send-completion timestamps, which for
 * an RC queue pair only fire once the remote side has acknowledged the
 * write, making them a valid proxy for real transfer cost.
 */
#include "kv_common.h"

static uint64_t get_opt_u64(int argc, char **argv, const char *flag, uint64_t def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return strtoull(argv[i + 1], NULL, 10);
    return def;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffer stdout so progress prints live */
    int ib_port = (int)get_opt_u64(argc, argv, "--ib-port", 1);
    int gid_idx = (int)get_opt_u64(argc, argv, "--gid-idx", 0);

    printf("[decode] waiting for prefill worker on TCP port %d...\n", KV_TCP_PORT);
    int sock = tcp_listen_accept(KV_TCP_PORT);
    if (sock < 0) { fprintf(stderr, "accept failed\n"); exit(1); }
    printf("[decode] control channel connected\n");

    struct kv_config cfg;
    if (recv_all(sock, &cfg, sizeof(cfg)) < 0) { fprintf(stderr, "recv cfg failed\n"); exit(1); }

    static uint32_t lengths[MAX_REQUESTS];
    if (cfg.num_requests > MAX_REQUESTS) { fprintf(stderr, "num_requests too large\n"); exit(1); }
    if (recv_all(sock, lengths, cfg.num_requests * sizeof(uint32_t)) < 0) {
        fprintf(stderr, "recv workload failed\n"); exit(1);
    }
    printf("[decode] received config: num_requests=%u block_size=%lu B cache=%lu MB\n",
           cfg.num_requests, (unsigned long)cfg.block_size,
           (unsigned long)(cfg.cache_capacity / (1024*1024)));

    uint64_t cache_aligned = (cfg.cache_capacity / cfg.block_size) * cfg.block_size;
    if (cache_aligned == 0) { fprintf(stderr, "cache_capacity smaller than one block\n"); exit(1); }

    struct ib_resources res;
    if (ib_init(&res, ib_port, gid_idx, cache_aligned) < 0) exit(1);
    if (qp_to_init(&res) < 0) exit(1);

    struct qp_info local, remote;
    memset(&local, 0, sizeof(local));
    local.qp_num = res.qp->qp_num;
    local.psn    = 0;
    local.lid    = res.local_lid;
    local.gid    = res.local_gid;
    local.addr   = (uint64_t)(uintptr_t)res.buf;   /* this IS the KV cache prefill writes into */
    local.rkey   = res.mr->rkey;
    if (exchange_qp_info(sock, &local, &remote) < 0) { fprintf(stderr, "qp exchange failed\n"); exit(1); }
    if (qp_to_rtr(&res, &remote) < 0) exit(1);
    if (qp_to_rts(&res, local.psn) < 0) exit(1);
    printf("[decode] QP connected.\n");

    /* Consume the one throwaway warmup block the prefill side sends
     * right after connecting -- must match kv_prefill.c exactly, or the
     * two sides' block counts will drift out of sync. */
    {
        struct ibv_wc wc;
        if (post_recv(&res, 1) < 0) exit(1);
        if (poll_one_completion(&res, &wc) < 0) exit(1);
        printf("[decode] warmup block received\n");
    }

    printf("[decode] Serving %u requests...\n", cfg.num_requests);
    uint64_t total_blocks_seen = 0;
    for (uint32_t r = 0; r < cfg.num_requests; r++) {
        uint32_t num_blocks = (lengths[r] + cfg.tokens_per_block - 1) / cfg.tokens_per_block;
        for (uint32_t b = 0; b < num_blocks; b++) {
            struct ibv_wc wc;
            /* Must be posted before the corresponding write-with-imm
             * arrives, or the peer gets an RNR NAK and has to retry. */
            if (post_recv(&res, 1) < 0) exit(1);
            if (poll_one_completion(&res, &wc) < 0) exit(1);
            total_blocks_seen++;
        }
        if ((r + 1) % 10 == 0 || r + 1 == cfg.num_requests)
            printf("[decode] request %u/%u complete (%lu blocks so far)\n",
                   r + 1, cfg.num_requests, (unsigned long)total_blocks_seen);
    }

    tcp_barrier(sock);
    close(sock);
    ib_destroy(&res);
    printf("[decode] done, received %lu blocks total\n", (unsigned long)total_blocks_seen);
    return 0;
}