/*
 *  (C) 2007 by System Software Laboratory, National Tsing Hua Univerity, Taiwan.
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __SOCK_H
#define __SOCK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net_types.h"

#define TCP_BACKLOG 1024
#define MAX_TCP_IOV_COUNT   1024
#define MAX_UDP_IOV_COUNT   1024

enum
{
    SOCK_SEND = 0,
    SOCK_RECV,
};

int sock_create(const char *proto);
int sock_close(int sock);
int sock_connect(int sockd, const char *hostname, int port);
int sock_brecv(int s, void *buf, int len);
int sock_brecvfrom(int s, void *buf, int len, char *hostname, int *port);
int sock_nbrecv(int s, void *buf, int len);
int sock_nbrecvfrom(int s, void *buf, int len, char *hostname, int *port);
int sock_bsend(int s, void *buf, int len);
int sock_bvectormsg(int s, struct iovec* vector, int count, char *hostname, int port, int send_recv);
int sock_bsendto(int s, void *buf, int len, char *hostname, int port);
int sock_nbsend(int s, void *buf, int len);
int sock_nbsendto(int s, void *buf, int len, char *hostname, int port);
int sock_bvector(int s, struct iovec* vector, int count, int send_recv);
int sock_nbvector(int s, struct iovec* vector, int count, int send_recv);
int sock_nbpeek(int s, void *buf, int len, char *hostname, int *port);
int sock_getsockopt(int s, int optname);
int sock_setsockopt(int s, int level, int optname, int val);
int sock_accept(int s);
int sock_tcp_server(const char *hostname, int port);

#ifdef __cplusplus
}
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
