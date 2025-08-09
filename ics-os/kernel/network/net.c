// Minimal network stack implementation.
// NOTE: This is a very small, synchronous handler intended only to bring up
// the rtl8139 and allow basic ICMP echo reply / ARP handling.

#include "../dextypes.h"
#include "net.h"
#include "../hardware/rtl8139/rtl8139.h"
#include "../debug/klog.h"
#include "../dexapi/dex32API.h"  // for api_addsystemcall prototype

// Utilities already available elsewhere:
// memcpy, memset, printf

// Forward static prototypes (used before their definitions below)
static void net_send_arp_request(uint32_t target_ip);
static void send_icmp_echo_request(uint32_t dst_ip, uint8_t *dst_mac);

uint8_t net_mac_addr[6] = {0};
// 10.0.2.15 (QEMU user default guest IP typical example)
uint32_t net_ip_addr     = (10u<<24) | (0u<<16) | (2u<<8) | 15u;
uint32_t net_ip_gateway  = (10u<<24) | (0u<<16) | (2u<<8) | 2u;  // 10.0.2.2
uint32_t net_ip_netmask  = (255u<<24)|(255u<<16)|(255u<<8)|0u;    // 255.255.255.0

// Minimal ARP cache entry for gateway (enough to send one ICMP echo request)
static uint8_t net_gateway_mac[6];
static int net_gateway_mac_valid = 0;

// ICMP echo tracking
static uint16_t icmp_ident = 0x1234;
static uint16_t icmp_seq   = 0;
static int icmp_gateway_echo_sent = 0; // send once automatically after ARP resolution

// --- User space event queue (very small ring) ---
struct net_event_rec {
    uint16_t type;   // NET_EVENT_*
    uint16_t len;    // bytes of data[] used
    uint8_t  data[64]; // small payload snippet (truncated if larger)
};

#define NET_EVENT_QUEUE_SIZE 32
static struct net_event_rec net_event_q[NET_EVENT_QUEUE_SIZE];
static volatile uint16_t net_event_head = 0; // write position
static volatile uint16_t net_event_tail = 0; // read position

void net_user_event(uint16_t type, const uint8_t *data, uint16_t len){
    if(len > 64) len = 64; // truncate
    uint16_t next = (net_event_head + 1) % NET_EVENT_QUEUE_SIZE;
    if(next == net_event_tail){
        // queue full, drop (could count drops later)
        return;
    }
    net_event_q[net_event_head].type = type;
    net_event_q[net_event_head].len = len;
    if(len && data) memcpy(net_event_q[net_event_head].data, data, len);
    net_event_head = next;
}

static int net_user_event_dequeue(struct net_event_rec *out){
    if(net_event_tail == net_event_head) return 0; // empty
    *out = net_event_q[net_event_tail];
    net_event_tail = (net_event_tail + 1) % NET_EVENT_QUEUE_SIZE;
    return 1;
}

// --- Syscall implementations ---
// Simple info structure
struct net_info; // forward (defined in header)

// function numbers (choose unused high range) 0xA8-0xAB
#define SYSCALL_NET_INFO 0xA8
#define SYSCALL_NET_RECV 0xA9
#define SYSCALL_NET_SEND 0xAA
#define SYSCALL_NET_CFG  0xAB

static DWORD sys_net_info(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e){
    (void)a;(void)b;(void)c;(void)d;(void)e;
    struct net_info *ni = (struct net_info*)a; // user buffer
    if(!ni) return (DWORD)-1;
    ni->ip = net_ip_addr;
    ni->gateway = net_ip_gateway;
    ni->netmask = net_ip_netmask;
    memcpy(ni->mac, net_mac_addr, 6);
    if(net_gateway_mac_valid) memcpy(ni->gateway_mac, net_gateway_mac, 6); else memset(ni->gateway_mac,0,6);
    ni->flags = net_gateway_mac_valid ? 1u : 0u;
    return 0;
}

