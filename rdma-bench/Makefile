CC = gcc
CFLAGS = -std=gnu99 -O2 -Wall -g
LIBS = -libverbs -lpthread -lrt -lm

COMMON_SRC = tract_rdma.c
COMMON_OBJ = tract_rdma.o

all: tract_host tract_prefill tract_decode test_lock

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

clean:
	rm -f tract_host tract_prefill tract_decode test_lock *.o
