#ifndef TRACT_COMMON_H
#define TRACT_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* max nodes that can talk to the host at once, one slot per node per lock.
 * cluster only has a handful of compute nodes so this is plenty */
#define TRACT_MAX_NODES     8

/* number of hash-striped locks protecting the table + one extra reserved
 * slot (index 0) for the allocator lock, kept separate on purpose, see
 * notes in tract_lock.h about why the allocator can't just reuse a stripe */
#define TRACT_NUM_LOCK_SLOTS 16
#define TRACT_ALLOC_LOCK_ID  0   /* stripe 0 is reserved, real stripes start at 1 */

#define TRACT_TABLE_SIZE     4096   /* hash table slots for the prefix cache index */

/* lock slot states, same 3-state thing the paper uses: idle/waiting/locked */
enum lock_state {
    LOCK_IDLE = 0,
    LOCK_WAITING = 1,
    LOCK_LOCKED = 2
};

/* one entry in the global lock array. one state byte per node so the
 * manager thread can see who is waiting without any node stepping on
 * another node's byte */
typedef struct {
    uint8_t state[TRACT_MAX_NODES];
} lock_slot_t;

/* one entry in the shared prefix-cache hash table. offset is relative to
 * the start of the data area, not an absolute pointer -- pointers don't
 * mean anything once you cross nodes */
typedef struct {
    uint32_t valid;
    uint64_t hash;
    uint64_t offset;
    uint32_t length;
} cache_entry_t;

/* sits at offset 0 of the shared region, basically the superblock */
typedef struct {
    uint64_t next_offset;     /* bump allocator cursor into the data area */
    uint64_t data_capacity;
    uint32_t table_size;
    uint32_t num_lock_slots;
    uint32_t num_nodes_connected;
} tract_header_t;

/* murmur-ish 64 bit mix, same one used in the shm baseline, good enough
 * for spreading block ids across the hash table */
static inline uint64_t block_hash(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* the hash table is split into TRACT_NUM_LOCK_SLOTS contiguous chunks,
 * one per lock stripe, so a writer only ever touches slots covered by
 * the one lock it's already holding. collisions get resolved by linear
 * probing but only within that chunk -- this is what makes it safe for
 * probing to happen while striped locks let different nodes write to
 * different chunks at the same time. table_size should be a lot bigger
 * than however many blocks you expect to actually cache, same as any
 * open addressing table, or a chunk can fill up and probing just wraps
 * forever */
static inline uint32_t partition_size_of(uint32_t table_size) {
    return table_size / TRACT_NUM_LOCK_SLOTS;
}
static inline uint32_t partition_of_hash(uint64_t h, uint32_t table_size) {
    uint32_t psize = partition_size_of(table_size);
    return (uint32_t)(h % table_size) / psize;
}
static inline uint32_t base_idx_of_hash(uint64_t h, uint32_t table_size) {
    uint32_t psize = partition_size_of(table_size);
    uint32_t part = partition_of_hash(h, table_size);
    return part * psize + (uint32_t)(h % psize);
}
static inline uint32_t lock_id_for_hash(uint64_t h, uint32_t table_size) {
    return 1 + partition_of_hash(h, table_size); /* stripe 0 stays reserved for the alloc lock */
}

/* layout helpers -- given the header, work out where each region starts
 * inside the one big registered buffer. keeping this in one place so
 * host/prefill/decode never disagree about where things live */
static inline uint64_t off_locks(void)  { return sizeof(tract_header_t); }
static inline uint64_t off_table(void) {
    return off_locks() + (uint64_t)(TRACT_NUM_LOCK_SLOTS + 1) * sizeof(lock_slot_t);
}
static inline uint64_t off_data(uint32_t table_size) {
    return off_table() + (uint64_t)table_size * sizeof(cache_entry_t);
}

/* quick and dirty synthetic workload, just enough to drive the prefill
 * loop through a bunch of differently sized requests. nothing fancy like
 * the gaussian stuff in the earlier kv baseline, this is about exercising
 * the lock + hash table, not modeling real request distributions */
/* payload check: every 4 KiB page of a block starts with a tag derived from
 * the block's hash and the page index. the data area starts zeroed and the
 * bump allocator never reuses an offset, so a page the writer hasn't
 * landed yet still reads as zero -- a reader that sees valid=1 but a page
 * without its tag has caught a torn or stale payload. */
#define TRACT_PAGE 4096
static inline uint64_t page_tag(uint64_t h, size_t page) {
    return (h ^ (0x9e3779b97f4a7c15ULL * (page + 1))) | 1;
}
static inline void stamp_payload(char *buf, size_t len, uint64_t h) {
    for (size_t p = 0; p * TRACT_PAGE < len; p++) {
        uint64_t t = page_tag(h, p);
        memcpy(buf + p * TRACT_PAGE, &t, sizeof(t));
    }
}
static inline int payload_ok(const char *buf, size_t len, uint64_t h) {
    for (size_t p = 0; p * TRACT_PAGE < len; p++) {
        uint64_t t;
        memcpy(&t, buf + p * TRACT_PAGE, sizeof(t));
        if (t != page_tag(h, p)) return 0;
    }
    return 1;
}

static int cmp_ll_(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}
/* sorts v in place; returns the p-th quantile in us */
static inline double pct_us(long long *v, int n, double p) {
    qsort(v, n, sizeof(long long), cmp_ll_);
    return v[(int)(p * (n - 1))] / 1000.0;
}
static inline double mean_us(const long long *v, int n) {
    long long s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return (double)s / n / 1000.0;
}

static inline int gen_num_blocks(int req_idx, int tokens_per_block, int min_tok, int max_tok) {
    unsigned seed = (unsigned)(req_idx * 2654435761u + 12345);
    int range = max_tok - min_tok + 1;
    int tok = min_tok + (int)(seed % (unsigned)range);
    int blocks = tok / tokens_per_block;
    return blocks < 1 ? 1 : blocks;
}

static inline long long tract_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#endif
