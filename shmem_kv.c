/* distributed shared memory baseline, using OpenSHMEM instead of a
 * single-node mmap. this is what the earlier "(a) shared-memory baseline"
 * in the paper couldn't actually test: a real cross-node PGAS put/get
 * model, not memcpy on one box pretending to be CXL. PE 0 is prefill
 * (writer), PE 1 is decode (reader). needs exactly 2 PEs.
 *
 * the whole point of comparing this against the RDMA verbs baseline is
 * that both ultimately move bytes over the same InfiniBand fabric -- the
 * difference is the programming model (PGAS put/get vs hand-rolled verbs)
 * and whatever the OpenSHMEM runtime does under the hood (it's usually
 * verbs or UCX itself on an IB cluster, so this tells us what that layer
 * costs on top, not a different physical transport).
 *
 * per block size, timing works the same way as the verbs baselines:
 *   - writer time = issue the put, fence, put the ready flag, quiet.
 *     quiet is what makes this comparable to the RDMA WRITE numbers,
 *     since it blocks until the put is actually complete, not just
 *     queued.
 *   - reader time = wait on the flag (this is the "wait" number) plus a
 *     touch of the payload (the "read" number). there's no real transfer
 *     cost on the read side since shmem_putmem already delivered the
 *     bytes into the reader's own address space by the time the flag is
 *     visible -- same as a CXL load, that's the point of a PGAS model.
 *
 * each iteration gets its own flag slot and writes into a small ring of
 * data slots, so the writer never has to wait for the reader before
 * starting the next block. that's deliberate -- it's the same decoupled prefill/
 * decode pattern the other two baselines use, and it avoids needing a
 * round trip per block just to avoid stomping on memory the reader
 * hasn't gotten to yet.
 *
 * usage: oshrun -np 2 [--hostfile hosts | --host a,b] ./shmem_kv
 *              [--sizes 4,8,12,16,24,32] [--iters 20] [--warmup 3]
 */

#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1024UL * 1024UL)
#define MAX_SIZES 16

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

static double mean_us(long long *v, int n) {
    long long sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    return (double)sum / n / 1000.0;
}

static double pct_us(long long *v, int n, double p) {
    long long *tmp = malloc((size_t)n * sizeof(long long));
    memcpy(tmp, v, (size_t)n * sizeof(long long));
    qsort(tmp, n, sizeof(long long), cmp_ll);
    int idx = (int)(p * (n - 1));
    double result = (double)tmp[idx] / 1000.0;
    free(tmp);
    return result;
}

