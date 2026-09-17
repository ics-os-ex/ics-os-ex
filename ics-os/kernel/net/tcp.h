#ifndef ICSOS_NET_TCP_H
#define ICSOS_NET_TCP_H

#include "checksum.h"
#include "ipv4.h"
#include "net_endian.h"

#define TCP_HDR_MIN       20
#define TCP_ECHO_PORT     7
#define TCP_TEST_PORT     7778
#define TCP_PCB_MAX       8
#define TCP_RX_MAX        1024
#define TCP_UNA_MAX       2048
#define TCP_MSS           1024

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RCVD     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_CLOSE_WAIT   6
#define TCP_LAST_ACK     7

struct tcp_hdr {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned int   seq;
    unsigned int   ack;
    unsigned char  off_res;
    unsigned char  flags;
    unsigned short window;
    unsigned short checksum;
    unsigned short urg;
} __attribute__((packed));

static inline unsigned int tcp_hdr_len(const struct tcp_hdr *h)
{
    return (unsigned int)(h->off_res >> 4) * 4u;
}

static inline unsigned short tcp_checksum(unsigned int src_ip_host,
                                          unsigned int dst_ip_host,
                                          const unsigned char *tcp,
                                          unsigned int tcp_len)
{
    unsigned int sum = 0;
    unsigned char ph[12];

    ph[0] = (unsigned char)((src_ip_host >> 24) & 0xFF);
    ph[1] = (unsigned char)((src_ip_host >> 16) & 0xFF);
    ph[2] = (unsigned char)((src_ip_host >> 8) & 0xFF);
    ph[3] = (unsigned char)(src_ip_host & 0xFF);
    ph[4] = (unsigned char)((dst_ip_host >> 24) & 0xFF);
    ph[5] = (unsigned char)((dst_ip_host >> 16) & 0xFF);
    ph[6] = (unsigned char)((dst_ip_host >> 8) & 0xFF);
    ph[7] = (unsigned char)(dst_ip_host & 0xFF);
    ph[8] = 0;
    ph[9] = IPV4_PROTO_TCP;
    ph[10] = (unsigned char)((tcp_len >> 8) & 0xFF);
    ph[11] = (unsigned char)(tcp_len & 0xFF);
    sum = net_checksum_add(ph, 12, 0);
    sum = net_checksum_add(tcp, tcp_len, sum);
    {
        unsigned short c = net_checksum_fold(sum);
        return c ? c : 0xFFFF;
    }
}

static inline unsigned int tcp_build_win(unsigned char *dst,
                                         unsigned short sport,
                                         unsigned short dport,
                                         unsigned int seq, unsigned int ack,
                                         unsigned char flags,
                                         const unsigned char *payload,
                                         unsigned int payload_len,
                                         unsigned int src_ip,
                                         unsigned int dst_ip,
                                         unsigned short window)
{
    struct tcp_hdr *h = (struct tcp_hdr *)dst;
    unsigned int i;
    unsigned int tcp_len = TCP_HDR_MIN + payload_len;

    h->src_port = net_htons(sport);
    h->dst_port = net_htons(dport);
    h->seq = net_htonl(seq);
    h->ack = net_htonl(ack);
    h->off_res = (5 << 4);
    h->flags = flags;
    h->window = net_htons(window);
    h->checksum = 0;
    h->urg = 0;
    for (i = 0; i < payload_len; i++)
        dst[TCP_HDR_MIN + i] = payload[i];
    h->checksum = net_htons(tcp_checksum(src_ip, dst_ip, dst, tcp_len));
    return tcp_len;
}

static inline unsigned int tcp_build(unsigned char *dst,
                                     unsigned short sport, unsigned short dport,
                                     unsigned int seq, unsigned int ack,
                                     unsigned char flags,
                                     const unsigned char *payload,
                                     unsigned int payload_len,
                                     unsigned int src_ip, unsigned int dst_ip)
{
    return tcp_build_win(dst, sport, dport, seq, ack, flags, payload,
                         payload_len, src_ip, dst_ip, 8192);
}

struct netif;
struct pbuf;
struct tcp_pcb;

void tcp_init(void);
void tcp_listen_echo(unsigned short port);
void tcp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len);
int  tcp_echo_client(struct netif *nif, unsigned int dst_host,
                     unsigned short dst_port, unsigned int timeout_spins);

/* Socket-facing TCP PCB API. */
struct tcp_pcb *tcp_pcb_new(struct netif *nif);
void tcp_pcb_free(struct tcp_pcb *pcb);
int  tcp_pcb_bind(struct tcp_pcb *pcb, unsigned short port);
int  tcp_pcb_connect(struct tcp_pcb *pcb, unsigned int dst_host,
                     unsigned short dst_port, unsigned int timeout_spins);
int  tcp_pcb_listen(struct tcp_pcb *pcb, unsigned short port);
struct tcp_pcb *tcp_pcb_accept(struct tcp_pcb *listener,
                               unsigned int timeout_spins);
int  tcp_pcb_send(struct tcp_pcb *pcb, const void *buf, unsigned int len);
int  tcp_pcb_recv(struct tcp_pcb *pcb, void *buf, unsigned int len,
                  unsigned int timeout_spins);
int  tcp_pcb_close(struct tcp_pcb *pcb);
int  tcp_pcb_state(struct tcp_pcb *pcb);
unsigned int tcp_pcb_remote_ip(struct tcp_pcb *pcb);
unsigned short tcp_pcb_remote_port(struct tcp_pcb *pcb);

/* Softnet / RTO: retransmit unacked data. */
void tcp_timer(unsigned int now_ticks);
unsigned int tcp_rexmit_count(void);
/* Test hook: drop the next data segment on the wire once (still queued). */
void tcp_test_drop_next_data(int enable);
int  tcp_echo_rexmit_selftest(struct netif *nif, unsigned int dst_host,
                             unsigned short dst_port,
                             unsigned int timeout_spins);

#endif
