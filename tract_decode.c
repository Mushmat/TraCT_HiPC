/* reader side. does NOT take any lock at all -- this is the whole point
 * of the paper's design, since the metadata publish (writing the cache
 * entry with valid=1, done last by the writer) is already the visibility
 * boundary. a reader can just RDMA READ the table slot in a loop until it
 * sees valid && hash match and knows the payload write already landed by
 * the time that's true, because the writer only flips valid after the
 * bulk payload write completed.
 *
 * needs to be run with the same --tokens-per-block/--num-requests/etc as
 * the prefill side so it recomputes the same block ids and hashes.
 *
 * usage: ./tract_decode --host-ip 172.16.201.x --host-port 7501
 *                        --bytes-per-token 4096 --tokens-per-block 64
 *                        --num-requests 20 */

#include "tract_peer.h"

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

int main(int argc, char **argv) {
    parse_args(argc, argv);

    size_t block_size = (size_t)tokens_per_block * bytes_per_token;
    printf("[decode] block_size=%zu B, expecting %d requests\n", block_size, num_requests);

    tract_peer_t peer;
    if (tract_peer_connect(&peer, host_ip, host_port, ib_port, gid_idx) != 0) {
        fprintf(stderr, "connect failed\n"); return 1;
    }

    char *payload = malloc(block_size);
    struct ibv_mr *payload_mr = ibv_reg_mr(peer.dev.pd, payload, block_size,
                                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!payload_mr) { fprintf(stderr, "reg payload mr failed\n"); return 1; }

    uint32_t table_size = TRACT_TABLE_SIZE;
    uint64_t data_base_off = off_data(table_size);

    long long total_wait_us = 0, total_xfer_us = 0;
    int total_blocks = 0;

    for (int r = 0; r < num_requests; r++) {
        int nblocks = gen_num_blocks(r, tokens_per_block, min_tokens, max_tokens);

        for (int b = 0; b < nblocks; b++) {
            uint64_t block_id = ((uint64_t)writer_id << 40) | ((uint64_t)r << 20) | (uint64_t)b;
            uint64_t h = block_hash(block_id);
            uint32_t base_idx = base_idx_of_hash(h, table_size);
            uint32_t lock_id = lock_id_for_hash(h, table_size);
            uint32_t psize = partition_size_of(table_size);
            uint32_t part_start = (lock_id - 1) * psize;

            long long t0 = tract_now_ns();
            cache_entry_t entry;
            /* no lock on this side at all -- reader just walks the same
               probe sequence the writer used. relies on the writer only
               publishing valid=1 after the payload bytes already landed,
               so seeing valid+matching hash here means the data's there.
               if a slot is valid but the wrong key, that key is settled
               (writer never un-publishes), so it's safe to move on to
               the next slot in the partition; if it's not valid yet we
               just haven't caught up to the writer, so sit and recheck */
            uint32_t idx = base_idx;
            for (;;) {
                uint64_t idx_off = off_table() + (uint64_t)idx * sizeof(cache_entry_t);
                tract_rdma_read_blocking(&peer, peer.lock_scratch, peer.lock_scratch_mr->lkey,
                                          sizeof(entry), idx_off);
                memcpy(&entry, peer.lock_scratch, sizeof(entry));
                if (entry.valid && entry.hash == h) break;
                if (entry.valid) {
                    idx = part_start + ((idx - part_start + 1) % psize);
                } else {
                    sched_yield();
                }
            }
            long long t1 = tract_now_ns();

            tract_rdma_read_blocking(&peer, payload, payload_mr->lkey, entry.length,
                                      data_base_off + entry.offset);
            long long t2 = tract_now_ns();

            total_wait_us += (t1 - t0) / 1000;
            total_xfer_us += (t2 - t1) / 1000;
            total_blocks++;
        }
        printf("req#%-4d blocks=%-4d done\n", r, nblocks);
    }

    printf("\n--- done: %d blocks, avg wait=%.1f us  avg payload-read=%.1f us ---\n",
           total_blocks, (double)total_wait_us / total_blocks, (double)total_xfer_us / total_blocks);

    return 0;
}
