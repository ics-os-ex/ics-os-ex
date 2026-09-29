#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termcap.h>

static int
check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

static int cap_len;
static char cap_buf[256];

static int
cap_outc(int c)
{
    if (cap_len < (int)sizeof cap_buf)
        cap_buf[cap_len] = (char)c;
    cap_len++;
    return c;
}

int
main(void)
{
    static char env[TCBUFSIZ + 64];
    char buf[TCBUFSIZ];
    char *tp;
    const char *s;
    char *g;
    char out[128];
    char path[] = "/tmp/icsos-tests/termcap_unit.termcap";
    const char seq[] = {0200, 'a', 'b', 0};
    FILE *f;
    int r;
    int ok = 1;
    const char raw[] =
        "xterm|vt100:am:co#80:li#25:"
        "cm=\\E[%i%d;%dH:"
        "ti=\\E[?1049h:"
        "te=\\E[?1049l:"
        "vb=\\a:"
        "cs=\\E[%i%d;%dr:"
        "bc=\\b:"
        "as=\\016:"
        "ae=\\017:"
        "a=AB:"
        "b=CD:";

    printf("TAP version 13\n1..32\n");

    sprintf(env, "TERMCAP=%s", raw);
    putenv(env);
    r = tgetent(buf, "xterm");
    ok &= check("tgetent returns 1", r == 1);
    ok &= check("tgetent copies entry", strncmp(buf, "xterm|vt100:", 12) == 0);
    ok &= check("tgetnum co", tgetnum("co") == 80);
    ok &= check("tgetnum li", tgetnum("li") == 25);
    ok &= check("tgetflag am", tgetflag("am") == 1);
    ok &= check("tgetflag missing", tgetflag("xx") == -1);

    tp = buf;
    s = tgetstr("cm", &tp);
    ok &= check("tgetstr cm", s && strcmp(s, "\033[%i%d;%dH") == 0);
    ok &= check("tgetstr cm pointer", s == buf);
    s = tgetstr("ti", &tp);
    ok &= check("tgetstr ti", s && strcmp(s, "\033[?1049h") == 0);
    s = tgetstr("te", &tp);
    ok &= check("tgetstr te", s && strcmp(s, "\033[?1049l") == 0);
    ok &= check("tgetstr advances pointer", tp != buf);

    tp = buf;
    s = tgetstr("a", &tp);
    ok &= check("tgetstr a", s && strcmp(s, "AB") == 0);
    s = tgetstr("b", &tp);
    ok &= check("tgetstr b", s && strcmp(s, "CD") == 0);

    tp = buf;
    s = tgetstr("vb", &tp);
    ok &= check("tgetstr vb", s && s[0] == '\a' && s[1] == '\0');
    s = tgetstr("as", &tp);
    ok &= check("tgetstr as", s && s[0] == 14 && s[1] == '\0');
    s = tgetstr("ae", &tp);
    ok &= check("tgetstr ae", s && s[0] == 15 && s[1] == '\0');
    s = tgetstr("bc", &tp);
    ok &= check("tgetstr bc", s && s[0] == '\b' && s[1] == '\0');

    tp = buf;
    s = tgetstr("cm", &tp);
    g = tgoto(s, 0, 1);
    ok &= check("tgoto cm row1 col0", g && strcmp(g, "\033[2;1H") == 0);
    g = tgoto(s, 79, 24);
    ok &= check("tgoto cm row25 col80", g && strcmp(g, "\033[25;80H") == 0);

    ok &= check("tparam digits",
                tparam("A%dB%dC", out, (int)sizeof out, 1, 2, 3, 4) &&
                strcmp(out, "A1B2C") == 0);
    ok &= check("tparam %i",
                tparam("%i%d;%dH", out, (int)sizeof out, 1, 0, 0, 0) &&
                strcmp(out, "2;1H") == 0);
    ok &= check("tparam %r",
                tparam("%r%d,%d", out, (int)sizeof out, 1, 2, 0, 0) &&
                strcmp(out, "2,1") == 0);
    ok &= check("tparam %2",
                tparam("%2", out, (int)sizeof out, 5, 0, 0, 0) &&
                strcmp(out, "05") == 0);
    ok &= check("tparam %%",
                tparam("%%", out, (int)sizeof out, 0, 0, 0, 0) &&
                strcmp(out, "%") == 0);

    cap_len = 0;
    tputs("\033[2J", 1, cap_outc);
    ok &= check("tputs plain",
                cap_len == 4 && memcmp(cap_buf, "\033[2J", 4) == 0);
    cap_len = 0;
    tputs("5.3\033[2J", 1, cap_outc);
    ok &= check("tputs strips padding",
                cap_len == 4 && memcmp(cap_buf, "\033[2J", 4) == 0);
    cap_len = 0;
    tputs(seq, 1, cap_outc);
    ok &= check("tputs maps 0200",
                cap_len == 3 &&
                cap_buf[0] == '\0' && cap_buf[1] == 'a' && cap_buf[2] == 'b');

    f = fopen(path, "w");
    if (f) {
        fprintf(f, "fileterm:co#65:li#20:cl=\\E[3J:\n");
        fclose(f);
    }
    sprintf(env, "TERMCAP=%s", path);
    putenv(env);
    r = tgetent(buf, "fileterm");
    ok &= check("tgetent file", r == 1);
    ok &= check("tgetnum file co", tgetnum("co") == 65);
    tp = buf;
    s = tgetstr("cl", &tp);
    ok &= check("tgetstr file cl", s && strcmp(s, "\033[3J") == 0);

    sprintf(env, "TERMCAP=%s", raw);
    putenv(env);
    r = tgetent(buf, "unknownterm");
    ok &= check("unknown term", r == 0);
    ok &= check("null buffer", tgetent(0, "xterm") == -1);

    return ok ? 0 : 1;
}
