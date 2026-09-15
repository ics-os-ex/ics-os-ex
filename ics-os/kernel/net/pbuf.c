#include "pbuf.h"

extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

static struct pbuf pbuf_pool[PBUF_POOL_COUNT];
static struct pbuf *pbuf_free_list;
static int pbuf_inited;

void pbuf_pool_init(void)
{
    int i;

    if (pbuf_inited)
        return;
    pbuf_free_list = 0;
    for (i = 0; i < PBUF_POOL_COUNT; i++) {
        pbuf_pool[i].next = pbuf_free_list;
        pbuf_pool[i].ref = 0;
        pbuf_pool[i].len = 0;
        pbuf_pool[i].flags = 0;
        pbuf_free_list = &pbuf_pool[i];
    }
    pbuf_inited = 1;
}

struct pbuf *pbuf_alloc(u16 len)
{
    struct pbuf *p;

    if (!pbuf_inited)
        pbuf_pool_init();
    if (len > PBUF_SIZE)
        return 0;
    if (!pbuf_free_list)
        return 0;
    p = pbuf_free_list;
    pbuf_free_list = p->next;
    p->next = 0;
    p->len = len;
    p->ref = 1;
    p->flags = 0;
    return p;
}

void pbuf_free(struct pbuf *p)
{
    if (!p)
        return;
    if (p->ref > 1) {
        p->ref--;
        return;
    }
    p->ref = 0;
    p->len = 0;
    p->next = pbuf_free_list;
    pbuf_free_list = p;
}

struct pbuf *pbuf_clone(const struct pbuf *src)
{
    struct pbuf *p;

    if (!src)
        return 0;
    p = pbuf_alloc(src->len);
    if (!p)
        return 0;
    memcpy(p->data, src->data, src->len);
    return p;
}
