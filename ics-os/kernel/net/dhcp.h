#ifndef ICSOS_NET_DHCP_H
#define ICSOS_NET_DHCP_H

#include "netif.h"
#include "inet_config.h"

#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67

#define DHCP_BOOTREQUEST  1
#define DHCP_BOOTREPLY    2

#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_ACK          5
#define DHCP_NAK          6

#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_REQ_IP        50
#define DHCP_OPT_LEASE_TIME    51
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_REQ     55
#define DHCP_OPT_END           255

#define DHCP_MAGIC_0  99
#define DHCP_MAGIC_1  130
#define DHCP_MAGIC_2  83
#define DHCP_MAGIC_3  99

#define DHCP_FIXED_LEN  236
#define DHCP_MAX_OPTS   64
#define DHCP_MIN_PKT    (DHCP_FIXED_LEN + 4)

struct dhcp_msg {
    unsigned char op;
    unsigned char htype;
    unsigned char hlen;
    unsigned char hops;
    unsigned int  xid;
    unsigned short secs;
    unsigned short flags;
    unsigned int  ciaddr;
    unsigned int  yiaddr;
    unsigned int  siaddr;
    unsigned int  giaddr;
    unsigned char chaddr[16];
    unsigned char sname[64];
    unsigned char file[128];
    /* magic + options follow */
} __attribute__((packed));

/* Host-order lease result. */
struct dhcp_lease {
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int server;
    unsigned int lease_secs;
};

/* Pure helpers (unit-testable). */
unsigned int dhcp_build_discover(unsigned char *dst, unsigned int dst_max,
                                 unsigned int xid,
                                 const unsigned char mac[6]);
unsigned int dhcp_build_request(unsigned char *dst, unsigned int dst_max,
                                unsigned int xid,
                                const unsigned char mac[6],
                                unsigned int req_ip_host,
                                unsigned int server_host);
/* RENEWING: ciaddr set, unicast; no requested-IP / server-id options. */
unsigned int dhcp_build_renew(unsigned char *dst, unsigned int dst_max,
                              unsigned int xid,
                              const unsigned char mac[6],
                              unsigned int ciaddr_host);
/* REBINDING: ciaddr set, broadcast flag; no requested-IP / server-id. */
unsigned int dhcp_build_rebind(unsigned char *dst, unsigned int dst_max,
                               unsigned int xid,
                               const unsigned char mac[6],
                               unsigned int ciaddr_host);

int dhcp_parse_reply(const unsigned char *pkt, unsigned int len,
                     unsigned int expect_xid,
                     unsigned char *msg_type_out,
                     struct dhcp_lease *lease);

/* Deliver inbound UDP/68 while a client wait is outstanding. */
int dhcp_udp_deliver(unsigned short dport, const unsigned char *payload,
                     unsigned int plen);

/* Lease FSM states (RFC 2131). */
#define DHCP_ST_INIT       0
#define DHCP_ST_BOUND      1
#define DHCP_ST_RENEWING   2
#define DHCP_ST_REBINDING  3

/* Pure helpers for T1/T2 (unit-testable). Defaults when lease_secs==0. */
unsigned int dhcp_t1_secs(unsigned int lease_secs);
unsigned int dhcp_t2_secs(unsigned int lease_secs);

/* Run DORA; fills cfg on success. Returns 0 on ACK. */
int dhcp_client(struct netif *nif, struct inet_config *cfg,
                unsigned int timeout_spins);
/* Unicast renew to the leasing server (RFC 2131 RENEWING). */
int dhcp_renew(struct netif *nif, struct inet_config *cfg,
               unsigned int timeout_spins);
/* Broadcast rebind (RFC 2131 REBINDING). */
int dhcp_rebind(struct netif *nif, struct inet_config *cfg,
                unsigned int timeout_spins);

/* Arm T1/T2 from now_secs + lease; used after ACK and by tests. */
void dhcp_arm_timers(unsigned int now_secs, unsigned int lease_secs);
/* Advance FSM using wall-clock seconds; may call renew/rebind. */
int dhcp_service(struct netif *nif, struct inet_config *cfg,
                 unsigned int now_secs, unsigned int timeout_spins);
/* Test hook: force T1/T2 into the past relative to now_secs. */
void dhcp_force_timer_due(unsigned int now_secs, int past_t2);
int dhcp_lease_state(void);

#endif
