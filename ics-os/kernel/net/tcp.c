#include "tcp.h"
#include "netif.h"
#include "pbuf.h"
#include "ipv4.h"
#include "net_endian.h"
#include "net_sync.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memmove(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);
extern int memcmp(const void *s1, const void *s2, unsigned int n);

struct tcp_pcb {
    unsigned char state;
    unsigned char echo;
    unsigned char in_use;
    unsigned char pending_accept; /* child ready for accept() */
    unsigned short local_port;
    unsigned short remote_port;
    unsigned int remote_ip;
    unsigned int snd_una;
    unsigned int snd_nxt;
    unsigned int rcv_nxt;
    unsigned int iss;
    unsigned char rx_buf[TCP_RX_MAX];
    unsigned int rx_len;
    struct tcp_pcb *parent; /* listen parent for accepted children */
    struct netif *nif;
};

static struct tcp_pcb pcbs[TCP_PCB_MAX];
static unsigned short ephemeral = 41000;

static struct tcp_pcb *tcp_find(unsigned short local, unsigned short remote,
                                unsigned int remote_ip, int allow_listen)
{
    int i;
    for (i = 0; i < TCP_PCB_MAX; i++) {
        if (!pcbs[i].in_use)
            continue;
        if (pcbs[i].state == TCP_LISTEN) {
            if (allow_listen && pcbs[i].local_port == local)
                return &pcbs[i];
            continue;
        }
        if (pcbs[i].local_port == local &&
            pcbs[i].remote_port == remote &&
            pcbs[i].remote_ip == remote_ip)
            return &pcbs[i];
    }
    return 0;
}

struct tcp_pcb *tcp_pcb_new(struct netif *nif)
{
    int i;
    for (i = 0; i < TCP_PCB_MAX; i++) {
        if (!pcbs[i].in_use) {
            memset(&pcbs[i], 0, sizeof(pcbs[i]));
            pcbs[i].in_use = 1;
            pcbs[i].state = TCP_CLOSED;
            pcbs[i].nif = nif;
            return &pcbs[i];
        }
    }
    return 0;
}

void tcp_pcb_free(struct tcp_pcb *pcb)
{
    if (!pcb)
        return;
    memset(pcb, 0, sizeof(*pcb));
}

int tcp_pcb_state(struct tcp_pcb *pcb)
{
    return pcb ? pcb->state : TCP_CLOSED;
}

unsigned int tcp_pcb_remote_ip(struct tcp_pcb *pcb)
{
    return pcb ? pcb->remote_ip : 0;
}

unsigned short tcp_pcb_remote_port(struct tcp_pcb *pcb)
{
    return pcb ? pcb->remote_port : 0;
}

static int tcp_output(struct tcp_pcb *pcb, unsigned char flags,
                      const unsigned char *payload, unsigned int payload_len)
{
    struct pbuf *p;
    unsigned int tcp_len;
    unsigned int seq;

    if (!pcb || !pcb->nif)
        return -1;
    if (TCP_HDR_MIN + payload_len > PBUF_SIZE)
        return -1;

    seq = pcb->snd_nxt;
    p = pbuf_alloc((u16)(TCP_HDR_MIN + payload_len));
    if (!p)
        return -1;
    tcp_len = tcp_build(p->data, pcb->local_port, pcb->remote_port,
                        seq, pcb->rcv_nxt, flags, payload, payload_len,
                        pcb->nif->ip, pcb->remote_ip);
    p->len = (u16)tcp_len;

    if (flags & TCP_SYN)
        pcb->snd_nxt = seq + 1;
    else if (payload_len)
        pcb->snd_nxt = seq + payload_len;
    if (flags & TCP_FIN)
        pcb->snd_nxt++;

    return ipv4_output(pcb->nif, p, pcb->remote_ip, IPV4_PROTO_TCP);
}

static void tcp_rx_append(struct tcp_pcb *pcb, const unsigned char *data,
                          unsigned int len)
{
    unsigned int space, n;
    if (!len || !data)
        return;
    space = TCP_RX_MAX - pcb->rx_len;
    n = len < space ? len : space;
    memcpy(pcb->rx_buf + pcb->rx_len, data, n);
    pcb->rx_len += n;
}