static DWORD sys_net_recv(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e){
    (void)b;(void)c;(void)d;(void)e;
    struct net_event_rec *uer = (struct net_event_rec*)a; // user supplied buffer
    if(!uer) return (DWORD)-1;
    struct net_event_rec tmp;
    if(!net_user_event_dequeue(&tmp)) return 0; // no event
    *uer = tmp; // copy out
    return 1; // one event returned
}

// sys_net_send: a=op, op=1 send additional gateway ICMP echoes (b=count)
static DWORD sys_net_send(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e){
    (void)c;(void)d;(void)e;
    if(a==1){
        unsigned cnt = b?b:1;
        if(!net_gateway_mac_valid) return (DWORD)-2;
        while(cnt--) send_icmp_echo_request(net_ip_gateway, net_gateway_mac);
        return 0;
    }
    return (DWORD)-1;
}

struct net_cfg_req { uint32_t ip, gateway, netmask; };
static DWORD sys_net_cfg(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e){
    (void)b;(void)c;(void)d;(void)e;
    struct net_cfg_req *cfg = (struct net_cfg_req*)a;
    if(!cfg) return (DWORD)-1;
    net_ip_addr = cfg->ip;
    net_ip_gateway = cfg->gateway;
    net_ip_netmask = cfg->netmask;
    net_gateway_mac_valid = 0;
    icmp_gateway_echo_sent = 0;
    net_send_arp_request(net_ip_addr);
    net_send_arp_request(net_ip_gateway);
    klog_info(KLOG_SUBSYS_NETWORK, "Network config updated: IP %d.%d.%d.%d GW %d.%d.%d.%d",
        (net_ip_addr>>24)&0xFF,(net_ip_addr>>16)&0xFF,(net_ip_addr>>8)&0xFF, net_ip_addr&0xFF,
        (net_ip_gateway>>24)&0xFF,(net_ip_gateway>>16)&0xFF,(net_ip_gateway>>8)&0xFF, net_ip_gateway&0xFF);
    return 0;
}

void net_register_syscalls(){
    api_addsystemcall(SYSCALL_NET_INFO, sys_net_info, 0, 0);
    api_addsystemcall(SYSCALL_NET_RECV, sys_net_recv, 0, 0);
    api_addsystemcall(SYSCALL_NET_SEND, sys_net_send, 0, 0);
    api_addsystemcall(SYSCALL_NET_CFG,  sys_net_cfg,  0, 0);
    klog_info(KLOG_SUBSYS_NETWORK, "Network syscalls registered (info=0x%X recv=0x%X send=0x%X cfg=0x%X)", SYSCALL_NET_INFO, SYSCALL_NET_RECV, SYSCALL_NET_SEND, SYSCALL_NET_CFG);
}

static uint16_t net_htons(uint16_t v){ return (v>>8) | (v<<8); }
static uint32_t net_htonl(uint32_t v){ return ((v>>24)&0xff) | ((v>>8)&0xff00) | ((v<<8)&0xff0000) | ((v<<24)&0xff000000); }

