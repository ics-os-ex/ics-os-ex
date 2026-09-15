#ifndef USB_DEBUG_H
#define USB_DEBUG_H

/*
  Framed debug RPC over CDC-ACM (Pico gadget <-> ICS-OS host).

  Console text is unframed. Requests from the gadget start with RS (0x1e):

    <RS>ICS <seq> <VERB> [args]<LF>

  Replies:

    <RS>ICS <seq> OK <nbytes><LF>
    <payload>
    <RS>ICS <seq> END<LF>

    <RS>ICS <seq> ERR <code> <msg><LF>

  Verbs: PING, KEYS, CMD, STATUS, SCREEN, FB, DMESG, REBOOT, KEXEC.
  KEYS args are lowercase hex (optional spaces). CMD args are the rest of
  the line (kernel console_execute). FB takes an integer scale 2..16.
  KEXEC takes a decimal byte count. The host ACKs with OK payload
  "ready", then that many raw body bytes follow; a second OK ("done")
  is sent before kexec_reboot.

  CDC bind prints an unframed stamp so Pico GET /log and GET /health still
  identify the kernel when STATUS RPC times out:

    ICSOS_VER release=<id> build=<hash>[-dirty] ts=<UTC> compiled=<date time>
*/

#define USB_DEBUG_RS        0x1e
#define USB_DEBUG_LINE_MAX  256
#define USB_DEBUG_HEX_MAX   128
#define USB_DEBUG_ICSOS_VER_PREFIX "ICSOS_VER "

enum {
    USB_DEBUG_VERB_NONE = 0,
    USB_DEBUG_VERB_PING,
    USB_DEBUG_VERB_KEYS,
    USB_DEBUG_VERB_CMD,
    USB_DEBUG_VERB_STATUS,
    USB_DEBUG_VERB_SCREEN,
    USB_DEBUG_VERB_FB,
    USB_DEBUG_VERB_DMESG,
    USB_DEBUG_VERB_REBOOT,
    USB_DEBUG_VERB_KEXEC
};

enum {
    USB_DEBUG_OK = 0,
    USB_DEBUG_ERR_PARSE = 1,
    USB_DEBUG_ERR_VERB = 2,
    USB_DEBUG_ERR_ARG = 3,
    USB_DEBUG_ERR_BUSY = 4,
    USB_DEBUG_ERR_IO = 5
};

typedef struct {
    unsigned int seq;
    int verb;
    unsigned int fb_scale;
    unsigned int kexec_bytes;
    unsigned int nkeys;
    unsigned char keys[USB_DEBUG_HEX_MAX];
    char cmd[USB_DEBUG_LINE_MAX];
} usb_debug_req;

static inline int usb_debug_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline unsigned int usb_debug_fb_scale(unsigned int scale)
{
    if (scale < 2)
        return 2;
    if (scale > 16)
        return 16;
    return scale;
}

static inline void usb_debug_fb_size(unsigned int sw, unsigned int sh,
                                     unsigned int scale,
                                     unsigned int *dw, unsigned int *dh)
{
    scale = usb_debug_fb_scale(scale);
    if (dw) {
        *dw = sw / scale;
        if (!*dw && sw)
            *dw = 1;
    }
    if (dh) {
        *dh = sh / scale;
        if (!*dh && sh)
            *dh = 1;
    }
}

static inline unsigned int usb_debug_ppm_bytes(unsigned int dw, unsigned int dh)
{
    /* "P6\nWWWW HHHH\n255\n" is under 32 bytes; caller may use 32. */
    return 32u + dw * dh * 3u;
}

