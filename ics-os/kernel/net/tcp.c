#include "tcp.h"
#include "netif.h"
#include "pbuf.h"
#include "ipv4.h"
#include "net_endian.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);
extern int memcmp(const void *s1, const void *s2, unsigned int n);
extern int printf(const char *fmt, ...);

#define TCP_PCB_MAX 2
#define TCP_RX_MAX  256

struct tcp_pcb {
    unsigned char state;
    unsigned char echo;       /* server: echo payload */
    unsigned char client;     /* client selftest PCB */
    unsigned char got_echo;
    unsigned short local_port;
    unsigned short remote_port;
    unsigned int remote_ip;
    unsigned int snd_una;
    unsigned int snd_nxt;
    unsigned int rcv_nxt;
    unsigned int iss;
    unsigned char tx_payload[64];
    unsigned int tx_len;
    struct netif *nif;
};

static struct tcp_pcb pcbs[TCP_PCB_MAX];
static unsigned short listen_port;

static struct tcp_pcb *tcp_find(unsigned short local, unsigned short remote,
                                unsigned int remote_ip, int allow_listen)
{
    int i;
    for (i = 0; i < TCP_PCB_MAX; i++) {
        if (pcbs[i].state == TCP_CLOSED)
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

static struct tcp_pcb *tcp_alloc(void)
{
    int i;
    for (i = 0; i < TCP_PCB_MAX; i++) {
        if (pcbs[i].state == TCP_CLOSED) {
            memset(&pcbs[i], 0, sizeof(pcbs[i]));
            return &pcbs[i];
        }
    }
    return 0;
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

void tcp_init(void)
{
    memset(pcbs, 0, sizeof(pcbs));
    listen_port = 0;
}

void tcp_listen_echo(unsigned short port)
{
    struct tcp_pcb *pcb = tcp_alloc();
    if (!pcb)
        return;
    pcb->state = TCP_LISTEN;
    pcb->local_port = port;
    pcb->echo = 1;
    listen_port = port;
}

static void tcp_handle(struct netif *nif, struct tcp_pcb *pcb,
                       const struct tcp_hdr *th, unsigned int seq,
                       unsigned int ack, unsigned char flags,
                       const unsigned char *payload, unsigned int plen,
                       unsigned short sport, unsigned int sip)
{
    if (pcb->state == TCP_LISTEN) {
        struct tcp_pcb *child;
        if (!(flags & TCP_SYN) || (flags & TCP_ACK))
            return;
        child = tcp_alloc();
        if (!child)
            return;
        child->nif = nif;
        child->state = TCP_SYN_RCVD;
        child->echo = 1;
        child->local_port = pcb->local_port;
        child->remote_port = sport;
        child->remote_ip = sip;
        child->iss = 2000;
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
            if (pcb->client && pcb->tx_len)
                (void)tcp_output(pcb, TCP_PSH | TCP_ACK,
                                 pcb->tx_payload, pcb->tx_len);
        } else if (flags & TCP_RST) {
            pcb->state = TCP_CLOSED;
        }
        return;
    }

    if (pcb->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK) {
            pcb->snd_una = ack;
            pcb->state = TCP_ESTABLISHED;
        }
        /* fall through to process payload/FIN if present */
    }

    if (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT ||
        pcb->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK)
            pcb->snd_una = ack;

        if (plen && seq == pcb->rcv_nxt) {
            pcb->rcv_nxt = seq + plen;
            if (pcb->client) {
                if (plen == pcb->tx_len &&
                    memcmp(payload, pcb->tx_payload, plen) == 0)
                    pcb->got_echo = 1;
            } else if (pcb->echo) {
                (void)tcp_output(pcb, TCP_PSH | TCP_ACK, payload, plen);
            }
            (void)tcp_output(pcb, TCP_ACK, 0, 0);
        }

        if (flags & TCP_FIN) {
            if (seq == pcb->rcv_nxt || seq + 1 == pcb->rcv_nxt ||
                (plen && seq + plen == pcb->rcv_nxt)) {
                if (!(flags & TCP_ACK) || seq == pcb->rcv_nxt)
                    pcb->rcv_nxt = seq + 1;
                if (pcb->state == TCP_ESTABLISHED) {
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
        /* RST unexpected segments. */
        if (!(flags & TCP_RST)) {
            struct tcp_pcb tmp;
            memset(&tmp, 0, sizeof(tmp));
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

    tcp_handle(nif, pcb, th, seq, ack, flags, payload, plen, sport, sip);
    pbuf_free(p);
}

int tcp_echo_client(struct netif *nif, unsigned int dst_host,
                    unsigned short dst_port, unsigned int timeout_spins)
{
    static const unsigned char payload[] = "ICS-TCP!";
    struct tcp_pcb *pcb;
    unsigned int spins;
    unsigned int syn_retries;

    if (!nif || !nif->configured)
        return -1;

    pcb = tcp_alloc();
    if (!pcb)
        return -1;
    pcb->nif = nif;
    pcb->client = 1;
    pcb->local_port = 41000;
    pcb->remote_port = dst_port;
    pcb->remote_ip = dst_host;
    pcb->iss = 1000;
    pcb->snd_nxt = pcb->iss;
    pcb->snd_una = pcb->iss;
    pcb->rcv_nxt = 0;
    pcb->tx_len = sizeof(payload) - 1;
    memcpy(pcb->tx_payload, payload, pcb->tx_len);
    pcb->got_echo = 0;
    pcb->state = TCP_SYN_SENT;

    if (tcp_output(pcb, TCP_SYN, 0, 0) != 0) {
        pcb->state = TCP_CLOSED;
        return -1;
    }

    spins = 0;
    syn_retries = 0;
    while (!pcb->got_echo && spins < timeout_spins) {
        netif_poll(nif);
        if (pcb->state == TCP_SYN_SENT && (spins % 200000) == 199999 &&
            syn_retries < 5) {
            pcb->snd_nxt = pcb->iss;
            (void)tcp_output(pcb, TCP_SYN, 0, 0);
            syn_retries++;
        }
        if (pcb->state == TCP_CLOSED)
            break;
        spins++;
        __asm__ volatile ("pause");
    }

    if (pcb->got_echo) {
        (void)tcp_output(pcb, TCP_FIN | TCP_ACK, 0, 0);
        pcb->state = TCP_FIN_WAIT_1;
        for (spins = 0; spins < 200000 && pcb->state != TCP_CLOSED; spins++) {
            netif_poll(nif);
            __asm__ volatile ("pause");
        }
        pcb->state = TCP_CLOSED;
        return 0;
    }
    pcb->state = TCP_CLOSED;
    return -1;
}