static uint16_t checksum16(const void *data, int len){
    const uint16_t *w = (const uint16_t*)data;
    uint32_t sum = 0;
    while(len > 1){ sum += *w++; len -= 2; }
    if(len) sum += *(const uint8_t*)w;
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void net_dump_mac(const uint8_t *m){
    printf("%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
}

void net_set_mac(const uint8_t *mac){
    memcpy(net_mac_addr, mac, 6);
}

// (net_send_arp_request declared later after struct arp_hdr definition)

// Frame building helper
static void net_send_frame(uint8_t *dest, uint16_t eth_type, const void *payload, uint16_t payload_len){
    // Build a simple contiguous buffer: Ethernet header (14) + payload
    uint16_t frame_len = 14 + payload_len;
    // For now allocate on stack (small packets only).
    uint8_t buf[1514];
    if(frame_len > sizeof(buf)) return; // drop
    memcpy(buf, dest, 6);
    memcpy(buf+6, net_mac_addr, 6);
    *(uint16_t*)(buf+12) = net_htons(eth_type);
    memcpy(buf+14, payload, payload_len);
    rtl8139_send_packet(buf, frame_len);
}

// ARP structures
struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa; // sender protocol (IP) address
    uint8_t  tha[6];
    uint32_t tpa; // target protocol (IP) address
} __attribute__((packed));

// Build and send an ARP request (who-has target_ip tell our IP)
static void net_send_arp_request(uint32_t target_ip){
    struct arp_hdr req;
    req.htype = net_htons(1);
    req.ptype = net_htons(NET_ETH_TYPE_IP);
    req.hlen = 6; req.plen = 4;
    req.oper = net_htons(1); // request
    memcpy(req.sha, net_mac_addr, 6);
    req.spa = net_htonl(net_ip_addr);
    memset(req.tha, 0x00, 6);
    req.tpa = net_htonl(target_ip);
    uint8_t bcast[6]; memset(bcast, 0xFF, 6);
    klog_info(KLOG_SUBSYS_NETWORK, "Sending ARP request for %d.%d.%d.%d", (target_ip>>24)&0xFF, (target_ip>>16)&0xFF, (target_ip>>8)&0xFF, target_ip & 0xFF);
    net_send_frame(bcast, NET_ETH_TYPE_ARP, &req, sizeof(req));
}

// IPv4 + ICMP
struct ipv4_hdr {
    uint8_t  ver_ihl; // version(4) + IHL(4)
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t hdr_checksum;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

struct icmp_echo {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t ident;
    uint16_t seq;
    uint8_t data[32];
} __attribute__((packed));

// --- Minimal TCP support (single passive listening port with echo) ---
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t  offset_reserved; // upper 4 bits: data offset
    uint8_t  flags;           // SYN=0x02, ACK=0x10, FIN=0x01, PSH=0x08
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));

static uint32_t tcp_listen_port = 1234; // host order
static int tcp_conn_established = 0;
static uint32_t tcp_iss = 0x1000;  // initial seq we send
static uint32_t tcp_peer_seq = 0;  // next seq expected from peer
static uint32_t tcp_my_seq = 0;    // next seq we will use

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *segment, uint16_t len){
    struct pseudo {
        uint32_t s;
        uint32_t d;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t l;
    } __attribute__((packed)) ph;
    ph.s = src_ip; ph.d = dst_ip; ph.zero=0; ph.proto=6; ph.l = net_htons(len);
    uint32_t sum = 0;
    const uint16_t *w = (const uint16_t*)&ph;
    for(unsigned i=0;i<sizeof(ph)/2;i++) sum += *w++;
    w = (const uint16_t*)segment;
    unsigned l2 = len;
    while(l2 > 1){ sum += *w++; l2 -= 2; }
    if(l2) sum += *(const uint8_t*)w;
    while(sum>>16) sum = (sum & 0xFFFF) + (sum>>16);
    return (uint16_t)(~sum);
}

static void tcp_send(uint32_t dst_ip, uint8_t *dst_mac, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack_seq, uint8_t flags, const uint8_t *payload, uint16_t pay_len){
    struct ipv4_hdr ip_out;
    struct tcp_hdr  th;
    uint16_t tcp_len = sizeof(th) + pay_len;
    ip_out.ver_ihl = 0x45;
    ip_out.tos = 0;
    ip_out.tot_len = net_htons(sizeof(ip_out) + tcp_len);
    ip_out.id = 0;
    ip_out.frag_off = 0;
    ip_out.ttl = 64;
    ip_out.proto = 6;
    ip_out.hdr_checksum = 0;
    ip_out.saddr = net_htonl(net_ip_addr);
    ip_out.daddr = dst_ip;
    ip_out.hdr_checksum = checksum16(&ip_out, sizeof(ip_out));
    th.src_port = net_htons(src_port);
    th.dst_port = net_htons(dst_port);
    th.seq = net_htonl(seq);
    th.ack_seq = net_htonl(ack_seq);
    th.offset_reserved = (5<<4);
    th.flags = flags;
    th.window = net_htons(1024);
    th.checksum = 0;
    th.urg_ptr = 0;
    uint8_t seg[sizeof(th)+512];
    if(pay_len > 512) pay_len = 512;
    memcpy(seg, &th, sizeof(th));
    if(pay_len) memcpy(seg+sizeof(th), payload, pay_len);
    th.checksum = tcp_checksum(ip_out.saddr, ip_out.daddr, seg, sizeof(th)+pay_len);
    memcpy(seg, &th, sizeof(th));
    uint8_t frame[sizeof(ip_out)+sizeof(th)+512];
    memcpy(frame, &ip_out, sizeof(ip_out));
    memcpy(frame+sizeof(ip_out), seg, sizeof(th)+pay_len);
    net_send_frame(dst_mac, NET_ETH_TYPE_IP, frame, sizeof(ip_out)+sizeof(th)+pay_len);
}

