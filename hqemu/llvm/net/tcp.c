/*
 *  (C) 2007 by System Software Laboratory, National Tsing Hua Univerity, Taiwan.
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <errno.h>
#include <pthread.h>
#include "cpu-all.h"
#include "sock.h"
#include "tcp.h"
#include "utils.h"

#define TCP_THREADS	1

#define unlikely(x) __builtin_expect(!!(x), 0)
#define tcp_malloc  llvm_malloc
#define lock_t      llvm_lock_t
#define lock_init   llvm_lock_init
#define spin_lock   llvm_spin_lock
#define spin_unlock llvm_spin_unlock

#define check_status(__ret)                                     \
    do {                                                        \
        if (unlikely(__ret < 0)) {                              \
            if (errno == EPIPE || errno == ECONNRESET           \
                    || errno == EBADF) goto drop_connection;    \
            net_error("%s: %s\n", __func__, strerror(errno));   \
        }                                                       \
    } while (0)

struct service_t
{
    list_t l;
    int sock;
    const char *hostname;
    int port;
};

static int tcp_initialized;
static int tcp_termination;
static int is_server;
static int timeout;
static int num_pollfd;
static struct pollfd *recv_pollfd;
static pthread_t *tcp_threads;

static lock_t send_lock;
static lock_t recv_lock;
static lock_t service_lock;
static list_t sendq[SENDQ_MAX];
static list_t recvq[RECVQ_MAX];
static list_t service_head;
struct queue_t *dropq;

/* hqemu */
static int use_callback;
static comm_tag_t recv_tag;
static comm_tag_t flush_tag;
static recv_callback_t recv_callback;
static flush_callback_t flush_callback;

static void *tcp_queue_func(void *argv);
static int TCP_Irecv(int, void *, comm_size_t, comm_tag_t, Request *);
static int TCP_Irecv_list(int, void **, comm_size_t *, int, comm_tag_t, Request *);
static int TCP_Wait(Request *, Status *);

void copy_tcg_context(CPUState *env);

static int ip2hex(const char *ip)
{
    int i, hex=0;
    const char *endptr = ".", *s, *p;

    s = ip;
    for (i = 0; i < 4; i++)
    {
        hex <<= 8;
        hex += strtoul(s, (char **)&endptr, 10);
        p = strchr(s, '.');
        s = p + 1;
    }
    return hex;
}

/*
 * move_data()
 *  Copy data from receive message to a list of buffers.
 *  buffer_list:		pointers to a list of buffers
 *  size_list:		poniters to a list of sizes of buffers
 *  list_count:		count of buffer list
 *  old_size:		data size had previously filled in
 *  request_size:	data size requested in this copy operation
 *  data:			data structure describing the receive message
 */
static void move_data(void **buffer_list, comm_size_t *size_list, int list_count, comm_size_t old_size, 
        comm_size_t request_size, struct buffer_msg_t *data)
{
    int i, index;
    char *buf;
    int total_size=0, size_remain=0;

    if (request_size == 0)
    {
        fprintf(stderr, "copy zero size. Nothing to do.\n");
        return;
    }

    for (i = 0; i < list_count; i++)
    {
        total_size += size_list[i];
        if (total_size > old_size)
        {
            /* fill in the remaining size in this buffer index */
            size_remain = total_size - old_size;

            buf = (char *)data->buffer;
            if (size_remain >= request_size)
            {
                memcpy(buffer_list[i], &buf[data->request_size], request_size);
                data->request_size += request_size;
                request_size = 0;
            }
            else
            {
                memcpy(buffer_list[i], &buf[data->request_size], size_remain);
                data->request_size += size_remain;
                request_size -= size_remain;
            }
            break;
        }
        else if (total_size == old_size)
            break;
    }

    if (request_size == 0)
        return;

    index = i+1;

    for (i = index; i < list_count; i++)
    {
        buf = (char *)data->buffer;
        if (request_size >= size_list[i])
        {
            memcpy(buffer_list[i], &buf[data->request_size], size_list[i]);
            data->request_size += size_list[i];
            request_size -= size_list[i];
        }
        else
        {
            memcpy(buffer_list[i], &buf[data->request_size], request_size);
            data->request_size += request_size;
            request_size = 0;
        }
        if (request_size == 0)
            break;
    }
}

static void *search_sendq(int sock, list_t *queue)
{
    list_t *l;
    list_for_each(l, queue)
    {
        struct buffer_msg_t *data = (struct buffer_msg_t *)l;
        if (sock == data->sock)
            return l;
    }

    return NULL;
}

static void *search_recvq(int sock, comm_tag_t tag, list_t *queue)
{
    list_t *l;
    list_for_each(l, queue)
    {
        struct buffer_msg_t *data = (struct buffer_msg_t *)l;
        int query_sock = data->sock;
        if (sock != ANY_SOURCE && query_sock != ANY_SOURCE && sock != query_sock)
            continue;

        comm_tag_t query_tag = data->tag;
        if (tag != ANY_TAG && query_tag != ANY_TAG && tag != query_tag)
            continue;

        return l;
    }
  
    return NULL;
}

static inline void tcp_drop_connection(int sock)
{
    int *s = tcp_malloc(sizeof(int));
    *s = sock;
    enqueue(dropq, s);
}

