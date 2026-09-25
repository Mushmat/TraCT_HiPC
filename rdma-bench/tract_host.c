/* the "host" node. this is standing in for the CXL device in the real
 * paper -- it owns the one shared memory region everyone else reads and
 * writes into over RDMA. the host itself never issues an RDMA op, it just
 * sits there as a passive target for however many peers connect (prefill
 * and decode workers on other nodes), and runs the lock manager thread
 * locally since it's the one process that actually has direct access to
 * the memory.
 *
 * usage: ./tract_host --port 7501 --num-peers 2 --data-mb 2048 */

#include "tract_rdma.h"
#include "tract_common.h"
#include "tract_lock.h"
#include <pthread.h>

static int listen_port = 7501;
static int expected_peers = 2;
static uint64_t data_mb = 2048;

static struct ib_dev dev;
static char *region = NULL;
static size_t region_size = 0;
static tract_header_t *hdr;
static lock_slot_t *locks;

static void *manager_thread_fn(void *arg) {
    (void)arg;
    /* just keeps sweeping forever, this is the whole lock manager from
     * the paper -- no queueing, just poll every stripe and hand the lock
     * to one waiter if nobody currently holds it */
    for (;;) {
        tract_lock_manager_pass(locks);
        sched_yield();
    }
    return NULL;
}

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--num-peers") && i + 1 < argc) expected_peers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data-mb") && i + 1 < argc) data_mb = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--port P] [--num-peers N] [--data-mb MB]\n", argv[0]);
            exit(0);
        }
    }
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    uint64_t data_capacity = data_mb * 1024ULL * 1024ULL;
    region_size = off_data(TRACT_TABLE_SIZE) + data_capacity;

    region = malloc(region_size);
    if (!region) { fprintf(stderr, "malloc %zu failed\n", region_size); return 1; }
    memset(region, 0, region_size);

    hdr = (tract_header_t *)region;
    hdr->next_offset = 0;
    hdr->data_capacity = data_capacity;
    hdr->table_size = TRACT_TABLE_SIZE;
    hdr->num_lock_slots = TRACT_NUM_LOCK_SLOTS;
    hdr->num_nodes_connected = 0;
    locks = (lock_slot_t *)(region + off_locks());

    printf("[host] region=%zu MB (table=%d entries, data=%llu MB)\n",
           region_size / (1024 * 1024), TRACT_TABLE_SIZE, (unsigned long long)data_mb);

    if (ib_dev_open(&dev, 1, 0) != 0) { fprintf(stderr, "ib_dev_open failed\n"); return 1; }

    struct ibv_mr *mr = ibv_reg_mr(dev.pd, region, region_size,
                                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { fprintf(stderr, "ibv_reg_mr failed\n"); return 1; }

    pthread_t mgr_tid;
    pthread_create(&mgr_tid, NULL, manager_thread_fn, NULL);

    int lfd = tcp_listen_on(listen_port);
    printf("[host] listening on port %d, waiting for %d peer(s)\n", listen_port, expected_peers);

    /* one QP per peer, all pointed at the same MR -- this is the part
     * that makes the host a real multi-node fan-in target instead of a
     * point to point link like the earlier RDMA baseline was */
    struct ib_qp *qps = calloc(expected_peers, sizeof(struct ib_qp));
    int *cfds = calloc(expected_peers, sizeof(int));

    for (int i = 0; i < expected_peers; i++) {
        int cfd = tcp_accept_one(lfd);
        cfds[i] = cfd;

        struct qp_handshake local, remote;
        memset(&local, 0, sizeof(local));

        if (ib_qp_create(&dev, &qps[i]) != 0) return 1;
        if (ib_qp_to_init(&dev, &qps[i]) != 0) return 1;

        local.qp_num = qps[i].qp->qp_num;
        local.psn = (uint32_t)(lrand48() & 0xffffff);
        local.lid = dev.local_lid;
        local.gid = dev.local_gid;
        local.buf_addr = (uint64_t)(uintptr_t)region;
        local.rkey = mr->rkey;
        local.node_id = (uint32_t)i;   /* host is the one assigning node ids, peers just echo it */

        if (recv_all_bytes(cfd, &remote, sizeof(remote)) < 0) { fprintf(stderr, "handshake recv failed\n"); return 1; }
        if (send_all_bytes(cfd, &local, sizeof(local)) < 0) { fprintf(stderr, "handshake send failed\n"); return 1; }

        if (ib_qp_to_rtr(&dev, &qps[i], &remote) != 0) return 1;
        if (ib_qp_to_rts(&dev, &qps[i], local.psn) != 0) return 1;

        hdr->num_nodes_connected++;
        printf("[host] peer %d connected (node_id=%d)\n", i, i);
    }

    printf("[host] all peers connected, region live. ctrl-c to stop.\n");

    /* nothing left for the host's main thread to actually do -- it never
     * initiates RDMA, the manager thread is already running, so just
     * hang around so the process (and its QPs) stay alive */
    for (;;) sleep(3600);

    return 0;
}
