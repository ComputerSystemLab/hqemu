/*
 *  (C) 2007 by System Software Laboratory, National Tsing Hua Univerity, Taiwan.
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __NET_TYPES_H
#define __NET_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "list.h"

//#define DEBUG_NET
#ifdef DEBUG_NET
#define net_debug(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define net_debug(fmt, ...)
#endif
#define net_error(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); exit(0); } while (0)


#ifndef offsetof
#define offsetof(__type, __member) ((size_t) &((__type *)0)->__member)
#endif

#define COMM_MAGIC_NR       0x41824182
#define ANY_SOURCE          ((uint32_t)-1)
#define ANY_TAG             ((comm_tag_t)-2)
#define CLOSE_TAG           ((comm_tag_t)-3)

#define NET_SUCCESS         0   /* Successful return code */
#define NET_ERR_ADDR        -1  /* Invalid address */
#define NET_ERR_PORT        -2  /* Invalid or duplicated port */
#define NET_ERR_REFUSED	    -3  /* Connection refused */
#define NET_ERR_HANDLE      -4  /* Invalid handle(sock) */
#define NET_ERR_BUFFER      -5  /* Invalid buffer pointer */
#define NET_ERR_SIZE        -6  /* Invalid size argument */
#define NET_ERR_TAG         -7  /* Invalid tag argument */
#define NET_ERR_REQUEST     -8  /* Invalid request pointer */
#define NET_ERR_STATUS      -9  /* Invalid status pointer */
#define NET_ERR_INTERNAL    -10 /* Internal error code */
#define NET_ERR_OTHER       -11 /* Other error */
#define NET_ERR_ARG         -12 /* Invalid argument */
#define NET_ERR_STATE       -13 /* Invalid state */
#define NET_ERR_PROTO       -14 /* Invalid protocol */
#define NET_ERR_HUP         -15 /* Disconnection */
#define NET_ERR_FLUSH       -16 /* Disconnection */

typedef uint32_t        comm_size_t;
typedef uint32_t        comm_tag_t;
typedef unsigned long   Request;

enum sendq_type
{
    SENDQ_INPROGRESS = 0,
    SENDQ_COMPLETE,
    SENDQ_MAX,
};

enum recvq_type
{
    RECVQ_INPROGRESS = 0,
    RECVQ_COMPLETE,
    RECVQ_MAX,
};

enum request_type
{
    MSG_SEND = 0,
    MSG_RECV,
    MSG_ISEND,
    MSG_IRECV,
};

enum request_status
{
    STATUS_COMPLETE = 0,
    STATUS_INPROGRESS,
    STATUS_HUP,
    STATUS_ERROR,
};

typedef struct Status
{
    comm_size_t count;
    int source;
    comm_tag_t tag;
    int error;
} Status;

#define fill_server_addr(__host, __addr, __port)    \
    do {                                            \
        (__host)->hostname = __addr;                \
        (__host)->port = __port;                    \
        (__host)->sock = -1;                        \
        (__host)->is_connected = 0;                 \
    } while(0)

struct buffer_msg_t
{
    list_t l;
    int sock;
    comm_tag_t tag;
    int type;
    void *buffer;
    comm_size_t size;
    void **buffer_list;         /* list of memory regions */
    comm_size_t *size_list;     /* list of size of memory regions */
    int list_count;             /* count of memory regions */
    comm_size_t request_size;   /* total request size */
    comm_size_t complete_size;  /* total size processed */
    int header_required;
    int is_flushed;
    Status status;              /* queue status */
    
    /* TCP only */
    int iov_idx;                /* index to the io vector */
    int iov_len_done;           /* size remained in current index 'iov_idx' */
};


/*
 * Network routine function pointer
 */
struct network_fns
{
    /* Network method name */
    const char *name;
    /* Initialize network server */
    int (*Initialize)(const char *hostname, int port, int init_flags);
    /* Terminate network service */
    int (*Finalize)(void);
    /* Establish a network connection */
    int (*Connect)(const char *hostname, int port);
    /* Blocking send */
    int (*Send)(int hndl, void *buffer, comm_size_t size, comm_tag_t tag);
    /* Blocking recv */
    int (*Recv)(int hndl, void *buffer, comm_size_t size, comm_tag_t tag, Status *status);
    /* Nonblocking send */
    int (*Isend)(int hndl, void *buffer, comm_size_t size, comm_tag_t tag, Request *request);
    /* Nonblocking recv */
    int (*Irecv)(int hndl, void *buffer, comm_size_t size, comm_tag_t tag, Request *request);
    /* Blocking send with a list of data */
    int (*Send_list)(int hndl, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag);
    /* Blocking recv with a list of data */
    int (*Recv_list)(int hndl, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag, Status *status);
    /* Nonblocking send with a list of data */
    int (*Isend_list)(int hndl, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag, Request *request);
    /* Nonblocking recv with a list of data */
    int (*Irecv_list)(int hndl, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag, Request *request);
    /* Blocking probe for incoming request */
    int (*Probe)(int hndl, comm_tag_t tag, Status *status);
    /* Nonblocking probe for incoming request */
    int (*Iprobe)(int hndl, comm_tag_t tag, int *flag, Status *status);
    /* Test if nonblocking send/recv is completed */
    int (*Test)(Request *request, Status *status);
    /* Wait for completion of nonblocking send/recv */
    int (*Wait)(Request *request, Status *status);
    /* Close a connection */
    int (*Close)(int hndl);
    /* Cancel a network operation */
    int (*Cancel)(void);
};

#ifdef __cplusplus
}
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
