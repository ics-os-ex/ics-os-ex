/*
  Host TAP for the CDC debug RPC line parser, hex KEYS, SCREEN dump,
  PPM size helpers, and the ICSOS_VER bind/STATUS stamps.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/hardware/usb/usb_debug.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    usb_debug_req req;
    unsigned char cells[80 * 25 * 2];
    char screen[80 * 25 + 25 + 4];
    char hdr[32];
    char okline[48];
    unsigned int dw, dh;
    int n;
    int ok = 1;
    int i;

    printf("TAP version 13\n1..21\n");

    ok &= check("PING",
                usb_debug_parse_line("\x1eICS 3 PING\n", &req) == 0 &&
                req.seq == 3 && req.verb == USB_DEBUG_VERB_PING);

    ok &= check("CMD ls",
                usb_debug_parse_line("ICS 7 CMD ls -l\n", &req) == 0 &&
                req.seq == 7 && req.verb == USB_DEBUG_VERB_CMD &&
                strcmp(req.cmd, "ls -l") == 0);

    ok &= check("KEYS hex",
                usb_debug_parse_line("ICS 1 KEYS 6c730a\n", &req) == 0 &&
                req.nkeys == 3 && req.keys[0] == 'l' && req.keys[1] == 's' &&
                req.keys[2] == '\n');

    ok &= check("KEYS spaces",
                usb_debug_parse_line("ICS 1 KEYS 0d 0a\n", &req) == 0 &&
                req.nkeys == 2 && req.keys[0] == '\r' && req.keys[1] == '\n');

    ok &= check("KEYS odd nibble fails",
                usb_debug_parse_line("ICS 1 KEYS 6c7\n", &req) ==
                USB_DEBUG_ERR_ARG);

    ok &= check("unknown verb",
                usb_debug_parse_line("ICS 1 FOO\n", &req) == USB_DEBUG_ERR_VERB);

    ok &= check("missing ICS prefix",
                usb_debug_parse_line("ping\n", &req) == USB_DEBUG_ERR_PARSE);

    ok &= check("FB scale clamp",
                usb_debug_parse_line("ICS 2 FB 64\n", &req) == 0 &&
                req.fb_scale == 16);

    ok &= check("KEXEC size and CRC32",
                usb_debug_parse_line("ICS 9 KEXEC 4096 2309737967\n", &req) == 0 &&
                req.kexec_bytes == 4096 && req.kexec_crc32 == 0x89abcdefu);

    ok &= check("KEXEC zero fails",
                usb_debug_parse_line("ICS 9 KEXEC 0\n", &req) ==
                USB_DEBUG_ERR_ARG);

    usb_debug_fb_size(1920, 1080, 8, &dw, &dh);
    ok &= check("FB 1920x1080 /8", dw == 240 && dh == 135);

    n = usb_debug_ppm_header(hdr, (int)sizeof(hdr), 2, 2);
    ok &= check("PPM header", n > 0 && memcmp(hdr, "P6\n2 2\n255\n", 11) == 0);

    memset(cells, 0, sizeof(cells));
    cells[0] = 'A';
    cells[2] = 'B';
    cells[(1 * 80 + 0) * 2] = 'C';
    n = usb_debug_screen_text(cells, 80, 2, screen, (int)sizeof(screen));
    ok &= check("SCREEN 80-col rows",
                n == 81 * 2 && screen[0] == 'A' && screen[1] == 'B' &&
                screen[80] == '\n' && screen[81] == 'C');

    n = usb_debug_format_ok(okline, (int)sizeof(okline), 12, 0);
    ok &= check("OK 0 framing",
                n > 0 && okline[0] == (char)USB_DEBUG_RS &&
                memcmp(okline + 1, "ICS 12 OK 0\n", 12) == 0);

    n = usb_debug_format_end(okline, (int)sizeof(okline), 12);
    ok &= check("END framing",
                n > 0 && memcmp(okline + 1, "ICS 12 END\n", 11) == 0);

    {
        char ver[160];
        char found[80];
        char tiny[8];
        const char *log;
        n = usb_debug_format_icsos_ver(ver, (int)sizeof(ver),
                                       "0.01-dev", "abc1234", 1,
                                       "2026-09-15T00:00:00Z",
                                       "Sep 15 2026 06:40:00");
        ok &= check("ICSOS_VER dirty",
                    n > 0 &&
                    strcmp(ver,
                           "ICSOS_VER release=0.01-dev build=abc1234-dirty "
                           "ts=2026-09-15T00:00:00Z compiled=Sep 15 2026 06:40:00\n") == 0);

        n = usb_debug_format_icsos_ver(ver, (int)sizeof(ver),
                                       "0.01-dev", "abc1234", 0,
                                       "2026-09-15T00:00:00Z", NULL);
        ok &= check("ICSOS_VER clean",
                    n > 0 &&
                    strstr(ver, "build=abc1234 ts=") != 0 &&
                    strstr(ver, "-dirty") == 0 &&
                    strstr(ver, "compiled=") == 0);

        n = usb_debug_format_status_ver(ver, (int)sizeof(ver),
                                        "0.01-dev", "deadbeef", 1,
                                        "2026-09-15T01:02:03Z",
                                        "Sep 15 2026 06:40:00");
        ok &= check("STATUS ver fields",
                    n > 0 &&
                    strstr(ver, "release=0.01-dev\n") != 0 &&
                    strstr(ver, "build=deadbeef-dirty\n") != 0 &&
                    strstr(ver, "ts=2026-09-15T01:02:03Z\n") != 0 &&
                    strstr(ver, "compiled=Sep 15 2026 06:40:00\n") != 0);

        log = "USB_CDC_CONSOLE_OK\n"
              "ICSOS_VER release=0.01-dev build=abc1234 ts=T\n"
              "usb: CDC-ACM console ready\n";
        n = usb_debug_find_icsos_ver(log, (int)strlen(log), found,
                                     (int)sizeof(found));
        ok &= check("find ICSOS_VER in log",
                    n > 0 &&
                    strncmp(found, "ICSOS_VER release=0.01-dev", 26) == 0 &&
                    strstr(found, "build=abc1234") != 0);

        n = usb_debug_find_icsos_ver("USB_CDC_CONSOLE_OK\nready\n", 24,
                                     found, (int)sizeof(found));
        ok &= check("find ICSOS_VER missing", n == 0 && found[0] == 0);

        n = usb_debug_find_icsos_ver(log, (int)strlen(log), tiny,
                                     (int)sizeof(tiny));
        ok &= check("find ICSOS_VER truncates",
                    n == 7 && tiny[7] == 0 &&
                    memcmp(tiny, "ICSOS_V", 7) == 0);
    }

    (void)i;
    return ok ? 0 : 1;
}