static void handle_tcp(uint8_t *pkt, unsigned len, struct ipv4_hdr *ip, uint8_t *src_mac){
    if(len < sizeof(struct tcp_hdr)) return;
    struct tcp_hdr *th = (struct tcp_hdr*)pkt;
    uint16_t dst_port = net_htons(th->dst_port);
    uint16_t src_port = net_htons(th->src_port);
    uint8_t  hdr_len = ((th->offset_reserved >> 4) & 0xF) * 4;
    if(hdr_len < sizeof(struct tcp_hdr) || len < hdr_len) return;
    uint8_t *payload = pkt + hdr_len;
    unsigned pay_len = len - hdr_len;
    if(dst_port != tcp_listen_port && !tcp_conn_established){
        // Send RST
        tcp_send(ip->saddr, src_mac, dst_port, src_port, 0, net_htonl(th->seq)+1, 0x04, 0, 0);
        return;
    }
    if(!tcp_conn_established){
        if(th->flags & 0x02){ // SYN
            tcp_peer_seq = net_htonl(th->seq) + 1;
            tcp_my_seq = tcp_iss;
            tcp_send(ip->saddr, src_mac, tcp_listen_port, src_port, tcp_my_seq, tcp_peer_seq, 0x12, 0, 0); // SYN+ACK
            tcp_my_seq++;
        }
        if((th->flags & 0x10) && net_htonl(th->ack_seq) == tcp_my_seq){
            tcp_conn_established = 1;
            klog_info(KLOG_SUBSYS_NETWORK, "TCP connection established (port %u)", tcp_listen_port);
            // Emit TCP_ESTABLISHED event: data = peer_ip(4) + src_port(2) + dst_port(2)
            uint8_t ev[8];
            uint32_t host_ip = net_htonl(ip->saddr);
            ev[0]=(host_ip>>24)&0xFF; ev[1]=(host_ip>>16)&0xFF; ev[2]=(host_ip>>8)&0xFF; ev[3]=host_ip&0xFF;
            ev[4]=(uint8_t)(src_port>>8); ev[5]=(uint8_t)(src_port & 0xFF);
            ev[6]=(uint8_t)(tcp_listen_port>>8); ev[7]=(uint8_t)(tcp_listen_port & 0xFF);
            net_user_event(NET_EVENT_TCP_ESTABLISHED, ev, sizeof(ev));
        }
        return;
    }
    // Established
    if(th->flags & 0x01){ // FIN
        tcp_peer_seq = net_htonl(th->seq) + 1;
        tcp_send(ip->saddr, src_mac, tcp_listen_port, src_port, tcp_my_seq, tcp_peer_seq, 0x11, 0, 0); // FIN+ACK
        tcp_my_seq++;
        tcp_conn_established = 0;
    klog_info(KLOG_SUBSYS_NETWORK, "TCP connection closed");
        uint8_t ev2[8];
        uint32_t host_ip2 = net_htonl(ip->saddr);
        ev2[0]=(host_ip2>>24)&0xFF; ev2[1]=(host_ip2>>16)&0xFF; ev2[2]=(host_ip2>>8)&0xFF; ev2[3]=host_ip2&0xFF;
        ev2[4]=(uint8_t)(src_port>>8); ev2[5]=(uint8_t)(src_port & 0xFF);
        ev2[6]=(uint8_t)(tcp_listen_port>>8); ev2[7]=(uint8_t)(tcp_listen_port & 0xFF);
        net_user_event(NET_EVENT_TCP_CLOSED, ev2, sizeof(ev2));
        return;
    }
    if(pay_len){
        tcp_peer_seq = net_htonl(th->seq) + pay_len;
        // Emit TCP_DATA event: data = pay_len(4) + peer_ip(4) + first bytes up to 56
        uint8_t evd[64];
        evd[0]=(uint8_t)(pay_len>>24); evd[1]=(uint8_t)(pay_len>>16); evd[2]=(uint8_t)(pay_len>>8); evd[3]=(uint8_t)pay_len;
        uint32_t host_ipd = net_htonl(ip->saddr);
        evd[4]=(host_ipd>>24)&0xFF; evd[5]=(host_ipd>>16)&0xFF; evd[6]=(host_ipd>>8)&0xFF; evd[7]=host_ipd&0xFF;
        unsigned copy = pay_len; if(copy>56) copy=56; if(copy) memcpy(evd+8, payload, copy);
        net_user_event(NET_EVENT_TCP_DATA, evd, 8+copy);
        // Echo payload back (PSH+ACK)
        tcp_send(ip->saddr, src_mac, tcp_listen_port, src_port, tcp_my_seq, tcp_peer_seq, 0x18, payload, pay_len);
        tcp_my_seq += pay_len;
    } else if(th->flags & 0x10){
        // Pure ACK ignore
    }
}