static void tcp_handle(struct netif *nif, struct tcp_pcb *pcb,
                       unsigned int seq, unsigned int ack, unsigned char flags,
                       const unsigned char *payload, unsigned int plen,
                       unsigned short sport, unsigned int sip)
{
    if (pcb->state == TCP_LISTEN) {
        struct tcp_pcb *child;
        if (!(flags & TCP_SYN) || (flags & TCP_ACK))
            return;
        child = tcp_pcb_new(nif);
        if (!child)
            return;
        child->state = TCP_SYN_RCVD;
        child->echo = pcb->echo;
        child->parent = pcb;
        child->local_port = pcb->local_port;
        child->remote_port = sport;
        child->remote_ip = sip;
        child->iss = 2000 + (unsigned int)(uintptr)child;
        child->snd_nxt = child->iss;
        child->snd_una = child->iss;
        child->rcv_nxt = seq + 1;
        (void)tcp_output(child, TCP_SYN | TCP_ACK, 0, 0);
        return;
    }

    if (pcb->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            pcb->rcv_nxt = seq + 1;
            pcb->snd_una = ack;
            pcb->state = TCP_ESTABLISHED;
            (void)tcp_output(pcb, TCP_ACK, 0, 0);
        } else if (flags & TCP_RST) {
            pcb->state = TCP_CLOSED;
        }
        return;
    }

    if (pcb->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK) {
            pcb->snd_una = ack;
            pcb->state = TCP_ESTABLISHED;
            pcb->pending_accept = 1;
        }
    }

    if (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT ||
        pcb->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK)
            pcb->snd_una = ack;

        if (plen && seq == pcb->rcv_nxt) {
            pcb->rcv_nxt = seq + plen;
            tcp_rx_append(pcb, payload, plen);
            if (pcb->echo)
                (void)tcp_output(pcb, TCP_PSH | TCP_ACK, payload, plen);
            (void)tcp_output(pcb, TCP_ACK, 0, 0);
        }

        if (flags & TCP_FIN) {
            if (seq == pcb->rcv_nxt ||
                (plen && seq + plen == pcb->rcv_nxt)) {
                pcb->rcv_nxt = seq + (plen ? plen : 0) + 1;
                if (pcb->state == TCP_ESTABLISHED ||
                    pcb->state == TCP_SYN_RCVD) {
                    pcb->state = TCP_CLOSE_WAIT;
                    (void)tcp_output(pcb, TCP_ACK, 0, 0);
                    (void)tcp_output(pcb, TCP_FIN | TCP_ACK, 0, 0);
                    pcb->state = TCP_LAST_ACK;
                }
            }
        }
        return;
    }

    if (pcb->state == TCP_FIN_WAIT_1) {
        if ((flags & TCP_ACK) && ack == pcb->snd_nxt)
            pcb->state = TCP_CLOSED;
        if (flags & TCP_FIN) {
            pcb->rcv_nxt = seq + 1;
            (void)tcp_output(pcb, TCP_ACK, 0, 0);
            pcb->state = TCP_CLOSED;
        }
        return;
    }

    if (pcb->state == TCP_LAST_ACK) {
        if (flags & TCP_ACK)
            pcb->state = TCP_CLOSED;
        return;
    }
}

void tcp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len)
{
    struct ipv4_hdr *ip;
    struct tcp_hdr *th;
    unsigned int thl, tcp_len, plen, seq, ack, sip;
    unsigned short sport, dport;
    unsigned char flags;
    const unsigned char *payload;
    struct tcp_pcb *pcb;

    if (!nif || !p || p->len < ip_hdr_len + TCP_HDR_MIN) {
        pbuf_free(p);
        return;
    }
    ip = (struct ipv4_hdr *)p->data;
    th = (struct tcp_hdr *)(p->data + ip_hdr_len);
    thl = tcp_hdr_len(th);
    if (thl < TCP_HDR_MIN || ip_hdr_len + thl > p->len) {
        pbuf_free(p);
        return;
    }
    tcp_len = p->len - ip_hdr_len;
    plen = tcp_len - thl;
    sport = net_ntohs(th->src_port);
    dport = net_ntohs(th->dst_port);
    seq = net_ntohl(th->seq);
    ack = net_ntohl(th->ack);
    flags = th->flags;
    sip = net_ntohl(ip->src);
    payload = p->data + ip_hdr_len + thl;

    pcb = tcp_find(dport, sport, sip, 0);
    if (!pcb)
        pcb = tcp_find(dport, 0, 0, 1);
    if (!pcb) {
        if (!(flags & TCP_RST)) {
            struct tcp_pcb tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.in_use = 1;
            tmp.nif = nif;
            tmp.local_port = dport;
            tmp.remote_port = sport;
            tmp.remote_ip = sip;
            tmp.snd_nxt = ack;
            tmp.rcv_nxt = seq + ((flags & TCP_SYN) ? 1 : plen);
            (void)tcp_output(&tmp, TCP_RST | TCP_ACK, 0, 0);
        }
        pbuf_free(p);
        return;
    }

    tcp_handle(nif, pcb, seq, ack, flags, payload, plen, sport, sip);
    pbuf_free(p);
}

void tcp_init(void)
{
    memset(pcbs, 0, sizeof(pcbs));
    ephemeral = 41000;
}

void tcp_listen_echo(unsigned short port)
{
    struct tcp_pcb *pcb = tcp_pcb_new(netif_default());
    if (!pcb)
        return;
    pcb->state = TCP_LISTEN;
    pcb->local_port = port;
    pcb->echo = 1;
}

int tcp_pcb_listen(struct tcp_pcb *pcb, unsigned short port)
{
    if (!pcb)
        return -1;
    pcb->state = TCP_LISTEN;
    pcb->local_port = port;
    pcb->echo = 0;
    return 0;
}

