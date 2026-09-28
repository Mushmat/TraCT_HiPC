#ifndef TRACT_PEER_H
#define TRACT_PEER_H

/* shared bits between prefill and decode -- both are just "peers" that
 * connect to the host and get handed the remote addr/rkey for the whole
 * shared region, plus their own node_id in the lock arrays */

#include "tract_rdma.h"
#include "tract_common.h"
#include "tract_lock.h"

typedef struct {
    struct ib_dev dev;
    struct ib_qp qp;
    uint64_t remote_base;   /* remote address of the start of the shared region */
    uint32_t remote_rkey;
    uint32_t node_id;

    /* tiny local scratch buffer + its own MR, used for the single-byte
     * lock reads/writes so we're not reusing the big data buffer's MR
     * for something completely unrelated */
    uint8_t *lock_scratch;
    struct ibv_mr *lock_scratch_mr;

    remote_mem_t rm;              /* wraps this peer's RDMA ops for tract_lock.h */
    tract_local_locks_t local_locks;
} tract_peer_t;

int tract_peer_connect(tract_peer_t *p, const char *host_ip, int host_port,
                        int ib_port, int gid_idx);

/* blocking RDMA write/read of len bytes to/from an offset in the shared
 * region, using the caller's own registered local buffer+mr+lkey */
void tract_rdma_write_blocking(tract_peer_t *p, void *local_buf, uint32_t lkey,
                                size_t len, uint64_t remote_off);
void tract_rdma_read_blocking(tract_peer_t *p, void *local_buf, uint32_t lkey,
                               size_t len, uint64_t remote_off);

#endif
