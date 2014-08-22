#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "tcp.h"

#define ITER	100

void copy_tcg_context(void*arg)
{
}

int main(int argc, char **argv)
{
	int i;
	int is_server = 1;
	int package_size = 0;
	struct network_fns *tcp;
	char *addr = "127.0.0.1";
	int port = 34182;
	int sock;
	Status status;
	Status recv;
	char *buf;
	struct timeval start, end, result;

	for (;;)
	{
		int ret = getopt(argc, argv, "s:i:p:");
		if (ret == -1)
			break;

		switch(ret)
		{
		case 's':
			is_server = 0;
			package_size = atoi(optarg);
			break;
		case 'i':
			addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		default:
			fprintf(stderr, "unknown argv.\n");
			exit(0);
		}
	}

	setup_tcp_module(1);
	tcp = retrieve_tcp_fns();

	if (is_server)
	{
		int flag = 0;
		sock = tcp->Initialize(addr, port, 0);
		if (sock <= 0)
		{
			fprintf(stderr, "Server: failed to create server on addr %s with port %d. (%d)\n", addr, port, sock);
			exit(0);
		}
		
		fprintf(stderr, "Server: listen on addr %s port %d.\n", addr, port);
		buf = (char *)malloc(1 * 1024 * 1024);
		while (1)
		{
			tcp->Iprobe(ANY_SOURCE, ANY_TAG, &flag, &status);
			if (flag == 0 || status.error == STATUS_HUP)
			{
				usleep(1000);
				continue;
			}
			
			tcp->Recv(status.source, buf, status.count, status.tag, &recv);
			tcp->Send(status.source, buf, status.count, status.tag);
		}
	}
	else
	{
		int retry = 0;
		while (1)
		{
			sock = tcp->Connect(addr, port);
			if (sock > 0)
				break;
			if (retry++ == 3)
			{
				fprintf(stderr, "Client: cannot connect to %s with port %d\n", addr, port);
				exit(0);
			}
			usleep(1000);
			
		}

		fprintf(stderr, "Client: connect to %s with port %d.\n", addr, port);

		buf = (char *)malloc(package_size);
		gettimeofday(&start, NULL);
		for (i = 0; i < ITER; i++)
		{
			tcp->Send(sock, buf, package_size, 0);
			tcp->Recv(sock, buf, package_size, 0, &recv);
		}
		gettimeofday(&end, NULL);
		timersub(&end, &start, &result);
		fprintf(stderr, "%f msec\n", (double)(result.tv_sec * 1e6 + result.tv_usec) / 1000 / ITER / 2);
	}
}
