/*
 * Tty termios, ioctl, and select(2) syscalls.
 *
 * tcgetattr/tcsetattr/tcflush expose the per-tty line-discipline flags
 * (ICANON/ECHO/ISIG) so user apps can switch between canonical shell mode
 * and raw full-screen mode. ioctl supports TIOCGWINSZ/TIOCSWINSZ on tty
 * fds. select polls tty input readiness (plus always-ready regular files
 * and block devices) with an optional millisecond-precision deadline from
 * getprecisetime().
 *
 * NOTE: struct termios / winsize / timeval / fd_set layouts must stay
 * byte-identical to sdk/include/termios.h, sys/ioctl.h, sys/time.h and
 * sys/select.h.
 */

#include "../dextypes.h"
#include "tty.h"
#include "../process/process.h"
#include "../vfs/posixfd.h"
#include "../stdlib/time.h"

extern void taskswitch(void);
extern DWORD getprecisetime(void);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);

#define K_EBADF   9
#define K_EINVAL  22
#define K_ENOTTY  25
#define K_EINTR   4

/* ---- termios (mirror of sdk/include/termios.h) ---- */

#define NCCS 32

struct termios {
   unsigned int c_iflag;
   unsigned int c_oflag;
   unsigned int c_cflag;
   unsigned int c_lflag;
   unsigned char c_line;
   unsigned char c_cc[NCCS];
};

/* c_iflag */
#define K_IGNBRK  0x001
#define K_BRKINT  0x002
#define K_IGNPAR  0x004
#define K_PARMRK  0x008
#define K_INPCK   0x010
#define KISTRIP   0x020
#define K_INLCR   0x040
#define K_IGNCR   0x080
#define K_ICRNL   0x100
#define K_IUCLC   0x200
#define K_IXON    0x400
#define K_IXANY   0x800
#define K_IXOFF   0x1000
#define K_IMAXBEL 0x2000
/* c_oflag */
#define K_OPOST   0x01
#define K_OLCUC   0x02
#define K_ONLCR   0x10
#define K_OCRNL   0x20
#define K_ONOCR   0x40
#define K_ONLRET  0x80
/* c_cflag */
#define K_CSIZE   0x30
#define K_CS5     0x20
#define K_CS6     0x30
#define K_CS7     0x20
#define K_CS8     0x30
#define K_CSTOPB  0x40
#define K_CREAD   0x80
#define K_CLOCAL  0x1000
/* c_lflag */
#define K_ISIG    0x01
#define K_ICANON  0x02
#define K_XCASE   0x04
#define K_ECHO    0x08
#define K_ECHOE   0x10
#define K_ECHOK   0x20
#define K_ECHONL  0x40
#define K_NOFLSH  0x80
#define K_TOSTOP  0x100
#define K_IEXTEN  0x8000
/* c_cc indices */
#define K_VINTR   0
#define K_VQUIT   1
#define K_VERASE  2
#define K_VKILL   3
#define K_VEOF    4
#define K_VTIME   5
#define K_VMIN    6
#define K_VSWTCH  7
#define K_VSTART  8
#define K_VSTOP   9
#define K_VSUSP   10
#define K_VEOL    11
#define K_VEOL2   12

/* tcsetattr actions */
#define K_TCSANOW   0
#define K_TCSADRAIN 1
#define K_TCSAFLUSH 2
/* tcflush queues */
#define K_TCIFLUSH  0
#define K_TCOFLUSH  1
#define K_TCIOFLUSH 2

/* ---- winsize (mirror of sdk/include/sys/ioctl.h) ---- */

#define K_TIOCGWINSZ 0x5413
#define K_TIOCSWINSZ 0x5414

struct winsize {
   unsigned int ws_row;
   unsigned int ws_col;
   unsigned int ws_xpixel;
   unsigned int ws_ypixel;
};

/* ---- select (mirror of sdk/include/sys/select.h) ---- */

struct k_timeval {
   long tv_sec;
   long tv_usec;
};

#define K_FD_SETSIZE 1024

static int k_fd_bit(long *set, int fd)
{
   return (int)((set[fd / 64] >> (fd % 64)) & 1UL);
}

static void k_fd_set(long *set, int fd)
{
   set[fd / 64] |= (1UL << (fd % 64));
}

static void k_fd_clr(long *set, int fd)
{
   set[fd / 64] &= ~(1UL << (fd % 64));
}

/* ---- termios syscalls ---- */

