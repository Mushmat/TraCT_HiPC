#include "shm_common.h"

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
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct kv_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tokens_per_block = (uint32_t)get_opt_u64(argc, argv, "--tokens-per-block", 64);
    cfg.bytes_per_token = get_opt_u64(argc, argv, "--bytes-per-token", 131072);
    cfg.block_size = (uint64_t)cfg.tokens_per_block * cfg.bytes_per_token;

    const char *wl = get_opt_str(argc, argv, "--workload", "static");
    cfg.workload_type = (strcmp(wl, "synthetic") == 0) ? WORKLOAD_SYNTHETIC : WORKLOAD_STATIC;

    uint32_t default_lens[4] = {1500, 3000, 4500, 6000};
    cfg.num_static_lens = 4;
    memcpy(cfg.static_lens, default_lens, sizeof(default_lens));
    cfg.reps_per_len = (uint32_t)get_opt_u64(argc, argv, "--reps-per-len", 5);

    cfg.synthetic_num_requests = (uint32_t)get_opt_u64(argc, argv, "--synthetic-requests", 50);
    cfg.synthetic_mean_input = get_opt_double(argc, argv, "--synthetic-mean", 4449);
    cfg.synthetic_stddev_input = get_opt_double(argc, argv, "--synthetic-stddev", 2424);

    unsigned int seed = (unsigned int)get_opt_u64(argc, argv, "--seed", 42);
    srand(seed);

    static uint32_t lengths[MAX_REQUESTS];
    uint32_t num_requests = generate_workload(&cfg, lengths);
    cfg.num_requests = num_requests;

    uint64_t total_blocks = 0;
    for (uint32_t r = 0; r < num_requests; r++)
        total_blocks += (lengths[r] + cfg.tokens_per_block - 1) / cfg.tokens_per_block;

    cfg.table_size = total_blocks * 4 + 16;
    cfg.data_capacity = total_blocks * cfg.block_size;

    printf("prefill: workload=%s num_requests=%u total_blocks=%lu block_size=%lu B cache=%lu MB\n",
           wl, num_requests, (unsigned long)total_blocks, (unsigned long)cfg.block_size,
           (unsigned long)(cfg.data_capacity / (1024 * 1024)));

    FILE *f = fopen(CONFIG_PATH_TMP, "wb");
    if (!f) { perror("fopen config"); exit(1); }
    fwrite(&cfg, sizeof(cfg), 1, f);
    fwrite(lengths, sizeof(uint32_t), num_requests, f);
    fclose(f);
    rename(CONFIG_PATH_TMP, CONFIG_PATH);

    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); exit(1); }

    size_t total_size = sizeof(shm_header_t) + cfg.table_size * sizeof(cache_entry_t) + cfg.data_capacity;
    if (ftruncate(fd, total_size) < 0) { perror("ftruncate"); exit(1); }

    shm_header_t *hdr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hdr == MAP_FAILED) { perror("mmap"); exit(1); }

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&hdr->lock, &mattr);
    hdr->next_offset = 0;
    hdr->data_capacity = cfg.data_capacity;
    hdr->table_size = cfg.table_size;
    memset(table_base(hdr), 0, cfg.table_size * sizeof(cache_entry_t));

    // touching all the pages up front so the first real block isn't
    // paying for page faults on top of everything else
    memset(data_base(hdr), 0, cfg.data_capacity);

    char *local_buf = malloc(cfg.block_size);
    memset(local_buf, 0xAB, cfg.block_size);

    static double block_lat_us[MAX_REQUESTS * 64];
    uint32_t block_count = 0;
    static double req_time_ms[MAX_REQUESTS];
    static double req_tput_gbs[MAX_REQUESTS];

    printf("\n%-10s %-12s %-14s %-18s %-16s\n",
           "req#", "in_tokens", "num_blocks", "transfer_time(ms)", "throughput(GB/s)");

    for (uint32_t r = 0; r < num_requests; r++) {
        uint32_t num_blocks = (lengths[r] + cfg.tokens_per_block - 1) / cfg.tokens_per_block;
        long long req_t0 = now_ns();

        for (uint32_t b = 0; b < num_blocks; b++) {
            uint64_t h = block_hash(r, b);
            uint64_t idx = h % cfg.table_size;

            long long t0 = now_ns();

            pthread_mutex_lock(&hdr->lock);
            while (table_base(hdr)[idx].valid) idx = (idx + 1) % cfg.table_size;
            uint64_t offset = hdr->next_offset;
            hdr->next_offset = (hdr->next_offset + cfg.block_size) % cfg.data_capacity;
            pthread_mutex_unlock(&hdr->lock);

            memcpy(data_base(hdr) + offset, local_buf, cfg.block_size);

            table_base(hdr)[idx].hash = h;
            table_base(hdr)[idx].offset = offset;
            table_base(hdr)[idx].length = cfg.block_size;
            table_base(hdr)[idx].valid = 1;

            long long t1 = now_ns();
            if (block_count < sizeof(block_lat_us) / sizeof(double))
                block_lat_us[block_count++] = (t1 - t0) / 1000.0;
        }

        long long req_t1 = now_ns();
        double ms = (req_t1 - req_t0) / 1e6;
        double total_bytes = (double)num_blocks * cfg.block_size;
        double gbs = (ms > 0) ? (total_bytes / 1e9) / (ms / 1000.0) : 0.0;
        req_time_ms[r] = ms;
        req_tput_gbs[r] = gbs;
        printf("%-10u %-12u %-14u %-18.3f %-16.3f\n", r, lengths[r], num_blocks, ms, gbs);
    }

    struct stats_summary block_stats = compute_stats(block_lat_us, block_count);
    struct stats_summary req_stats = compute_stats(req_time_ms, num_requests);
    struct stats_summary tput_stats = compute_stats(req_tput_gbs, num_requests);

    printf("\n--- Summary (%u requests, %u blocks, %lu B/block) ---\n",
           num_requests, block_count, (unsigned long)cfg.block_size);
    printf("per-block latency (us): mean=%.2f  p50=%.2f  p99=%.2f  min=%.2f  max=%.2f\n",
           block_stats.mean, block_stats.p50, block_stats.p99, block_stats.min, block_stats.max);
    printf("per-request transfer time (ms): mean=%.3f  p50=%.3f  p99=%.3f  min=%.3f  max=%.3f\n",
           req_stats.mean, req_stats.p50, req_stats.p99, req_stats.min, req_stats.max);
    printf("per-request throughput (GB/s): mean=%.3f  p50=%.3f  p99=%.3f  min=%.3f  max=%.3f\n",
           tput_stats.mean, tput_stats.p50, tput_stats.p99, tput_stats.min, tput_stats.max);

    munmap(hdr, total_size);
    close(fd);
    while (access(CONFIG_PATH, F_OK) == 0) usleep(10000);
    shm_unlink(SHM_NAME);
    free(local_buf);
    printf("\nprefill done\n");
    return 0;
}
