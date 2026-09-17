#include "sock.h"
#include "tcp.h"
#include "udp.h"
#include "netif.h"
#include "net_endian.h"
#include "net_sync.h"
#include "../process/process.h"

extern void *malloc(unsigned int);
extern void free(void *);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

#define EBADF  9
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define EIO    5
#define ENOSYS 38
#define ENOTCONN 107
#define EPROTONOSUPPORT 93
#define EADDRINUSE 98

#define SOCK_TCP 1
#define SOCK_UDP 2

#define SOCK_UDP_RX_MAX 512
#define SOCK_UDP_SLOTS  8
#define SOCK_UDP_EPH_BASE 42000
#define SOCK_LISTEN_PORT  7779

struct sock {
    int type;
    int listening;
    int bound;
    int refs; /* shared across dup2 / fork clones */
    unsigned short local_port;
    unsigned int local_ip;
    unsigned int peer_ip;
    unsigned short peer_port;
    struct tcp_pcb *pcb;
    struct netif *nif;
    unsigned char rx_buf[SOCK_UDP_RX_MAX];
    unsigned int rx_len;
    unsigned int rx_from_ip;
    unsigned short rx_from_port;
};

static struct sock *udp_socks[SOCK_UDP_SLOTS];
static unsigned short udp_eph = SOCK_UDP_EPH_BASE;

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

static void fill_sockaddr(struct sockaddr *addr, unsigned int *addrlen,
                          unsigned int ip_host, unsigned short port_host)
{
    struct sockaddr_in *in;
    if (!addr || !addrlen || *addrlen < sizeof(*in))
        return;
    in = (struct sockaddr_in *)addr;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = net_htons(port_host);
    in->sin_addr = net_htonl(ip_host);
    *addrlen = sizeof(*in);
}

static int udp_register(struct sock *s)
{
    int i;
    for (i = 0; i < SOCK_UDP_SLOTS; i++) {
        if (!udp_socks[i]) {
            udp_socks[i] = s;
            return 0;
        }
    }
    return -1;
}

static void udp_unregister(struct sock *s)
{
    int i;
    for (i = 0; i < SOCK_UDP_SLOTS; i++) {
        if (udp_socks[i] == s)
            udp_socks[i] = 0;
    }
}

static unsigned short udp_alloc_ephemeral(void)
{
    unsigned short p = udp_eph++;
    if (udp_eph < SOCK_UDP_EPH_BASE)
        udp_eph = SOCK_UDP_EPH_BASE;
    return p;
}

int sock_udp_deliver(unsigned short dport, unsigned short sport,
                     unsigned int sip, const unsigned char *payload,
                     unsigned int plen)
{
    int i;
    for (i = 0; i < SOCK_UDP_SLOTS; i++) {
        struct sock *s = udp_socks[i];
        unsigned int n;
        if (!s || s->type != SOCK_UDP || !s->bound || s->local_port != dport)
            continue;
        if (s->peer_port && (s->peer_ip != sip || s->peer_port != sport))
            continue;
        if (s->rx_len)
            return 1;
        n = plen < SOCK_UDP_RX_MAX ? plen : SOCK_UDP_RX_MAX;
        if (n && payload)
            memcpy(s->rx_buf, payload, n);
        s->rx_len = n;
        s->rx_from_ip = sip;
        s->rx_from_port = sport;
        return 1;
    }
    return 0;
}

static int udp_ensure_local(struct sock *s)
{
    if (s->local_port)
        return 0;
    s->local_port = udp_alloc_ephemeral();
    s->bound = 1;
    return 0;
}

long sys_socket(int domain, int type, int protocol)
{
    struct sock *s;
    int fd;
    long ret;

    (void)protocol;
    net_lock();
    if (domain != AF_INET) {
        ret = -EPROTONOSUPPORT;
        goto out;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        ret = -EPROTONOSUPPORT;
        goto out;
    }
    if (!netif_default()) {
        ret = -ENOSYS;
        goto out;
    }

    s = (struct sock *)malloc(sizeof(*s));
    if (!s) {
        ret = -ENOMEM;
        goto out;
    }
    memset(s, 0, sizeof(*s));
    s->nif = netif_default();
    s->refs = 1;
    s->type = (type == SOCK_STREAM) ? SOCK_TCP : SOCK_UDP;
    if (s->type == SOCK_TCP) {
        s->pcb = tcp_pcb_new(s->nif);
        if (!s->pcb) {
            free(s);
            ret = -ENOMEM;
            goto out;
        }
    } else if (udp_register(s) != 0) {
        free(s);
        ret = -ENOMEM;
        goto out;
    }

    fd = sock_fd_alloc();
    if (fd < 0) {
        if (s->pcb)
            tcp_pcb_free(s->pcb);
        if (s->type == SOCK_UDP)
            udp_unregister(s);
        free(s);
        ret = fd;
        goto out;
    }
    sock_fd_install(fd, s);
    ret = fd;
out:
    net_unlock();
    return ret;
}