static inline void tcp_drain_connection(void)
{
    int i, sock, *s;
    struct pollfd *tmp;
    struct buffer_msg_t *data;
    list_t *l, *n;

    while(1)
    {
        s = dequeue(dropq);
        if (s == NULL)
            return;

        sock = *s;
        free(s);

        /* remove this socket from polling list. */
        for (i = 0; i < num_pollfd; i++)
        {
            if (recv_pollfd[i].fd == sock)
                break;
        }
        if (i == num_pollfd)
            return;

        if (num_pollfd == 1)
            free(recv_pollfd);
        else
        {
            tmp = tcp_malloc((num_pollfd-1) * sizeof(struct pollfd));
            if (i != 0)
                memcpy(tmp, recv_pollfd, i * sizeof(struct pollfd));
            if (i != num_pollfd-1)
                memcpy(&tmp[i], &recv_pollfd[i+1], (num_pollfd-i-1)*sizeof(struct pollfd));
            free(recv_pollfd);
            recv_pollfd = tmp;
        }
        num_pollfd--;

        /* remove all pending send requests. */
        list_for_each_safe(l, n, &recvq[RECVQ_INPROGRESS])
        {
            data = (struct buffer_msg_t *)l;
            if (data->sock == sock)
            {
                if (data->status.error == STATUS_HUP)
                {
                    list_del(l);
                    free(data);
                }
                else
                    data->status.error = STATUS_HUP;
            }
        }

        list_for_each_safe(l, n, &sendq[SENDQ_INPROGRESS])
        {
            data = (struct buffer_msg_t *)l;
            if (data->sock == sock)
            {
                list_del(l);
                if (data->type == MSG_ISEND)
                    free(data->buffer);
                free(data);
            }
        }

#if 0
        sock_close(sock);
#endif
    }
}

static inline int tcp_block_send(int sock, char *buf, int size)
{
    int ret;
    int comp = size;

    do {
        ret = sock_nbsend(sock, buf, comp);
        if (ret < 0)
            return -1;
        comp -= ret;
        buf += ret;
    } while (comp != 0);

    return (size - comp);
}

static int tcp_block_sendv(int sock, char **buffer_list, comm_size_t *size_list,
        int list_count, comm_size_t last)
{
    int ret, i, idx=0, count, comp=0;
    struct iovec io_vector[MAX_TCP_IOV_COUNT + 1];

    if (list_count > MAX_TCP_IOV_COUNT)
        net_error("%s: exceeding max number of vector buffer (%d).\n", __func__, MAX_TCP_IOV_COUNT);

restart:
    count = 0;
    if (last)
    {
        io_vector[count].iov_base = buffer_list[idx] + last;
        io_vector[count].iov_len = size_list[idx] - last;
        count = 1;
    }

    for (i = count + idx; i < list_count; i++)
    {
        io_vector[count].iov_base = buffer_list[i];
        io_vector[count].iov_len = size_list[i];
        count++;
    }

    ret = sock_nbvector(sock, io_vector, count, SOCK_SEND);
    if (ret < 0)
        return -1;

    comp += ret;
    for (i = 0; i < count; i++)
    {
        if (ret < io_vector[i].iov_len)
        {
            last += ret;
            break;
        }

        ret -= io_vector[i].iov_len;
        last = 0;
        idx++;
    }

    if (idx != list_count)
        goto restart;
    
    return comp;
}

static inline int tcp_block_recv(int sock, char *buf, int size)
{
    int ret;
    int comp = size;

    do {
        ret = sock_nbrecv(sock, buf, comp);
        if (ret < 0)
            return -1;
        comp -= ret;
        buf += ret;
    } while (comp != 0);

    return (size - comp);
}

#define MAX_SEND_COUNT  1
static void tcp_send_messages(void)
{
    int ret, sock, count = MAX_SEND_COUNT;
    comm_tag_t tag;
    struct buffer_msg_t *data = NULL;
    struct tcp_msg_header tcp_header;
    list_t *l, *n;
    Status *status;

    list_for_each_safe(l, n, &sendq[SENDQ_INPROGRESS])
    {
        data = (struct buffer_msg_t *)l;
        sock = data->sock;
        tag = data->tag;

        if (data->header_required == 1)
        {
            fill_tcp_header(&tcp_header, data->request_size, tag);
            ret = tcp_block_send(sock, (char *)&tcp_header, TCP_HEADER_SIZE);
            check_status(ret);
            
            data->header_required = 0;
        }

        int list_count = data->list_count - data->iov_idx;
        if (list_count == 1 && data->iov_len_done == 0)
        {
            ret = tcp_block_send(sock,
                    (char *)data->buffer_list[data->iov_idx],
                    data->size_list[data->iov_idx]);
        }
        else
        {
            ret = tcp_block_sendv(sock,
                    (char **)&data->buffer_list[data->iov_idx],
                    &data->size_list[data->iov_idx],
                    list_count, data->iov_len_done);
        }
        check_status(ret);

        data->complete_size += ret;
        if (data->complete_size != data->request_size)
            net_error("%s: internal error.\n", __func__);

        /* we have finished sending all data of this send request */
        status = &data->status;
        status->count = data->request_size;
        status->source = sock;
        status->tag = tag;
        status->error = STATUS_COMPLETE;

        list_del_init(l);
        list_add_tail(l, &sendq[SENDQ_COMPLETE]);
        
        if (--count == 0)
            break;

        continue;

drop_connection:
        tcp_drop_connection(sock);
    }
}

static void flush_queue(void)
{
    list_t *l;

    list_for_each(l, &recvq[RECVQ_COMPLETE])
    {
        struct buffer_msg_t *data = (struct buffer_msg_t *)l;
        data->is_flushed = 1;
    }
}

static inline int tcp_recv_complete(struct buffer_msg_t *data)
{
    int ret, sock;
    comm_tag_t tag;
    comm_size_t size_remain, total_size;
    struct buffer_msg_t *user_data;
    Status *status;

    sock = data->sock;
    tag = data->tag;
    user_data = search_recvq(sock, tag, &recvq[RECVQ_INPROGRESS]);
    if (user_data == NULL)
        return 0;

    user_data->sock = sock;
    user_data->tag = tag;

    total_size = user_data->request_size - user_data->complete_size;
    size_remain = data->complete_size - data->request_size;
    if (total_size >= size_remain)
    {
        /* we are safe to take the receive message and remove it form complete list */
        move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                user_data->complete_size, size_remain, data);

        user_data->complete_size += size_remain;
        total_size -= size_remain;
        ret = 1;
    }
    else 
    {
        move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                user_data->complete_size, total_size, data);

        user_data->complete_size += total_size;
        total_size = 0;
        ret = 0;
    }

    if (total_size == 0)
    {
        status = &user_data->status;
        status->count = user_data->request_size;
        status->source = sock;
        status->tag = tag;
        status->error = STATUS_COMPLETE;
    }

    return ret;
}

