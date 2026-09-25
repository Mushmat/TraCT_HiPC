/* standalone correctness check for the two tier lock, no RDMA involved.
 * point remote_mem_t at plain memory instead of an RDMA READ/WRITE and
 * run a bunch of real pthreads through acquire/release plus a manager
 * thread doing the same sweeps it would do on the host. if this doesn't
 * hold up here it definitely won't hold up over the network, so this is
 * worth getting right before wiring up any actual RDMA */
#include "tract_common.h"
#include "tract_lock.h"
#include <pthread.h>

/* master node's gcc is old (4.8.5) and doesn't have C11 stdatomic.h, so
 * using the gcc __sync builtins instead -- these have been around since
 * gcc 4.1 and do the same job for plain ints/longs */

/* 3 fake "nodes", 2 threads on each. threads on the same node share that
 * node's local lock (tier 1), different nodes only meet at the global
 * lock array (tier 2). first version of this test had every thread share
 * one local lock, which meant the mutex did all the work and the global
 * array never saw more than one waiter -- so it wasn't testing much */
#define NUM_NODES        3
#define THREADS_PER_NODE 2
#define NUM_WORKERS      (NUM_NODES * THREADS_PER_NODE)
#define ITERS_PER_WORKER 2000

/* just one lock slot in play (plus the reserved alloc slot at 0) so we
 * get real contention instead of everyone spreading across stripes */
#define TEST_LOCK_ID 1

static uint8_t shared_region[4096]; /* way more than we need, just backing bytes */
static lock_slot_t *locks_view;      /* same bytes, viewed as lock_slot_t[] */

static volatile int in_critical_section = 0;
static volatile int violations = 0;      /* should stay 0 -- two workers in the CS at once */
static volatile long shared_counter = 0; /* should end up exactly NUM_WORKERS*ITERS if CS is real */
static volatile int manager_running = 1;
static volatile int last_node = -1;
static volatile long node_handoffs = 0;  /* times the lock moved from one node to another */
static int max_waiting_nodes = 0;         /* most nodes seen waiting at once by the manager */
static int broken_manager = 0;            /* ./test_lock --broken hands the lock to everyone, should FAIL */

static uint8_t mem_read_byte(remote_mem_t *self, uint64_t off) {
    (void)self;
    return shared_region[off];
}
static void mem_write_byte(remote_mem_t *self, uint64_t off, uint8_t val) {
    (void)self;
    shared_region[off] = val;
}

typedef struct {
    int node_id;
    tract_local_locks_t *ll;
    remote_mem_t *rm;
} worker_arg_t;

static void *worker_fn(void *argp) {
    worker_arg_t *a = argp;
    for (int i = 0; i < ITERS_PER_WORKER; i++) {
        tract_lock_acquire(a->ll, a->rm, TEST_LOCK_ID, a->node_id);

        int before = __sync_fetch_and_add(&in_critical_section, 1);
        if (before != 0) __sync_fetch_and_add(&violations, 1); /* someone else was already in here */
        if (last_node != a->node_id) { node_handoffs++; last_node = a->node_id; }
        /* do a bit of "work" so a race actually has a chance to show up */
        long v = shared_counter;
        for (volatile int k = 0; k < 50; k++) ;
        shared_counter = v + 1;
        __sync_fetch_and_sub(&in_critical_section, 1);

        tract_lock_release(a->ll, a->rm, TEST_LOCK_ID, a->node_id);
    }
    return NULL;
}

static void manager_sweep(void) {
    int waiting = 0;
    for (int n = 0; n < TRACT_MAX_NODES; n++)
        if (locks_view[TEST_LOCK_ID].state[n] == LOCK_WAITING) waiting++;
    if (waiting > max_waiting_nodes) max_waiting_nodes = waiting;

    if (broken_manager) {
        /* deliberately wrong: grant every waiter, just to prove the test can catch it */
        for (int lid = 0; lid <= TRACT_NUM_LOCK_SLOTS; lid++)
            for (int n = 0; n < TRACT_MAX_NODES; n++)
                if (locks_view[lid].state[n] == LOCK_WAITING) locks_view[lid].state[n] = LOCK_LOCKED;
        return;
    }
    tract_lock_manager_pass(locks_view);
}

static void *manager_fn(void *argp) {
    (void)argp;
    while (manager_running) {
        manager_sweep();
        sched_yield();
    }
    /* a couple more passes after the flag drops so nobody's left hanging
     * mid-shutdown waiting on a sweep that never comes */
    for (int i = 0; i < 100; i++) manager_sweep();
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--broken")) broken_manager = 1;

    memset(shared_region, 0, sizeof(shared_region));
    locks_view = (lock_slot_t *)(shared_region + off_locks());

    remote_mem_t rm = { .ctx = NULL, .read_byte = mem_read_byte, .write_byte = mem_write_byte };
    tract_local_locks_t node_locks[NUM_NODES];   /* one local lock set per node */
    for (int n = 0; n < NUM_NODES; n++) tract_local_locks_init(&node_locks[n]);

    pthread_t mgr;
    pthread_create(&mgr, NULL, manager_fn, NULL);

    pthread_t workers[NUM_WORKERS];
    worker_arg_t args[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        int node = i % NUM_NODES;
        args[i].node_id = node;
        args[i].ll = &node_locks[node];
        args[i].rm = &rm;
        pthread_create(&workers[i], NULL, worker_fn, &args[i]);
    }
    for (int i = 0; i < NUM_WORKERS; i++) pthread_join(workers[i], NULL);

    manager_running = 0;
    pthread_join(mgr, NULL);

    long expected = (long)NUM_WORKERS * ITERS_PER_WORKER;
    long got = shared_counter;
    int viol = violations;

    printf("%d nodes x %d threads, %d iters each%s\n", NUM_NODES, THREADS_PER_NODE, ITERS_PER_WORKER,
           broken_manager ? " (BROKEN manager on purpose)" : "");
    printf("expected counter = %ld, got = %ld\n", expected, got);
    printf("mutual exclusion violations = %d\n", viol);
    printf("lock moved between nodes %ld times, up to %d nodes waiting at once\n",
           node_handoffs, max_waiting_nodes);

    if (got == expected && viol == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
