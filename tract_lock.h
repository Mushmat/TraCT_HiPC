#ifndef TRACT_LOCK_H
#define TRACT_LOCK_H

#include "tract_common.h"
#include <pthread.h>
#include <sched.h>

/* this is the whole point of abstracting remote access behind function
 * pointers: the real prefill/decode code fills these in with RDMA
 * read/write, but a test harness can just point them at plain memory and
 * run the exact same acquire/release code with real pthreads, no IB card
 * needed. that's how we can actually check the lock is correct before
 * ever touching the cluster */
typedef struct remote_mem {
    void *ctx;
    /* read/write a single byte at absolute offset off in the shared region */
    uint8_t (*read_byte)(struct remote_mem *self, uint64_t off);
    void    (*write_byte)(struct remote_mem *self, uint64_t off, uint8_t val);
} remote_mem_t;

static inline uint64_t lock_slot_off(uint32_t lock_id) {
    return off_locks() + (uint64_t)lock_id * sizeof(lock_slot_t);
}

static inline uint64_t lock_state_off(uint32_t lock_id, uint32_t node_id) {
    return lock_slot_off(lock_id) + node_id; /* state[] is just bytes in a row */
}

/* two tier lock, tier 1 is a plain local mutex so only one thread on this
 * process/node is ever poking the global lock array at a time (this is
 * what keeps local contention from turning into a storm of remote
 * traffic), tier 2 is the actual cross-node lock via the manager thread */
typedef struct {
    pthread_mutex_t local_locks[TRACT_NUM_LOCK_SLOTS + 1];
} tract_local_locks_t;

static inline void tract_local_locks_init(tract_local_locks_t *ll) {
    for (int i = 0; i <= TRACT_NUM_LOCK_SLOTS; i++)
        pthread_mutex_init(&ll->local_locks[i], NULL);
}

/* blocks until we hold lock_id globally. node_id is our slot index in the
 * per-lock state array (assigned once at connect time, stays fixed) */
static inline void tract_lock_acquire(tract_local_locks_t *ll, remote_mem_t *rm,
                                       uint32_t lock_id, uint32_t node_id) {
    pthread_mutex_lock(&ll->local_locks[lock_id]);

    uint64_t off = lock_state_off(lock_id, node_id);
    rm->write_byte(rm, off, LOCK_WAITING);

    /* spin-wait for the manager thread to flip us to LOCKED. busy poll is
     * what the paper does too (no queueing), the manager thread is what
     * bounds how long any one waiter sits here */
    while (rm->read_byte(rm, off) != LOCK_LOCKED) {
        sched_yield();
    }
}

static inline void tract_lock_release(tract_local_locks_t *ll, remote_mem_t *rm,
                                       uint32_t lock_id, uint32_t node_id) {
    uint64_t off = lock_state_off(lock_id, node_id);
    rm->write_byte(rm, off, LOCK_IDLE);
    pthread_mutex_unlock(&ll->local_locks[lock_id]);
}

/* runs colocated with the shared region (the host process), so it just
 * touches memory directly, no RDMA needed for its own scans -- this is
 * the piece that turns "waiting" into "locked" for exactly one node per
 * lock id at a time. one pass = one sweep over every lock id */
static inline void tract_lock_manager_pass(lock_slot_t *locks) {
    for (int lid = 0; lid <= TRACT_NUM_LOCK_SLOTS; lid++) {
        int already_locked = 0;
        int waiting_node = -1;
        for (int n = 0; n < TRACT_MAX_NODES; n++) {
            uint8_t s = locks[lid].state[n];
            if (s == LOCK_LOCKED) { already_locked = 1; break; }
            if (s == LOCK_WAITING && waiting_node < 0) waiting_node = n;
        }
        if (!already_locked && waiting_node >= 0) {
            locks[lid].state[waiting_node] = LOCK_LOCKED;
        }
    }
}

#endif
