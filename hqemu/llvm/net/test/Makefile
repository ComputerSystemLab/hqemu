
top = /home/tera/research/qemu/i386-linux-user
obj = $(top)/llvm/utils.o $(top)/llvm/net/sock.o $(top)/llvm/net/tcp.o

all: latency.o
	gcc -O2 -o latency latency.o $(obj) -lpthread -lm

.c.o:
	gcc -I.. -I../.. -O2 -c -o $@ $<

clean:
	rm -f *.o latency