long sys_bind(int fd, const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s;
    unsigned short port;
    int i;
    long ret;

    net_lock();
    s = sock_from_fd(fd);
    if (!s || !addr || addrlen < sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    if (addr->sa_family != AF_INET) {
        ret = -EPROTONOSUPPORT;
        goto out;
    }
    port = sockaddr_port(addr);
    if (s->type == SOCK_UDP && port) {
        for (i = 0; i < SOCK_UDP_SLOTS; i++) {
            if (udp_socks[i] && udp_socks[i] != s &&
                udp_socks[i]->bound && udp_socks[i]->local_port == port) {
                ret = -EADDRINUSE;
                goto out;
            }
        }
    }
    s->local_port = port;
    s->local_ip = sockaddr_ip(addr);
    s->bound = 1;
    ret = 0;
out:
    net_unlock();
    return ret;
}

long sys_listen(int fd, int backlog)
{
    struct sock *s;
    unsigned short port;
    long ret;

    (void)backlog;
    net_lock();
    s = sock_from_fd(fd);
    if (!s || s->type != SOCK_TCP || !s->pcb) {
        ret = -EINVAL;
        goto out;
    }
    port = s->local_port ? s->local_port : SOCK_LISTEN_PORT;
    if (tcp_pcb_listen(s->pcb, port) != 0) {
        ret = -EINVAL;
        goto out;
    }
    s->local_port = port;
    s->listening = 1;
    ret = 0;
out:
    net_unlock();
    return ret;
}

long sys_accept(int fd, struct sockaddr *addr, unsigned int *addrlen)
{
    struct sock *s;
    struct tcp_pcb *child;
    struct sock *ns;
    int nfd;
    long ret;

    net_lock();
    s = sock_from_fd(fd);
    if (!s || !s->listening || !s->pcb) {
        ret = -EINVAL;
        goto out;
    }
    child = tcp_pcb_accept(s->pcb, 8000000);
    if (!child) {
        ret = -EAGAIN;
        goto out;
    }
    ns = (struct sock *)malloc(sizeof(*ns));
    if (!ns) {
        tcp_pcb_close(child);
        ret = -ENOMEM;
        goto out;
    }
    memset(ns, 0, sizeof(*ns));
    ns->type = SOCK_TCP;
    ns->refs = 1;
    ns->pcb = child;
    ns->nif = s->nif;
    nfd = sock_fd_alloc();
    if (nfd < 0) {
        tcp_pcb_close(child);
        free(ns);
        ret = nfd;
        goto out;
    }
    sock_fd_install(nfd, ns);
    fill_sockaddr(addr, addrlen, tcp_pcb_remote_ip(child),
                  tcp_pcb_remote_port(child));
    ret = nfd;
out:
    net_unlock();
    return ret;
}

long sys_connect(int fd, const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s;
    unsigned int ip;
    unsigned short port;
    long ret;

    net_lock();
    s = sock_from_fd(fd);
    if (!s || !addr || addrlen < sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    if (addr->sa_family != AF_INET) {
        ret = -EPROTONOSUPPORT;
        goto out;
    }
    ip = sockaddr_ip(addr);
    port = sockaddr_port(addr);
    if (s->type == SOCK_TCP) {
        if (!s->pcb) {
            ret = -EINVAL;
            goto out;
        }
        if (s->bound && s->local_port)
            (void)tcp_pcb_bind(s->pcb, s->local_port);
        if (tcp_pcb_connect(s->pcb, ip, port, 4000000) != 0) {
            ret = -ENOTCONN;
            goto out;
        }
        ret = 0;
        goto out;
    }
    s->peer_ip = ip;
    s->peer_port = port;
    (void)udp_ensure_local(s);
    ret = 0;
out:
    net_unlock();
    return ret;
}

static long sock_udp_send(struct sock *s, const void *buf, unsigned long len,
                          unsigned int dst_ip, unsigned short dst_port)
{
    if (!dst_ip || !dst_port)
        return -ENOTCONN;
    (void)udp_ensure_local(s);
    if (udp_send(s->nif, dst_ip, s->local_port, dst_port,
                 (const unsigned char *)buf, (unsigned int)len) != 0)
        return -EIO;
    return (long)len;
}

static long sock_udp_recv(struct sock *s, void *buf, unsigned long len,
                          struct sockaddr *addr, unsigned int *addrlen,
                          unsigned int timeout_spins)
{
    unsigned int spins = 0;
    unsigned int n;

    (void)udp_ensure_local(s);
    while (s->rx_len == 0 && spins++ < timeout_spins) {
        /* Drop softnet lock so other CPUs can TX/RX under stress. */
        net_unlock();
        netif_poll(s->nif ? s->nif : netif_default());
        __asm__ volatile ("pause");
        net_lock();
    }
    if (s->rx_len == 0)
        return 0;
    n = s->rx_len < len ? s->rx_len : (unsigned int)len;
    memcpy(buf, s->rx_buf, n);
    fill_sockaddr(addr, addrlen, s->rx_from_ip, s->rx_from_port);
    s->rx_len = 0;
    return (long)n;
}

long sys_send(int fd, const void *buf, unsigned long len, int flags)
{
    struct sock *s;
    long ret;

    (void)flags;
    net_lock();
    s = sock_from_fd(fd);
    if (!s || !buf) {
        ret = -EINVAL;
        goto out;
    }
    if (s->type == SOCK_TCP) {
        int n;
        if (!s->pcb) {
            ret = -ENOTCONN;
            goto out;
        }
        n = tcp_pcb_send(s->pcb, buf, (unsigned int)len);
        ret = n < 0 ? -EIO : n;
        goto out;
    }
    ret = sock_udp_send(s, buf, len, s->peer_ip, s->peer_port);
out:
    net_unlock();
    return ret;
}

long sys_recv(int fd, void *buf, unsigned long len, int flags)
{
    struct sock *s;
    long ret;

    (void)flags;
    net_lock();
    s = sock_from_fd(fd);
    if (!s || !buf) {
        ret = -EINVAL;
        goto out;
    }
    if (s->type == SOCK_TCP) {
        int n;
        if (!s->pcb) {
            ret = -ENOTCONN;
            goto out;
        }
        n = tcp_pcb_recv(s->pcb, buf, (unsigned int)len, 4000000);
        ret = n < 0 ? -EIO : n;
        goto out;
    }
    ret = sock_udp_recv(s, buf, len, 0, 0, 4000000);
out:
    net_unlock();
    return ret;
}

long sys_sendto(int fd, const void *buf, unsigned long len,
                const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s;
    long ret;

    net_lock();
    s = sock_from_fd(fd);
    if (!s || !buf) {
        ret = -EINVAL;
        goto out;
    }
    if (s->type != SOCK_UDP) {
        ret = -EINVAL;
        goto out;
    }
    if (!addr || addrlen < sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    if (addr->sa_family != AF_INET) {
        ret = -EPROTONOSUPPORT;
        goto out;
    }
    ret = sock_udp_send(s, buf, len, sockaddr_ip(addr), sockaddr_port(addr));
out:
    net_unlock();
    return ret;
}

long sys_recvfrom(int fd, void *buf, unsigned long len,
                  struct sockaddr *addr, unsigned int *addrlen)
{
    struct sock *s;
    long ret;

    net_lock();
    s = sock_from_fd(fd);
    if (!s || !buf) {
        ret = -EINVAL;
        goto out;
    }
    if (s->type != SOCK_UDP) {
        ret = -EINVAL;
        goto out;
    }
    ret = sock_udp_recv(s, buf, len, addr, addrlen, 4000000);
out:
    net_unlock();
    return ret;
}

void sock_close(void *sock)
{
    struct sock *s = (struct sock *)sock;
    if (!s)
        return;
    net_lock();
    if (s->refs > 1) {
        s->refs--;
        net_unlock();
        return;
    }
    s->refs = 0;
    if (s->type == SOCK_UDP)
        udp_unregister(s);
    if (s->pcb)
        tcp_pcb_close(s->pcb);
    net_unlock();
    free(s);
}

int sock_retain(void *sock)
{
    struct sock *s = (struct sock *)sock;
    if (!s)
        return 0;
    net_lock();
    s->refs++;
    net_unlock();
    return 1;
}

int sock_read(void *sock, void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    int ret;

    if (!s || !buf || n <= 0)
        return -EBADF;
    net_lock();
    if (s->type == SOCK_TCP) {
        if (!s->pcb) {
            ret = -EBADF;
            goto out;
        }
        ret = tcp_pcb_recv(s->pcb, buf, (unsigned int)n, 4000000);
        goto out;
    }
    ret = (int)sock_udp_recv(s, buf, (unsigned long)n, 0, 0, 4000000);
out:
    net_unlock();
    return ret;
}

int sock_write(void *sock, const void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    int ret;

    if (!s || !buf || n <= 0)
        return -EBADF;
    net_lock();
    if (s->type == SOCK_TCP) {
        if (!s->pcb) {
            ret = -EBADF;
            goto out;
        }
        ret = tcp_pcb_send(s->pcb, buf, (unsigned int)n);
        goto out;
    }
    ret = (int)sock_udp_send(s, buf, (unsigned long)n, s->peer_ip, s->peer_port);
out:
    net_unlock();
    return ret;
}
