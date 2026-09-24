/*
  Host TAP for canonical tty read remainder.

  Regression: sh.exe SYSREAD(1) used to discard the rest of the line after
  the first byte, so "ls" became "l" and Ctrl-C submitted leftover junk.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/console/tty_canon.h"

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
    char line[8];
    char out[8];
    int off;
    int done;
    int n;
    int ok = 1;

    printf("TAP version 13\n1..8\n");

    memcpy(line, "ls\n", 3);
    off = 0;
    n = tty_canon_read_copy(line, 3, &off, out, 1, &done);
    ok &= check("first byte is l", n == 1 && out[0] == 'l' && off == 1);
    ok &= check("line not done after first byte", done == 0);

    n = tty_canon_read_copy(line, 3, &off, out, 1, &done);
    ok &= check("second byte is s", n == 1 && out[0] == 's' && off == 2);
    ok &= check("still not done", done == 0);

    n = tty_canon_read_copy(line, 3, &off, out, 1, &done);
    ok &= check("third byte is newline", n == 1 && out[0] == '\n' && done == 1);

    off = 0;
    n = tty_canon_read_copy(line, 3, &off, out, 8, &done);
    ok &= check("large n returns the whole line",
                n == 3 && done == 1 && memcmp(out, "ls\n", 3) == 0);

    n = tty_canon_read_copy(0, 3, &off, out, 1, &done);
    ok &= check("null canon is zero", n == 0);

    off = 0;
    n = tty_canon_read_copy(line, 3, &off, out, 0, &done);
    ok &= check("n<=0 is zero", n == 0);

    return ok ? 0 : 1;
}
