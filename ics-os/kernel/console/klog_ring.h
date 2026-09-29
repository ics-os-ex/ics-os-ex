/*
 * klog_ring.h — self-contained fixed-record ring buffer for the kernel log.
 *
 * This header is pure C with no kernel dependencies (standard integer types
 * only) so it can be compiled both inside the kernel unity build and on the
 * host by the unit test (tests/klog_unit.c). The kernel-facing layer lives in
 * console/klog.c.
 */
#ifndef KLOG_RING_H
#define KLOG_RING_H

/* Sized to fit under the 4 MiB user-ELF kernel ceiling: the ring is static BSS
 * (~10 KiB at these settings). Tune up only after freeing kernel memory. */
#define KLOG_LINE_MAX 96
#define KLOG_COUNT    96

typedef struct {
    unsigned int   tick;   /* ticks at the moment the record was written */
    unsigned char  level;  /* KLOG_* severity (0 = most severe) */
    unsigned short len;    /* bytes of text stored (excluding NUL) */
    char           text[KLOG_LINE_MAX];
} klog_rec_t;

typedef struct {
    klog_rec_t   recs[KLOG_COUNT];
    unsigned int head;   /* index of the next slot to overwrite */
    unsigned int count;  /* records currently stored, 0..KLOG_COUNT */
} klog_ring_t;

static void klog_ring_init(klog_ring_t *r)
{
    unsigned int i;
    for (i = 0; i < KLOG_COUNT; i++)
        r->recs[i].len = 0;
    r->head = 0;
    r->count = 0;
}

/* Append a NUL-terminated record. Text longer than KLOG_LINE_MAX-1 is
 * truncated. Returns the number of text bytes stored. */
static int klog_ring_push(klog_ring_t *r, unsigned int tick,
                          unsigned char level, const char *text)
{
    unsigned int len = 0;
    klog_rec_t *rec;
    while (text[len] != 0 && len < KLOG_LINE_MAX - 1)
        len++;
    rec = &r->recs[r->head];
    rec->tick = tick;
    rec->level = level;
    rec->len = (unsigned short)len;
    while (len > 0) {
        len--;
        rec->text[len] = text[len];
    }
    rec->text[rec->len] = 0;
    r->head = (r->head + 1) % KLOG_COUNT;
    if (r->count < KLOG_COUNT)
        r->count++;
    return (int)rec->len;
}

/* 0-based slot of the oldest stored record, or -1 if the ring is empty. */
static int klog_ring_oldest(const klog_ring_t *r)
{
    if (r->count == 0)
        return -1;
    return (int)((r->head + KLOG_COUNT - r->count) % KLOG_COUNT);
}

static const klog_rec_t *klog_ring_at(const klog_ring_t *r, unsigned int slot)
{
    return &r->recs[slot % KLOG_COUNT];
}

static unsigned int klog_ring_count(const klog_ring_t *r)
{
    return r->count;
}

static void klog_ring_clear(klog_ring_t *r)
{
    r->head = 0;
    r->count = 0;
}

#endif /* KLOG_RING_H */
