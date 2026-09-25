# CXL KV Cache over RDMA

This is the code behind our HiPC 2026 Student Research Symposium submission,
*Emulating a CXL Shared-Memory KV Cache over One-Sided RDMA for Disaggregated
LLM Inference*.

The short version of the idea: TraCT (Yoon et al.) speeds up disaggregated
LLM serving by moving the KV cache handoff off the network and onto a shared
CXL memory pool, then layers a small software protocol on top since CXL has
no cross-node coherence or atomics of its own (offset-addressed objects, a
shared prefix index, a two-tier lock). Multi-host CXL hardware is still hard
to get hold of, so we wanted to know how much of TraCT's win is the hardware
and how much is the protocol. This repo ports that protocol onto plain
one-sided RDMA (InfiniBand verbs) and measures it on a cluster we actually
have access to.

## What's here

- `rdma-bench/` - the C code: a raw RDMA verbs microbenchmark and a
  TraCT-style layer (host + prefill + decode) built on top of it.
- `paper/` - the paper source and the scripts that turn our measurements
  into the figures in it.

## rdma-bench

Everything here talks to an InfiniBand HCA through `libibverbs`. It'll also
work over soft-RoCE if you don't have real IB hardware (`rdma link add rxe0
type rxe netdev <your-eth-iface>`).

Build with:

```
cd rdma-bench
make
```

You'll need `libibverbs-dev` and a C compiler. This was built and run with
GCC 4.8.5 on the cluster, but any reasonably recent gcc/clang should be
fine.

What you get:

- `verbs_common.{c,h}` - a small RDMA verbs harness (connection setup over
  a TCP handshake, then SEND/RECV, RDMA WRITE, RDMA WRITE_WITH_IMM, and
  RDMA READ, each timed). This is what the raw verbs numbers in the paper
  come from.
- `tract_host` / `tract_prefill` / `tract_decode` - the three roles in the
  TraCT-style layer. The host owns one big registered memory region (header
  + lock array + prefix index + data area) and runs the lock manager
  thread; it never issues RDMA itself. Prefill writes KV blocks in using
  the two-tier lock and publishes index entries; decode reads them back
  without ever taking a lock, since the entry's `valid` bit is the
  visibility point.

  ```
  ./tract_host --port 7501 --num-peers 2 --data-mb 2048
  ./tract_prefill --host-ip <host> --host-port 7501 --num-requests 20
  ./tract_decode  --host-ip <host> --host-port 7501 --num-requests 20
  ```

  Decode needs to be started with the same block-size / request-count flags
  as prefill, since both sides recompute the same block ids and hashes
  independently rather than exchanging them.

- `test_lock.c` - a standalone correctness check for the two-tier lock that
  doesn't touch RDMA at all. It simulates several nodes as pthreads talking
  to plain memory instead of a remote region, so you can catch a broken
  lock before ever getting near the cluster. `./test_lock` runs the normal
  version; `./test_lock --broken` deliberately grants the lock to every
  waiter at once, just to confirm the checker actually detects a violation
  when there is one.

- `tract_common.h` / `tract_lock.h` / `tract_peer.{c,h}` / `tract_rdma.{c,h}`
  - the shared bits: memory layout helpers, the lock itself, and the QP
  setup/connection code used by all three roles.

## paper

- `main.tex` - the paper.
- `fig_timing.tex` - the three per-block timing diagrams (Fig. 1), hand
  drawn in TikZ.
- `data.py` - every number we measured, copied over from the raw cluster
  output, with a comment on where each block came from.
- `make_charts.py` - reads `data.py` and produces the three result plots
  (Fig. 2) as PDF/PNG into `figs/`.

To regenerate the figures:

```
cd paper
python make_charts.py
```

Then build the PDF the normal way (`pdflatex main.tex` twice, so the
figure references resolve).

## Authors

Chirayu Choudhary, Karthikeyan Vaidyanathan, Jerome Anand.
International Institute of Information Technology, Bangalore.