int main(int argc, char **argv) {
    int sizes_mb[MAX_SIZES] = {4, 8, 12, 16, 24, 32};
    int n_sizes = 6;
    int timed_iters = 20;
    int warmup_iters = 3;
    int n_slots = 4;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sizes") && i + 1 < argc) {
            n_sizes = 0;
            char *tok = strtok(argv[++i], ",");
            while (tok && n_sizes < MAX_SIZES) {
                sizes_mb[n_sizes++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            timed_iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) {
            warmup_iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--slots") && i + 1 < argc) {
            n_slots = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--sizes 4,8,12,16,24,32] [--iters 20] [--warmup 3] [--slots 4]\n", argv[0]);
            return 0;
        }
    }

    /* stdout goes through oshrun's pipe, not a tty -- line-buffer it so
       results aren't lost if the job gets killed before exit */
    setvbuf(stdout, NULL, _IOLBF, 0);

    shmem_init();
    int me = shmem_my_pe();
    int npes = shmem_n_pes();

    if (npes != 2) {
        if (me == 0) fprintf(stderr, "this needs exactly 2 PEs (0=prefill, 1=decode), got %d\n", npes);
        shmem_finalize();
        return 1;
    }

    const int WRITER = 0, READER = 1;
    /* source buffer comes from the symmetric heap, not malloc: the heap is
       registered with the HCA once at startup, whereas a malloc'd source
       gets registered (or bounced) on every put, which made the numbers
       slow and all over the place. the verbs baseline registers its
       source MR up front too, so this keeps the comparison fair.
       shmem_malloc is collective, so the reader allocates it as well. */
    size_t max_bytes = 0;
    for (int i = 0; i < n_sizes; i++)
        if ((size_t)sizes_mb[i] * MB > max_bytes) max_bytes = (size_t)sizes_mb[i] * MB;
    char *payload = shmem_malloc(max_bytes);
    if (!payload) {
        fprintf(stderr, "PE%d: shmem_malloc failed for the %zu byte source buffer\n", me, max_bytes);
        shmem_finalize();
        return 1;
    }
    if (me == WRITER) memset(payload, 0xab, max_bytes); /* content doesn't matter */

    long long *writer_t = malloc((size_t)timed_iters * sizeof(long long));
    long long *wait_t   = malloc((size_t)timed_iters * sizeof(long long));
    long long *read_t   = malloc((size_t)timed_iters * sizeof(long long));

    if (me == WRITER)
        printf("[shmem_kv] writer (PE0), %d sizes, %d timed + %d warmup iters each, %d slots\n",
               n_sizes, timed_iters, warmup_iters, n_slots);

    for (int s = 0; s < n_sizes; s++) {
        size_t block_bytes = (size_t)sizes_mb[s] * MB;
        int count = timed_iters + warmup_iters;

        /* small ring of data slots, reused round robin, like the verbs
         * baseline's ring. one slot per iteration needed 23 x 32 MiB of
         * symmetric heap, more than Open MPI gives by default, and its
         * size knob isn't honoured consistently across versions. the
         * reader only touches a byte of each block, so a slot being
         * overwritten before it's read doesn't change what we time.
         * flags stay one per iteration so every wait is for a fresh put. */
        char *data_arena  = shmem_malloc((size_t)n_slots * block_bytes);
        long *flags_arena = shmem_malloc((size_t)count * sizeof(long));
        if (!data_arena || !flags_arena) {
            fprintf(stderr, "PE%d: shmem_malloc failed at %d MiB (arena too big?)\n", me, sizes_mb[s]);
            shmem_finalize();
            return 1;
        }
        if (me == READER) memset(flags_arena, 0, (size_t)count * sizeof(long));
        shmem_barrier_all();

        if (me == WRITER) {
            for (int i = 0; i < count; i++) {
                long long t0 = now_ns();
                shmem_putmem(data_arena + (size_t)(i % n_slots) * block_bytes, payload, block_bytes, READER);
                shmem_fence();               /* payload put must land before the flag put */
                shmem_long_p(&flags_arena[i], 1, READER);
                shmem_quiet();                /* block until both puts are actually complete */
                long long t1 = now_ns();
                if (i >= warmup_iters) writer_t[i - warmup_iters] = t1 - t0;
            }
        } else {
            for (int i = 0; i < count; i++) {
                long long t0 = now_ns();
                shmem_long_wait_until(&flags_arena[i], SHMEM_CMP_EQ, 1);
                long long t1 = now_ns();
                /* data's already local at this point -- a PGAS put lands
                   directly in our address space, this is just touching it
                   so the compiler can't optimize the access away */
                volatile char touch = data_arena[(size_t)(i % n_slots) * block_bytes];
                (void)touch;
                long long t2 = now_ns();
                if (i >= warmup_iters) {
                    wait_t[i - warmup_iters] = t1 - t0;
                    read_t[i - warmup_iters] = t2 - t1;
                }
            }
        }

        shmem_barrier_all(); /* neither side frees until both are done with this size */
        shmem_free(data_arena);
        shmem_free(flags_arena);

        if (me == WRITER) {
            /* median alongside the mean: with 20 samples a single stall
               drags the mean far from what a typical block costs */
            double w_mean = mean_us(writer_t, timed_iters);
            double w_med  = pct_us(writer_t, timed_iters, 0.5);
            double w_p99  = pct_us(writer_t, timed_iters, 0.99);
            double gbps = (block_bytes / (w_med * 1e-6)) / 1e9;
            printf("dsm_writer %2dMB mean=%8.2f us  median=%8.2f us  p99=%8.2f us  %.3f GB/s@median\n",
                   sizes_mb[s], w_mean, w_med, w_p99, gbps);
        } else {
            double wait_mean = mean_us(wait_t, timed_iters);
            double read_mean = mean_us(read_t, timed_iters);
            printf("dsm_reader %2dMB wait_mean=%8.2f us  read_mean=%8.2f us\n",
                   sizes_mb[s], wait_mean, read_mean);
        }
    }

    shmem_free(payload);
    shmem_finalize();
    return 0;
}
