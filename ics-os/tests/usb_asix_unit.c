/*
  Host TAP for the AX88179/178A (ASIX) USB Gigabit NIC data-path logic in
  kernel/hardware/usb/usb_asix.h.

  Two pure pieces are exercised, both of which the driver reuses verbatim so
  the unit test validates exactly the code that moves real frames:

    * asix_tx_header()  the 8-byte TX header (frame length + the enable-padding
                        flag when the 8-byte header + frame lands on an OUT-ep
                        MPS boundary).
    * asix_rx_parse()/asix_rx_iter_*()  splitting a received "bundle" bulk
                        transfer into the individual Ethernet frames it carries,
                        skipping dummy / CRC-error / drop / runt entries.

  The real-device gate (test-usb-net-asix) is blocked on a QEMU usb-host
  SuperSpeed vendor-control limitation, so this host test is the standing
  validation of the bulk RX/TX data path.
 */
#include <stdio.h>
#include <string.h>

#include "kernel/hardware/usb/usb_asix.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

/* One frame in a synthetic RX bundle. */
typedef struct {
    int len;            /* Ethernet frame length (on-wire len minus 2) */
    unsigned int flags; /* error bits: bit 29 = CRC, bit 31 = drop */
} frame_spec;

/*
  Lay out `n` frames as an AX88179 RX bundle in `buf`: each frame is 2 IP-align
  bytes + its Ethernet bytes + zero padding to a multiple of 8; then a 4-byte
  RX header per frame (on-wire length in bits 16..28 plus the error bits); then
  a trailing 4-byte bundle header (low word = count, high word = entry offset).
  Returns the total size (got).
*/
static int build_bundle(unsigned char *buf, const frame_spec *fs, int n)
{
    int data_off = 0, i, v;
    unsigned int off;

    for (i = 0; i < n; i++) {
        int pkt_len = fs[i].len + 2;
        int padded = (pkt_len + 7) & ~7;
        buf[data_off] = 0xAA;                       /* IP-align byte 0 */
        buf[data_off + 1] = 0xBB;                   /* IP-align byte 1 */
        memset(buf + data_off + 2, 0x40 + i, fs[i].len);
        memset(buf + data_off + 2 + fs[i].len, 0, padded - pkt_len);
        data_off += padded;
    }
    off = (unsigned int)data_off;
    for (i = 0; i < n; i++) {
        v = ((fs[i].len + 2) << 16) | (int)fs[i].flags;
        buf[off + i * 4 + 0] = (unsigned char)(v & 0xFF);
        buf[off + i * 4 + 1] = (unsigned char)((v >> 8) & 0xFF);
        buf[off + i * 4 + 2] = (unsigned char)((v >> 16) & 0xFF);
        buf[off + i * 4 + 3] = (unsigned char)((v >> 24) & 0xFF);
    }
    v = ((int)off << 16) | (n & 0xFFFF);
    buf[off + n * 4 + 0] = (unsigned char)(v & 0xFF);
    buf[off + n * 4 + 1] = (unsigned char)((v >> 8) & 0xFF);
    buf[off + n * 4 + 2] = (unsigned char)((v >> 16) & 0xFF);
    buf[off + n * 4 + 3] = (unsigned char)((v >> 24) & 0xFF);
    return (int)(off + n * 4 + 4);
}

