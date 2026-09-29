#include <termcap.h>
#include <stddef.h>
#include <string.h>

void *fopen(const char *path, const char *mode);
int fclose(void *f);
int fread(void *ptr, size_t size, size_t nmemb, void *f);
char *getenv(const char *name);
int atoi(const char *s);
int sprintf(char *buf, const char *fmt, ...);

#define TC_FILE_MAX 8192
#define TC_NORM_MAX 16384

static char tc_file[TC_FILE_MAX];
static char tc_norm[TC_NORM_MAX];
static char tc_storebuf[TCBUFSIZ];
static const char *tc_entry;

static const char tc_builtin_xterm[] =
    "xterm|vt100|ICS-OS 80x25 VT100/xterm console:"
    ":am:bs:eo:km:xn:"
    ":co#80:li#25:"
    ":cl=\\E[2J\\E[H:"
    ":cm=\\E[%i%d;%dH:"
    ":cr=\\r:"
    ":le=\\b:"
    ":nd=\\E[C:"
    ":up=\\E[A:"
    ":do=\\E[B:"
    ":ce=\\E[K:"
    ":cd=\\E[J:"
    ":ho=\\E[H:"
    ":vi=\\E[?25l:"
    ":ve=\\E[?25h:"
    ":me=\\E[0m:"
    ":so=\\E[1m:"
    ":se=\\E[0m:"
    ":us=\\E[4m:"
    ":ue=\\E[0m:"
    ":md=\\E[1m:"
    ":Co#256:"
    ":AF=\\E[3%dm:"
    ":AB=\\E[4%dm:"
    ":mr=\\E[7m:"
    ":mb=\\E[4m:"
    ":cs=\\E[%i%d;%dr:"
    ":sr=\\E[r:"
    ":al=\\E[L:"
    ":AL=\\E[%dL:"
    ":dl=\\E[M:"
    ":DL=\\E[%dM:"
    ":RI=\\EM:"
    ":vb=\\a:"
    ":ti=\\E[?1049h:"
    ":te=\\E[?1049l:"
    ":TI=:"
    ":TE=:"
    ":RK=:"
    ":ks=\\E[?1h\\E>:"
    ":ke=\\E[?1l\\E<:"
    ":as=\\016:"
    ":ae=\\017:"
    ":bc=\\b:";

static const char tc_builtin_dumb[] =
    "dumb|dumb terminal:"
    ":am:"
    ":co#80:li#25:"
    ":cl=\\r\\r\\r:"
    ":cr=\\r:"
    ":le=\\b:"
    ":nd= :"
    ":up=\\n:"
    ":bc=\\b:";

static const char *
tc_field(const char *field, const char **tc_end)
{
    const char *p;
    const char *end;
    const char *q;
    size_t flen;

    if (!tc_entry || !field || !field[0])
        return 0;
    p = tc_entry;
    end = p + strlen(p);
    flen = strlen(field);
    while (p < end) {
        p = strchr(p, ':');
        if (!p)
            break;
        ++p;
        if (p >= end)
            break;
        if (strncmp(p, field, flen) == 0 &&
            (p[flen] == ':' || p[flen] == '=' || p[flen] == '#'))
            break;
    }
    if (p && p < end &&
        (p[flen] == ':' || p[flen] == '=' || p[flen] == '#')) {
        if (tc_end) {
            q = strchr(p + flen + 1, ':');
            if (!q)
                q = end;
            *tc_end = q;
        }
        return p;
    }
    if (tc_end)
        *tc_end = 0;
    return 0;
}

static int
tc_name_match(const char *entry, const char *term)
{
    const char *p;
    const char *end;
    const char *q;
    size_t n;

    if (!entry || !term || !term[0])
        return 0;
    if (!strchr(entry, ':'))
        return 0;
    p = entry;
    end = p + strlen(p);
    n = strlen(term);
    while (p < end) {
        if (*p == ':')
            return 0;
        q = p;
        while (q < end && *q != '|' && *q != ':')
            ++q;
        if ((size_t)(q - p) == n && strncmp(p, term, n) == 0)
            return 1;
        if (q >= end)
            break;
        p = q + 1;
    }
    return 0;
}

static int
tc_store(const char *term, const char *entry, char *buf)
{
    size_t len;

    if (!term || !term[0] || !entry || !entry[0] || !buf)
        return 0;
    if (!tc_name_match(entry, term))
        return 0;
    len = strlen(entry);
    if (len >= TCBUFSIZ)
        return -1;
    strcpy(tc_storebuf, entry);
    strcpy(buf, entry);
    tc_entry = tc_storebuf;
    return 1;
}

