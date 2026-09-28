/*
 * kv_prefill.c -- "prefill worker" role of the KV-transfer baseline.
 *
 * This models the RDMA-based KV path used by today's disaggregated
 * serving systems (NIXL/UCX in the TraCT paper's baseline): the prefill
 * side "produces" KV blocks (compute is a NOOP here, per instruction --
 * we're only characterizing the transfer, not real GPU work) and pushes
 * each block directly into the decode worker's registered memory via
 * RDMA_WRITE_WITH_IMM, so the decode side gets a completion notification
 * per block, the same way a real KV cache write would need to signal
 * "this block is now ready to read."
 *
 * Protocol with kv_decode.c:
 *   1. TCP connect (control channel)
 *   2. Send kv_config, then the generated workload (array of per-request
 *      input-token counts) -- BEFORE any RDMA setup, because the decode
 *      side needs to know cache_capacity to size its registered buffer.
 *   3. Bring up the RDMA QP (same INIT->RTR->RTS flow as the task 2 benchmark).
 *   4. For every request, for every block: RDMA WRITE_WITH_IMM, time it.
 *   5. Print per-request results and an overall summary.
 */
#include "kv_common.h"

static uint64_t get_opt_u64(int argc, char **argv, const char *flag, uint64_t def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return strtoull(argv[i + 1], NULL, 10);
    return def;
}
static double get_opt_double(int argc, char **argv, const char *flag, double def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atof(argv[i + 1]);
    return def;
}
static const char *get_opt_str(int argc, char **argv, const char *flag, const char *def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return def;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffer stdout so progress prints live */
    const char *decode_ip = get_opt_str(argc, argv, "--decode-ip", NULL);
    if (!decode_ip) {
        fprintf(stderr,
            "usage: %s --decode-ip <ip> [options]\n"
            "  --ib-port <n>              default 1\n"
            "  --gid-idx <n>              default 0\n"
            "  --tokens-per-block <n>     default 64\n"
            "  --bytes-per-token <n>      default 131072  (see note below)\n"
            "  --cache-capacity-mb <n>    default 4096\n"
            "  --workload static|synthetic  default static\n"
            "  --reps-per-len <n>         default 5 (all reps timed; one global\n"
            "                              warmup block is sent separately beforehand)\n"
            "  --synthetic-requests <n>   default 50\n"
            "  --synthetic-mean <f>       default 4449\n"
            "  --synthetic-stddev <f>     default 2424\n"
            "\n"
            "NOTE on --bytes-per-token: this is an ASSUMPTION, not a measured\n"
            "fact. Default 131072 (128 KB/token) is estimated for an 8B-class\n"
            "Llama-architecture model via 2 * num_layers * num_kv_heads * head_dim\n"
            "* dtype_bytes = 2*32*8*128*2. Override with the real figure for\n"
            "whichever model this is meant to represent.\n",
            argv[0]);
        exit(1);
    }
    int ib_port = (int)get_opt_u64(argc, argv, "--ib-port", 1);
    int gid_idx = (int)get_opt_u64(argc, argv, "--gid-idx", 0);

    struct kv_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tokens_per_block = (uint32_t)get_opt_u64(argc, argv, "--tokens-per-block", 64);
    cfg.bytes_per_token  = get_opt_u64(argc, argv, "--bytes-per-token", 131072);
    cfg.block_size       = (uint64_t)cfg.tokens_per_block * cfg.bytes_per_token;
    cfg.cache_capacity   = get_opt_u64(argc, argv, "--cache-capacity-mb", 4096) * 1024ULL * 1024ULL;

    const char *wl = get_opt_str(argc, argv, "--workload", "static");
    cfg.workload_type = (strcmp(wl, "synthetic") == 0) ? WORKLOAD_SYNTHETIC : WORKLOAD_STATIC;

    /* Static workload defaults mirror the paper's Section 5.1 sweep */
    uint32_t default_lens[4] = {1500, 3000, 4500, 6000};
    cfg.num_static_lens = 4;
    memcpy(cfg.static_lens, default_lens, sizeof(default_lens));
    cfg.reps_per_len = (uint32_t)get_opt_u64(argc, argv, "--reps-per-len", 5);

    /* Synthetic defaults mirror workload A in the paper's Table 1 */
    cfg.synthetic_num_requests = (uint32_t)get_opt_u64(argc, argv, "--synthetic-requests", 50);
    cfg.synthetic_mean_input   = get_opt_double(argc, argv, "--synthetic-mean", 4449);
    cfg.synthetic_stddev_input = get_opt_double(argc, argv, "--synthetic-stddev", 2424);

    /* Fixed default seed makes runs reproducible for comparing configs;
     * pass --seed 0 (or any value) explicitly for a different draw. */
    unsigned int seed = (unsigned int)get_opt_u64(argc, argv, "--seed", 42);
    srand(seed);

    static uint32_t lengths[MAX_REQUESTS];
    uint32_t num_requests = generate_workload(&cfg, lengths);
    cfg.num_requests = num_requests;

    printf("[prefill] workload=%s num_requests=%u block_size=%lu B (%u tok/block x %lu B/tok) cache=%lu MB\n",
           wl, num_requests, (unsigned long)cfg.block_size, cfg.tokens_per_block,
           (unsigned long)cfg.bytes_per_token, (unsigned long)(cfg.cache_capacity / (1024*1024)));

    /* Step 2: send config + workload BEFORE any RDMA setup */
    int sock = tcp_connect(decode_ip, KV_TCP_PORT);
    if (sock < 0) { fprintf(stderr, "failed to connect to decode worker\n"); exit(1); }
    if (send_all(sock, &cfg, sizeof(cfg)) < 0) { fprintf(stderr, "send cfg failed\n"); exit(1); }
    if (send_all(sock, lengths, num_requests * sizeof(uint32_t)) < 0) {
        fprintf(stderr, "send workload failed\n"); exit(1);
    }

    /* Step 3: RDMA setup. Source buffer only needs to hold ONE block --
     * content is irrelevant (compute is a NOOP), so we reuse the same
     * small buffer for every write, only the DESTINATION offset moves. */
    struct ib_resources res;
    if (ib_init(&res, ib_port, gid_idx, cfg.block_size) < 0) exit(1);
    if (qp_to_init(&res) < 0) exit(1);

    struct qp_info local, remote;
    memset(&local, 0, sizeof(local));
    local.qp_num = res.qp->qp_num;
    local.psn    = 0;
    local.lid    = res.local_lid;
    local.gid    = res.local_gid;
    local.addr   = 0;   /* prefill's own buffer is never targeted remotely, address irrelevant */
    local.rkey   = 0;
    if (exchange_qp_info(sock, &local, &remote) < 0) { fprintf(stderr, "qp exchange failed\n"); exit(1); }
    if (qp_to_rtr(&res, &remote) < 0) exit(1);
    if (qp_to_rts(&res, local.psn) < 0) exit(1);
    printf("[prefill] QP connected. decode cache base=0x%lx rkey=%u\n",
           (unsigned long)remote.addr, remote.rkey);

    /* Warmup: the first RDMA operation on a freshly-created QP pays extra
     * cost (buffer/page pinning, TLB misses, etc.) that has nothing to do
     * with steady-state transfer speed. Send exactly one throwaway block
     * before starting the timed loop, so that cost doesn't leak into the
     * per-block/per-request stats. This mirrors the WARMUP_ITERS pattern
     * from the earlier TCP and verbs benchmarks. */
    {
        struct qp_info warmup_target = remote;   /* offset 0 -- will be overwritten later anyway */
        struct ibv_wc wc;
        if (post_send(&res, MODE_RDMA_WRITE_IMM, cfg.block_size, &warmup_target, 0, 1) < 0) exit(1);
        if (poll_one_completion(&res, &wc) < 0) exit(1);
        printf("[prefill] warmup block sent (excluded from stats)\n");
    }

    /* Step 4: the actual transfer loop */
    uint64_t cache_aligned = (cfg.cache_capacity / cfg.block_size) * cfg.block_size;
    if (cache_aligned == 0) { fprintf(stderr, "cache_capacity smaller than one block\n"); exit(1); }

    static double block_lat_us[MAX_REQUESTS * 64];  /* generous upper bound */
    uint32_t block_count = 0;
    static double req_time_ms[MAX_REQUESTS];
    static double req_tput_gbs[MAX_REQUESTS];

    uint64_t global_block_idx = 0;
    printf("\n%-10s %-12s %-14s %-18s %-16s\n",
           "req#", "in_tokens", "num_blocks", "transfer_time(ms)", "throughput(GB/s)");

    for (uint32_t r = 0; r < num_requests; r++) {
        uint32_t input_tokens = lengths[r];
        uint32_t num_blocks = (input_tokens + cfg.tokens_per_block - 1) / cfg.tokens_per_block;
        long long req_t0 = now_ns();

        for (uint32_t b = 0; b < num_blocks; b++) {
            uint64_t offset = (global_block_idx * cfg.block_size) % cache_aligned;
            struct qp_info target = remote;
            target.addr = remote.addr + offset;

            struct ibv_wc wc;
            long long t0 = now_ns();
            if (post_send(&res, MODE_RDMA_WRITE_IMM, cfg.block_size, &target, r, 1) < 0) exit(1);
            if (poll_one_completion(&res, &wc) < 0) exit(1);
            long long t1 = now_ns();

            if (block_count < sizeof(block_lat_us)/sizeof(double))
                block_lat_us[block_count++] = (t1 - t0) / 1000.0;
            global_block_idx++;
        }

        long long req_t1 = now_ns();
        double ms = (req_t1 - req_t0) / 1e6;
        double total_bytes = (double)num_blocks * cfg.block_size;
        double gbs = (ms > 0) ? (total_bytes / 1e9) / (ms / 1000.0) : 0.0;

        req_time_ms[r] = ms;
        req_tput_gbs[r] = gbs;
        printf("%-10u %-12u %-14u %-18.3f %-16.3f\n", r, input_tokens, num_blocks, ms, gbs);
    }

    struct stats_summary block_stats = compute_stats(block_lat_us, block_count);
    struct stats_summary req_stats   = compute_stats(req_time_ms, num_requests);
    struct stats_summary tput_stats  = compute_stats(req_tput_gbs, num_requests);

    printf("\n--- Summary (%u requests, %u blocks, %lu B/block) ---\n",
           num_requests, block_count, (unsigned long)cfg.block_size);
    printf("per-block latency (us): mean=%.2f  p50=%.2f  p99=%.2f  min=%.2f  max=%.2f\n",
           block_stats.mean, block_stats.p50, block_stats.p99, block_stats.min, block_stats.max);
    printf("per-request transfer time (ms): mean=%.3f  p50=%.3f  p99=%.3f  min=%.3f  max=%.3f\n",
           req_stats.mean, req_stats.p50, req_stats.p99, req_stats.min, req_stats.max);
    printf("per-request throughput (GB/s): mean=%.3f  p50=%.3f  p99=%.3f  min=%.3f  max=%.3f\n",
           tput_stats.mean, tput_stats.p50, tput_stats.p99, tput_stats.min, tput_stats.max);

    tcp_barrier(sock);
    close(sock);
    ib_destroy(&res);
    printf("\n[prefill] done\n");
    return 0;
}