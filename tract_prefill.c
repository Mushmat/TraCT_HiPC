/* writer side. connects to the host, then for each block: grabs an offset
 * from the shared allocator (protected by its own dedicated lock, see the
 * note below on why that can't just share a table stripe), grabs the
 * table-stripe lock for where this block's hash entry lives, RDMA writes
 * the actual payload unlocked (matches the paper -- payload bytes don't
 * need the lock, only the metadata publish does), then writes the table
 * entry under the stripe lock and marks it valid.
 *
 * usage: ./tract_prefill --host-ip 172.16.201.x --host-port 7501
 *                         --bytes-per-token 4096 --tokens-per-block 64
 *                         --num-requests 20 */

#include "tract_peer.h"
#include <sys/time.h>

static char host_ip[64] = "127.0.0.1";
static int host_port = 7501;
static int ib_port = 1, gid_idx = 0;
static int bytes_per_token = 4096;
static int tokens_per_block = 64;
static int num_requests = 20;
static int min_tokens = 1500, max_tokens = 6000;
static int writer_id = 0;

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host-ip") && i+1<argc) strncpy(host_ip, argv[++i], sizeof(host_ip)-1);
        else if (!strcmp(argv[i], "--host-port") && i+1<argc) host_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ib-port") && i+1<argc) ib_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gid-idx") && i+1<argc) gid_idx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bytes-per-token") && i+1<argc) bytes_per_token = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tokens-per-block") && i+1<argc) tokens_per_block = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--num-requests") && i+1<argc) num_requests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--writer-id") && i+1<argc) writer_id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s --host-ip IP [--host-port P] [--bytes-per-token B] "
                   "[--tokens-per-block T] [--num-requests N] [--writer-id W]\n", argv[0]);
            exit(0);
        }
    }
}

/* allocator lock is deliberately its own lock id (TRACT_ALLOC_LOCK_ID),
 * separate from the hash-striped table locks. the table locks are
 * striped by hash so different nodes can hold different stripes at the
 * same time -- great for throughput, but it means the allocator's
 * next_offset counter can't ride along on a table stripe lock, because
 * two writers could be holding two different stripes and still race on
 * the same counter. giving the allocator its own dedicated lock id keeps
 * that read-modify-write actually serialized across everyone. */
