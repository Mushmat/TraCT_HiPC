# Every number here is copied from raw cluster output (see SOURCE comments).
MB = 1024 * 1024

# ---- kv_shm_baseline: POSIX shm memcpy, single node (master), static workload,
# 20 requests / 1180 blocks. SOURCE: transcript user msgs 545/549/553.
shm = {  # block MB: (mean per-block us, mean per-request GB/s, p99 per-block us)
    4:  (360.82, 11.616, 402.88),
    8:  (620.07, 13.533, 668.63),   # default run (second 8MB run: 629.52 us, 13.258)
    12: (887.48, 13.547, 925.63),   # 12025856 B = 11.47 MiB, plotted at 11.47
    16: (1235.39, 13.581, 1276.74),
    24: (2114.09, 11.913, 2238.55),
    32: (5017.17, 7.006, 13714.09),
}
shm_exact_mb = {4: 4, 8: 8, 12: 12025856 / MB, 16: 16, 24: 24, 32: 32}

# ---- kv_rdma_baseline: RDMA WRITE_WITH_IMM prefill->decode, master node via HCA
# loopback (--decode-ip 127.0.0.1), with warmup fix. SOURCE: upload 3046607a.
rdma = {  # block MB: (mean per-block us, mean per-request GB/s, p99 per-block us)
    4:  (847.68, 4.941, 863.89),
    8:  (1689.22, 4.947, 1731.51),
    32: (6761.79, 4.958, 7142.61),
}

# ---- verbs microbenchmark (master node), RDMA WRITE. SOURCE: result2.txt
# client.c prints MiB/s (divides by 1024^2), so recompute in decimal GB/s from latency
verbs_write_4MB_GBps = 4194304 / 728.89e-6 / 1e9   # = 5.75 GB/s
verbs_write_256K_us = 48.68
verbs_read_256K_us = 48.75
verbs_write_small_us = 1.34                # 1 B RDMA WRITE
verbs_read_small_us = 1.48                 # 1 B RDMA READ

# ---- tract_rdma (TraCT-style layer over RDMA). SOURCE: this session's pastes.
# 256 KiB blocks (4096 B/token x 64 tok), 20 requests, 1193 blocks
tract_256k = [
    # (label, nodes, writer us/blk, reader wait us, reader payload-read us)
    ("1 node, reader after writer", 1, 71.7, 1.4, 63.1),
    ("1 node, reader concurrent", 1, 112.6, 2236.4, 51.6),
    ("3 nodes, reader concurrent", 3, 133.4, 2426.1, 71.3),
    ("3 nodes, 2 writers (W0)", 3, 93.0, 2.4, 71.3),
    ("3 nodes, 2 writers (W1)", 3, 95.2, 4.7, 71.3),
]
# 3 nodes (host compute00, writer compute01, reader compute02), reader started first
tract = {  # block MB: (writer us/blk, reader wait us, reader payload-read us)
    4:  (1137.8, 2644.4, 1066.1),
    8:  (2218.4, 1636.9, 2125.8),
    32: (16520.5, 9532.8, 8476.9),
}

# blocks per request for the tract synthetic workload (same for every run)
tract_blocks = [75, 44, 75, 44, 74, 34, 73, 34, 73, 33, 63, 32, 63, 23, 62, 93, 62, 92, 52, 92]
# 32 MiB tract writer, per-request ms/block (3 nodes)
tract32_ms_per_blk = [8.558, 8.576, 8.578, 8.597, 8.577, 9.196, 10.559, 11.903, 13.180, 14.277,
                      15.361, 16.391, 17.384, 18.200, 19.266, 20.925, 22.779, 25.099, 27.769, 30.435]
# 32 MiB shm per-request GB/s (static workload)
shm32_gbps = [7.763, 7.802, 7.812, 7.833, 7.779, 7.418, 7.417, 7.797, 7.682, 6.763,
              6.865, 6.862, 6.443, 6.247, 6.222, 6.285, 6.210, 6.235, 6.417, 6.270]
# 32 MiB kv_rdma_baseline per-request GB/s (warmup-fixed run)
rdma32_gbps = [4.900, 4.908, 4.910, 4.948, 4.997, 4.998, 4.930, 4.970, 4.934, 4.987,
               5.015, 4.920, 5.006, 4.952, 4.998, 4.929, 4.990, 4.919, 4.989, 4.961]
shm_blocks = [24]*5 + [47]*5 + [71]*5 + [94]*5

def gbps(block_bytes, us):
    return block_bytes / (us * 1e-6) / 1e9

if __name__ == "__main__":
    for mb, (w, wait, rd) in tract.items():
        b = mb * MB
        print(f"tract {mb}MB writer {gbps(b,w):.2f} GB/s reader {gbps(b,rd):.2f} GB/s")
    for mb, (us, g, _) in rdma.items():
        print(f"rdma {mb}MB per-block {gbps(mb*MB,us):.2f} GB/s (per-request mean {g})")
    for mb, (us, g, _) in shm.items():
        print(f"shm {mb}MB per-block {gbps(shm_exact_mb[mb]*MB,us):.2f} GB/s (per-request mean {g})")
    print("writer overhead vs rdma baseline:",
          {mb: f"{(tract[mb][0]/rdma[mb][0]-1)*100:.1f}%" for mb in rdma})
    print("metadata cost @256K (tract 1-node writer - verbs write 256K):", 71.7 - verbs_write_256K_us)
    print("shm/rdma speedup:", {mb: round(shm[mb][1]/rdma[mb][1],2) for mb in rdma})
    t32 = [b*32*MB/(ms*1e-3)/1e9 for b, ms in zip(tract_blocks, [m*bb for m,bb in zip(tract32_ms_per_blk,tract_blocks)])]
    print("tract32 per-request GB/s first/last:", round(32*MB/(8.558e-3)/1e9,2), round(32*MB/(30.435e-3)/1e9,2))
    print("cumulative GB written by tract32 at req i:",
          [round(sum(tract_blocks[:i+1])*32*MB/1e9,1) for i in range(20)])
    print("cumulative GB shm32:", [round(sum(shm_blocks[:i+1])*32*MB/1e9,1) for i in range(20)])
