#include "shm_common.h"

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    FILE *f = NULL;
    while (!(f = fopen(CONFIG_PATH, "rb"))) usleep(10000);

    struct kv_config cfg;
    if (fread(&cfg, sizeof(cfg), 1, f) != 1) { fprintf(stderr, "bad config\n"); exit(1); }
    static uint32_t lengths[MAX_REQUESTS];
    if (fread(lengths, sizeof(uint32_t), cfg.num_requests, f) != cfg.num_requests) {
        fprintf(stderr, "bad workload\n"); exit(1);
    }
    fclose(f);

    printf("decode: waiting for shared memory (%u requests, block_size=%lu B)\n",
           cfg.num_requests, (unsigned long)cfg.block_size);

    int fd = -1;
    while ((fd = shm_open(SHM_NAME, O_RDWR, 0600)) < 0) usleep(10000);

    size_t total_size = sizeof(shm_header_t) + cfg.table_size * sizeof(cache_entry_t) + cfg.data_capacity;
    shm_header_t *hdr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hdr == MAP_FAILED) { perror("mmap"); exit(1); }

    char *local_buf = malloc(cfg.block_size);
    uint64_t total_blocks_seen = 0;

    printf("decode: attached, serving %u requests\n", cfg.num_requests);

    for (uint32_t r = 0; r < cfg.num_requests; r++) {
        uint32_t num_blocks = (lengths[r] + cfg.tokens_per_block - 1) / cfg.tokens_per_block;
        for (uint32_t b = 0; b < num_blocks; b++) {
            uint64_t h = block_hash(r, b);
            uint64_t idx = h % cfg.table_size;

            while (1) {
                if (table_base(hdr)[idx].valid && table_base(hdr)[idx].hash == h) break;
                if (table_base(hdr)[idx].valid) { idx = (idx + 1) % cfg.table_size; continue; }
                sched_yield();
            }

            memcpy(local_buf, data_base(hdr) + table_base(hdr)[idx].offset, table_base(hdr)[idx].length);
            total_blocks_seen++;
        }
        if ((r + 1) % 10 == 0 || r + 1 == cfg.num_requests)
            printf("decode: request %u/%u done (%lu blocks so far)\n",
                   r + 1, cfg.num_requests, (unsigned long)total_blocks_seen);
    }

    munmap(hdr, total_size);
    close(fd);
    free(local_buf);
    remove(CONFIG_PATH);
    printf("decode: done, %lu blocks total\n", (unsigned long)total_blocks_seen);
    return 0;
}
