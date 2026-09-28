CC = gcc
CFLAGS = -std=gnu99 -O2 -Wall -g
LIBS = -libverbs -lpthread -lrt -lm

# OpenSHMEM compiler wrapper, only needed for shmem_kv. we used the one from
# Open MPI 4.1.6 built --with-ucx --enable-oshmem (see the README); put its
# bin/ first on PATH before building.
OSHCC = oshcc

# MPI compiler wrapper, only needed for mpi_rma_kv. the same Open MPI build
# provides it.
MPICC = mpicc

COMMON_SRC = tract_rdma.c
COMMON_OBJ = tract_rdma.o

all: shm_prefill shm_decode kv_prefill kv_decode tract_host tract_prefill tract_decode test_lock

# shared-memory baseline (one node, stands in for CXL)
shm_prefill: shm_prefill.c shm_common.c shm_common.h
	$(CC) $(CFLAGS) -o $@ shm_prefill.c shm_common.c -lpthread -lrt -lm

shm_decode: shm_decode.c shm_common.c shm_common.h
	$(CC) $(CFLAGS) -o $@ shm_decode.c shm_common.c -lpthread -lrt -lm

# RDMA WRITE_WITH_IMM baseline (prefill and decode on two nodes)
kv_prefill: kv_prefill.c kv_common.c verbs_common.c kv_common.h verbs_common.h
	$(CC) $(CFLAGS) -o $@ kv_prefill.c kv_common.c verbs_common.c $(LIBS)

kv_decode: kv_decode.c kv_common.c verbs_common.c kv_common.h verbs_common.h
	$(CC) $(CFLAGS) -o $@ kv_decode.c kv_common.c verbs_common.c $(LIBS)

tract_host: tract_host.c $(COMMON_OBJ) tract_common.h tract_lock.h tract_rdma.h
	$(CC) $(CFLAGS) -o $@ tract_host.c $(COMMON_OBJ) $(LIBS)

tract_prefill: tract_prefill.c tract_peer.c $(COMMON_OBJ) tract_common.h tract_lock.h tract_rdma.h tract_peer.h
	$(CC) $(CFLAGS) -o $@ tract_prefill.c tract_peer.c $(COMMON_OBJ) $(LIBS)

tract_decode: tract_decode.c tract_peer.c $(COMMON_OBJ) tract_common.h tract_lock.h tract_rdma.h tract_peer.h
	$(CC) $(CFLAGS) -o $@ tract_decode.c tract_peer.c $(COMMON_OBJ) $(LIBS)

test_lock: test_lock.c tract_common.h tract_lock.h
	$(CC) $(CFLAGS) -o $@ test_lock.c -lpthread

$(COMMON_OBJ): tract_rdma.c tract_rdma.h tract_common.h
	$(CC) $(CFLAGS) -c -o $@ tract_rdma.c

# not part of `all` on purpose -- needs an OpenSHMEM install, which the
# verbs targets above don't
shmem_kv: shmem_kv.c
	$(OSHCC) -std=gnu99 -O2 -Wall -g -o $@ shmem_kv.c

# same baseline written against MPI-3 one-sided RMA instead of OpenSHMEM
mpi_rma_kv: mpi_rma_kv.c
	$(MPICC) -std=gnu99 -O2 -Wall -g -o $@ mpi_rma_kv.c

clean:
	rm -f shm_prefill shm_decode kv_prefill kv_decode tract_host tract_prefill tract_decode test_lock shmem_kv mpi_rma_kv *.o
