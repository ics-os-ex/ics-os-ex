#include "sock.h"
#include "tcp.h"
#include "udp.h"
#include "netif.h"
#include "net_endian.h"
#include "../process/process.h"

extern void *malloc(unsigned int);
extern void free(void *);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int printf(const char *fmt, ...);

#define EBADF  9
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define EIO    5
#define ENOSYS 38
#define ENOTCONN 107
#define EPROTONOSUPPORT 93

#define SOCK_TCP 1
#define SOCK_UDP 2

struct sock {
    int type; /* SOCK_TCP / SOCK_UDP */
    int listening;
    unsigned short local_port;
    unsigned int local_ip;
    struct tcp_pcb *pcb;
    struct netif *nif;
};

static unsigned int sockaddr_ip(const struct sockaddr *addr)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    return net_ntohl(in->sin_addr);
}

static unsigned short sockaddr_port(const struct sockaddr *addr)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    return net_ntohs(in->sin_port);
}

long sys_socket(int domain, int type, int protocol)
{
    struct sock *s;
    int fd;

    (void)protocol;
    if (domain != AF_INET)
        return -EPROTONOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    if (!netif_default())
        return -ENOSYS;

    s = (struct sock *)malloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    memset(s, 0, sizeof(*s));
    s->nif = netif_default();
    s->type = (type == SOCK_STREAM) ? SOCK_TCP : SOCK_UDP;
    if (s->type == SOCK_TCP) {
        s->pcb = tcp_pcb_new(s->nif);
        if (!s->pcb) {
            free(s);
            return -ENOMEM;
        }
    }

    fd = sock_fd_alloc();
    if (fd < 0) {
        if (s->pcb)
            tcp_pcb_free(s->pcb);
        free(s);
        return fd;
    }
    sock_fd_install(fd, s);
    return fd;
}

long sys_bind(int fd, const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s = sock_from_fd(fd);
    if (!s || !addr || addrlen < sizeof(struct sockaddr_in))
        return -EINVAL;
    if (addr->sa_family != AF_INET)
        return -EPROTONOSUPPORT;
    s->local_port = sockaddr_port(addr);
    s->local_ip = sockaddr_ip(addr);
    return 0;
}

long sys_listen(int fd, int backlog)
{
    struct sock *s = sock_from_fd(fd);
    (void)backlog;
    if (!s || s->type != SOCK_TCP || !s->pcb)
        return -EINVAL;
    if (tcp_pcb_listen(s->pcb, s->local_port ? s->local_port : 7) != 0)
        return -EINVAL;
    s->listening = 1;
    return 0;
}

long sys_accept(int fd, struct sockaddr *addr, unsigned int *addrlen)
{
    struct sock *s = sock_from_fd(fd);
    struct tcp_pcb *child;
    struct sock *ns;
    int nfd;

    if (!s || !s->listening || !s->pcb)
        return -EINVAL;
    child = tcp_pcb_accept(s->pcb, 4000000);
    if (!child)
        return -EAGAIN;
    ns = (struct sock *)malloc(sizeof(*ns));
    if (!ns)
        return -ENOMEM;
    memset(ns, 0, sizeof(*ns));
    ns->type = SOCK_TCP;
    ns->pcb = child;
    ns->nif = s->nif;
    nfd = sock_fd_alloc();
    if (nfd < 0) {
        tcp_pcb_close(child);
        free(ns);
        return nfd;
    }
    sock_fd_install(nfd, ns);
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        /* remote filled after accept via pcb - skip for now */
        *addrlen = sizeof(*in);
    }
    return nfd;
}

long sys_connect(int fd, const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s = sock_from_fd(fd);
    unsigned int ip;
    unsigned short port;

    if (!s || !addr || addrlen < sizeof(struct sockaddr_in))
        return -EINVAL;
    if (addr->sa_family != AF_INET)
        return -EPROTONOSUPPORT;
    ip = sockaddr_ip(addr);
    port = sockaddr_port(addr);
    if (s->type == SOCK_TCP) {
        if (!s->pcb)
            return -EINVAL;
        if (tcp_pcb_connect(s->pcb, ip, port, 4000000) != 0)
            return -ENOTCONN;
        return 0;
    }
    /* UDP: remember peer for send */
    s->local_ip = ip; /* reuse as peer */
    s->local_port = port;
    return 0;
}

long sys_send(int fd, const void *buf, unsigned long len, int flags)
{
    struct sock *s = sock_from_fd(fd);
    (void)flags;
    if (!s || !buf)
        return -EINVAL;
    if (s->type == SOCK_TCP) {
        int n;
        if (!s->pcb)
            return -ENOTCONN;
        n = tcp_pcb_send(s->pcb, buf, (unsigned int)len);
        return n < 0 ? -EIO : n;
    }
    if (udp_send(s->nif, s->local_ip, 40001, s->local_port,
                 (const unsigned char *)buf, (unsigned int)len) != 0)
        return -EIO;
    return (long)len;
}

long sys_recv(int fd, void *buf, unsigned long len, int flags)
{
    struct sock *s = sock_from_fd(fd);
    (void)flags;
    if (!s || !buf)
        return -EINVAL;
    if (s->type == SOCK_TCP) {
        int n;
        if (!s->pcb)
            return -ENOTCONN;
        n = tcp_pcb_recv(s->pcb, buf, (unsigned int)len, 4000000);
        return n < 0 ? -EIO : n;
    }
    return -ENOSYS;
}

void sock_close(void *sock)
{
    struct sock *s = (struct sock *)sock;
    if (!s)
        return;
    if (s->pcb)
        tcp_pcb_close(s->pcb);
    free(s);
}

int sock_read(void *sock, void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    if (!s || s->type != SOCK_TCP || !s->pcb)
        return -EBADF;
    return tcp_pcb_recv(s->pcb, buf, (unsigned int)n, 4000000);
}

int sock_write(void *sock, const void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    if (!s || s->type != SOCK_TCP || !s->pcb)
        return -EBADF;
    return tcp_pcb_send(s->pcb, buf, (unsigned int)n);
}
