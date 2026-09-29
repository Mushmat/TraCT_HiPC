# Emulating a CXL Shared-Memory KV Cache over One-Sided RDMA

Code for our HiPC 2026 Student Research Symposium paper, *Emulating a CXL
Shared-Memory KV Cache over One-Sided RDMA for Disaggregated LLM
Inference* (Chirayu Choudhary,
IIIT Bangalore).

In disaggregated LLM serving, prefill workers hand their KV cache blocks to
decode workers, usually over RDMA. TraCT (Yoon et al., arXiv:2512.18194)
replaces that hop with a CXL shared memory pool, and because CXL has no
cross-host coherence or atomics it adds a small software protocol on top:
offset-addressed objects, a shared prefix index and a two-tier lock.
Multi-host CXL is still hard to get, so we ported that protocol onto plain
one-sided RDMA and measured it against three transfer baselines on an
InfiniBand cluster:

| Path | Programs | Nodes |
|---|---|---|
| CPU load/store (POSIX shared memory, stands in for CXL) | `shm_prefill`, `shm_decode` | 1 |
| NIC: RDMA WRITE_WITH_IMM | `kv_prefill`, `kv_decode` | 2 |
| NIC: OpenSHMEM put (distributed shared memory) | `shmem_kv` | 2 |
| NIC + TraCT metadata (our TraCT-style layer) | `tract_host`, `tract_prefill`, `tract_decode` | 3 |

## Files

Shared-memory baseline
- `shm_prefill.c`, `shm_decode.c`, `shm_common.{c,h}`: prefill creates a
  pre-faulted POSIX shared memory segment (header with a process-shared
  mutex, an open-addressed table, a data area), copies each block in and
  sets its `valid` bit; decode polls the entry and reads the block.

RDMA baseline
- `kv_prefill.c`, `kv_decode.c`, `kv_common.{c,h}`: one reliable-connected
  queue pair, set up by a TCP exchange of QP number, LID, PSN, address and
  rkey. Each block is one `RDMA_WRITE_WITH_IMM` into a 4 GiB registered
  ring on the decode side; the immediate is what wakes decode.
- `verbs_common.{c,h}`: the small verbs helper both programs use (device
  and QP setup, post and poll).

OpenSHMEM baseline
- `shmem_kv.c`: prefill and decode are two PEs. Per block, prefill does
  `shmem_putmem` into a 4-slot ring in decode's symmetric heap,
  `shmem_fence`, `shmem_long_p` on a per-block ready flag and
  `shmem_quiet`; decode waits with `shmem_long_wait_until` and reads the
  block from its own memory. The source buffer lives in the symmetric heap
  too, so all memory is registered once.
- `mpi_rma_kv.c`: the same baseline written against MPI-3 one-sided RMA
  (`MPI_Win_allocate`, `MPI_Put`, `MPI_Win_flush`), for anyone who would
  rather use MPI. The paper numbers come from `shmem_kv`.

TraCT-style layer over RDMA
- `tract_host.c`: owns one registered region laid out like the CXL device
  (header, a global lock array of 17 locks x 8 node bytes, a 4096-slot
  prefix index, the data area) and runs the lock manager thread. It never
  issues RDMA itself.
- `tract_prefill.c`: writes KV blocks. Takes lock 0 (the allocator) to
  bump the offset, writes the payload without a lock, then takes the
  table stripe's lock and publishes the index entry with `valid=1` in a
  single WRITE after the payload has completed.
- `tract_decode.c`: reads blocks back without ever taking a lock, since
  the entry's `valid` bit is the visibility point.
- `tract_common.h`, `tract_lock.h`, `tract_peer.{c,h}`, `tract_rdma.{c,h}`:
  memory layout, the two-tier lock, and the QP setup shared by all three.
- `test_lock.c`: checks the lock over plain memory, with pthreads standing
  in for nodes, before going near the cluster. `./test_lock --broken`
  uses a manager that grants every waiter at once, to confirm the checker
  catches a violation.

## Building