static inline int usb_debug_ppm_header(char *out, int outsz,
                                       unsigned int dw, unsigned int dh)
{
    char tmp[32];
    unsigned int n = 0;
    unsigned int v;
    int i;
    const char *p;

    if (!out || outsz < 16)
        return 0;
    tmp[n++] = 'P';
    tmp[n++] = '6';
    tmp[n++] = '\n';
    v = dw;
    if (!v)
        tmp[n++] = '0';
    else {
        char d[10];
        int nd = 0;
        while (v && nd < 10) {
            d[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (nd)
            tmp[n++] = d[--nd];
    }
    tmp[n++] = ' ';
    v = dh;
    if (!v)
        tmp[n++] = '0';
    else {
        char d[10];
        int nd = 0;
        while (v && nd < 10) {
            d[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (nd)
            tmp[n++] = d[--nd];
    }
    tmp[n++] = '\n';
    tmp[n++] = '2';
    tmp[n++] = '5';
    tmp[n++] = '5';
    tmp[n++] = '\n';
    if ((int)n > outsz)
        return 0;
    p = tmp;
    for (i = 0; i < (int)n; i++)
        out[i] = p[i];
    return (int)n;
}

static inline int usb_debug_screen_text(const unsigned char *cells,
                                        int cols, int rows,
                                        char *out, int outsz)
{
    int x, y, n = 0;

    if (!cells || !out || cols <= 0 || rows <= 0 || outsz <= 0)
        return 0;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            unsigned char c = cells[(y * cols + x) * 2];
            if (n + 1 >= outsz)
                return n;
            if (c < 32 || c > 126)
                c = ' ';
            out[n++] = (char)c;
        }
        if (n + 1 >= outsz)
            return n;
        out[n++] = '\n';
    }
    if (n < outsz)
        out[n] = 0;
    return n;
}

static inline int usb_debug_parse_hex(const char *s, unsigned char *out,
                                      unsigned int maxn, unsigned int *nkeys)
{
    int hi = -1;
    unsigned int n = 0;

    if (nkeys)
        *nkeys = 0;
    if (!s || !out)
        return USB_DEBUG_ERR_ARG;
    while (*s && *s != '\n' && *s != '\r') {
        int v;
        if (*s == ' ' || *s == '\t') {
            s++;
            continue;
        }
        v = usb_debug_hexval((unsigned char)*s);
        if (v < 0)
            return USB_DEBUG_ERR_ARG;
        if (hi < 0)
            hi = v;
        else {
            if (n >= maxn)
                return USB_DEBUG_ERR_ARG;
            out[n++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
        s++;
    }
    if (hi >= 0)
        return USB_DEBUG_ERR_ARG;
    if (nkeys)
        *nkeys = n;
    return USB_DEBUG_OK;
}

static inline int usb_debug_verb_from(const char *s, int n)
{
    if (n == 4 && s[0] == 'P' && s[1] == 'I' && s[2] == 'N' && s[3] == 'G')
        return USB_DEBUG_VERB_PING;
    if (n == 4 && s[0] == 'K' && s[1] == 'E' && s[2] == 'Y' && s[3] == 'S')
        return USB_DEBUG_VERB_KEYS;
    if (n == 3 && s[0] == 'C' && s[1] == 'M' && s[2] == 'D')
        return USB_DEBUG_VERB_CMD;
    if (n == 6 && s[0] == 'S' && s[1] == 'T' && s[2] == 'A' &&
        s[3] == 'T' && s[4] == 'U' && s[5] == 'S')
        return USB_DEBUG_VERB_STATUS;
    if (n == 6 && s[0] == 'S' && s[1] == 'C' && s[2] == 'R' &&
        s[3] == 'E' && s[4] == 'E' && s[5] == 'N')
        return USB_DEBUG_VERB_SCREEN;
    if (n == 2 && s[0] == 'F' && s[1] == 'B')
        return USB_DEBUG_VERB_FB;
    if (n == 5 && s[0] == 'D' && s[1] == 'M' && s[2] == 'E' &&
        s[3] == 'S' && s[4] == 'G')
        return USB_DEBUG_VERB_DMESG;
    if (n == 6 && s[0] == 'R' && s[1] == 'E' && s[2] == 'B' &&
        s[3] == 'O' && s[4] == 'O' && s[5] == 'T')
        return USB_DEBUG_VERB_REBOOT;
    if (n == 5 && s[0] == 'K' && s[1] == 'E' && s[2] == 'X' &&
        s[3] == 'E' && s[4] == 'C')
        return USB_DEBUG_VERB_KEXEC;
    return USB_DEBUG_VERB_NONE;
}

static inline unsigned int usb_debug_parse_u32(const char *s)
{
    unsigned int v = 0;
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return v;
}

/*
  Parse one request line. `line` may start with RS. Returns USB_DEBUG_OK
  or an ERR_* code. On parse failure `out->verb` is NONE.
*/
static inline int usb_debug_parse_line(const char *line, usb_debug_req *out)
{
    const char *p;
    const char *verb;
    int vn;
    unsigned int seq = 0;

    if (!out)
        return USB_DEBUG_ERR_PARSE;
    out->seq = 0;
    out->verb = USB_DEBUG_VERB_NONE;
    out->fb_scale = 8;
    out->kexec_bytes = 0;
    out->nkeys = 0;
    out->cmd[0] = 0;
    if (!line)
        return USB_DEBUG_ERR_PARSE;
    p = line;
    if ((unsigned char)*p == USB_DEBUG_RS)
        p++;
    if (p[0] != 'I' || p[1] != 'C' || p[2] != 'S' || p[3] != ' ')
        return USB_DEBUG_ERR_PARSE;
    p += 4;
    while (*p >= '0' && *p <= '9') {
        seq = seq * 10u + (unsigned int)(*p - '0');
        p++;
    }
    if (*p != ' ')
        return USB_DEBUG_ERR_PARSE;
    p++;
    out->seq = seq;
    verb = p;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r')
        p++;
    vn = (int)(p - verb);
    out->verb = usb_debug_verb_from(verb, vn);
    if (out->verb == USB_DEBUG_VERB_NONE)
        return USB_DEBUG_ERR_VERB;
    if (*p == ' ')
        p++;
    while (*p == ' ')
        p++;
    if (out->verb == USB_DEBUG_VERB_KEYS)
        return usb_debug_parse_hex(p, out->keys, USB_DEBUG_HEX_MAX, &out->nkeys);
    if (out->verb == USB_DEBUG_VERB_CMD) {
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < USB_DEBUG_LINE_MAX - 1)
            out->cmd[i++] = *p++;
        out->cmd[i] = 0;
        return USB_DEBUG_OK;
    }
    if (out->verb == USB_DEBUG_VERB_FB) {
        if (*p)
            out->fb_scale = usb_debug_fb_scale(usb_debug_parse_u32(p));
        return USB_DEBUG_OK;
    }
    if (out->verb == USB_DEBUG_VERB_KEXEC) {
        out->kexec_bytes = usb_debug_parse_u32(p);
        if (!out->kexec_bytes)
            return USB_DEBUG_ERR_ARG;
        return USB_DEBUG_OK;
    }
    return USB_DEBUG_OK;
}

static inline int usb_debug_format_ok(char *out, int outsz,
                                      unsigned int seq, unsigned int nbytes)
{
    char tmp[48];
    unsigned int n = 0;
    unsigned int v;
    int i;
    char d[10];
    int nd;

    if (!out || outsz < 16)
        return 0;
    tmp[n++] = (char)USB_DEBUG_RS;
    tmp[n++] = 'I';
    tmp[n++] = 'C';
    tmp[n++] = 'S';
    tmp[n++] = ' ';
    v = seq;
    nd = 0;
    if (!v)
        d[nd++] = '0';
    while (v && nd < 10) {
        d[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (nd)
        tmp[n++] = d[--nd];
    tmp[n++] = ' ';
    tmp[n++] = 'O';
    tmp[n++] = 'K';
    tmp[n++] = ' ';
    v = nbytes;
    nd = 0;
    if (!v)
        d[nd++] = '0';
    while (v && nd < 10) {
        d[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (nd)
        tmp[n++] = d[--nd];
    tmp[n++] = '\n';
    if ((int)n > outsz)
        return 0;
    for (i = 0; i < (int)n; i++)
        out[i] = tmp[i];
    return (int)n;
}

static inline int usb_debug_cat(char *dst, int used, int cap, const char *s)
{
    int i = 0;
    if (!dst || used < 0 || used >= cap)
        return used;
    if (!s)
        return used;
    while (s[i] && used + i < cap - 1) {
        dst[used + i] = s[i];
        i++;
    }
    dst[used + i] = 0;
    return used + i;
}

/*
  Unframed bind stamp. Dirty is 0/1 (ICSOS_GIT_DIRTY[0]=='1'). Always
  NUL-terminates when outsz > 0. Returns bytes written excluding NUL.
*/
static inline int usb_debug_format_icsos_ver(char *out, int outsz,
                                             const char *release,
                                             const char *hash,
                                             int dirty,
                                             const char *ts,
                                             const char *compiled)
{
    int n = 0;
    if (!out || outsz < 2)
        return 0;
    out[0] = 0;
    n = usb_debug_cat(out, n, outsz, USB_DEBUG_ICSOS_VER_PREFIX);
    n = usb_debug_cat(out, n, outsz, "release=");
    n = usb_debug_cat(out, n, outsz, release);
    n = usb_debug_cat(out, n, outsz, " build=");
    n = usb_debug_cat(out, n, outsz, hash);
    if (dirty)
        n = usb_debug_cat(out, n, outsz, "-dirty");
    n = usb_debug_cat(out, n, outsz, " ts=");
    n = usb_debug_cat(out, n, outsz, ts);
    if (compiled && compiled[0]) {
        n = usb_debug_cat(out, n, outsz, " compiled=");
        n = usb_debug_cat(out, n, outsz, compiled);
    }
    n = usb_debug_cat(out, n, outsz, "\n");
    return n;
}

/* release=/build=/ts=/compiled= lines for the STATUS RPC body. */
static inline int usb_debug_format_status_ver(char *out, int outsz,
                                              const char *release,
                                              const char *hash,
                                              int dirty,
                                              const char *ts,
                                              const char *compiled)
{
    int n = 0;
    if (!out || outsz < 2)
        return 0;
    out[0] = 0;
    n = usb_debug_cat(out, n, outsz, "release=");
    n = usb_debug_cat(out, n, outsz, release);
    n = usb_debug_cat(out, n, outsz, "\nbuild=");
    n = usb_debug_cat(out, n, outsz, hash);
    if (dirty)
        n = usb_debug_cat(out, n, outsz, "-dirty");
    n = usb_debug_cat(out, n, outsz, "\nts=");
    n = usb_debug_cat(out, n, outsz, ts);
    n = usb_debug_cat(out, n, outsz, "\n");
    if (compiled && compiled[0]) {
        n = usb_debug_cat(out, n, outsz, "compiled=");
        n = usb_debug_cat(out, n, outsz, compiled);
        n = usb_debug_cat(out, n, outsz, "\n");
    }
    return n;
}

/*
  Copy the first ICSOS_VER line from a console ring (no NUL required).
  Returns length excluding NUL, or 0 if absent.
*/
static inline int usb_debug_find_icsos_ver(const char *buf, int nbuf,
                                           char *out, int outsz)
{
    const char *pfx = USB_DEBUG_ICSOS_VER_PREFIX;
    int plen = 0;
    int i, j;

    if (out && outsz > 0)
        out[0] = 0;
    if (!buf || nbuf <= 0 || !out || outsz < 2)
        return 0;
    while (pfx[plen])
        plen++;
    if (nbuf < plen)
        return 0;
    for (i = 0; i <= nbuf - plen; i++) {
        int hit = 1;
        int start;
        int len;
        for (j = 0; j < plen; j++) {
            if (buf[i + j] != pfx[j]) {
                hit = 0;
                break;
            }
        }
        if (!hit)
            continue;
        start = i;
        i += plen;
        while (i < nbuf && buf[i] != '\n' && buf[i] != '\r')
            i++;
        len = i - start;
        if (len >= outsz)
            len = outsz - 1;
        for (j = 0; j < len; j++)
            out[j] = buf[start + j];
        out[len] = 0;
        return len;
    }
    return 0;
}

static inline int usb_debug_format_end(char *out, int outsz, unsigned int seq)
{
    char tmp[32];
    unsigned int n = 0;
    unsigned int v;
    int i, nd;
    char d[10];

    if (!out || outsz < 12)
        return 0;
    tmp[n++] = (char)USB_DEBUG_RS;
    tmp[n++] = 'I';
    tmp[n++] = 'C';
    tmp[n++] = 'S';
    tmp[n++] = ' ';
    v = seq;
    nd = 0;
    if (!v)
        d[nd++] = '0';
    while (v && nd < 10) {
        d[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (nd)
        tmp[n++] = d[--nd];
    tmp[n++] = ' ';
    tmp[n++] = 'E';
    tmp[n++] = 'N';
    tmp[n++] = 'D';
    tmp[n++] = '\n';
    if ((int)n > outsz)
        return 0;
    for (i = 0; i < (int)n; i++)
        out[i] = tmp[i];
    return (int)n;
}

#endif
