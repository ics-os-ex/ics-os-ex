#ifndef USB_ASIX_H
#define USB_ASIX_H

/*
  Host-testable AX88179/178A (ASIX) USB Gigabit NIC data-path logic.

  The NIC moves Ethernet frames over two bulk endpoints with a small
  per-transfer header. This header isolates the two pure, hardware-independent
  pieces so they can be unit-tested on the host and reused by the driver:

    * asix_tx_header()   builds the 8-byte TX header (frame length + the
                         enable-padding flag) that precedes each outbound frame.
    * asix_rx_iter_*()   splits a received "bundle" bulk transfer into the
                         individual Ethernet frames it carries, one frame per
                         call (no allocation, so the driver can process an
                         arbitrary number of frames).
    * asix_rx_parse()    convenience wrapper that drains the iterator into a
                         caller-supplied array; used by the unit test.

  Types are plain C (unsigned char/int) so the header compiles standalone in a
  host unit test. Constant names use the ASIX_ and USB_ASIX_ prefixes to avoid
  clashing with the other usb-*.h parsers when included together.
*/

#define USB_ASIX_TXHDR_SZ      8
#define ASIX_RX_MIN_LEN        (2 + 14)   /* 2 IP-align bytes + Ethernet header */
#define ASIX_RXHDR_CRC_ERR     (1u << 29)
#define ASIX_RXHDR_DROP_ERR    (1u << 31)

typedef struct {
    unsigned int off;   /* offset of the Ethernet frame (after 2 IP-align bytes) */
    unsigned int len;   /* frame length in bytes (on-wire len minus 2) */
} asix_rx_frame;

/*
  Fill the 8-byte AX TX header for a frame of `len` bytes, where the OUT
  endpoint max-packet size is `mps_out`. Bytes 0-3 (LE) are the frame length;
  bytes 4-7 (LE) carry 0x80008000 (enable padding) when the 8-byte header plus
  the frame lands exactly on an MPS boundary, else zero.
*/
static inline void asix_tx_header(unsigned int len, unsigned int mps_out,
                                  unsigned char hdr[8])
{
    hdr[0] = (unsigned char)(len & 0xFF);
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);
    if (mps_out && ((len + USB_ASIX_TXHDR_SZ) % mps_out) == 0) {
        hdr[4] = 0x00;
        hdr[5] = 0x80;
        hdr[6] = 0x00;
        hdr[7] = 0x80;
    } else {
        hdr[4] = 0;
        hdr[5] = 0;
        hdr[6] = 0;
        hdr[7] = 0;
    }
}

typedef struct {
    const unsigned char *buf;
    unsigned int got;
    unsigned int n;         /* entry count */
    unsigned int off;       /* offset of the entry array */
    unsigned int idx;       /* next entry index */
    unsigned int data_off;  /* next frame-data offset */
    int valid;
} asix_rx_iter;

/*
  An AX88179/178A RX bulk transfer is a bundle: frame data at the front, then a
  4-byte RX header per frame, and a trailing 4-byte bundle header whose low
  word is the entry count and high word is the offset of the entry array. Each
  entry's high 13 bits (16..28) are the on-wire frame length INCLUDING the 2
  leading IP-align bytes; bit 29 is a CRC error and bit 31 a drop error. A
  frame's on-wire length is rounded up to a multiple of 8.
*/
static inline void asix_rx_iter_init(asix_rx_iter *it,
                                     const unsigned char *buf, unsigned int got)
{
    unsigned int rx_hdr;
    it->buf = buf;
    it->got = got;
    it->n = 0;
    it->off = 0;
    it->idx = 0;
    it->data_off = 0;
    it->valid = 0;
    if (!buf || got < 4)
        return;
    rx_hdr = (unsigned int)buf[got - 4] |
             ((unsigned int)buf[got - 3] << 8) |
             ((unsigned int)buf[got - 2] << 16) |
             ((unsigned int)buf[got - 1] << 24);
    it->n = rx_hdr & 0xFFFF;
    it->off = rx_hdr >> 16;
    if (it->n == 0 || it->off + it->n * 4 > got - 4)
        return;
    it->valid = 1;
}

/*
  Return 1 and fill `frame` with the next extractable Ethernet frame, or 0
  when the bundle is exhausted. Skips dummy (len 0) entries and entries with a
  CRC/drop flag or a runt length (< 16 bytes) while still advancing the data
  offset. A frame whose data would run past the entry array is treated as
  corruption and ends the parse. frame.off/len index directly into `buf`.
*/
static inline int asix_rx_iter_next(asix_rx_iter *it, asix_rx_frame *frame)
{
    unsigned int e, pkt_len, padded;
    if (!it->valid)
        return 0;
    while (it->idx < it->n) {
        e = (unsigned int)it->buf[it->off + it->idx * 4] |
            ((unsigned int)it->buf[it->off + it->idx * 4 + 1] << 8) |
            ((unsigned int)it->buf[it->off + it->idx * 4 + 2] << 16) |
            ((unsigned int)it->buf[it->off + it->idx * 4 + 3] << 24);
        it->idx++;
        pkt_len = (e >> 16) & 0x1fff;
        padded = (pkt_len + 7) & 0xfff8u;
        if (pkt_len == 0)
            continue;
        if (it->data_off + padded > it->off)
            return 0;                      /* corrupt: data runs past metadata */
        if ((e & (ASIX_RXHDR_CRC_ERR | ASIX_RXHDR_DROP_ERR)) ||
            pkt_len < ASIX_RX_MIN_LEN) {
            it->data_off += padded;
            continue;
        }
        frame->off = it->data_off + 2;
        frame->len = pkt_len - 2;
        it->data_off += padded;
        return 1;
    }
    it->valid = 0;
    return 0;
}

/*
  Drain the bundle into `frames`; returns how many were written (0..max_frames).
*/
static inline int asix_rx_parse(const unsigned char *buf, unsigned int got,
                                asix_rx_frame *frames, int max_frames)
{
    asix_rx_iter it;
    asix_rx_frame f;
    int count = 0;
    if (!frames || max_frames < 1)
        return 0;
    asix_rx_iter_init(&it, buf, got);
    while (count < max_frames && asix_rx_iter_next(&it, &f))
        frames[count++] = f;
    return count;
}

#endif
