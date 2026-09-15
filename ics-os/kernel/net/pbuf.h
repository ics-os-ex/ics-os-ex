#ifndef ICSOS_NET_PBUF_H
#define ICSOS_NET_PBUF_H

#include "../types.h"
#include "ethernet.h"

/* Room for Ethernet frame; virtio-net prepends its own hdr in driver buffers. */
#define PBUF_SIZE        ETH_MAX_FRAME
#define PBUF_POOL_COUNT  32

struct pbuf {
    struct pbuf *next;
    u16 len;
    u8  ref;
    u8  flags;
    u8  data[PBUF_SIZE];
};

void pbuf_pool_init(void);
struct pbuf *pbuf_alloc(u16 len);
void pbuf_free(struct pbuf *p);
struct pbuf *pbuf_clone(const struct pbuf *src);

#endif
