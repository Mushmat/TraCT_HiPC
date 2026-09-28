#ifndef SHM_COMMON_H
#define SHM_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>

#define SHM_NAME "/kv_shm_baseline"
#define CONFIG_PATH "/tmp/kv_shm_config.bin"
#define CONFIG_PATH_TMP "/tmp/kv_shm_config.bin.tmp"
#define MAX_REQUESTS 4096
#define MAX_STATIC_LENS 8

enum workload_type { WORKLOAD_STATIC = 0, WORKLOAD_SYNTHETIC = 1 };

typedef struct {
    volatile int valid;
    uint64_t hash;
    uint64_t offset;
    uint64_t length;
} cache_entry_t;

typedef struct {
    pthread_mutex_t lock;
    uint64_t next_offset;
    uint64_t data_capacity;
    uint64_t table_size;
} shm_header_t;

struct kv_config {
    uint32_t tokens_per_block;
    uint64_t bytes_per_token;
    uint64_t block_size;
    uint32_t workload_type;
    uint32_t num_static_lens;
    uint32_t static_lens[MAX_STATIC_LENS];
    uint32_t reps_per_len;
    uint32_t synthetic_num_requests;
    double synthetic_mean_input;
    double synthetic_stddev_input;
    uint32_t num_requests;
    uint64_t table_size;
    uint64_t data_capacity;
};

uint32_t generate_workload(const struct kv_config *cfg, uint32_t *out_lengths);
uint64_t block_hash(uint32_t req_idx, uint32_t block_idx);

struct stats_summary {
    double mean, p50, p99, min, max;
};
struct stats_summary compute_stats(double *values, uint32_t n);

static inline cache_entry_t *table_base(shm_header_t *hdr) {
    return (cache_entry_t *)((char *)hdr + sizeof(shm_header_t));
}
static inline char *data_base(shm_header_t *hdr) {
    return (char *)table_base(hdr) + hdr->table_size * sizeof(cache_entry_t);
}

static inline long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#endif
