/*
 *  (C) 2007 by System Software Laboratory, National Tsing Hua Univerity, Taiwan.
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include "sock.h"

/* 
 * sock_create()
 *  Create an endpoint for communication.
 */
int sock_create(const char *proto)
{
    int p_num;
    struct protoent *pep;
    
    if ((pep = getprotobyname(proto)) == NULL)
    {
	perror("Protocol not support");
	return -1;
    }
    p_num = pep->p_proto;
	
    if (!strcmp(proto, "tcp"))
	return socket(AF_INET, SOCK_STREAM, p_num);
    else if (!strcmp(proto, "udp"))
	return socket(AF_INET, SOCK_DGRAM, p_num);
    else
	return -1;
}

/*
 * sock_init()
 *  Initiate socket connection data.
 */
static int sock_init(struct sockaddr *saddrp, const char *hostname, int port)
{
    struct hostent *hep;
    
    memset((char *) saddrp, 0, sizeof(struct sockaddr_in));
    if (hostname == NULL)
    {
	if ((hep = gethostbyname("localhost")) == NULL)
	    return -1;
    }
    else if ((hep = gethostbyname(hostname)) == NULL)
	return -1;
	
    ((struct sockaddr_in *) saddrp)->sin_family = AF_INET;
    ((struct sockaddr_in *) saddrp)->sin_port = htons(port);
    
    memcpy(&((struct sockaddr_in *) saddrp)->sin_addr, hep->h_addr,
	    hep->h_length);

    return 0;
}

/*
 * sock_close()
 *  Close a socket connection.
 */
int sock_close(int sock)
{
    return close(sock);
}

/*
 * sock_connect()
 *  Initiate a connection on a socket.
 */
int sock_connect(int sock, const char *hostname, int port)
{
    struct sockaddr saddr;

    if (sock_init(&saddr, hostname, port) != 0)
	return -1;

connect_sock_restart:
    if (connect(sock, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
	if (errno == EINTR)
	    goto connect_sock_restart;
	return -1;
    }
    return sock;
}

/* 
 * sock_brecv()
 *  Blocking receive. Returns -1 if it cannot get all len bytes and
 *  the # of bytes received otherwise.
 */
int sock_brecv(int s, void *buf, int len)
{
    int flag=0;
    int oldfl, ret, comp = len;
    int olderrno;

    /* if previous socket is nonblocking, reset it to blocking */
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
	flag = 1;
    }

    while (comp)
    {
brecv_restart:
	if ((ret = recv(s, (char *) buf, comp, 0)) < 0)
	{
	    if (errno == EINTR)
		goto brecv_restart;
	    
	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}
	if (!ret)
	{
	    if (flag)
		fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = EPIPE;
	    return -1;
	}
	comp -= ret;
	buf += ret;
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
    
    return (len - comp);
}

/* 
 * sock_brecvfrom()
 *  Blocking receive. Returns -1 if it cannot get all len bytes and
 *  the # of bytes received otherwise.
 */
int sock_brecvfrom(int s, void *buf, int len, char *hostname, int *port)
{
    int flag=0;
    int oldfl, ret, comp = len;
    int olderrno;
    struct sockaddr_in c;
    int clen = sizeof(struct sockaddr_in);

    /* if previous socket is nonblocking, reset it to blocking */
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
	flag = 1;
    }

    while (comp)
    {
brecvfrom_restart:
	if ((ret = recvfrom(s, (char *) buf, comp, 0, (struct sockaddr *)&c, (socklen_t *)&clen)) < 0)
	{
	    if (errno == EINTR)
		goto brecvfrom_restart;
	    
	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}
	if (!ret)
	{
	    if (flag)
		fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = EPIPE;
	    return -1;
	}
	comp -= ret;
	buf += ret;
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
    
    strcpy(hostname, inet_ntoa(c.sin_addr));
    *port = ntohs(c.sin_port);
    
    return (len - comp);
}

/* 
 * sock_nbrecv()
 *  Nonblocking receive.
 */
int sock_nbrecv(int s, void *buf, int len)
{
    int ret, comp = len;

    while (comp)
    {
nbrecv_restart:
	ret = recv(s, buf, comp, 0);
	if (!ret)
	{
	    /* socket closed */
	    errno = EPIPE;
	    return -1;
	}
	if (ret == -1 && errno == EWOULDBLOCK)
	    return (len - comp);    /* return amount completed */
	if (ret == -1 && errno == EINTR)
	    goto nbrecv_restart;
	else if (ret == -1)
	    return -1;
	comp -= ret;
	buf += ret;
    }

    return (len - comp);
}

/* 
 * sock_nbrecvfrom()
 *  Nonblocking receive.
 */
int sock_nbrecvfrom(int s, void *buf, int len, char *hostname, int *port)
{
    int ret, comp = len;
    struct sockaddr_in c;
    int clen = sizeof(struct sockaddr_in);

    while (comp)
    {
nbrecvfrom_restart:
	ret = recvfrom(s, (char *) buf, comp, 0, (struct sockaddr *)&c, (socklen_t *)&clen);
	if (!ret)
	{
	    /* socket closed */
	    errno = EPIPE;
	    return -1;
	}
	if (ret == -1 && errno == EWOULDBLOCK)
	    return (len - comp);    /* return amount completed */
	if (ret == -1 && errno == EINTR)
	    goto nbrecvfrom_restart;
	else if (ret == -1) 
	    return -1;
	comp -= ret;
	buf += ret;
    }

    strcpy(hostname, inet_ntoa(c.sin_addr));
    *port = ntohs(c.sin_port);

    return (len - comp);
}

/* 
 * sock_bsend()
 *  Blocking send.
 */
int sock_bsend(int s, void *buf, int len)
{
    int flag=0;
    int oldfl, ret, comp = len;
    int olderrno;

    /* if previous socket is nonblocking, reset it to blocking */
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	flag = 1;
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
    }

    while (comp)
    {
bsend_restart:
	if ((ret = send(s, (char *) buf, comp, MSG_NOSIGNAL)) < 0){
	    if (errno == EINTR)
		goto bsend_restart;

	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}
	comp -= ret;
	buf += ret;
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);

    return (len - comp);
}