Needs `libibverbs` (headers included) and gcc. We used GCC 4.8.5 with
MLNX_OFED on a Mellanox 40 Gb/s QDR InfiniBand cluster.

```
make                 # shm_*, kv_*, tract_*, test_lock
make shmem_kv        # needs oshcc on PATH, see below
make mpi_rma_kv      # needs mpicc on PATH
```

### OpenSHMEM

Our cluster's MPI modules were too old for proper one-sided puts, so we
built UCX and Open MPI (which ships OpenSHMEM) in our home directory. No
admin access is needed. With `ucx-1.12.1.tar.gz` and
`openmpi-4.1.6.tar.bz2` in `~/build`:

```
cd ~/build && tar xzf ucx-1.12.1.tar.gz && cd ucx-1.12.1
./contrib/configure-release --prefix=$HOME/opt/ucx --with-verbs --without-java
make -j16 && make install
$HOME/opt/ucx/bin/ucx_info -d | grep -E "Transport|Device"   # want rc_verbs / rc_mlx5 on mlx5_0:1

# build Open MPI on local disk rather than NFS to avoid clock skew trouble
mkdir -p /tmp/$USER && cd /tmp/$USER && tar xjf ~/build/openmpi-4.1.6.tar.bz2 && cd openmpi-4.1.6
./configure --prefix=$HOME/opt/ompi --with-ucx=$HOME/opt/ucx \
  --enable-oshmem --without-verbs --without-tm \
  --disable-mpi-fortran --disable-oshmem-fortran
make -j16 && make install
$HOME/opt/ompi/bin/oshmem_info | grep spml                    # want spml: ucx

export PATH=$HOME/opt/ompi/bin:$HOME/opt/ucx/bin:$PATH
export LD_LIBRARY_PATH=$HOME/opt/ompi/lib:$HOME/opt/ucx/lib:$LD_LIBRARY_PATH
```

## Running

All four baselines use 64 tokens per block, and block size is
`--tokens-per-block` x `--bytes-per-token`: 65536 gives 4 MiB blocks,
131072 gives 8 MiB, 524288 gives 32 MiB.

Shared memory (one node; decode waits for prefill on its own and exits
when done):

```
./shm_decode & ./shm_prefill --bytes-per-token 65536; wait
```

RDMA (decode listens on TCP port 7001 for the QP exchange, then the data
goes over InfiniBand):

```
ssh -n <decode-node> "$PWD/kv_decode" &
sleep 2
./kv_prefill --decode-ip <decode-node-ip> --bytes-per-token 65536
```

Both replay the same static workload, 20 requests of 1500, 3000, 4500 and
6000 tokens (1180 blocks), and print a per-request table plus a per-block
mean, p50 and p99.

OpenSHMEM (from a two-node job):

```
export UCX_NET_DEVICES=mlx5_0:1
NODES=$(sort -u $PBS_NODEFILE | tr '\n' ',' | sed 's/,$//')
oshrun -x PATH -x LD_LIBRARY_PATH -x UCX_NET_DEVICES --prefix $HOME/opt/ompi \
  --mca spml ucx -np 2 --host $NODES ./shmem_kv --sizes 4,8,12,16,24,32
```

It times 20 blocks per size after 3 warm-up blocks and prints the writer's
mean, median and p99 per block.

TraCT-style layer (host, writer and reader on three nodes; the reader
must use the same block size and request count as the writer, since both
sides recompute the same block ids and hashes):

```
./tract_host --port 7501 --num-peers 2 --data-mb 2048
./tract_prefill --host-ip <host> --host-port 7501 --num-requests 20
./tract_decode  --host-ip <host> --host-port 7501 --num-requests 20
```

Lock check, no RDMA needed:

```
./test_lock
./test_lock --broken
```

## A note on measuring

Run the network paths across nodes. In HCA loopback on a single node the
data never crosses the link, so RDMA looks faster than it really is. On our
cluster every network path, raw verbs included, shows occasional
multi-millisecond stalls, so we report per-block medians rather than means.
