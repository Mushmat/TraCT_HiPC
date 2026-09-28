/* distributed shared memory baseline, MPI-3 one-sided RMA version. same
 * idea as shmem_kv.c (which needs OpenSHMEM, not installed on this
 * cluster) -- a real cross-node put/get model instead of memcpy on one
 * box pretending to be CXL, compared against the hand-rolled verbs code.
 * rank 0 is prefill (writer), rank 1 is decode (reader). needs exactly 2
 * ranks.
 *
 * MPI_Win_allocate gives each rank a window backed by ordinary process
 * memory, and MPI_Put lets one rank write directly into another rank's
 * window without that rank doing anything -- that's the "distributed
 * shared memory" part, same as OpenSHMEM's shmem_putmem. we use passive
 * target sync (MPI_Win_lock_all once, then MPI_Win_flush per put) rather
 * than fence, since fence is a collective barrier-style epoch and would
 * force the writer to sync up with the reader every block, which is
 * exactly the round-trip we're trying to avoid -- see the note on
 * per-iteration offsets below for why.
 *
 * timing matches the other baselines:
 *   - writer time = issue the payload put, flush it, issue the flag put,
 *     flush that. flush is what makes this comparable to the RDMA WRITE
 *     numbers -- it blocks until the put has actually landed, not just
 *     been queued.
 *   - reader time = spin on the flag (the "wait" number) plus a touch of
 *     the payload (the "read" number). the flag and the payload both
 *     live in the reader's own window; the reader polls it with
 *     Win_sync + Iprobe so the writer's puts can progress (see the
 *     reader loop) -- once the flag shows up,
 *     the payload's already there too (same ordering guarantee as
 *     shmem_kv.c: payload put is flushed before the flag put is even
 *     issued, so there's no way to see the flag before the payload has
 *     landed).
 *
 * each iteration writes to its own offset and its own flag slot, so the
 * writer never has to wait for the reader to catch up before starting
 * the next block -- same decoupled prefill/decode pattern as the other
 * two baselines, and it means we don't need a round trip per block just
 * to avoid overwriting data the reader hasn't gotten to yet.
 *
 * usage: mpirun -np 2 --host nodeA,nodeB ./mpi_rma_kv
 *              [--sizes 4,8,12,16,24,32] [--iters 20] [--warmup 3]
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>

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

static double p99_us(long long *v, int n) {
    long long *tmp = malloc((size_t)n * sizeof(long long));
    memcpy(tmp, v, (size_t)n * sizeof(long long));
    qsort(tmp, n, sizeof(long long), cmp_ll);
    int idx = (int)(0.99 * (n - 1));
    double result = (double)tmp[idx] / 1000.0;
    free(tmp);
    return result;
}

int main(int argc, char **argv) {
    int sizes_mb[MAX_SIZES] = {4, 8, 12, 16, 24, 32};
    int n_sizes = 6;
    int timed_iters = 20;
    int warmup_iters = 3;

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
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--sizes 4,8,12,16,24,32] [--iters 20] [--warmup 3]\n", argv[0]);
            return 0;
        }
    }

    /* output is piped through mpirun, not a terminal, so stdio defaults to
       full buffering -- a printf per size sweep would otherwise sit in
       memory and vanish if the job gets killed (e.g. walltime limit)
       before the program exits normally. line-buffer it instead so every
       result line hits the pipe as soon as it's printed. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    MPI_Init(&argc, &argv);
    int me, nprocs;
    MPI_Comm_rank(MPI_COMM_WORLD, &me);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    const int WRITER = 0, READER = 1;

    if (nprocs != 2) {
        if (me == 0) fprintf(stderr, "this needs exactly 2 ranks (0=prefill, 1=decode), got %d\n", nprocs);
        MPI_Finalize();
        return 1;
    }

    char *payload = NULL;
    if (me == WRITER) {
        size_t max_bytes = 0;
        for (int i = 0; i < n_sizes; i++)
            if ((size_t)sizes_mb[i] * MB > max_bytes) max_bytes = (size_t)sizes_mb[i] * MB;
        payload = malloc(max_bytes);
        memset(payload, 0xab, max_bytes); /* content doesn't matter, just moving bytes */
    }

    long long *writer_t = malloc((size_t)timed_iters * sizeof(long long));
    long long *wait_t   = malloc((size_t)timed_iters * sizeof(long long));
    long long *read_t   = malloc((size_t)timed_iters * sizeof(long long));

    if (me == WRITER)
        printf("[mpi_rma_kv] writer (rank0), %d sizes, %d timed + %d warmup iters each\n",
               n_sizes, timed_iters, warmup_iters);

    for (int s = 0; s < n_sizes; s++) {
        size_t block_bytes = (size_t)sizes_mb[s] * MB;
        int count = timed_iters + warmup_iters;

        /* only the reader needs real backing memory for these windows --
           the writer is never a target, so its window is zero sized.
           MPI_Win_allocate is collective, every rank has to call it
           together, but each can pass a different size */
        char *data_arena = NULL;
        long *flags_arena = NULL;
        MPI_Win data_win, flags_win;

        /* writer gets one page instead of 0 bytes: zero-size windows are
           legal MPI but osc/rdma in older Open MPI refuses them */
        MPI_Win_allocate((MPI_Aint)((me == READER) ? (size_t)count * block_bytes : 4096),
                          1, MPI_INFO_NULL, MPI_COMM_WORLD, &data_arena, &data_win);
        MPI_Win_allocate((MPI_Aint)((me == READER) ? (size_t)count * sizeof(long) : 4096),
                          1, MPI_INFO_NULL, MPI_COMM_WORLD, &flags_arena, &flags_win);

        if (me == READER) memset(flags_arena, 0, (size_t)count * sizeof(long));
        MPI_Barrier(MPI_COMM_WORLD);
        if (me == WRITER) fprintf(stderr, "[%dMB] windows up, starting\n", sizes_mb[s]);

        if (me == WRITER) {
            MPI_Win_lock_all(0, data_win);
            MPI_Win_lock_all(0, flags_win);

            for (int i = 0; i < count; i++) {
                long one = 1;
                long long t0 = now_ns();
                MPI_Put(payload, (int)block_bytes, MPI_BYTE, READER,
                        (MPI_Aint)((size_t)i * block_bytes), (int)block_bytes, MPI_BYTE, data_win);
                MPI_Win_flush(READER, data_win); /* payload must land before the flag does */
                MPI_Put(&one, 1, MPI_LONG, READER,
                        (MPI_Aint)((size_t)i * sizeof(long)), 1, MPI_LONG, flags_win);
                MPI_Win_flush(READER, flags_win);
                long long t1 = now_ns();
                if (i >= warmup_iters) writer_t[i - warmup_iters] = t1 - t0;
            }

            MPI_Win_unlock_all(data_win);
            MPI_Win_unlock_all(flags_win);
        } else {
            /* the reader can't just spin on its own memory. with no async
               progress thread (the default over tcp), the writer's puts
               only get applied while the target is inside an MPI call, so
               a plain spin loop deadlocks: we wait for the flag, the
               writer's flush waits for us. Iprobe keeps the progress engine
               turning, and Win_sync makes the window's public copy visible
               to our loads under MPI's separate memory model. */
            MPI_Win_lock_all(0, data_win);
            MPI_Win_lock_all(0, flags_win);
            int dummy;

            for (int i = 0; i < count; i++) {
                long long t0 = now_ns();
                for (;;) {
                    MPI_Win_sync(flags_win);
                    if (*(volatile long *)&flags_arena[i] == 1) break;
                    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
                               &dummy, MPI_STATUS_IGNORE);
                }
                long long t1 = now_ns();
                MPI_Win_sync(data_win);
                volatile char touch = data_arena[(size_t)i * block_bytes];
                (void)touch;
                long long t2 = now_ns();
                if (i >= warmup_iters) {
                    wait_t[i - warmup_iters] = t1 - t0;
                    read_t[i - warmup_iters] = t2 - t1;
                }
            }

            MPI_Win_unlock_all(flags_win);
            MPI_Win_unlock_all(data_win);
        }

        MPI_Barrier(MPI_COMM_WORLD); /* neither side frees until both are done with this size */
        MPI_Win_free(&data_win);
        MPI_Win_free(&flags_win);

        if (me == WRITER) {
            double w_mean = mean_us(writer_t, timed_iters);
            double w_p99  = p99_us(writer_t, timed_iters);
            double gbps = (block_bytes / (w_mean * 1e-6)) / 1e9;
            printf("dsm_writer %2dMB mean=%8.2f us  p99=%8.2f us  %.3f GB/s\n",
                   sizes_mb[s], w_mean, w_p99, gbps);
        } else {
            double wait_mean = mean_us(wait_t, timed_iters);
            double read_mean = mean_us(read_t, timed_iters);
            printf("dsm_reader %2dMB wait_mean=%8.2f us  read_mean=%8.2f us\n",
                   sizes_mb[s], wait_mean, read_mean);
        }
    }

    MPI_Finalize();
    return 0;
}
