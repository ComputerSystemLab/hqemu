/*
 *  (C) 2007 by System Software Laboratory, National Tsing Hua Univerity, Taiwan.
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __TCP_H
#define __TCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "net_types.h"

#define TCP_BUF_SIZE	160*1024

#define fill_tcp_header(__header, __size, __tag) \
    do {                                         \
        (__header)->magic_nr = COMM_MAGIC_NR;    \
        (__header)->size     = __size;           \
        (__header)->tag      = __tag;            \
    } while (0)


typedef int (*recv_callback_t)(int sock, char *buf, int size);
typedef int (*flush_callback_t)(int sock, char *buf, int size);

int setup_tcp_module(int is_server);
int setup_tcp_callback(int is_server, comm_tag_t rtag, recv_callback_t recv_fn,
        comm_tag_t ftag, flush_callback_t flush_fn);
struct network_fns *retrieve_tcp_fns(void);
void tcp_fork_start(void);
void tcp_fork_end(int child);

struct tcp_msg_header
{
    uint32_t magic_nr;	/* magic number */
    comm_size_t size;	/* payload size */
    comm_tag_t tag;		/* tag */
};
#define TCP_HEADER_SIZE      sizeof(struct tcp_msg_header)

#ifdef __cplusplus
}
#endif

#endif
/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