static uint64_t alloc_offset(tract_peer_t *p, uint64_t len, uint32_t node_id) {
    tract_lock_acquire(&p->local_locks, &p->rm, TRACT_ALLOC_LOCK_ID, node_id);

    /* note: has to land in p->lock_scratch itself (not just any local
       var) since that's the buffer actually registered under
       lock_scratch_mr -- the NIC only knows about bytes inside the
       registered region, a stack address with someone else's lkey just
       errors out */
    tract_rdma_read_blocking(p, p->lock_scratch, p->lock_scratch_mr->lkey, sizeof(uint64_t), 0);
    uint64_t cur;
    memcpy(&cur, p->lock_scratch, sizeof(cur));

    uint64_t got = cur;
    uint64_t updated = cur + len;
    memcpy(p->lock_scratch, &updated, sizeof(updated));
    tract_rdma_write_blocking(p, p->lock_scratch, p->lock_scratch_mr->lkey, sizeof(updated), 0);

    tract_lock_release(&p->local_locks, &p->rm, TRACT_ALLOC_LOCK_ID, node_id);
    return got;
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    srand48(getpid());

    size_t block_size = (size_t)tokens_per_block * bytes_per_token;
    printf("[prefill] block_size=%zu B (%d tok/block x %d B/tok), %d requests\n",
           block_size, tokens_per_block, bytes_per_token, num_requests);

    tract_peer_t peer;
    if (tract_peer_connect(&peer, host_ip, host_port, ib_port, gid_idx) != 0) {
        fprintf(stderr, "connect failed\n"); return 1;
    }

    char *payload = malloc(block_size);
    memset(payload, 0xab, block_size); /* content doesn't matter, just moving bytes */
    struct ibv_mr *payload_mr = ibv_reg_mr(peer.dev.pd, payload, block_size,
                                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!payload_mr) { fprintf(stderr, "reg payload mr failed\n"); return 1; }

    uint32_t table_size = TRACT_TABLE_SIZE;
    uint64_t data_base_off = off_data(table_size);

    long long total_blocks_us = 0;
    int total_blocks = 0;

    for (int r = 0; r < num_requests; r++) {
        int nblocks = gen_num_blocks(r, tokens_per_block, min_tokens, max_tokens);
        long long t0 = tract_now_ns();

        for (int b = 0; b < nblocks; b++) {
            /* block id just needs to be unique-ish per request+index so the
               hash spreads out, this isn't trying to model real prefix reuse */
            /* writer_id shifts each writer into its own id space so two
               concurrent prefill processes don't compute the exact same
               block id (and therefore the exact same hash) -- otherwise
               they'd just keep stomping the same table entry, which
               doesn't actually test anything interesting. they'll still
               land in the same lock partition sometimes purely by chance,
               which is the real cross-node contention we want to see */
            uint64_t block_id = ((uint64_t)writer_id << 40) | ((uint64_t)r << 20) | (uint64_t)b;
            uint64_t h = block_hash(block_id);
            uint32_t base_idx = base_idx_of_hash(h, table_size);
            uint32_t lock_id = lock_id_for_hash(h, table_size);
            uint32_t psize = partition_size_of(table_size);
            uint32_t part_start = (lock_id - 1) * psize; /* lock_id 1 maps to partition 0, etc */

            uint64_t offset = alloc_offset(&peer, block_size, peer.node_id);

            /* bulk payload write is unlocked and happens before the
               metadata publish -- same ordering the paper uses since
               payload bytes bypass the cache entirely on real CXL/GPU
               DMA and only the metadata publish is the visibility point */
            tract_rdma_write_blocking(&peer, payload, payload_mr->lkey, block_size,
                                       data_base_off + offset);

            tract_lock_acquire(&peer.local_locks, &peer.rm, lock_id, peer.node_id);

            /* linear probe for a free slot, but only within our own
               partition -- that's what makes it safe to do this while
               some other node holds a different stripe's lock and is
               probing its own partition at the same time. if the
               partition is full this wraps forever, which just means
               table_size needs to be bigger relative to how much you're
               actually caching */
            cache_entry_t existing;
            uint32_t idx = base_idx;
            for (;;) {
                uint64_t idx_off = off_table() + (uint64_t)idx * sizeof(cache_entry_t);
                tract_rdma_read_blocking(&peer, peer.lock_scratch, peer.lock_scratch_mr->lkey,
                                          sizeof(existing), idx_off);
                memcpy(&existing, peer.lock_scratch, sizeof(existing));
                if (!existing.valid || existing.hash == h) break; /* free slot, or updating the same key */
                idx = part_start + ((idx - part_start + 1) % psize);
            }

            cache_entry_t entry;
            entry.valid = 1;
            entry.hash = h;
            entry.offset = offset;
            entry.length = (uint32_t)block_size;
            uint64_t entry_off = off_table() + (uint64_t)idx * sizeof(cache_entry_t);
            /* same deal as alloc_offset -- has to go through lock_scratch
               since that's the buffer actually registered with the NIC,
               cache_entry_t easily fits in the 64 bytes we set aside */
            memcpy(peer.lock_scratch, &entry, sizeof(entry));
            tract_rdma_write_blocking(&peer, peer.lock_scratch, peer.lock_scratch_mr->lkey, sizeof(entry), entry_off);

            tract_lock_release(&peer.local_locks, &peer.rm, lock_id, peer.node_id);
        }

        long long t1 = tract_now_ns();
        double ms = (t1 - t0) / 1e6;
        total_blocks_us += (t1 - t0) / 1000;
        total_blocks += nblocks;
        printf("req#%-4d blocks=%-4d time=%.3f ms  (%.3f ms/block)\n", r, nblocks, ms, ms / nblocks);
    }

    printf("\n--- done: %d requests, %d blocks, avg %.1f us/block ---\n",
           num_requests, total_blocks, (double)total_blocks_us / total_blocks);

    return 0;
}