static void tcp_recv_messages(void)
{
    int i, ret, sock, count = 0;
    comm_tag_t tag;
    int payload_size=0;
    struct tcp_msg_header tcp_header;
    int header_size = TCP_HEADER_SIZE;
    struct buffer_msg_t *data = NULL;
    char *rbuf;
    Status *status;

    if (num_pollfd == 0)
        return;
    do
    {
        count = poll(recv_pollfd, num_pollfd, timeout);
    } while (count < 0 && errno == EINTR);

    if (count <= 0)
        return;

    /* Some message has arrived, we need to check it */
    for (i = 0; i < num_pollfd; i++)
    {
        sock = recv_pollfd[i].fd;

        if (recv_pollfd[i].revents == 0)
            continue;
        else if (recv_pollfd[i].revents & POLLHUP)
            goto drop_connection;
        else if (recv_pollfd[i].revents & POLLIN)
        {
            ret = sock_nbpeek(sock, &tcp_header, TCP_HEADER_SIZE, NULL, NULL);
            check_status(ret);

            if (ret < header_size)  /* the header does not fully arrive */
                continue;

            if (tcp_header.magic_nr != COMM_MAGIC_NR)
                goto drop_connection;

            ret = tcp_block_recv(sock, (char *)&tcp_header, TCP_HEADER_SIZE);
            check_status(ret);

            payload_size = tcp_header.size;
            rbuf = tcp_malloc(payload_size);
            ret = tcp_block_recv(sock, rbuf, payload_size);
            check_status(ret);

            tag = tcp_header.tag;
            net_debug("%s: one incoming request size=%d tag=%d\n", __func__, payload_size, tag);

            if (use_callback)
            {
                if (tag == recv_tag)
                {
                    recv_callback(sock, rbuf, payload_size);
                    free(rbuf);
                    continue;
                }
                else if (tag == flush_tag)
                {
                    flush_callback(sock, rbuf, payload_size);
                    free(rbuf);
                    continue;
                }
            }
            if (is_server && tag == flush_tag)
                flush_queue();

            data = tcp_malloc(sizeof(struct buffer_msg_t));
            INIT_LIST_HEAD(&data->l);
            data->type = MSG_RECV;
            data->sock = sock;
            data->tag = tag;
            data->buffer = rbuf;
            data->size = payload_size;
            data->buffer_list = &data->buffer;
            data->size_list = &data->size;
            data->list_count = 1;
            data->request_size = 0;
            data->complete_size = payload_size;
            data->is_flushed = 0;

            status = &data->status;
            status->count = payload_size;
            status->source = sock;
            status->tag = tag;
            status->error = STATUS_COMPLETE;

            list_add_tail(&data->l, &recvq[RECVQ_COMPLETE]);
            
            continue;
        }

drop_connection:
        tcp_drop_connection(sock);
    }

#if 0
    if (list_empty(&recvq[RECVQ_COMPLETE]) || list_empty(&recvq[RECVQ_INPROGRESS]))
        return;

    list_t *l, *n;
    list_for_each_safe(l, n, &recvq[RECVQ_COMPLETE])
    {
        data = (struct buffer_msg_t *)l;
        ret = tcp_recv_complete(data);
        if (ret == 1)
        {
            list_del(&data->l);
            free(data->buffer);
            free(data);
        }
    }
#endif
}

void tcp_fork_start(void)
{
    int i;

    if (tcp_threads)
    {
        tcp_termination = 1;
        for (i = 0; i < TCP_THREADS; i++)
            pthread_join(tcp_threads[i], NULL);
    }
}

void tcp_fork_end(int child)
{
    int i, ret;

    if (child == 0)
    {
        if (tcp_threads)
        {
            tcp_termination = 0;
            for (i = 0; i < TCP_THREADS; i++)
            {
                ret = pthread_create(&tcp_threads[i], NULL, tcp_queue_func, NULL);
                if (ret < 0)
                    net_error("thread creation failed\n");
            }
        }
    }
    else
    {
        if (tcp_threads)
            free(tcp_threads);

        tcp_threads = NULL;
        tcp_termination = 0;
    }
}

/*
 * TCP_Initialize()
 *  Create server service.
 */
static int TCP_Initialize(const char *hostname, int port, int init_flags)
{
    int ret = NET_SUCCESS, sock = -1;
    static const char *inaddr_any = "0.0.0.0";
    struct service_t *service = NULL;
    list_t *l = NULL;

    if (tcp_initialized == 0)
        net_error("TCP/IP module is not initialized\n");

    if (port < 0)
        return NET_ERR_PORT;

    /* create TCP server */
    spin_lock(&service_lock);
    list_for_each(l, &service_head)
    {
        if (((struct service_t *)l)->port == port)
        {
            ret = NET_ERR_PORT;
            goto failed;
        }
    }

    sock = sock_tcp_server(hostname, port);
    if (sock == -1)
    {
        ret = NET_ERR_ADDR;
        goto failed;
    }
    service = tcp_malloc(sizeof(struct service_t));
    INIT_LIST_HEAD(&service->l);
    service->sock = sock;
    service->hostname = (hostname == NULL) ? inaddr_any : strdup(hostname);
    service->port = port;
    
    list_add_tail(&service->l, &service_head);
    spin_unlock(&service_lock);
    
    net_debug("%s: create tcp socket %d on %s:%d\n", __func__, sock, hostname, port);
    return sock;

failed:
    spin_unlock(&service_lock);
    fprintf(stderr, "Error: create tcp socket on %s:%d\n", hostname, port);

    return ret;
}

/*
 * TCP_Finalize()
 *  Close TCP/IP service.
 */
static int TCP_Finalize(void)
{
    int i;

    if (tcp_threads != NULL)
    {
        tcp_termination = 1;
        for (i = 0; i < TCP_THREADS; i++)
            pthread_join(tcp_threads[i], NULL);
        free(tcp_threads);
        tcp_threads = NULL;
    }

    return NET_SUCCESS;
}