static int
tc_builtin(const char *term, char *buf)
{
    if (!term || !term[0] || !buf)
        return 0;
    if (strcmp(term, "xterm") == 0 ||
        strcmp(term, "vt100") == 0)
        return tc_store(term, tc_builtin_xterm, buf);
    if (strcmp(term, "dumb") == 0)
        return tc_store(term, tc_builtin_dumb, buf);
    return 0;
}

static char *
tc_read_file(const char *path)
{
    void *f;
    const char *p;
    int len;
    int nlen;
    int in_comment;
    unsigned char c;

    f = fopen(path, "r");
    if (!f)
        return (char *)1;
    len = 0;
    while (len < TC_FILE_MAX - 1 && fread(&c, 1, 1, f) == 1)
        tc_file[len++] = (char)c;
    tc_file[len] = 0;
    fclose(f);
    if (len == TC_FILE_MAX - 1)
        return 0;

    p = tc_file;
    nlen = 0;
    in_comment = 0;
    while (*p) {
        if (nlen >= TC_NORM_MAX - 1)
            return 0;
        if (*p == '\n') {
            in_comment = 0;
            tc_norm[nlen++] = *p++;
        } else if (in_comment) {
            ++p;
        } else if (*p == '#' && (nlen == 0 || tc_norm[nlen - 1] == '\n')) {
            in_comment = 1;
            ++p;
        } else if (*p == ' ' || *p == '\t') {
            ++p;
        } else if (*p == '\\' && p[1] == '\n') {
            p += 2;
        } else if (*p == '\\' && !p[1]) {
            ++p;
        } else {
            tc_norm[nlen++] = *p++;
        }
    }
    tc_norm[nlen] = 0;
    return tc_norm;
}

static const char *
tc_find_entry(const char *file, const char *term)
{
    const char *p;
    const char *nl;

    if (!file || !file[0] || !term || !term[0])
        return 0;
    p = file;
    while (*p) {
        nl = strchr(p, '\n');
        if (tc_name_match(p, term))
            return p;
        if (!nl)
            break;
        p = nl + 1;
    }
    return 0;
}

static int
tc_load(const char *path, const char *term, char *buf)
{
    const char *file;
    const char *entry;
    int r;

    file = tc_read_file(path);
    if (!file)
        return -1;
    if (file == (char *)1)
        return 0;
    entry = tc_find_entry(file, term);
    if (!entry)
        return 0;
    r = tc_store(term, entry, buf);
    return r;
}

int
tgetent(char *buf, const char *id)
{
    const char *term;
    const char *tcenv;
    void *probe;
    int r;

    if (!buf)
        return -1;
    term = (id && id[0]) ? id : 0;
    if (!term)
        term = getenv("TERM");
    if (!term || !term[0])
        term = "xterm";

    tcenv = getenv("TERMCAP");
    if (tcenv && tcenv[0]) {
        probe = fopen(tcenv, "r");
        if (probe) {
            fclose(probe);
            r = tc_load(tcenv, term, buf);
            if (r)
                return r;
        } else {
            r = tc_store(term, tcenv, buf);
            if (r)
                return r;
        }
    }

    r = tc_load("/icsos/etc/termcap", term, buf);
    if (r)
        return r;
    r = tc_load("/etc/termcap", term, buf);
    if (r)
        return r;
    return tc_builtin(term, buf);
}

int
tgetflag(const char *id)
{
    const char *p;
    size_t flen;

    if (!id || !id[0] || !tc_entry)
        return -1;
    p = tc_field(id, 0);
    if (!p)
        return -1;
    flen = strlen(id);
    if (p[flen] == ':')
        return 1;
    return 0;
}

int
tgetnum(const char *id)
{
    const char *p;
    const char *q;
    char numbuf[32];
    size_t flen;
    size_t n;

    if (!id || !id[0] || !tc_entry)
        return -1;
    p = tc_field(id, &q);
    if (!p)
        return -1;
    flen = strlen(id);
    if (p[flen] != '#')
        return -1;
    p += flen + 1;
    if (q < p)
        q = p + strlen(p);
    n = (size_t)(q - p);
    if (n >= sizeof numbuf)
        return -1;
    memcpy(numbuf, p, n);
    numbuf[n] = 0;
    return atoi(numbuf);
}

