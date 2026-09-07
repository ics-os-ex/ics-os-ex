/*
 * termtest: guest self-test for the ICS-OS terminal stack.
 *
 * Exercises the pieces vim depends on:
 *   - tcgetattr/tcsetattr: canonical -> raw -> canonical round-trip
 *   - ioctl TIOCGWINSZ: 25x80 window
 *   - clock_gettime(CLOCK_MONOTONIC): monotonic millisecond clock
 *   - select(2) / poll(2) with zero timeout (non-blocking readiness)
 *   - VT100/xterm interpreter end-to-end via DSR-6 (CSI 6 n -> CSI row;col R):
 *     the kernel must parse the escape we write and inject the cursor
 *     position report back into the input queue.
 *
 * Prints TERMTEST_PASS on success, TERMTEST_FAIL + reason on failure.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <time.h>

static int fail(const char *msg)
{
   printf("termtest: FAIL %s errno=%d\n", msg, errno);
   printf("TERMTEST_FAIL\n");
   return 1;
}

static int expect(int cond, const char *msg)
{
   if (!cond)
      return fail(msg);
   return 0;
}

/* Read exactly n bytes from stdin into buf; return 0 on success. */
static int read_exact(int n, char *buf)
{
   int got = 0;
   while (got < n) {
      int r = read(0, buf + got, n - got);
      if (r <= 0)
         return -1;
      got += r;
   }
   return 0;
}

int main(void)
{
   struct termios raw, canon, back;
   struct winsize ws;
   struct timespec t1, t2;
   char buf[32];
   int n;

   printf("termtest: begin\n");

   /* 1. initial canonical mode */
   if (tcgetattr(0, &canon))
      return fail("tcgetattr(initial)");
   if (!(canon.c_lflag & (ICANON | ECHO | ISIG)))
      return fail("initial mode not canonical/echo/isig");
   printf("termtest: initial canonical OK (lflag=0x%x)\n", canon.c_lflag);

   /* 2. switch to raw mode */
   raw = canon;
   raw.c_iflag &= ~(unsigned int)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                  INLCR | IGNCR | ICRNL);
   raw.c_oflag &= ~(unsigned int)OPOST;
   raw.c_lflag &= ~(unsigned int)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG);
   raw.c_cflag &= ~(unsigned int)(CSIZE | PARENB);
   raw.c_cflag |= (unsigned int)CS8;
   raw.c_cc[VMIN] = 1;
   raw.c_cc[VTIME] = 0;
   if (tcsetattr(0, TCSANOW, &raw))
      return fail("tcsetattr(raw)");
   if (tcgetattr(0, &back))
      return fail("tcgetattr(raw)");
   if (back.c_lflag & (ICANON | ECHO | ISIG))
      return fail("raw mode not applied");
   printf("termtest: raw mode OK (lflag=0x%x)\n", back.c_lflag);

   /* 3. window size */
   memset(&ws, 0, sizeof(ws));
   if (ioctl(0, TIOCGWINSZ, &ws))
      return fail("ioctl(TIOCGWINSZ)");
   if (ws.ws_row != 25 || ws.ws_col != 80)
      return fail("TIOCGWINSZ not 25x80");
   printf("termtest: winsize OK (%ux%u)\n", ws.ws_row, ws.ws_col);

   /* 4. monotonic clock */
   if (clock_gettime(CLOCK_MONOTONIC, &t1))
      return fail("clock_gettime(1)");
   if (clock_gettime(CLOCK_MONOTONIC, &t2))
      return fail("clock_gettime(2)");
   if (t2.tv_sec < t1.tv_sec ||
       (t2.tv_sec == t1.tv_sec && t2.tv_nsec < t1.tv_nsec))
      return fail("clock not monotonic");
   printf("termtest: clock_gettime OK (sec=%ld)\n", t2.tv_sec);

   /* 5. DSR-6 end-to-end: home then report */
   tcflush(0, TCIFLUSH);
   write(1, "\x1b[H", 3);            /* cursor home -> row 1, col 1 */
   write(1, "\x1b[6n", 4);           /* DSR: report cursor position */
    if (read_exact(6, buf))
       return fail("DSR(1) read");
    if (memcmp(buf, "\x1b[1;1R", 6) != 0) {
      printf("termtest: FAIL DSR(1) got [%s]\n", buf);
      printf("TERMTEST_FAIL\n");
      return 1;
   }
   printf("termtest: DSR home OK\n");

   /* 6. DSR-6 after explicit CUP */
   tcflush(0, TCIFLUSH);
   write(1, "\x1b[5;10H", 7);        /* cursor to row 5, col 10 */
   write(1, "\x1b[6n", 4);
    if (read_exact(7, buf))
       return fail("DSR(2) read");
    if (memcmp(buf, "\x1b[5;10R", 7) != 0) {
      printf("termtest: FAIL DSR(2) got [%s]\n", buf);
      printf("TERMTEST_FAIL\n");
      return 1;
   }
   printf("termtest: DSR CUP OK\n");

   /* 7. select(2) zero-timeout: no pending input after flush */
   tcflush(0, TCIFLUSH);
   {
      fd_set rfds;
      struct timeval tv;
      FD_ZERO(&rfds);
      FD_SET(0, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 0;
      n = select(1, &rfds, 0, 0, &tv);
      if (n < 0)
         return fail("select");
      if (n != 0)
         return fail("select reports spurious input");
   }
   printf("termtest: select zero-timeout OK\n");

   /* 8. poll(2) zero timeout */
   {
      struct pollfd pfd;
      pfd.fd = 0;
      pfd.events = POLLIN;
      pfd.revents = 0;
      n = poll(&pfd, 1, 0);
      if (n < 0)
         return fail("poll");
      if (n != 0)
         return fail("poll reports spurious input");
   }
   printf("termtest: poll zero-timeout OK\n");

   /* 9. restore canonical mode */
   if (tcsetattr(0, TCSANOW, &canon))
      return fail("tcsetattr(canonical restore)");
   if (tcgetattr(0, &back))
      return fail("tcgetattr(restore)");
   if (!(back.c_lflag & (ICANON | ECHO | ISIG)))
      return fail("canonical restore failed");
   printf("termtest: canonical restore OK\n");

   printf("termtest: all checks passed\n");
   printf("TERMTEST_PASS\n");
   return 0;
}