int sock_bvectormsg(int s, struct iovec* vector, int count, char *hostname, int port, int send_recv)
{
    int i, oldfl, ret=0, comp=0, flag=0, len=0, olderrno;
    struct sockaddr_in sa;
    struct msghdr msg;
    struct iovec io_vector[MAX_TCP_IOV_COUNT+1];
    struct iovec *p = io_vector;
    int cnt = count;

    if (count > MAX_TCP_IOV_COUNT)
	net_error("%s: vector count exceeds max limit %d\n", __func__, MAX_TCP_IOV_COUNT);
    
    for (i=0; i<count; i++)
	len += vector[i].iov_len;

    memcpy(io_vector, vector, count*sizeof(struct iovec));
    memset(&sa, 0, sizeof(struct sockaddr_in));
    memset(&msg, 0, sizeof(struct msghdr));

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_aton(hostname, &sa.sin_addr) == 0)
	net_error("%s: set hostname.\n", __func__);

    msg.msg_name = (void *)&sa;
    msg.msg_namelen = sizeof(struct sockaddr_in);
    msg.msg_iov = io_vector;
    msg.msg_iovlen = cnt;

    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
	flag = 1;
    }

    while (1)
    {
bvectormsg_restart:
	if (send_recv == SOCK_RECV)
	    ret = recvmsg(s, &msg, 0);
	else if (send_recv == SOCK_SEND)
	    ret = sendmsg(s, &msg, 0);

	if (ret < 0)
	{
	    if (errno == EINTR)
		goto bvectormsg_restart;

	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}

	comp += ret;
	if (comp == len)
	    break;

	for (;;p++,cnt--)
	{
	    ret -= p->iov_len;
	    if (ret == 0)
	    {
		p++;
		cnt--;
		break;
	    }
	    else if (ret < 0)
	    {
		ret += p->iov_len;
		p->iov_base += ret;
		p->iov_len -= ret;
		break;
	    }
	}
	msg.msg_iov = p;
	msg.msg_iovlen = cnt;
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);

    return comp;
}