struct tcp_pcb *tcp_pcb_accept(struct tcp_pcb *listener,
                               unsigned int timeout_spins)
{
    unsigned int spins = 0;
    int i;

    if (!listener || listener->state != TCP_LISTEN)
        return 0;
    while (spins++ < timeout_spins) {
        netif_poll(listener->nif ? listener->nif : netif_default());
        for (i = 0; i < TCP_PCB_MAX; i++) {
            if (pcbs[i].in_use && pcbs[i].parent == listener &&
                pcbs[i].pending_accept &&
                pcbs[i].state == TCP_ESTABLISHED) {
                pcbs[i].pending_accept = 0;
                return &pcbs[i];
            }
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

int tcp_pcb_connect(struct tcp_pcb *pcb, unsigned int dst_host,
                    unsigned short dst_port, unsigned int timeout_spins)
{
    unsigned int spins = 0;
    unsigned int syn_retries = 0;

    if (!pcb || !pcb->nif)
        return -1;
    pcb->local_port = ephemeral++;
    if (ephemeral < 41000)
        ephemeral = 41000;
    pcb->remote_port = dst_port;
    pcb->remote_ip = dst_host;
    pcb->iss = 1000 + pcb->local_port;
    pcb->snd_nxt = pcb->iss;
    pcb->snd_una = pcb->iss;
    pcb->rcv_nxt = 0;
    pcb->rx_len = 0;
    pcb->state = TCP_SYN_SENT;

    if (tcp_output(pcb, TCP_SYN, 0, 0) != 0) {
        pcb->state = TCP_CLOSED;
        return -1;
    }

    while (pcb->state != TCP_ESTABLISHED && spins < timeout_spins) {
        netif_poll(pcb->nif);
        if (pcb->state == TCP_CLOSED)
            return -1;
        if (pcb->state == TCP_SYN_SENT && (spins % 200000) == 199999 &&
            syn_retries < 5) {
            pcb->snd_nxt = pcb->iss;
            (void)tcp_output(pcb, TCP_SYN, 0, 0);
            syn_retries++;
        }
        spins++;
        __asm__ volatile ("pause");
    }
    return pcb->state == TCP_ESTABLISHED ? 0 : -1;
}

int tcp_pcb_send(struct tcp_pcb *pcb, const void *buf, unsigned int len)
{
    if (!pcb || pcb->state != TCP_ESTABLISHED || !buf || !len)
        return -1;
    if (tcp_output(pcb, TCP_PSH | TCP_ACK, (const unsigned char *)buf, len) != 0)
        return -1;
    return (int)len;
}

int tcp_pcb_recv(struct tcp_pcb *pcb, void *buf, unsigned int len,
                 unsigned int timeout_spins)
{
    unsigned int spins = 0;
    unsigned int n;

    if (!pcb || !buf || !len)
        return -1;
    while (pcb->rx_len == 0 && pcb->state == TCP_ESTABLISHED &&
           spins++ < timeout_spins) {
        netif_poll(pcb->nif ? pcb->nif : netif_default());
        __asm__ volatile ("pause");
    }
    if (pcb->rx_len == 0)
        return (pcb->state == TCP_ESTABLISHED) ? 0 : -1;
    n = pcb->rx_len < len ? pcb->rx_len : len;
    memcpy(buf, pcb->rx_buf, n);
    if (n < pcb->rx_len)
        memmove(pcb->rx_buf, pcb->rx_buf + n, pcb->rx_len - n);
    pcb->rx_len -= n;
    return (int)n;
}

int tcp_pcb_close(struct tcp_pcb *pcb)
{
    unsigned int spins;
    if (!pcb)
        return -1;
    if (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT) {
        (void)tcp_output(pcb, TCP_FIN | TCP_ACK, 0, 0);
        pcb->state = (pcb->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK
                                                    : TCP_FIN_WAIT_1;
        for (spins = 0; spins < 200000 && pcb->state != TCP_CLOSED; spins++) {
            netif_poll(pcb->nif);
            __asm__ volatile ("pause");
        }
    }
    tcp_pcb_free(pcb);
    return 0;
}

int tcp_echo_client(struct netif *nif, unsigned int dst_host,
                    unsigned short dst_port, unsigned int timeout_spins)
{
    static const unsigned char payload[] = "ICS-TCP!";
    unsigned char buf[16];
    struct tcp_pcb *pcb;
    int n;
    int ret = -1;

    net_lock();
    pcb = tcp_pcb_new(nif);
    if (!pcb)
        goto out;
    if (tcp_pcb_connect(pcb, dst_host, dst_port, timeout_spins) != 0) {
        tcp_pcb_free(pcb);
        goto out;
    }
    if (tcp_pcb_send(pcb, payload, sizeof(payload) - 1) < 0) {
        tcp_pcb_close(pcb);
        goto out;
    }
    n = tcp_pcb_recv(pcb, buf, sizeof(buf), timeout_spins);
    if (n != (int)(sizeof(payload) - 1) ||
        memcmp(buf, payload, sizeof(payload) - 1) != 0) {
        tcp_pcb_close(pcb);
        goto out;
    }
    tcp_pcb_close(pcb);
    ret = 0;
out:
    net_unlock();
    return ret;
}
