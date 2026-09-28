#include "shm_common.h"

static double sample_gaussian(double mean, double stddev) {
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    double val = mean + z * stddev;
    if (val < 1.0) val = 1.0;
    return val;
}

uint32_t generate_workload(const struct kv_config *cfg, uint32_t *out_lengths) {
    uint32_t n = 0;
    if (cfg->workload_type == WORKLOAD_STATIC) {
        for (uint32_t i = 0; i < cfg->num_static_lens; i++) {
            for (uint32_t r = 0; r < cfg->reps_per_len; r++) {
                if (n >= MAX_REQUESTS) return n;
                out_lengths[n++] = cfg->static_lens[i];
            }
        }
    } else {
        for (uint32_t i = 0; i < cfg->synthetic_num_requests && n < MAX_REQUESTS; i++)
            out_lengths[n++] = (uint32_t)sample_gaussian(cfg->synthetic_mean_input, cfg->synthetic_stddev_input);
    }
    return n;
}

uint64_t block_hash(uint32_t req_idx, uint32_t block_idx) {
    uint64_t h = ((uint64_t)req_idx << 32) | block_idx;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

struct stats_summary compute_stats(double *values, uint32_t n) {
    struct stats_summary s;
    memset(&s, 0, sizeof(s));
    if (n == 0) return s;
    qsort(values, n, sizeof(double), cmp_double);
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += values[i];
    s.mean = sum / n;
    s.min = values[0];
    s.max = values[n - 1];
    s.p50 = values[(uint32_t)(0.50 * (n - 1))];
    s.p99 = values[(uint32_t)(0.99 * (n - 1))];
    return s;
}
