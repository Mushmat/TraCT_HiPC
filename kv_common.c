#ifndef KV_COMMON_H
#define KV_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "verbs_common.h"

#define KV_TCP_PORT       7001   /* separate from the plain verbs benchmark's 6001 */
#define MAX_REQUESTS      4096
#define MAX_STATIC_LENS   8

enum workload_type { WORKLOAD_STATIC = 0, WORKLOAD_SYNTHETIC = 1 };

/* Everything needed to reproduce the exact same workload on both sides.
 * The prefill worker decides these (from its CLI args) and sends this
 * struct to the decode worker over the control-channel TCP socket, so
 * there's no risk of the two sides disagreeing about block size, cache
 * capacity, or request count -- unlike task 2's benchmark, where sizes
 * had to match via hardcoded constants on both sides. Sent as a raw
 * struct: fine within one cluster of matching x86_64 machines, but not
 * portable across architectures/compilers -- a real system would use a
 * proper serialization format here. */
struct kv_config {
    uint32_t tokens_per_block;
    uint64_t bytes_per_token;    /* see note in kv_prefill.c on how this is estimated */
    uint64_t block_size;         /* = tokens_per_block * bytes_per_token, computed */
    uint64_t cache_capacity;     /* decode-side registered buffer size (bytes);
                                     block placement wraps within this, like a
                                     bounded prefix-cache region */
    uint32_t workload_type;      /* enum workload_type */
    uint32_t num_static_lens;
    uint32_t static_lens[MAX_STATIC_LENS];
    uint32_t reps_per_len;       /* repetitions per static length (all timed;
                                     a separate global warmup block precedes
                                     the whole run, see kv_prefill.c) */
    uint32_t synthetic_num_requests;
    double   synthetic_mean_input;
    double   synthetic_stddev_input;
    uint32_t num_requests;       /* resolved total request count, filled in after generation */
};

/* Generates the actual list of requests (input token count per request)
 * from the config. Both static and synthetic paths write into
 * `out_lengths` (caller-allocated, size >= MAX_REQUESTS) and return the
 * count written. This is called ONCE, on the prefill side; the resulting
 * array is sent to decode directly rather than regenerated independently. */
uint32_t generate_workload(const struct kv_config *cfg, uint32_t *out_lengths);

/* ---- simple statistics over a latency/size sample array ---- */
struct stats_summary {
    double mean;
    double p50;
    double p99;
    double min;
    double max;
};

/* Computes mean/p50/p99/min/max. Sorts `values` in place (ascending). */
struct stats_summary compute_stats(double *values, uint32_t n);

#endif /* KV_COMMON_H */