char *
tgetstr(const char *id, char **buf)
{
    const char *p;
    const char *q;
    char *r;
    char *result;
    size_t flen;
    char c;
    int n;

    if (!id || !id[0] || !buf || !*buf || !tc_entry)
        return 0;
    p = tc_field(id, &q);
    if (!p)
        return 0;
    flen = strlen(id);
    if (p[flen] != '=')
        return 0;
    p += flen + 1;
    if (!q)
        q = p + strlen(p);
    r = result = *buf;
    while (p < q) {
        *r = *p++;
        if (*r == '\\') {
            c = *p++;
            switch (c) {
            case 'E':
                *r = 033;
                break;
            case 'a':
                *r = 007;
                break;
            case 'b':
                *r = '\b';
                break;
            case 'f':
                *r = '\f';
                break;
            case 'n':
                *r = '\n';
                break;
            case 'r':
                *r = '\r';
                break;
            case 't':
                *r = '\t';
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
                n = c - '0';
                if (*p >= '0' && *p <= '7')
                    n = 8 * n + (*p++ - '0');
                if (*p >= '0' && *p <= '7')
                    n = 8 * n + (*p++ - '0');
                *r = (char)n;
                break;
            default:
                *r = c;
                break;
            }
        } else if (*r == '^') {
            *r = (char)(*p++ & 037);
            if (!*r)
                *r = (char)0200;
        }
        ++r;
    }
    *r++ = 0;
    *buf = r;
    return result;
}

static int
tc_emit(int c, int (*outc)(int))
{
    if (c == 0200)
        c = 0;
    return outc(c);
}

void
tputs(const char *str, int affcnt, int (*outc)(int))
{
    const char *p;
    int c;
    int num;

    if (!str || !str[0] || !outc)
        return;
    p = str;
    num = 0;
    if (*p >= '0' && *p <= '9') {
        do {
            num += *p++ - '0';
            num *= 10;
        } while (*p >= '0' && *p <= '9');
        if (*p == '.') {
            ++p;
            if (*p >= '0' && *p <= '9')
                num += *p++ - '0';
        }
        if (*p == '*')
            ++p;
    }
    while ((c = (unsigned char)*p++) != 0)
        tc_emit(c, outc);
    (void)affcnt;
}

char *
tparam(const char *ctl, char *buf, int buflen,
       int row, int col, int row2, int col2)
{
    int av[5];
    int ac;
    int atmp;
    char c;
    char *r;
    char *z;
    char *bufend;
    char numbuf[32];
    const char *fmt;

    if (!ctl || !buf || buflen < 2)
        return 0;
    av[0] = row;
    av[1] = col;
    av[2] = row2;
    av[3] = col2;
    av[4] = 0;
    ac = 0;
    r = buf;
    bufend = r + buflen - 1;
    while (*ctl) {
        *r = *ctl++;
        if (*r != '%') {
            if (++r > bufend)
                return 0;
            continue;
        }
        if (ac > 4)
            ac = 4;
        fmt = 0;
        c = *ctl++;
        if (!c)
            break;
        switch (c) {
        case '%':
            break;
        case 'd':
            fmt = "%d";
            break;
        case '2':
            fmt = "%02d";
            break;
        case '3':
            fmt = "%03d";
            break;
        case '+':
        case '.':
            if (ac < 5) {
                *r = (char)av[ac++];
                if (c == '+' && *ctl)
                    *r += *ctl++;
                if (!*r)
                    *r = (char)0200;
            } else {
                --r;
            }
            break;
        case '>':
            if (ac < 5 && *ctl) {
                if (av[ac] > (*ctl++ & 0377))
                    av[ac] += *ctl;
                ++ctl;
            }
            --r;
            break;
        case 'r':
            atmp = av[0];
            av[0] = av[1];
            av[1] = atmp;
            atmp = av[2];
            av[2] = av[3];
            av[3] = atmp;
            --r;
            break;
        case 'i':
            ++av[0];
            ++av[1];
            ++av[2];
            ++av[3];
            --r;
            break;
        case 'n':
            av[0] ^= 0140;
            av[1] ^= 0140;
            av[2] ^= 0140;
            av[3] ^= 0140;
            --r;
            break;
        case 'B':
            av[0] = (av[0] / 10) * 16 + (av[0] % 10);
            av[1] = (av[1] / 10) * 16 + (av[1] % 10);
            av[2] = (av[2] / 10) * 16 + (av[2] % 10);
            av[3] = (av[3] / 10) * 16 + (av[3] % 10);
            --r;
            break;
        case 'D':
            av[0] -= (av[0] & 15) << 1;
            av[1] -= (av[1] & 15) << 1;
            av[2] -= (av[2] & 15) << 1;
            av[3] -= (av[3] & 15) << 1;
            --r;
            break;
        default:
            if (r + 1 > bufend)
                return 0;
            *++r = c;
            break;
        }
        if (fmt) {
            sprintf(numbuf, fmt, av[ac < 5 ? ac : 4]);
            if (ac < 5)
                ++ac;
            for (z = numbuf; *z && r < bufend; ++z)
                *r++ = *z;
            if (r >= bufend)
                return 0;
            --r;
        }
        if (++r > bufend)
            return 0;
    }
    *r = 0;
    return buf;
}

char *
tgoto(const char *cm, int col, int line)
{
    static char tgoto_buf[128];

    return tparam(cm, tgoto_buf, (int)sizeof tgoto_buf, line, col, 0, 0);
}
