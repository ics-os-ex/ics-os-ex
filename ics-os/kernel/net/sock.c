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
extern void *memmove(void *d, const void *s, unsigned int n);

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
    int type; /* SOCK_TCP / SOCK_UDP */
    int listening;
    int bound;
    unsigned short local_port;
    unsigned int local_ip; /* host order; 0 = INADDR_ANY */
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
            return 1; /* drop while full; still consumed from wire */
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
    } else if (udp_register(s) != 0) {
        free(s);
        return -ENOMEM;
    }

    fd = sock_fd_alloc();
    if (fd < 0) {
        if (s->pcb)
            tcp_pcb_free(s->pcb);
        if (s->type == SOCK_UDP)
            udp_unregister(s);
        free(s);
        return fd;
    }
    sock_fd_install(fd, s);
    return fd;
}

long sys_bind(int fd, const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s = sock_from_fd(fd);
    unsigned short port;
    int i;

    if (!s || !addr || addrlen < sizeof(struct sockaddr_in))
        return -EINVAL;
    if (addr->sa_family != AF_INET)
        return -EPROTONOSUPPORT;
    port = sockaddr_port(addr);
    if (s->type == SOCK_UDP && port) {
        for (i = 0; i < SOCK_UDP_SLOTS; i++) {
            if (udp_socks[i] && udp_socks[i] != s &&
                udp_socks[i]->bound && udp_socks[i]->local_port == port)
                return -EADDRINUSE;
        }
    }
    s->local_port = port;
    s->local_ip = sockaddr_ip(addr);
    s->bound = 1;
    return 0;
}

long sys_listen(int fd, int backlog)
{
    struct sock *s = sock_from_fd(fd);
    unsigned short port;
    (void)backlog;
    if (!s || s->type != SOCK_TCP || !s->pcb)
        return -EINVAL;
    port = s->local_port ? s->local_port : SOCK_LISTEN_PORT;
    if (tcp_pcb_listen(s->pcb, port) != 0)
        return -EINVAL;
    s->local_port = port;
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
    child = tcp_pcb_accept(s->pcb, 8000000);
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
    fill_sockaddr(addr, addrlen, tcp_pcb_remote_ip(child),
                  tcp_pcb_remote_port(child));
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
    s->peer_ip = ip;
    s->peer_port = port;
    (void)udp_ensure_local(s);
    return 0;
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
        netif_poll(s->nif ? s->nif : netif_default());
        __asm__ volatile ("pause");
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
    return sock_udp_send(s, buf, len, s->peer_ip, s->peer_port);
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
    return sock_udp_recv(s, buf, len, 0, 0, 4000000);
}

long sys_sendto(int fd, const void *buf, unsigned long len,
                const struct sockaddr *addr, unsigned int addrlen)
{
    struct sock *s = sock_from_fd(fd);
    if (!s || !buf)
        return -EINVAL;
    if (s->type != SOCK_UDP)
        return -EINVAL;
    if (!addr || addrlen < sizeof(struct sockaddr_in))
        return -EINVAL;
    if (addr->sa_family != AF_INET)
        return -EPROTONOSUPPORT;
    return sock_udp_send(s, buf, len, sockaddr_ip(addr), sockaddr_port(addr));
}

long sys_recvfrom(int fd, void *buf, unsigned long len,
                  struct sockaddr *addr, unsigned int *addrlen)
{
    struct sock *s = sock_from_fd(fd);
    if (!s || !buf)
        return -EINVAL;
    if (s->type != SOCK_UDP)
        return -EINVAL;
    return sock_udp_recv(s, buf, len, addr, addrlen, 4000000);
}

void sock_close(void *sock)
{
    struct sock *s = (struct sock *)sock;
    if (!s)
        return;
    if (s->type == SOCK_UDP)
        udp_unregister(s);
    if (s->pcb)
        tcp_pcb_close(s->pcb);
    free(s);
}

int sock_read(void *sock, void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    if (!s || !buf || n <= 0)
        return -EBADF;
    if (s->type == SOCK_TCP) {
        if (!s->pcb)
            return -EBADF;
        return tcp_pcb_recv(s->pcb, buf, (unsigned int)n, 4000000);
    }
    return (int)sock_udp_recv(s, buf, (unsigned long)n, 0, 0, 4000000);
}

int sock_write(void *sock, const void *buf, long n)
{
    struct sock *s = (struct sock *)sock;
    if (!s || !buf || n <= 0)
        return -EBADF;
    if (s->type == SOCK_TCP) {
        if (!s->pcb)
            return -EBADF;
        return tcp_pcb_send(s->pcb, buf, (unsigned int)n);
    }
    return (int)sock_udp_send(s, buf, (unsigned long)n, s->peer_ip, s->peer_port);
}