/* 
 * sock_bsendto()
 *  Blocking send.
 */
int sock_bsendto(int s, void *buf, int len, char *hostname, int port)
{
    int flag=0;
    int oldfl, ret, comp = len;
    int olderrno;
    struct sockaddr_in sa;

    /* if previous socket is nonblocking, reset it to blocking */
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
	flag = 1;
    }

    memset((char *)&sa, 0, sizeof(struct sockaddr_in));
    sa.sin_family = AF_INET;
    inet_aton(hostname, &sa.sin_addr);
    sa.sin_port = htons(port);
    
    while (comp)
    {
bsendto_restart:
	if ((ret = sendto(s, (char *)buf, comp, 0, (struct sockaddr *)&sa, sizeof(struct sockaddr_in))) < 0)
	{
	    if (errno == EINTR)
		goto bsendto_restart;

	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}
	comp -= ret;
	buf += ret;
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);

    return (len - comp);
}

/* 
 * sock_nbsend()
 *  Nonblocking send. Should always return 0 when nothing gets done! 
 */
int sock_nbsend(int s, void *buf, int len)
{
    int ret, comp = len;

    while (comp)
    {
nbsend_restart:
	ret = send(s, (char *) buf, comp, MSG_NOSIGNAL);
	if (ret == 0 || (ret == -1 && errno == EWOULDBLOCK))
	    return (len - comp);    /* return amount completed */
	if (ret == -1 && errno == EINTR)
	    goto nbsend_restart;
	else if (ret == -1)
	    return -1;

	comp -= ret;
	buf += ret;
    }

    return (len - comp);
}

/* 
 * sock_nbsendto()
 *  Nonblocking send. Should always return 0 when nothing gets done! 
 */
int sock_nbsendto(int s, void *buf, int len, char *hostname, int port)
{
    int ret, comp = len;
    struct sockaddr_in sa;

    while (comp)
    {
nbsendto_restart:
	ret = sendto(s, (char *) buf, comp, 0, (struct sockaddr *)&sa, sizeof(struct sockaddr_in));
	if (ret == 0 || (ret == -1 && errno == EWOULDBLOCK))
	    return (len - comp);    /* return amount completed */
	if (ret == -1 && errno == EINTR)
	    goto nbsendto_restart;
	else if (ret == -1)
	    return -1;

	comp -= ret;
	buf += ret;
    }

    return (len - comp);
}

/* 
 * sock_bvector()
 *  Blocking vector send.
 */
int sock_bvector(int s, struct iovec* vector, int count, int send_recv)
{
    int flag=0;
    int oldfl, i, ret=0, comp=0, len=0;
    int olderrno;
    struct iovec io_vector[MAX_TCP_IOV_COUNT+1];
    struct iovec *p = io_vector;
    int cnt = count;

    for (i=0; i<count; i++)
	len += vector[i].iov_len;

    memcpy(io_vector, vector, count*sizeof(struct iovec));
    
    /* if previous socket is nonblocking, reset it to blocking */
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
    {
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));
	flag = 1;
    }

    while (1)
    {
bvector_restart:
	if (send_recv == SOCK_RECV)
	    ret = readv(s, p, cnt);
	else if (send_recv == SOCK_SEND)
	    ret = writev(s, p, cnt);

	if (ret < 0)
	{
	    if (errno == EINTR)
		goto bvector_restart;

	    olderrno = errno;
	    if (flag)
		fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
	    errno = olderrno;
	    return -1;
	}

	comp += ret;
	if (comp == len)
	    break;

	for (;;p++,cnt--)
	{
	    ret -= p->iov_len;
	    if (ret == 0)
	    {
		p++;
		cnt--;
		break;
	    }
	    else if (ret < 0)
	    {
		ret += p->iov_len;
		p->iov_base += ret;
		p->iov_len -= ret;
		break;
	    }
	}
    }

    if (flag)
	fcntl(s, F_SETFL, oldfl | O_NONBLOCK);

    return comp;
}

/* 
 * sock_nbvector()
 *  Nonblocking vector send.
 */