static void handle_arp(uint8_t *frame, unsigned len){
    if(len < sizeof(struct arp_hdr)) return;
    struct arp_hdr *arp = (struct arp_hdr*)frame;
    if(arp->htype != net_htons(1) || arp->ptype != net_htons(NET_ETH_TYPE_IP) || arp->hlen != 6 || arp->plen != 4) return;
    uint16_t op = net_htons(arp->oper);
    if(op == 1){ // request
        if(arp->tpa == net_htonl(net_ip_addr)){
            klog_info(KLOG_SUBSYS_NETWORK, "ARP request for our IP, sending reply");
            // Build reply
            struct arp_hdr reply;
            reply.htype = net_htons(1);
            reply.ptype = net_htons(NET_ETH_TYPE_IP);
            reply.hlen = 6; reply.plen = 4;
            reply.oper = net_htons(2);
            memcpy(reply.sha, net_mac_addr, 6);
            reply.spa = net_htonl(net_ip_addr);
            memcpy(reply.tha, arp->sha, 6);
            reply.tpa = arp->spa;
            net_send_frame(arp->sha, NET_ETH_TYPE_ARP, &reply, sizeof(reply));
            klog_info(KLOG_SUBSYS_NETWORK, "ARP reply sent");
        }
        // Enqueue ARP request event (store sender MAC + IP)
        uint8_t ev[10];
        memcpy(ev, arp->sha, 6);
        memcpy(ev+6, &arp->spa, 4);
        net_user_event(NET_EVENT_ARP_REQUEST, ev, sizeof(ev));
    } else if(op == 2) { // reply
        // Cache gateway MAC if this is from gateway
        if(arp->spa == net_htonl(net_ip_gateway) && !net_gateway_mac_valid) {
            memcpy(net_gateway_mac, arp->sha, 6);
            net_gateway_mac_valid = 1;
            klog_info(KLOG_SUBSYS_NETWORK, "Cached gateway MAC %02x:%02x:%02x:%02x:%02x:%02x", net_gateway_mac[0],net_gateway_mac[1],net_gateway_mac[2],net_gateway_mac[3],net_gateway_mac[4],net_gateway_mac[5]);
            net_user_event(NET_EVENT_ARP_REPLY, arp->sha, 6);
        }
    }
}