static void add_connection(int sock)
{
    struct pollfd *tmp;
    int oldfl = 0;
    
    /* set it to non-blocking operation */
    oldfl = fcntl(sock, F_GETFL, 0);
    if ((oldfl & O_NONBLOCK) == 0)
        fcntl(sock, F_SETFL, oldfl | O_NONBLOCK);

    /* Don't delay send to coalesce packets  */
    if (sock_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
        net_error("%s: set socket to no delay\n", __func__);
    if (sock_setsockopt(sock, SOL_SOCKET, SO_SNDBUF, TCP_BUF_SIZE) < 0)
        net_error("%s: set send buffer size\n", __func__);
    if (sock_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, TCP_BUF_SIZE) < 0)
        net_error("%s: set recv buffer size\n", __func__);

    tmp = tcp_malloc((num_pollfd + 1) * sizeof(struct pollfd));
    if (num_pollfd == 0)
    {
        tmp[0].fd = sock;
        tmp[0].events = POLLIN | POLLHUP;
        recv_pollfd = tmp;
    }
    else
    {
        memcpy(tmp, recv_pollfd, num_pollfd * sizeof(struct pollfd));
        tmp[num_pollfd].fd = sock;
        tmp[num_pollfd].events = POLLIN | POLLHUP;
        free(recv_pollfd);
        recv_pollfd = tmp;
    }
    num_pollfd++;
}

/*
 * TCP_Connect()
 *  Establish a connection.
 */
static int TCP_Connect(const char *hostname, int port)
{
    int ret, sock;
    int iphex;

    if (hostname == NULL)
        return NET_ERR_ADDR;
    if (port < 0)
        return NET_ERR_PORT;

    iphex = ip2hex(hostname);
    if ((iphex & 0xFF) == 0xFF)
        return NET_ERR_REFUSED;

    if ((sock = sock_create("tcp")) < 0)
        net_error("%s: initialize socket\n", __func__);

    ret = sock_connect(sock, hostname, port);
    if (ret < 0)
        return NET_ERR_REFUSED;

    spin_lock(&recv_lock);
    add_connection(sock);
    spin_unlock(&recv_lock);

    net_debug("%s: socket %d connect to %s:%d\n", __func__, sock, hostname, port);

    return sock;
}