int main(void)
{
    unsigned char buf[8192];
    unsigned char hdr[8];
    asix_rx_frame frames[32];
    int got, nf, ok = 1;
    frame_spec fs2[2];
    frame_spec fs3[3];
    frame_spec fs1[1];

    printf("TAP version 13\n1..24\n");

    /* ---------------- TX header: length + padding flag ---------------- */
    asix_tx_header(64, 1024, hdr);
    ok &= check("TX hdr len=64 mps=1024: len LE, no pad flag",
                hdr[0] == 0x40 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0 &&
                hdr[4] == 0 && hdr[5] == 0 && hdr[6] == 0 && hdr[7] == 0);
    asix_tx_header(1016, 1024, hdr);
    ok &= check("TX hdr len=1016 mps=1024: (1016+8)%1024==0 sets 0x80008000",
                hdr[0] == 0xF8 && hdr[1] == 0x03 && hdr[2] == 0 && hdr[3] == 0 &&
                hdr[4] == 0x00 && hdr[5] == 0x80 && hdr[6] == 0x00 &&
                hdr[7] == 0x80);
    asix_tx_header(2040, 1024, hdr);
    ok &= check("TX hdr len=2040 mps=1024: (2040+8)%1024==0 sets pad flag",
                hdr[0] == 0xF8 && hdr[1] == 0x07 && hdr[5] == 0x80 &&
                hdr[7] == 0x80);
    asix_tx_header(56, 64, hdr);
    ok &= check("TX hdr len=56 mps=64: (56+8)%64==0 sets pad flag",
                hdr[0] == 0x38 && hdr[4] == 0x00 && hdr[5] == 0x80 &&
                hdr[7] == 0x80);
    asix_tx_header(64, 64, hdr);
    ok &= check("TX hdr len=64 mps=64: (64+8)%64!=0, no pad flag",
                hdr[0] == 0x40 && hdr[4] == 0 && hdr[5] == 0 && hdr[7] == 0);
    asix_tx_header(0, 1024, hdr);
    ok &= check("TX hdr len=0: all zero, no pad flag",
                hdr[0] == 0 && hdr[3] == 0 && hdr[4] == 0 && hdr[7] == 0);
    asix_tx_header(64, 0, hdr);
    ok &= check("TX hdr mps=0: no div-by-zero, no pad flag",
                hdr[0] == 0x40 && hdr[4] == 0 && hdr[5] == 0 && hdr[7] == 0);
    asix_tx_header(1514, 1024, hdr);
    ok &= check("TX hdr len=1514 (max MTU) mps=1024: no pad flag",
                hdr[0] == 0xEA && hdr[1] == 0x05 && hdr[5] == 0 && hdr[7] == 0);

    /* ---------------- RX parse: two valid frames ---------------- */
    fs2[0].len = 60;  fs2[0].flags = 0;
    fs2[1].len = 100; fs2[1].flags = 0;
    got = build_bundle(buf, fs2, 2);
    nf = asix_rx_parse(buf, (unsigned int)got, frames, 32);
    ok &= check("RX two valid frames: got size", got == 180);
    ok &= check("RX two valid frames: count 2", nf == 2);
    ok &= check("RX two valid frames: frame0 off=2 len=60",
                nf >= 1 && frames[0].off == 2 && frames[0].len == 60);
    ok &= check("RX two valid frames: frame1 off=66 len=100",
                nf >= 2 && frames[1].off == 66 && frames[1].len == 100);
    ok &= check("RX two valid frames: frame0 payload intact (0x40..0x40)",
                nf >= 1 && buf[2] == 0x40 && buf[2 + 59] == 0x40);
    ok &= check("RX two valid frames: frame1 payload intact (0x41..0x41)",
                nf >= 2 && buf[66] == 0x41 && buf[66 + 99] == 0x41);

    /* ---------------- RX parse: CRC-error + runt are skipped ---------------- */
    fs3[0].len = 60;  fs3[0].flags = 0;
    fs3[1].len = 100; fs3[1].flags = ASIX_RXHDR_CRC_ERR;
    fs3[2].len = 10;  fs3[2].flags = 0;             /* runt (< 16 bytes) */
    got = build_bundle(buf, fs3, 3);
    nf = asix_rx_parse(buf, (unsigned int)got, frames, 32);
    ok &= check("RX CRC+runt: only the clean frame survives", nf == 1);
    ok &= check("RX CRC+runt: surviving frame off=2 len=60",
                nf >= 1 && frames[0].off == 2 && frames[0].len == 60);

    /* ---------------- RX parse: drop-error frame is skipped ---------------- */
    fs1[0].len = 96; fs1[0].flags = ASIX_RXHDR_DROP_ERR;
    got = build_bundle(buf, fs1, 1);
    nf = asix_rx_parse(buf, (unsigned int)got, frames, 32);
    ok &= check("RX drop-error frame: nothing delivered", nf == 0);

    /* ---------------- RX parse: dummy (len 0) entries are skipped ---------- */
    fs1[0].len = 0;  fs1[0].flags = 0;
    got = build_bundle(buf, fs1, 1);
    nf = asix_rx_parse(buf, (unsigned int)got, frames, 32);
    ok &= check("RX dummy entry (len 0): nothing delivered", nf == 0);

    /* ---------------- RX parse: bounds / degenerate inputs ---------------- */
    ok &= check("RX NULL buffer: 0 frames",
                asix_rx_parse(0, 180, frames, 32) == 0);
    ok &= check("RX NULL out array: 0 frames",
                asix_rx_parse(buf, (unsigned int)got, 0, 32) == 0);
    ok &= check("RX got < 4 (no bundle header): 0 frames",
                asix_rx_parse(buf, 3, frames, 32) == 0);
    ok &= check("RX empty bundle (count 0): 0 frames",
                asix_rx_parse(buf, (unsigned int)build_bundle(buf, fs1, 0),
                              frames, 32) == 0);
    /* Truncated: a bundle claiming a valid header but cut short must not
       run past the metadata; a corrupt entry offset that overruns returns 0. */
    fs1[0].len = 60; fs1[0].flags = 0;
    got = build_bundle(buf, fs1, 1);
    ok &= check("RX truncated (got cut to 8): 0 frames, no OOB",
                asix_rx_parse(buf, 8, frames, 32) == 0);

    /* max_frames caps the count but never overruns the caller array */
    fs3[0].len = 60;  fs3[0].flags = 0;
    fs3[1].len = 80;  fs3[1].flags = 0;
    fs3[2].len = 100; fs3[2].flags = 0;
    got = build_bundle(buf, fs3, 3);
    ok &= check("RX max_frames=2: caps at 2, not 3",
                asix_rx_parse(buf, (unsigned int)got, frames, 2) == 2);

    return ok ? 0 : 1;
}