static void send_icmp_echo_reply(struct ipv4_hdr *ip, struct icmp_echo *echo, uint8_t *src_mac){
    // swap src/dst
    struct ipv4_hdr ip_out;
    struct icmp_echo echo_out;
    memcpy(&echo_out, echo, sizeof(struct icmp_echo));
    echo_out.type = 0; // echo reply
    echo_out.csum = 0;
    echo_out.csum = checksum16(&echo_out, sizeof(struct icmp_echo));
    ip_out.ver_ihl = 0x45; // v4, IHL=5
    ip_out.tos = 0;
    ip_out.tot_len = net_htons(sizeof(struct ipv4_hdr)+sizeof(struct icmp_echo));
    ip_out.id = 0;
    ip_out.frag_off = 0;
    ip_out.ttl = 64;
    ip_out.proto = 1; // ICMP
    ip_out.hdr_checksum = 0;
    ip_out.saddr = net_htonl(net_ip_addr);
    ip_out.daddr = ip->saddr;
    ip_out.hdr_checksum = checksum16(&ip_out, sizeof(struct ipv4_hdr));

    uint8_t payload[sizeof(struct ipv4_hdr)+sizeof(struct icmp_echo)];
    memcpy(payload, &ip_out, sizeof(ip_out));
    memcpy(payload+sizeof(ip_out), &echo_out, sizeof(echo_out));
    net_send_frame(src_mac, NET_ETH_TYPE_IP, payload, sizeof(payload));
}

// Minimal outbound ICMP echo request (to already-known MAC)
static void send_icmp_echo_request(uint32_t dst_ip, uint8_t *dst_mac){
    struct ipv4_hdr ip_out;
    struct icmp_echo echo;
    // Prepare ICMP echo
    echo.type = 8; // request
    echo.code = 0;
    echo.ident = net_htons(icmp_ident);
    echo.seq   = net_htons(icmp_seq++);
    // Simple payload pattern
    for(int i=0;i<sizeof(echo.data);i++) echo.data[i] = (uint8_t)i;
    echo.csum = 0;
    echo.csum = checksum16(&echo, sizeof(echo));
    // IPv4 header
    ip_out.ver_ihl = 0x45;
    ip_out.tos = 0;
    ip_out.tot_len = net_htons(sizeof(ip_out) + sizeof(echo));
    ip_out.id = 0;
    ip_out.frag_off = 0;
    ip_out.ttl = 64;
    ip_out.proto = 1; // ICMP
    ip_out.hdr_checksum = 0;
    ip_out.saddr = net_htonl(net_ip_addr);
    ip_out.daddr = net_htonl(dst_ip);
    ip_out.hdr_checksum = checksum16(&ip_out, sizeof(ip_out));
    uint8_t payload[sizeof(ip_out)+sizeof(echo)];
    memcpy(payload, &ip_out, sizeof(ip_out));
    memcpy(payload+sizeof(ip_out), &echo, sizeof(echo));
    net_send_frame(dst_mac, NET_ETH_TYPE_IP, payload, sizeof(payload));
    klog_info(KLOG_SUBSYS_NETWORK, "ICMP echo request sent seq=%u ident=0x%04x", icmp_seq-1, icmp_ident);
}

static void handle_ipv4(uint8_t *frame, unsigned len, uint8_t *src_mac){
    if(len < sizeof(struct ipv4_hdr)) return;
    struct ipv4_hdr *ip = (struct ipv4_hdr*)frame;
    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if((ip->ver_ihl>>4) != 4 || ihl < 20) return;
    if(len < ihl) return;
    if(ip->daddr != net_htonl(net_ip_addr)) return; // not for us
    
    klog_debug(KLOG_SUBSYS_NETWORK, "IPv4 packet received, protocol=%d", ip->proto);
    
    if(ip->proto == 1){ // ICMP
        if(len < ihl + sizeof(struct icmp_echo)) return;
        struct icmp_echo *echo = (struct icmp_echo*)((uint8_t*)ip + ihl);
        if(echo->type == 8){ // echo request
            klog_info(KLOG_SUBSYS_NETWORK, "ICMP ping request received, sending reply");
            send_icmp_echo_reply(ip, echo, src_mac);
            klog_info(KLOG_SUBSYS_NETWORK, "ICMP echo reply sent");
            net_user_event(NET_EVENT_ICMP_ECHO_REQ, (uint8_t*)echo, sizeof(struct icmp_echo));
        } else if(echo->type == 0) { // echo reply
            klog_info(KLOG_SUBSYS_NETWORK, "ICMP echo reply received (ident=0x%04x seq=%u)", net_htons(echo->ident), net_htons(echo->seq));
            net_user_event(NET_EVENT_ICMP_ECHO_REP, (uint8_t*)echo, sizeof(struct icmp_echo));
        }
    } else if(ip->proto == 6){ // TCP
        klog_debug(KLOG_SUBSYS_NETWORK, "TCP packet received");
        handle_tcp(((uint8_t*)ip)+ihl, len - ihl, ip, src_mac);
    }
}