static int TCP_Send(int sock, void *buffer, comm_size_t size, comm_tag_t tag)
{
    int ret, ssize;
    struct tcp_msg_header *tcp_header;
    struct buffer_msg_t *data;
    static char static_sbuf[TCP_BUF_SIZE];
    char *sbuf = NULL;
    Request request;
    Status status;

    if (sock == ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (tag == ANY_TAG)
        return NET_ERR_TAG;
    if (size <= 0)
        return NET_ERR_SIZE;
    if (buffer == NULL)
        return NET_ERR_BUFFER;

    /* 
     * Search sendq to check if there exists any pending send-request with 
     * the same socket 'sock'.
     */
    spin_lock(&send_lock);
    data = search_sendq(sock, &sendq[SENDQ_INPROGRESS]);
    if (data != NULL)
    {
        /* 
         * Some send operation is already in the send queue. We need to queue this send request 
         * in order not to ruin the sending order.
         */
        data = tcp_malloc(sizeof(struct buffer_msg_t));
        INIT_LIST_HEAD(&data->l);
        data->type = MSG_SEND;
        data->sock = sock;
        data->tag = tag;
        data->buffer = buffer;
        data->size = size;
        data->buffer_list = &data->buffer;
        data->size_list = &data->size;
        data->list_count = 1;
        data->iov_idx = 0;
        data->iov_len_done = 0;
        data->request_size = size;
        data->complete_size = 0;
        data->is_flushed = 0;

        data->status.count = size;
        data->status.source = sock;
        data->status.tag = tag;
        data->status.error = STATUS_INPROGRESS;

        request = (Request)data;

        list_add_tail(&data->l, &sendq[SENDQ_INPROGRESS]);
        spin_unlock(&send_lock);

        return TCP_Wait(&request, &status);
    }

    /* 
     * If we go here, no send request with same 'sock' is in sendq.
     * We are safe to send the message.
     */
    ssize = size + sizeof(struct tcp_msg_header);
    if (ssize > TCP_BUF_SIZE)
    {
        sbuf = tcp_malloc(ssize);

        tcp_header = (struct tcp_msg_header *)sbuf;
        fill_tcp_header(tcp_header, size, tag);
        memcpy(tcp_header + 1, buffer, size);
        ret = tcp_block_send(sock, sbuf, ssize);
        check_status(ret);

        spin_unlock(&send_lock);
    }
    else
    {
        tcp_header = (struct tcp_msg_header *)static_sbuf;
        fill_tcp_header(tcp_header, size, tag);
        memcpy(tcp_header + 1, buffer, size);
        ret = tcp_block_send(sock, static_sbuf, ssize);
        check_status(ret);

        spin_unlock(&send_lock);
    }

    if (sbuf)
        free(sbuf);

    return NET_SUCCESS;

drop_connection:
    if (sbuf)
        free(sbuf);

    tcp_drop_connection(sock);
    spin_unlock(&send_lock);

    return NET_ERR_HUP;
}

/*
 * TCP_Recv()
 *  Blocking reveive of one region of data.
 */
static int TCP_Recv(int sock, void *buffer, comm_size_t size, comm_tag_t tag, Status *status)
{
    int ret;
    Request request;

    if (sock < 0 && sock != ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (buffer == NULL)
        return NET_ERR_BUFFER;
    if (size <= 0)
        return NET_ERR_SIZE;
    if (status == NULL)
        return NET_ERR_STATUS;

    ret = TCP_Irecv(sock, buffer, size, tag, &request);
    if (ret != NET_SUCCESS)
        return ret;

    return TCP_Wait(&request, status);
}

/*
 * TCP_Isend()
 *  Nonblocking send of one region of data.
 *  On success, total size of data sent will be returned.
 */
static int TCP_Isend(int sock, void *buffer, comm_size_t size, comm_tag_t tag, Request *request)
{
    struct buffer_msg_t *data;

    if (sock == ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (tag == ANY_TAG)
        return NET_ERR_TAG;
    if (size <= 0)
        return NET_ERR_SIZE;
    if (buffer == NULL)
        return NET_ERR_BUFFER;
    if (request == NULL)
        return NET_ERR_REQUEST;

    data = tcp_malloc(sizeof(struct buffer_msg_t));
    INIT_LIST_HEAD(&data->l);
    data->sock = sock;
    data->tag = tag;
    data->type = MSG_ISEND;
    data->buffer = buffer;
    data->size = size;
    data->buffer_list = &data->buffer;
    data->size_list = &data->size;
    data->list_count = 1;
    data->iov_idx = 0;
    data->iov_len_done = 0;
    data->request_size = size;
    data->complete_size = 0;
    data->header_required = 1;
    data->is_flushed = 0;

    data->status.count = size;
    data->status.source = sock;
    data->status.tag = tag;
    data->status.error = STATUS_INPROGRESS;

    *request = (Request)data;

    spin_lock(&send_lock);
    list_add_tail(&data->l, &sendq[SENDQ_INPROGRESS]);
    spin_unlock(&send_lock);

    return NET_SUCCESS;
}

/*
 * TCP_Irecv()
 *  Nonblocking reveive of one region of data.
 */
static int TCP_Irecv(int sock, void *buffer, comm_size_t size, comm_tag_t tag, Request *request)
{
    struct buffer_msg_t *data=NULL, *user_data=NULL;
    comm_size_t total_size=0, size_remain=0;
    Status *status;

    if (sock < 0 && sock != ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (buffer == NULL)
        return NET_ERR_BUFFER;
    if (size <= 0)
        return NET_ERR_SIZE;
    if (request == NULL)
        return NET_ERR_REQUEST;

    user_data = tcp_malloc(sizeof(struct buffer_msg_t));
    INIT_LIST_HEAD(&user_data->l);
    user_data->sock = sock;
    user_data->tag = tag;
    user_data->type = MSG_IRECV;
    user_data->buffer = buffer;
    user_data->size = size;
    user_data->buffer_list = &user_data->buffer;
    user_data->size_list = &user_data->size;
    user_data->list_count = 1;
    user_data->iov_idx = 0;
    user_data->iov_len_done = 0;
    user_data->request_size = size;
    user_data->complete_size = 0;
    user_data->is_flushed = 0;

    total_size = size;

    spin_lock(&recv_lock);
    do
    {
        /* quickly search for complete queue once to fill in buffer */
        data = search_recvq(sock, tag, &recvq[RECVQ_COMPLETE]);
        if (data != NULL)
        {
            if (sock == ANY_SOURCE)
                sock = user_data->sock = data->sock;
            if (tag == ANY_TAG)
                tag = user_data->tag = data->tag;

            size_remain = data->complete_size - data->request_size;

            if (data->is_flushed == 1)
                user_data->is_flushed = 1;

            if (total_size >= size_remain)
            {
                /* we are safe to take the receive message and remove it form complete list */
                move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                        user_data->complete_size, size_remain, data);

                user_data->complete_size += size_remain;
                total_size -= size_remain;

                /* the data buffer has been copied, we are safe to free it */
                list_del(&data->l);
                free(data->buffer);
                free(data);
            }
            else 
            {
                move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                        user_data->complete_size, total_size, data);

                user_data->complete_size += total_size;
                total_size = 0;
            }
        }
    } while (data != NULL && total_size != 0);
	
    /* No other matched message in the completion queue, just keep track it in the pending queue */

    *request = (Request)user_data;

    status = &user_data->status;
    status->count = user_data->request_size;
    status->source = sock;
    status->tag = tag;
    status->error = (total_size == 0) ? STATUS_COMPLETE : STATUS_INPROGRESS;

    list_add_tail(&user_data->l, &recvq[RECVQ_INPROGRESS]);

    spin_unlock(&recv_lock);

    return NET_SUCCESS;
}

static int TCP_Send_list(int sock, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag)
{
    int i, ret;
    struct tcp_msg_header tcp_header;
    struct iovec io_vector[MAX_TCP_IOV_COUNT+1];
    struct buffer_msg_t *data = NULL;
    comm_size_t total_size = 0;
    Request request;
    Status status;

    if (sock == ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (tag == ANY_TAG)
        return NET_ERR_TAG;
    if (buffer_list == NULL)
        return NET_ERR_BUFFER;
    if (size_list == NULL)
        return NET_ERR_SIZE;

    for (i = 0; i < list_count; i++)
        total_size += size_list[i];
	
    if (total_size == 0)
        return NET_ERR_SIZE;

    /* 
     * Search sendq to check if there exists any pending send-request with 
     * the same socket 'sock'.
     */
    spin_lock(&send_lock);
    data = search_sendq(sock, &sendq[SENDQ_INPROGRESS]);
    if (data != NULL)
    {
        /* 
         * Some send operation is already in the send queue. We need to queue this send request 
         * in order not to ruin the sending order.
         */
        data = tcp_malloc(sizeof(struct buffer_msg_t));
        INIT_LIST_HEAD(&data->l);
        data->sock = sock;
        data->tag = tag;
        data->type = MSG_SEND;
        data->buffer = NULL;
        data->size = 0;
        data->buffer_list = buffer_list;
        data->size_list = size_list;
        data->list_count = list_count;
        data->iov_idx = 0;
        data->iov_len_done = 0;
        data->request_size = total_size;
        data->complete_size = 0;
        data->header_required = 1;
        data->is_flushed = 0;

        data->status.count = total_size;
        data->status.source = sock;
        data->status.tag = tag;
        data->status.error = STATUS_INPROGRESS;

        request = (Request)data;

        list_add_tail(&data->l, &sendq[SENDQ_INPROGRESS]);
        spin_unlock(&send_lock);

        return TCP_Wait(&request, &status);
    }

    /* 
     * If we go here, no send request with same 'sock' is in sendq.
     * We are safe to send the message.
     */

    for (i = 0; i < list_count; i++)
    {
        io_vector[i].iov_base = buffer_list[i];
        io_vector[i].iov_len = size_list[i];
    }

    /* In order to make the header are fully transferred, Use blocking send to send it */
    fill_tcp_header(&tcp_header, total_size, tag);
    ret = tcp_block_send(sock, (char *)&tcp_header, TCP_HEADER_SIZE);
    check_status(ret);

    ret = sock_bvector(sock, io_vector, list_count, SOCK_SEND);
    check_status(ret);

    spin_unlock(&send_lock);
    return NET_SUCCESS;

drop_connection:
    tcp_drop_connection(sock);
    spin_unlock(&send_lock);

    return NET_ERR_INTERNAL;
}

static int TCP_Recv_list(int sock, void **buffer_list, comm_size_t *size_list, int list_count, comm_tag_t tag, Status *status)
{
    int ret, i;
    comm_size_t total_size = 0;
    Request request;

    if (sock < 0 && sock != ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (buffer_list == NULL)
        return NET_ERR_BUFFER;
    if (size_list == NULL)
        return NET_ERR_SIZE;
    if (status == NULL)
        return NET_ERR_STATUS;

    for (i = 0; i < list_count; i++)
        total_size += size_list[i];

    if (total_size == 0)
        return NET_ERR_SIZE;
	
    ret = TCP_Irecv_list(sock, buffer_list, size_list, list_count, tag, &request);
    if (ret != NET_SUCCESS)
        return ret;
	
    return TCP_Wait(&request, status);
}

/*
 * TCP_Isend_list()
 *  Nonblocking send of a list of data regions.
 *  On success, total size of data sent will be returned.
 */
static int TCP_Isend_list(int sock, void **buffer_list, comm_size_t *size_list, 
		int list_count, comm_tag_t tag, Request *request)
{
    int i;
    struct buffer_msg_t *data = NULL;
    comm_size_t total_size = 0;

    if (sock == ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (tag == ANY_TAG)
        return NET_ERR_TAG;
    if (buffer_list == NULL)
        return NET_ERR_BUFFER;
    if (size_list == NULL)
        return NET_ERR_SIZE;
    if (request == NULL)
        return NET_ERR_REQUEST;

    for (i = 0; i < list_count; i++)
        total_size += size_list[i];
	
    if (total_size == 0)
        return NET_ERR_SIZE;

    data = tcp_malloc(sizeof(struct buffer_msg_t));
    INIT_LIST_HEAD(&data->l);
    data->sock = sock;
    data->tag = tag;
    data->type = MSG_SEND;
    data->buffer = NULL;
    data->size = 0;
    data->buffer_list = buffer_list;
    data->size_list = size_list;
    data->list_count = list_count;
    data->iov_idx = 0;
    data->iov_len_done = 0;
    data->request_size = total_size;
    data->complete_size = 0;
    data->is_flushed = 0;

    data->status.count = total_size;
    data->status.source = sock;
    data->status.tag = tag;
    data->status.error = STATUS_INPROGRESS;

    *request = (Request)data;

    spin_lock(&send_lock);
    list_add_tail(&data->l, &sendq[SENDQ_INPROGRESS]);
    spin_unlock(&send_lock);

    return NET_SUCCESS;
}
/*
 * TCP_Irecv_list()
 *  Nonblocking receive of a list of data regions.
 */
static int TCP_Irecv_list(int sock, void **buffer_list, comm_size_t *size_list, 
		int list_count, comm_tag_t tag, Request *request)
{
    int i;
    struct buffer_msg_t *data=NULL, *user_data=NULL;
    comm_size_t total_size=0, size_remain=0;
    Status *status;

    if (sock < 0 && sock != ANY_SOURCE)
        return NET_ERR_HANDLE;
    if (buffer_list == NULL)
        return NET_ERR_BUFFER;
    if (size_list == NULL)
        return NET_ERR_SIZE;
    if (request == NULL)
        return NET_ERR_REQUEST;

    for (i = 0; i < list_count; i++)
        total_size += size_list[i];

    if (total_size == 0)
        return NET_ERR_SIZE;

    user_data = tcp_malloc(sizeof(struct buffer_msg_t));
    INIT_LIST_HEAD(&user_data->l);
    user_data->sock = sock;
    user_data->tag = tag;
    user_data->type = MSG_IRECV;
    user_data->buffer = NULL;
    user_data->size = 0;
    user_data->buffer_list = buffer_list;
    user_data->size_list = size_list;
    user_data->list_count = list_count;
    user_data->iov_idx = 0;
    user_data->iov_len_done = 0;
    user_data->request_size = total_size;
    user_data->complete_size = 0;
    user_data->is_flushed = 0;

    spin_lock(&recv_lock);
    do
    {
        /* quickly search for complete queue once to fill in buffer */
        data = search_recvq(sock, tag, &recvq[RECVQ_COMPLETE]);
        if (data != NULL)
        {
            if (sock == ANY_SOURCE)
                sock = user_data->sock = data->sock;
            if (tag == ANY_TAG)
                tag = user_data->tag = data->tag;

            size_remain = data->complete_size - data->request_size;

            if (data->is_flushed == 1)
                user_data->is_flushed = 1;

            if (total_size >= size_remain)
            {
                /* we are safe to take the receive message and remove it form complete list */
                move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                        user_data->complete_size, size_remain, data);

                user_data->complete_size += size_remain;
                total_size -= size_remain;

                /* the data buffer has been copied, we are safe to free it */
                list_del(&data->l);
                free(data->buffer);
                free(data);
            }
            else 
            {
                move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                        user_data->complete_size, total_size, data);

                user_data->complete_size += total_size;
                total_size = 0;
            }
        }
    } while (data != NULL && total_size != 0);
	
    /* No other matched message in the completion queue, just keep track it in the pending queue */
    *request = (Request)user_data;

    status = &user_data->status;
    status->count = user_data->request_size;
    status->source = sock;
    status->tag = tag;
    status->error = (total_size == 0) ? STATUS_COMPLETE : STATUS_INPROGRESS;

    list_add_tail(&user_data->l, &recvq[RECVQ_INPROGRESS]);

    spin_unlock(&recv_lock);

    return NET_SUCCESS;
}


/*
 * TCP_Probe()
 *
 * Blocking probe for incoming request.
 */
static int TCP_Probe(int sock, comm_tag_t tag, Status *status)
{
    struct buffer_msg_t *data = NULL;

    if (status == NULL)
        return NET_ERR_STATUS;

    while (1)
    {
        spin_lock(&recv_lock);
        data = search_recvq(sock, tag, &recvq[RECVQ_COMPLETE]);
        if (data != NULL)
        {
            status->count = data->status.count;
            status->source = data->status.source;
            status->tag = data->status.tag;
            status->error = STATUS_COMPLETE;
            spin_unlock(&recv_lock);

            return NET_SUCCESS;
        }

        data = search_recvq(sock, tag, &recvq[RECVQ_INPROGRESS]);
        if (data != NULL && data->status.source != ANY_SOURCE &&
                data->status.tag != ANY_TAG)
        {
            status->count = data->status.count;
            status->source = data->status.source;
            status->tag = data->status.tag;
            status->error = data->status.error;
            spin_unlock(&recv_lock);

            return NET_SUCCESS;
        }
        spin_unlock(&recv_lock);

        usleep(10);
    }

    return NET_SUCCESS;
}

/*
 * TCP_Iprobe()
 *  Nonblocking probe for incoming request.
 */
static int TCP_Iprobe(int sock, comm_tag_t tag, int *flag, Status *status)
{
    struct buffer_msg_t *data = NULL;
	
    if (flag == NULL)
        net_error("%s: null pointer\n", __func__);
    if (status == NULL)
        return NET_ERR_STATUS;

    spin_lock(&recv_lock);
    data = search_recvq(sock, tag, &recvq[RECVQ_COMPLETE]);
    if (data != NULL)
    {
        status->count = data->status.count;
        status->source = data->status.source;
        status->tag = data->status.tag;
        status->error = STATUS_COMPLETE;
        *flag = 1;

        spin_unlock(&recv_lock);
        return NET_SUCCESS;
    }

    data = search_recvq(sock, tag, &recvq[RECVQ_INPROGRESS]);
    if (data != NULL && data->status.source != ANY_SOURCE &&
            data->status.tag != ANY_TAG)
    {
        status->count = data->status.count;
        status->source = data->status.source;
        status->tag = data->status.tag;
        status->error = data->status.error;
        *flag = 1;

        spin_unlock(&recv_lock);
        return NET_SUCCESS;
    }
    spin_unlock(&recv_lock);

    *flag = 0;

    return NET_SUCCESS;
}

static int TCP_Test(Request *request, Status *status)
{
    net_error("%s not implemented\n", __func__);

    if (request == NULL)
        return NET_ERR_REQUEST;
    if (status == NULL)
        return NET_ERR_STATUS;

    return NET_SUCCESS;
}


/*
 * TCP_Wait()
 *  Blocking wait for completion of receive request.
 */
static int TCP_Wait(Request *request, Status *status)
{
    int sock, type;
    comm_tag_t tag;
    struct buffer_msg_t *data = NULL, *user_data = NULL;
    comm_size_t size_remain=0, total_size=0;
    int *errorp;

    if (request == NULL)
        return NET_ERR_REQUEST;
    if (status == NULL)
        return NET_ERR_STATUS;

    user_data = (struct buffer_msg_t *)(*request);

    type = user_data->type;
    errorp = &user_data->status.error;

    if (type == MSG_SEND || type == MSG_ISEND)
    {
        while (1)
        {
            if (*errorp == STATUS_COMPLETE)
            {
                /* remove user_data from complete queue. */
                spin_lock(&send_lock);
                list_del(&user_data->l);
                spin_unlock(&send_lock);

                status->count = user_data->status.count;
                status->source = user_data->status.source;
                status->tag = user_data->status.tag;
                status->error = STATUS_COMPLETE;

                if (user_data->type == MSG_ISEND)
                    free(user_data->buffer);
                free(user_data);

                return NET_SUCCESS;
            }
            usleep(10);
        }
    }
    else if (type == MSG_RECV || type == MSG_IRECV)
    {
        sock = user_data->sock;
        tag = user_data->tag;
        while (1)
        {
            if (*errorp == STATUS_COMPLETE)
            {
		spin_lock(&recv_lock);
                list_del(&user_data->l);
		spin_unlock(&recv_lock);

                if (user_data->is_flushed == 1)
                    net_error("%s: fixme.\n", __func__);

                status->count = user_data->status.count;
                status->source = user_data->status.source;
                status->tag = user_data->status.tag;
                status->error = STATUS_COMPLETE;
                free(user_data);

                return NET_SUCCESS;
            }

            spin_lock(&recv_lock);

            total_size = user_data->request_size - user_data->complete_size;

            /* find one completing recv-request */
            data = search_recvq(sock, tag, &recvq[RECVQ_COMPLETE]);
            if (data != NULL)
            {
                if (sock == ANY_SOURCE)
                    sock = user_data->sock = data->sock;
                if (tag == ANY_TAG)
                    tag = user_data->tag = data->tag;

                /* early return if this is a flushed data. */
                if (data->is_flushed == 1)
                {
                    list_del(&data->l);
                    user_data->is_flushed = 1;
                    user_data->status.error = STATUS_COMPLETE;
                    spin_unlock(&recv_lock);

                    free(data->buffer);
                    free(data);
                    continue;
                }

                size_remain = data->complete_size - data->request_size;
                if (total_size >= size_remain)
                {
                    /* we are safe to take the receive message and remove it form complete list */
                    move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                            user_data->complete_size, size_remain, data);

                    user_data->complete_size += size_remain;
                    total_size -= size_remain;

                    /* the data buffer has been copied, we are safe to free it */
                    list_del(&data->l);
                    free(data->buffer);
                    free(data);
                }
                else 
                {
                    move_data(user_data->buffer_list, user_data->size_list, user_data->list_count, 
                            user_data->complete_size, total_size, data);

                    user_data->complete_size += total_size;
                    total_size = 0;
                }

                if (total_size == 0)
                {
                    user_data->status.error = STATUS_COMPLETE;
                    spin_unlock(&recv_lock);
                    continue;
                }
            }
            
            spin_unlock(&recv_lock);			

            usleep(10);
        }
    }

    return NET_SUCCESS;
}

/* 
 * TCP_Close()
 *  Close a socket connection.
 */
static int TCP_Close(int sock)
{
    int i;
    struct pollfd *tmp = NULL;
    list_t *l;

    if (sock < 0)
        return NET_ERR_HANDLE;

    spin_lock(&recv_lock);

    for (i = 0; i < num_pollfd; i++)
    {
        if (recv_pollfd[i].fd == sock)
            break;
    }
    if (i == num_pollfd)
        return NET_ERR_HANDLE;
    
    if (num_pollfd == 1)
        free(recv_pollfd);
    else
    {
        tmp = tcp_malloc((num_pollfd - 1) * sizeof(struct pollfd));
        if (i != 0)
            memcpy(tmp, recv_pollfd, i * sizeof(struct pollfd));
        if (i != num_pollfd-1)
            memcpy(&tmp[i], &recv_pollfd[i + 1], (num_pollfd-i-1)*sizeof(struct pollfd));
        free(recv_pollfd);
        recv_pollfd = tmp;
        
    }
    num_pollfd--;

    /* 
     * search service of server 
     */
    list_for_each(l, &service_head)
    {
        if (((struct service_t *)l)->sock == sock)
        {
            list_del(l);
            break;
        }
    }
    spin_unlock(&recv_lock);

    sock_close(sock);

    return NET_SUCCESS;
}

/*
 * TCP_Cancel()
 *  Cancel a processing of a job.
 */
static int TCP_Cancel(void)
{
    net_error("%s not implemented\n", __func__);
	
    return NET_SUCCESS;
}

/*
 * tcp_check_connection()
 *
 * Check if there is any new connection.
 *
 * Return 0 if no any new connection. Otherwise, return 1.
 */
static int tcp_check_connection(void)
{
    int sock;
    struct service_t *service;
    list_t *l;

    list_for_each(l, &service_head)
    {
        service = (struct service_t *)l;
        if ((sock = sock_accept(service->sock)) > 0)
        {
            struct sockaddr_in cin;
            int cin_len = sizeof(struct sockaddr_in);

            memset(&cin, 0, sizeof(struct sockaddr_in));
            getsockname(sock, (struct sockaddr *)&cin, (socklen_t *)&cin_len);
            net_debug("Socket %d accepts TCP connection %d from %s:%d\n",
                    service->sock, sock, inet_ntoa(cin.sin_addr), cin.sin_port);

            add_connection(sock);
            return 1;
        }
    }

    spin_lock(&send_lock);
    tcp_drain_connection();
    spin_unlock(&send_lock);

    return 0;
}

static void *tcp_queue_func(void *argv)
{
    if (is_server == 0)
        copy_tcg_context(NULL);

    while (1)
    {
        /* 
         * Check incoming connection.
         * Polling and buffering for incoming messages
         */
        spin_lock(&recv_lock);
        tcp_check_connection();
        tcp_recv_messages();
        spin_unlock(&recv_lock);

        /* 
         * Process specified maximum amount of pending send request
         */
        spin_lock(&send_lock);
        tcp_send_messages();
        spin_unlock(&send_lock);

        if (tcp_termination)
            break;

        usleep(10);
    }

    pthread_exit(NULL);
}

int setup_tcp_callback(int server, comm_tag_t rtag, recv_callback_t recv_fn,
        comm_tag_t ftag, flush_callback_t flush_fn)
{
    if (server == 0)
    {
        use_callback = 1;
        recv_tag = rtag;
        recv_callback = recv_fn;
        flush_tag = ftag;
        flush_callback = flush_fn;
    }
    else
        flush_tag = ftag;

    return 1;
}

int setup_tcp_module(int server)
{
    int i, ret;

    if (tcp_initialized == 1)
        return 1;

    net_debug("Initializing TCP/IP module.\n");
	
    INIT_LIST_HEAD(&sendq[SENDQ_INPROGRESS]);
    INIT_LIST_HEAD(&sendq[SENDQ_COMPLETE]);
    INIT_LIST_HEAD(&recvq[RECVQ_INPROGRESS]);
    INIT_LIST_HEAD(&recvq[RECVQ_COMPLETE]);
    INIT_LIST_HEAD(&service_head);
    dropq = create_queue();

    lock_init(&send_lock);
    lock_init(&recv_lock);
    lock_init(&service_lock);

    is_server = server;
    timeout = (server) ? 1 : 10;

    tcp_threads = (pthread_t *)tcp_malloc(TCP_THREADS * sizeof(pthread_t));
    for (i = 0; i < TCP_THREADS; i++)
    {
        ret = pthread_create(&tcp_threads[i], NULL, tcp_queue_func, NULL);
        if (ret < 0)
            net_error("thread creation failed\n");
    }

    tcp_initialized = 1;
    return 1;
}

struct network_fns tcp_fns = {
    "tcp",
    TCP_Initialize,
    TCP_Finalize,
    TCP_Connect,
    TCP_Send,
    TCP_Recv,
    TCP_Isend,
    TCP_Irecv,
    TCP_Send_list,
    TCP_Recv_list,
    TCP_Isend_list,
    TCP_Irecv_list,
    TCP_Probe,
    TCP_Iprobe,
    TCP_Test,
    TCP_Wait,
    TCP_Close,
    TCP_Cancel,
};

struct network_fns *retrieve_tcp_fns(void)
{
    return &tcp_fns;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