long sys_tcgetattr(int fd, void *t)
{
   tty_t *tt = posix_fd_tty(fd);
   struct termios *io = (struct termios *)t;
   if (!tt || !io)
      return -K_EBADF;
   io->c_iflag = K_ICRNL | K_BRKINT;
   io->c_oflag = K_OPOST | K_ONLCR;
   io->c_cflag = K_CREAD | K_CS8;
   io->c_lflag = 0;
   if (tt->flags & TTY_ISIG)
      io->c_lflag |= K_ISIG;
   if (tt->flags & TTY_ICANON)
      io->c_lflag |= K_ICANON;
   if (tt->flags & TTY_ECHO)
      io->c_lflag |= K_ECHO;
   io->c_line = 0;
   memset(io->c_cc, 0, NCCS);
   io->c_cc[K_VINTR] = 3;
   io->c_cc[K_VQUIT] = 0x1C;
   io->c_cc[K_VERASE] = 127;
   io->c_cc[K_VKILL] = 12;
   io->c_cc[K_VEOF] = 4;
   io->c_cc[K_VTIME] = 0;
   io->c_cc[K_VMIN] = 1;
   return 0;
}

long sys_tcsetattr(int fd, int action, const void *t)
{
   tty_t *tt = posix_fd_tty(fd);
   const struct termios *io = (const struct termios *)t;
   if (!tt || !io)
      return -K_EBADF;
   if (action < 0 || action > 2)
      return -K_EINVAL;
   if (action == K_TCSAFLUSH)
      tty_flush_input(tt);
   /* A mode change invalidates any partial canonical line or half-eaten
      escape sequence. */
   tt->canon_len = 0;
   tt->line_ready = 0;
   tt->canon_esc = 0;
   tt->flags &= ~(TTY_ICANON | TTY_ECHO | TTY_ISIG);
   if (io->c_lflag & K_ICANON)
      tt->flags |= TTY_ICANON;
   if (io->c_lflag & K_ECHO)
      tt->flags |= TTY_ECHO;
   if (io->c_lflag & K_ISIG)
      tt->flags |= TTY_ISIG;
   return 0;
}

long sys_tcflush(int fd, int q)
{
   tty_t *tt = posix_fd_tty(fd);
   if (!tt)
      return -K_EBADF;
   if (q < 0 || q > 2)
      return -K_EINVAL;
   if (q != K_TCOFLUSH)
      tty_flush_input(tt);
   return 0;
}

/* ---- ioctl ---- */

long sys_ttyioctl(int fd, unsigned long req, void *arg)
{
   tty_t *tt = posix_fd_tty(fd);
   if (!tt)
      return -K_EBADF;
   switch (req) {
   case K_TIOCGWINSZ:
      if (!arg)
         return -K_EINVAL;
      {
         struct winsize *w = (struct winsize *)arg;
         w->ws_row = (unsigned int)tt->ddl ? 25 : 0;
         w->ws_col = (unsigned int)tt->ddl ? 80 : 0;
         w->ws_xpixel = 640;
         w->ws_ypixel = 400;
      }
      return 0;
   case K_TIOCSWINSZ:
      /* fixed 80x25 VGA console: accept and ignore */
      return 0;
   default:
      return -K_ENOTTY;
   }
}

/* ---- select ---- */

long sys_ttyselect(long nfds, long rfds, long wfds, long efds, long tvp)
{
   struct k_timeval tv;
   long deadline = -1;
   long *rset = (long *)rfds;
   long *wset = (long *)wfds;
   long *eset = (long *)efds;
   int lim, fd;
   long count;

  (void)eset;
    if (nfds <= 0 || nfds > K_FD_SETSIZE)
       return -K_EINVAL;
    lim = (int)nfds;
   if (tvp) {
      memcpy(&tv, (void *)tvp, sizeof(tv));
      if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
         return -K_EINVAL;
      deadline = getprecisetime() + tv.tv_sec * 1000 + tv.tv_usec / 1000;
   }

   for (;;) {
       count = 0;
   for (fd = 0; fd < lim; fd++) {
           if (rset && k_fd_bit(rset, fd)) {
              if (posix_fd_selectable(fd, 0))
                 count++;
           }
           if (wset && k_fd_bit(wset, fd)) {
              if (posix_fd_selectable(fd, 1))
                 count++;
           }
        }
       if (count) {
          for (fd = 0; fd < lim; fd++) {
             if (rset && k_fd_bit(rset, fd) && !posix_fd_selectable(fd, 0))
                k_fd_clr(rset, fd);
             if (wset && k_fd_bit(wset, fd) && !posix_fd_selectable(fd, 1))
                k_fd_clr(wset, fd);
          }
          return count;
       }
       if (current_process && current_process->pending_sig == SIGINT) {
          current_process->pending_sig = 0;
          return -K_EINTR;
       }
       if (deadline >= 0) {
          if (getprecisetime() >= deadline) {
             for (fd = 0; fd < lim; fd++) {
                if (rset && k_fd_bit(rset, fd))
                   k_fd_clr(rset, fd);
                if (wset && k_fd_bit(wset, fd))
                   k_fd_clr(wset, fd);
             }
             return 0;
          }
       }
       taskswitch();
    }
}