void ethernet_handle_packet(uint8_t *data, unsigned len){
    if(len < 14) return;
    uint8_t *dest = data;
    uint8_t *src  = data + 6;
    uint16_t eth_type = (data[12] << 8) | data[13];
    uint8_t *payload = data + 14;
    unsigned payload_len = (len >= 14)? (len - 14) : 0;
    (void)dest; // currently unused, but retained for future filtering

    klog_debug(KLOG_SUBSYS_NETWORK, "Ethernet frame received, type=0x%04x, length=%d", eth_type, len);

    switch(eth_type){
        case NET_ETH_TYPE_ARP:
            klog_debug(KLOG_SUBSYS_NETWORK, "Processing ARP packet");
            handle_arp(payload, payload_len);
            break;
        case NET_ETH_TYPE_IP:
            klog_debug(KLOG_SUBSYS_NETWORK, "Processing IPv4 packet");
            handle_ipv4(payload, payload_len, src);
            break;
        default:
            klog_debug(KLOG_SUBSYS_NETWORK, "Unknown ethernet type 0x%04x, ignoring", eth_type);
            break; // ignored
    }
}

void net_init(){
    uint8_t mac[6];
    get_mac_addr(mac);
    net_set_mac(mac);
    
    klog_info(KLOG_SUBSYS_NETWORK, "Network stack initialized");
    klog_info(KLOG_SUBSYS_NETWORK, "MAC Address: %02x:%02x:%02x:%02x:%02x:%02x", 
              net_mac_addr[0], net_mac_addr[1], net_mac_addr[2], 
              net_mac_addr[3], net_mac_addr[4], net_mac_addr[5]);
    klog_info(KLOG_SUBSYS_NETWORK, "IP Address: %d.%d.%d.%d", 
              (net_ip_addr>>24)&0xFF, (net_ip_addr>>16)&0xFF, 
              (net_ip_addr>>8)&0xFF, net_ip_addr & 0xFF);
    klog_info(KLOG_SUBSYS_NETWORK, "Gateway: %d.%d.%d.%d", 
              (net_ip_gateway>>24)&0xFF, (net_ip_gateway>>16)&0xFF, 
              (net_ip_gateway>>8)&0xFF, net_ip_gateway & 0xFF);
    klog_info(KLOG_SUBSYS_NETWORK, "Netmask: %d.%d.%d.%d", 
              (net_ip_netmask>>24)&0xFF, (net_ip_netmask>>16)&0xFF, 
              (net_ip_netmask>>8)&0xFF, net_ip_netmask & 0xFF);
              
    printf("Network MAC: ");
    net_dump_mac(net_mac_addr); printf("\n");
    printf("IP Address: %d.%d.%d.%d\n", (net_ip_addr>>24)&0xFF,(net_ip_addr>>16)&0xFF,(net_ip_addr>>8)&0xFF, net_ip_addr & 0xFF);

    // Proactively announce ourselves and query gateway to exercise TX early.
    net_send_arp_request(net_ip_addr);      // Gratuitous (target = self)
    net_send_arp_request(net_ip_gateway);   // Query gateway
}

void net_periodic(){
    // Poll NIC if needed (e.g., IRQ fallback)
    rtl8139_periodic();
    // If we have gateway MAC but haven’t sent an echo yet, send one
    if(net_gateway_mac_valid && !icmp_gateway_echo_sent) {
        send_icmp_echo_request(net_ip_gateway, net_gateway_mac);
        icmp_gateway_echo_sent = 1;
    }
}
