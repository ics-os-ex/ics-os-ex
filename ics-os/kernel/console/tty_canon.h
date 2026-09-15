#ifndef TTY_CANON_H
#define TTY_CANON_H

/*
  Canonical-line read copy-out. Host-testable.

  POSIX read() on a canonical tty returns min(n, remaining) and keeps the
  rest of the line for the next read. Dropping unread bytes made userland
  sh.exe (which reads one byte at a time) see only the first character of
  each submitted line.
*/

static inline int tty_canon_read_copy(const char *canon, int len, int *off,
                                      char *buf, int n, int *done)
{
    int i = 0;

    if (done)
        *done = 0;
    if (!canon || !off || !buf || n <= 0 || len < 0)
        return 0;
    if (*off < 0)
        *off = 0;
    if (*off > len)
        *off = len;
    while (i < n && *off < len) {
        buf[i++] = canon[*off];
        (*off)++;
    }
    if (done)
        *done = (*off >= len);
    return i;
}

#endif