int sock_nbvector(int s, struct iovec* vector, int count, int send_recv)
{
    int ret = 0;

    do
    {
	if (send_recv == SOCK_RECV)
	    ret = readv(s, vector, count);
	else if (send_recv == SOCK_SEND)
	    ret = writev(s, vector, count);
    } while(ret == -1 && errno == EINTR);

    /* return zero if can't do any work at all */
    if(ret == -1 && errno == EWOULDBLOCK)
	return 0;

    /* if data transferred or an error */
    return ret;
}

int sock_nbpeek(int s, void *buf, int len, char *hostname, int *port)
{
    int ret, comp = len;
    struct sockaddr_in c;
    int clen = sizeof(struct sockaddr_in);

    while (comp)
    {
nbpeeka_restart:
	ret = recvfrom(s, buf, comp, MSG_PEEK, (struct sockaddr *)&c, (socklen_t *)&clen);
	if (!ret)
	{
	    /* socket closed */
	    errno = EPIPE;
	    return -1;
	}
	if (ret == -1 && errno == EWOULDBLOCK)
	    return (len - comp);
	if (ret == -1 && errno == EINTR)
	    goto nbpeeka_restart;
	else if (ret == -1)
	    return -1;
	comp -= ret;
    }

    if (hostname != NULL && port != NULL)
    {
	strcpy(hostname, inet_ntoa(c.sin_addr));
	*port = ntohs(c.sin_port);
    }
    
    return (len - comp);
}

/* 
 * sock_getsockopt()
 *  Get socket options.
 */
int sock_getsockopt(int s, int optname)
{
    int val, len=sizeof(int);

    if (getsockopt(s, SOL_SOCKET, optname, &val, (socklen_t *)&len) == -1)
	return -1;
    else
	return val;
}

/*
 * sock_setsockopt()
 *  Set socket options.
 */
int sock_setsockopt(int s, int level, int optname, int val)
{
    if (setsockopt(s, level, optname, &val, sizeof(val)) == -1)
	return -1;
    else
	return val;
}

/*
 * sock_bind()
 *  Bind a port to a socket.
 */
static int sock_bind(int sock, const char *hostname, int port)
{
    struct sockaddr_in saddr;
    
    memset((char *) &saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    
    if (hostname)
	saddr.sin_addr.s_addr = inet_addr(hostname);
    else
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    
bind_sock_restart:
    if (bind(sock, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
	if (errno == EINTR)
	    goto bind_sock_restart;
	return -1;
    }
    return sock;
}

int sock_accept(int listen_sock)
{
    int sock;
    struct sockaddr_in cin;
    int cin_len = sizeof(struct sockaddr_in);

    memset(&cin, 0, sizeof(struct sockaddr_in));
    sock = accept(listen_sock, (struct sockaddr *)&cin, (socklen_t *)&cin_len);
    if (sock < 0)
    {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == ENETDOWN)
                || (errno == EPROTO) || (errno == ENOPROTOOPT) || (errno == EHOSTDOWN)
                || (errno == ENONET) || (errno == EHOSTUNREACH) || (errno == EOPNOTSUPP)
                || (errno == ENETUNREACH))
        {
            return 0;
        }
        else
            net_error("accept failed\n");
    }

    return sock;
}


/*
 * sock_tcp_server()
 *  Initialize a tcp server socket. On success, a valid socket number will return.
 */
int sock_tcp_server(const char *hostname, int port)
{
    int oldfl = 0;
    int sock;
    
    /* create a socket */
    if ((sock = sock_create("tcp")) < 0)
	net_error("initialize server socket failed\n");

    /* set it to non-blocking operation */
    oldfl = fcntl(sock, F_GETFL, 0);
    if (!(oldfl & O_NONBLOCK))
	fcntl(sock, F_SETFL, oldfl | O_NONBLOCK);

    /* setup for a fast restart to avoid bind addr in use errors */
    sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    
    /* bind it to the appropriate port */
    if ((sock_bind(sock, hostname, port)) == -1)
	return -1;
    
    /* go ahead and listen to the socket */
    if (listen(sock, TCP_BACKLOG) != 0)
	return -1;

    return sock;
}


/*
 * vim: ts=8 sts=4 sw=4
 */
