/*
 * POSIX helpers on top of the ICS-OS syscall libc (tccsdk.c).
 * Linked into every user program that needs a C compiler or POSIX I/O.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/io_uring.h>
#include <sys/wait.h>
#include <dirent.h>
#include <pwd.h>
#include <spawn.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/poll.h>

extern unsigned long dexsdk_systemcall(int function_number,long p1,long p2,
                  long p3,long p4,long p5);
extern void *malloc(size_t size);
extern void free(void *ptr);
extern void *memset(void *d, int c, size_t n);
extern void *memcpy(void *d, const void *s, size_t n);
extern size_t strlen(const char *s);
extern int printf(const char *fmt, ...);
extern int vsprintf(char *buf, const char *fmt, va_list ap);
extern void charputc(char c);
extern int getchar(void);
extern void exit(int status);

#define FXN_GETCWD   0x43
#define FXN_CHDIR    0x42
#define FXN_RENAME   0xA2
#define FXN_REBOOT   0xA3
#define FXN_FSTAT    0x58
#define FXN_STAT     36
#define FXN_TIME     0x55
#define FXN_SYSREAD  0xA4
#define FXN_SYSWRITE 0xA5
#define FXN_SYSOPEN  0xA7
#define FXN_SYSCLOSE 0xA8
#define FXN_SYSLSEEK 0xA9
#define FXN_PREADV   0xAA
#define FXN_PWRITEV  0xAB
#define FXN_FSYNC    0xAC
#define FXN_URING_SETUP 0xAD
#define FXN_URING_ENTER 0xAE
#define FXN_FSTATFD  0xAF
#define FXN_FDFILE   0xB0
#define FXN_WAITPID  0xB1
#define FXN_SPAWN    0xB2
#define FXN_EXECVE   0xB3
#define FXN_GETDENTS 0xB4
#define FXN_MMAP     0xB6
#define FXN_MUNMAP   0xB7
#define FXN_TCGETATTR 0xC0
#define FXN_TCSETATTR 0xC1
#define FXN_TCFUSH    0xC2
#define FXN_TTYIOCTL  0xC3
#define FXN_TTYSELECT 0xC4
#define FXN_DUP 0xC5
#define FXN_DELAY 0x9B
#define FXN_PRECISTIME 0x96

static long ics_sys(int n, long a, long b, long c, long d, long e)
{
   long r = (long)dexsdk_systemcall(n, a, b, c, d, e);
   if (r < 0) {
      errno = (int)(-r);
      return -1;
   }
   return r;
}

int open(const char *path, int flags, ...)
{
   va_list ap;
   int mode = 0666;
   long r;
   va_start(ap, flags);
   if (flags & O_CREAT)
      mode = va_arg(ap, int);
   va_end(ap);
   r = ics_sys(FXN_SYSOPEN, (long)path, flags, mode, 0, 0);
   return (int)r;
}

int creat(const char *path, int mode)
{
   return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd)
{
    if (fd >= 0 && fd < 3)
       return 0;
    return (int)ics_sys(FXN_SYSCLOSE, fd, 0, 0, 0, 0);
}

int dup(int oldfd)
{
    return (int)ics_sys(FXN_DUP, oldfd, 0, 0, 0, 0);
}

ssize_t read(int fd, void *buf, size_t n)
{
   return (ssize_t)ics_sys(FXN_SYSREAD, fd, (long)buf, (long)n, 0, 0);
}

ssize_t write(int fd, const void *buf, size_t n)
{
   return (ssize_t)ics_sys(FXN_SYSWRITE, fd, (long)buf, (long)n, 0, 0);
}

long lseek(int fd, long off, int whence)
{
   return ics_sys(FXN_SYSLSEEK, fd, off, whence, 0, 0);
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
   return (ssize_t)ics_sys(FXN_PREADV, fd, (long)iov, iovcnt, (long)offset, 0);
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
   return (ssize_t)ics_sys(FXN_PWRITEV, fd, (long)iov, iovcnt, (long)offset, 0);
}

ssize_t pread(int fd, void *buf, size_t n, off_t offset)
{
   struct iovec iov;
   iov.iov_base = buf;
   iov.iov_len = n;
   return preadv(fd, &iov, 1, offset);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset)
{
   struct iovec iov;
   iov.iov_base = (void *)buf;
   iov.iov_len = n;
   return pwritev(fd, &iov, 1, offset);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
   long off = lseek(fd, 0, SEEK_CUR);
   ssize_t n;
   if (off < 0)
      return -1;
   n = preadv(fd, iov, iovcnt, off);
   if (n > 0)
      lseek(fd, off + (long)n, SEEK_SET);
   return n;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
   long off = lseek(fd, 0, SEEK_CUR);
   ssize_t n;
   if (off < 0)
      return -1;
   n = pwritev(fd, iov, iovcnt, off);
   if (n > 0)
      lseek(fd, off + (long)n, SEEK_SET);
   return n;
}

int fsync(int fd)
{
   return (int)ics_sys(FXN_FSYNC, fd, 0, 0, 0, 0);
}

#define FDOPEN_SLOTS 16
static struct {
   FILE *stream;
   int fd;
} fdopen_slots[FDOPEN_SLOTS];

/* Called by tccsdk fclose() after flushing its userspace write buffer. */
int ics_fdopen_close(FILE *stream)
{
   int i;
   for (i=0;i<FDOPEN_SLOTS;i++)
      if (fdopen_slots[i].stream==stream) {
         int fd=fdopen_slots[i].fd;
         fdopen_slots[i].stream=0;
         fdopen_slots[i].fd=-1;
         close(fd);
         return 1;
      }
   return 0;
}

int ics_fdopen_fd(FILE *stream)
{
   int i;
   for (i=0;i<FDOPEN_SLOTS;i++)
      if (fdopen_slots[i].stream==stream)
         return fdopen_slots[i].fd;
   return -1;
}

FILE *fdopen(int fd, const char *mode)
{
   long p;
   int i;
   (void)mode;
   if (fd == 0) return stdin;
   if (fd == 1) return stdout;
   if (fd == 2) return stderr;
   for (i=0;i<FDOPEN_SLOTS;i++)
      if (fdopen_slots[i].stream && fdopen_slots[i].fd==fd) {
         errno=22;
         return 0;
      }
   /* Kernel file_PCB* so DEX fwrite/fclose still work (TinyCC ELF output). */
   p = (long)dexsdk_systemcall(FXN_FDFILE, fd, 0, 0, 0, 0);
   if (!p)
      return 0;
   for (i=0;i<FDOPEN_SLOTS;i++)
      if (fdopen_slots[i].stream==(FILE *)p) {
         errno=16;
         return 0;
      }
   for (i=0;i<FDOPEN_SLOTS;i++)
      if (!fdopen_slots[i].stream) {
         fdopen_slots[i].stream=(FILE *)p;
         fdopen_slots[i].fd=fd;
         return (FILE *)p;
      }
   errno=24;
   return 0;
}

int unlink(const char *path)
{
   return remove((char *)path);
}

int rmdir(const char *path)
{
   return remove((char *)path);
}

char *getcwd(char *buf, size_t size)
{
   static char tmp[256];
   if (!buf) {
      buf = tmp;
      size = sizeof(tmp);
   }
   if (!dexsdk_systemcall(FXN_GETCWD, (long)buf, (int)size, 0, 0, 0))
      return 0;
   return buf;
}

int chdir(const char *path)
{
   return dexsdk_systemcall(FXN_CHDIR, (long)path, 0, 0, 0, 0) ? 0 : -1;
}

int isatty(int fd)
{
   return (fd == 0 || fd == 1 || fd == 2);
}

int access(const char *path, int mode)
{
   FILE *f = fopen(path, "r");
   (void)mode;
   if (!f) {
      errno = 2;
      return -1;
   }
   fclose(f);
   return 0;
}

int chmod(const char *path, mode_t mode)
{
   (void)path;
   (void)mode;
   return 0;
}

int rename(const char *oldpath, const char *newpath)
{
   int r = dexsdk_systemcall(FXN_RENAME, (long)oldpath, (long)newpath, 0, 0, 0);
   return r ? 0 : -1;
}

struct sdk_mm_map {
   struct sdk_mm_map *next;
   char *orig;
   char *base;
   size_t length;
   size_t page_count;
   unsigned long *free_pages;
};

static struct sdk_mm_map *sdk_mm_maps;

static int
sdk_mm_all_free(const struct sdk_mm_map *m)
{
   size_t i;
   for (i = 0; i < m->page_count; i++)
      if (!(m->free_pages[i >> 6] & (1UL << (i & 63))))
         return 0;
   return 1;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
   size_t page = 4096;
   size_t pages;
   size_t rlen;
   size_t bitmap_words;
   char *orig;
   char *base;
   struct sdk_mm_map *m;
   ssize_t n;
   (void)addr;
   (void)prot;

   if (length == 0)
      return MAP_FAILED;

   /* Anonymous maps go through the kernel so they live in a VA window
      disjoint from the sbrk malloc arena (required by GCC's zone GC). */
   if ((flags & MAP_ANONYMOUS) || fd < 0) {
      void *p = (void *)dexsdk_systemcall(FXN_MMAP, (long)length, (long)flags, 0, 0, 0);
      if (!p || p == (void *)(long)-1)
         return MAP_FAILED;
      return p;
   }

   pages = (length + page - 1) / page;
   rlen = pages * page;
   orig = (char *)malloc(rlen + page);
   if (!orig)
      return MAP_FAILED;
   base = (char *)((((size_t)orig + page - 1) & ~(size_t)(page - 1)));

   if ((flags & MAP_ANONYMOUS) || fd < 0)
      memset(base, 0, rlen);
   else {
      n = pread(fd, base, rlen, offset);
      if (n < 0) {
         free(orig);
         return MAP_FAILED;
      }
      if ((size_t)n < rlen)
         memset(base + n, 0, rlen - (size_t)n);
   }

   bitmap_words = (pages + 63) / 64;
   m = (struct sdk_mm_map *)malloc(sizeof(*m));
   if (!m) {
      free(orig);
      return MAP_FAILED;
   }
   m->free_pages = (unsigned long *)malloc(bitmap_words * sizeof(unsigned long));
   if (!m->free_pages) {
      free(m);
      free(orig);
      return MAP_FAILED;
   }
   memset(m->free_pages, 0, bitmap_words * sizeof(unsigned long));
   m->next = sdk_mm_maps;
   m->orig = orig;
   m->base = base;
   m->length = rlen;
   m->page_count = pages;
   sdk_mm_maps = m;
   return base;
}

int munmap(void *addr, size_t length)
{
   char *a = (char *)addr;
   char *end;
   struct sdk_mm_map *m;
   struct sdk_mm_map *prev;
   size_t first;
   size_t last;
   size_t i;

   if (length == 0)
      return 0;

   if (dexsdk_systemcall(FXN_MUNMAP, (long)addr, (long)length, 0, 0, 0) == 0)
      return 0;

   end = a + length;

   m = sdk_mm_maps;
   prev = 0;
   while (m) {
      if (a >= m->base && end <= m->base + m->length) {
         first = (size_t)(a - m->base) / 4096;
         last = (size_t)(end - 1 - m->base) / 4096;
         for (i = first; i <= last; i++)
            m->free_pages[i >> 6] |= (1UL << (i & 63));
         if (sdk_mm_all_free(m)) {
            if (prev)
               prev->next = m->next;
            else
               sdk_mm_maps = m->next;
            free(m->free_pages);
            free(m->orig);
            free(m);
         }
         return 0;
      }
      prev = m;
      m = m->next;
   }
   return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
   (void)addr; (void)len; (void)prot;
   return 0;
}

double ldexp(double x, int exp)
{
   while (exp > 0) { x *= 2.0; exp--; }
   while (exp < 0) { x /= 2.0; exp++; }
   return x;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
   return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
   return (unsigned long long)strtoul(nptr, endptr, base);
}

int sscanf(const char *str, const char *fmt, ...)
{
   /* Minimal: "%d.%d" used by tcc_new() for TCC_VERSION. */
   va_list ap;
   int *a, *b;
   (void)fmt;
   va_start(ap, fmt);
   a = va_arg(ap, int *);
   b = va_arg(ap, int *);
   va_end(ap);
   *a = 0; *b = 0;
   if (str) {
      *a = atoi(str);
      while (*str && *str != '.') str++;
      if (*str == '.') *b = atoi(str + 1);
   }
   return 2;
}

int execvp(const char *file, char *const argv[])
{
   char path[256];
   int i, n;

   if (!file || !file[0]) {
      errno = ENOENT;
      return -1;
   }
   for (i = 0; file[i]; i++)
      if (file[i] == '/')
         return execv(file, argv);

   n = 0;
   memcpy(path, "/icsos/apps/", 12);
   n = 12;
   for (i = 0; file[i] && n < 255; i++)
      path[n++] = file[i];
   path[n] = 0;
   return execv(path, argv);
}

static void posix_join_argv(char *dst, int max, char *const argv[])
{
   int n = 0, i;

   dst[0] = 0;
   if (!argv || max <= 1)
      return;
   for (i = 0; argv[i]; i++) {
      int l = (int)strlen(argv[i]);
      if (n && n + 1 < max)
         dst[n++] = ' ';
      if (n + l >= max)
         l = max - n - 1;
      if (l <= 0)
         break;
      memcpy(dst + n, argv[i], (size_t)l);
      n += l;
      dst[n] = 0;
      if (n >= max - 1)
         break;
   }
}

pid_t waitpid(pid_t pid, int *status, int options)
{
   return (pid_t)ics_sys(FXN_WAITPID, pid, (long)status, options, 0, 0);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
   char cmd[4096];
   long r;

   (void)file_actions;
   (void)attrp;
   (void)envp;
   if (!path) {
      errno = EINVAL;
      return EINVAL;
   }
   posix_join_argv(cmd, (int)sizeof(cmd), argv);
   if (!cmd[0]) {
      int n = (int)strlen(path);
      if (n >= (int)sizeof(cmd))
         n = (int)sizeof(cmd) - 1;
      memcpy(cmd, path, (size_t)n);
      cmd[n] = 0;
   }
   r = ics_sys(FXN_SPAWN, (long)path, (long)cmd, 0, 0, 0);
   if (r < 0)
      return errno;
   if (pid)
      *pid = (pid_t)r;
   return 0;
}

int execv(const char *path, char *const argv[])
{
   char cmd[4096];

   if (!path) {
      errno = EINVAL;
      return -1;
   }
   posix_join_argv(cmd, (int)sizeof(cmd), argv);
   if (!cmd[0]) {
      int n = (int)strlen(path);
      if (n >= (int)sizeof(cmd))
         n = (int)sizeof(cmd) - 1;
      memcpy(cmd, path, (size_t)n);
      cmd[n] = 0;
   }
   ics_sys(FXN_EXECVE, (long)path, (long)cmd, 0, 0, 0);
   return -1;
}

/* Convert days-since-1970-01-01 to a civil date (Howard Hinnant's algorithm).
   Works for the whole representable range, including pre-epoch negatives. */
static void icsos_civil_from_days(long z, int *year, int *month, int *day)
{
   z += 719468;
   {
      long era = (z >= 0 ? z : z - 146096) / 146097;
      long doe = z - era * 146097;              /* [0, 146096] */
      long yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; /* [0,399] */
      long y = yoe + era * 400;
      long doy = doe - (365*yoe + yoe/4 - yoe/100); /* [0,365] */
      long mp = (5*doy + 2)/153;                  /* [0,11] */
      long d = doy - (153*mp+2)/5 + 1;            /* [1,31] */
      long m = mp + (mp < 10 ? 3 : -9);           /* [1,12] */
      *year = (int)(y + (m <= 2));
      *month = (int)m;
      *day = (int)d;
   }
}

/* localtime: the ICS-OS kernel clock is UTC; TZ is not implemented, so local
   time is reported as UTC (tm_isdst=0). */
struct tm *localtime(const time_t *t)
{
   static struct tm tm;
   time_t v = t ? *t : 0;
   long days = v / 86400;
   long rem = v - days * 86400;
   int year, month, day;

   if (rem < 0) { rem += 86400; days--; }
   icsos_civil_from_days(days, &year, &month, &day);

   memset(&tm, 0, sizeof(tm));
   tm.tm_sec  = (int)(rem % 60);
   tm.tm_min  = (int)((rem / 60) % 60);
   tm.tm_hour = (int)(rem / 3600);
   tm.tm_mday = day;
   tm.tm_mon  = month - 1;
   tm.tm_year = year - 1900;
   tm.tm_wday = (int)(((days % 7) + 7) % 7 + 4) % 7;  /* 1970-01-01 was a Thursday */
   {
      static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
      int leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
      tm.tm_yday = cum[month - 1] + day - 1 + ((leap && month > 2) ? 1 : 0);
   }
   tm.tm_isdst = 0;
   return &tm;
}

static const char icsos_months[12][3] = {
   "Jan","Feb","Mar","Apr","May","Jun",
   "Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char icsos_wdays[7][3] = {
   "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
};

/* strftime: supports the subset GAS uses for its listing header
   ("%Y-%m-%dT%H:%M:%S.000%z") plus the common date conversions. */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
   size_t o = 0;

   if (!tm || max == 0)
      return 0;
   while (*fmt) {
      char c = *fmt++;
      if (c != '%') {
         if (o + 1 < max) s[o++] = c;
         continue;
      }
      c = *fmt++;
      switch (c) {
      case 'Y': { int n = sprintf(s+o, "%04d", tm->tm_year + 1900); o += (size_t)n; } break;
      case 'y': { int n = sprintf(s+o, "%02d", (tm->tm_year + 1900) % 100); o += (size_t)n; } break;
      case 'm': { int n = sprintf(s+o, "%02d", tm->tm_mon + 1); o += (size_t)n; } break;
      case 'd': { int n = sprintf(s+o, "%02d", tm->tm_mday); o += (size_t)n; } break;
      case 'H': { int n = sprintf(s+o, "%02d", tm->tm_hour); o += (size_t)n; } break;
      case 'M': { int n = sprintf(s+o, "%02d", tm->tm_min); o += (size_t)n; } break;
      case 'S': { int n = sprintf(s+o, "%02d", tm->tm_sec); o += (size_t)n; } break;
      case 'B': if (o+3 < max) { memcpy(s+o, icsos_months[tm->tm_mon], 3); o += 3; } break;
      case 'b': if (o+3 < max) { memcpy(s+o, icsos_months[tm->tm_mon], 3); o += 3; } break;
      case 'A': if (o+3 < max) { memcpy(s+o, icsos_wdays[tm->tm_wday], 3); o += 3; } break;
      case 'a': if (o+3 < max) { memcpy(s+o, icsos_wdays[tm->tm_wday], 3); o += 3; } break;
      case 'Z': if (o+3 < max) { memcpy(s+o, "UTC", 3); o += 3; } break;
      case 'z': if (o+5 < max) { memcpy(s+o, "+0000", 5); o += 5; } break;
      case 'n': if (o+1 < max) s[o++] = '\n'; break;
      case 't': if (o+1 < max) s[o++] = '\t'; break;
      case '%': if (o+1 < max) s[o++] = '%'; break;
      default:  if (o+1 < max) s[o++] = '%';
                if (o+1 < max) s[o++] = c;
                break;
      }
   }
   if (o < max) s[o] = '\0';
   return o;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
   int t;
   (void)tz;
   t = dexsdk_systemcall(FXN_TIME, 0, 0, 0, 0, 0);
   if (tv) {
      tv->tv_sec = t;
      tv->tv_usec = 0;
   }
   return 0;
}

time_t time(time_t *t)
{
   time_t v = (time_t)dexsdk_systemcall(FXN_TIME, 0, 0, 0, 0, 0);
   if (t) *t = v;
   return v;
}

clock_t clock(void)
{
    return (clock_t)time(0);
}

/*
 * Termios / tty control (syscalls 0xC0-0xC3). The struct termios layout is
 * passed by pointer and read directly by the kernel, so it must match
 * kernel/console/tty_tc.c byte-for-byte.
 */
int tcgetattr(int fd, struct termios *termios_p)
{
    if (!termios_p) {
       errno = 22;
       return -1;
    }
    return (int)ics_sys(FXN_TCGETATTR, fd, (long)termios_p, 0, 0, 0);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    if (!termios_p) {
       errno = 22;
       return -1;
    }
    return (int)ics_sys(FXN_TCSETATTR, fd, optional_actions, (long)termios_p, 0, 0);
}

int tcflush(int fd, int queue_selector)
{
    return (int)ics_sys(FXN_TCFUSH, fd, queue_selector, 0, 0, 0);
}

/*
 * ioctl for tty window size (syscall 0xC3).
 */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    long arg;
    va_start(ap, request);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)ics_sys(FXN_TTYIOCTL, fd, (long)request, arg, 0, 0);
}

/*
 * select(2) (syscall 0xC4). The kernel mutates the fd_set words in place and
 * returns the ready count (or -EINTR if a signal arrived).
 */
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
    long tv = 0;
    if (timeout)
       tv = (long)timeout;
    return (int)ics_sys(FXN_TTYSELECT, nfds,
                        (long)readfds, (long)writefds,
                        (long)exceptfds, tv);
}

/*
 * poll(2) on top of select(2).
 */
int poll(struct pollfd *fds, unsigned int nfds, int timeout)
{
    fd_set rfds, wfds, efds;
    struct timeval tv;
    int maxfd = 1;
    int i, n;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    for (i = 0; i < (int)nfds; i++) {
       if (fds[i].fd < 0) continue;
       if (fds[i].fd + 1 > maxfd) maxfd = fds[i].fd + 1;
       if (fds[i].events & (POLLIN | POLLPRI)) FD_SET(fds[i].fd, &rfds);
       if (fds[i].events & POLLOUT)           FD_SET(fds[i].fd, &wfds);
       FD_SET(fds[i].fd, &efds);
    }
    if (timeout < 0) {
       /* blocking: pass NULL timeout */
       n = select(maxfd, &rfds, &wfds, &efds, 0);
    } else {
       tv.tv_sec = timeout / 1000;
       tv.tv_usec = (timeout % 1000) * 1000;
       n = select(maxfd, &rfds, &wfds, &efds, &tv);
    }
    if (n < 0)
       return -1;
    for (i = 0; i < (int)nfds; i++) {
       short r = 0;
       if (fds[i].fd < 0) { fds[i].revents = 0; continue; }
       if (FD_ISSET(fds[i].fd, &rfds)) r |= POLLIN;
       if (FD_ISSET(fds[i].fd, &wfds)) r |= POLLOUT;
       if (FD_ISSET(fds[i].fd, &efds)) r |= POLLHUP;
       fds[i].revents = r;
    }
    return n;
}

/*
 * Monotonic millisecond clock (syscall 0x96 getprecisetime).
 */
int clock_gettime(int clockid, struct timespec *tp)
{
    long ms;
    if (!tp) {
       errno = 22;
       return -1;
    }
    ms = (long)dexsdk_systemcall(FXN_PRECISTIME, 0, 0, 0, 0, 0);
    if (clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME) {
       tp->tv_sec = ms / 1000;
       tp->tv_nsec = (ms % 1000) * 1000000L;
    } else {
       errno = 22;
       return -1;
    }
    return 0;
}

/*
 * Microsecond sleep. POSIX permits sleeping longer than requested, so the
 * sub-millisecond remainder is rounded up to a whole millisecond and the
 * kernel delay syscall (0x9B) is used: it halts the CPU and waits for the
 * tick interrupt, yielding to other runnable tasks instead of busy-spinning.
 */
int usleep(useconds_t usec)
{
    unsigned long ms;
    if (usec == 0)
       return 0;
    ms = (unsigned long)((usec + 999) / 1000);
    dexsdk_systemcall(FXN_DELAY, ms, 0, 0, 0, 0);
    return 0;
}

/*
 * ftruncate is not supported by the kernel VFS; report ENOSYS so callers can
 * fall back (vim only needs it for certain file writes).
 */
int ftruncate(int fd, off_t length)
{
    (void)fd;
    (void)length;
    errno = 38;
    return -1;
}

int fstat(int fd, struct stat *buf)
{
   if (!buf) {
      errno = 22;
      return -1;
   }
   return (int)ics_sys(FXN_FSTATFD, fd, (long)buf, 0, 0, 0);
}

int stat(const char *path, struct stat *buf)
{
   int fd, r;
   fd = open(path, O_RDONLY);
   if (fd < 0)
      return -1;
   r = fstat(fd, buf);
   close(fd);
   return r;
}

void abort(void)
{
   exit(1);
}

void *calloc(size_t nmemb, size_t size)
{
   size_t n;
   void *p;
   if (size != 0 && nmemb > ((size_t)-1) / size)
      return 0;
   n = nmemb * size;
   p = malloc(n);
   if (p) memset(p, 0, n);
   return p;
}

char *strdup(const char *s)
{
   size_t n = strlen(s) + 1;
   char *p = (char *)malloc(n);
   if (p) memcpy(p, s, n);
   return p;
}

char *strerror(int errnum)
{
   (void)errnum;
   return "error";
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

int system(const char *cmd)
{
   (void)cmd;
   return -1;
}

void _exit(int status)
{
   exit(status);
}

static int is_digit_base(int c, int base)
{
   int v;
   if (c >= '0' && c <= '9') v = c - '0';
   else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
   else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
   else return -1;
   return (v < base) ? v : -1;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
   const char *s = nptr;
   unsigned long acc = 0;
   int v, neg = 0;
   while (isspace((unsigned char)*s)) s++;
   if (*s == '+' || *s == '-') {
      neg = (*s == '-');
      s++;
   }
   if (base == 0) {
      if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
      else if (s[0] == '0') base = 8;
      else base = 10;
   } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s += 2;
   }
   while ((v = is_digit_base((unsigned char)*s, base)) >= 0) {
      acc = acc * (unsigned long)base + (unsigned long)v;
      s++;
   }
   if (endptr) *endptr = (char *)s;
   return neg ? (unsigned long)(-(long)acc) : acc;
}

long strtol(const char *nptr, char **endptr, int base)
{
   return (long)strtoul(nptr, endptr, base);
}

long atol(const char *s)
{
   return strtol(s, 0, 10);
}

double strtod(const char *nptr, char **endptr)
{
   const char *s = nptr;
   int neg = 0;
   double acc = 0.0, frac = 0.0, div = 1.0;
   while (isspace((unsigned char)*s)) s++;
   if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
   while (*s >= '0' && *s <= '9') {
      acc = acc * 10.0 + (*s - '0');
      s++;
   }
   if (*s == '.') {
      s++;
      while (*s >= '0' && *s <= '9') {
         div *= 10.0;
         frac = frac * 10.0 + (*s - '0');
         s++;
      }
      acc += frac / div;
   }
   if (*s == 'e' || *s == 'E') {
      int e = 0, eneg = 0;
      s++;
      if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
      while (*s >= '0' && *s <= '9') { e = e * 10 + (*s - '0'); s++; }
      if (eneg) while (e--) acc /= 10.0;
      else while (e--) acc *= 10.0;
   }
   if (endptr) *endptr = (char *)s;
   return neg ? -acc : acc;
}

float strtof(const char *nptr, char **endptr)
{
   return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr)
{
   return (long double)strtod(nptr, endptr);
}

double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x)
{
   long i = (long)x;
   if (x < 0 && (double)i != x) i--;
   return (double)i;
}
double ceil(double x)
{
   long i = (long)x;
   if (x > 0 && (double)i != x) i++;
   return (double)i;
}

/*
 * Base-10 logarithm for the freestanding target (no libm). Scale x into
 * [1.0, 10.0) to recover the integer part, then interpolate the fractional
 * part over a per-decade table (linear error is << 1 over each unit interval,
 * so floor(log10(x)) is always exact). Loops are bounded so non-finite input
 * (inf/NaN) returns the integer exponent without hanging.
 */
double log10(double x)
{
   static const double tbl[9] = {
      0.0, 0.30103, 0.47712, 0.60206, 0.69897,
      0.77815, 0.84510, 0.90309, 0.95424
   };
   long k, n, i;
   double t, frac, v0, v1;
   if (x <= 0.0)
      return -1.0e300;
   t = x;
   k = 0;
   for (i = 0; i < 700 && t >= 10.0; i++) { t /= 10.0; k++; }
   for (i = 0; i < 700 && t < 1.0; i++) { t *= 10.0; k--; }
   if (!(t >= 1.0 && t < 10.0))
      return (double)k;
   n = (long)t;
   if (n < 1) n = 1;
   if (n > 9) n = 9;
   frac = t - (double)n;
   v0 = tbl[n - 1];
   v1 = (n == 9) ? 1.0 : tbl[n];
   return (double)k + v0 + frac * (v1 - v0);
}

int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isgraph(int c) { return c > 0x20 && c < 0x7f; }
int iscntrl(int c) { return (unsigned)c < 0x20 || c == 0x7f; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && !isspace(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

int puts(const char *s)
{
   printf("%s\n", s);
   return 0;
}

int putchar(int c)
{
   charputc((char)c);
   return c;
}

void rewind(FILE *f)
{
   fseek(f, 0, SEEK_SET);
}

int fprintf(FILE *f, const char *fmt, ...)
{
   va_list ap;
   int r;
   va_start(ap, fmt);
   r = vfprintf(f, fmt, ap);
   va_end(ap);
   return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
   char buf[1024];
   int n;
   va_list ap2;
   char *big;

   if (f == stdout || f == stderr)
      return vprintf(fmt, ap);

   va_copy(ap2, ap);
   n = vsnprintf(buf, sizeof(buf), fmt, ap);
   if (n < 0) {
      va_end(ap2);
      return n;
   }
   if ((size_t)n < sizeof(buf)) {
      va_end(ap2);
      return (int)fwrite(buf, 1, (size_t)n, f);
   }
   big = (char *)malloc((size_t)n + 1);
   if (!big) {
      va_end(ap2);
      return -1;
   }
   vsnprintf(big, (size_t)n + 1, fmt, ap2);
   va_end(ap2);
   n = (int)fwrite(big, 1, (size_t)n, f);
   free(big);
   return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
   va_list ap;
   int r;
   va_start(ap, fmt);
   r = vsnprintf(buf, n, fmt, ap);
   va_end(ap);
   return r;
}

static void qsort_swap(char *a, char *b, size_t n)
{
   while (n--) {
      char t = *a;
      *a++ = *b;
      *b++ = t;
   }
}

static void qsort_rec(char *base, int left, int right, size_t size,
                      int (*cmp)(const void *, const void *))
{
   int i, last;
   if (left >= right) return;
   qsort_swap(base + left * size, base + ((left + right) / 2) * size, size);
   last = left;
   for (i = left + 1; i <= right; i++) {
      if (cmp(base + i * size, base + left * size) < 0) {
         last++;
         qsort_swap(base + last * size, base + i * size, size);
      }
   }
   qsort_swap(base + left * size, base + last * size, size);
   qsort_rec(base, left, last - 1, size, cmp);
   qsort_rec(base, last + 1, right, size, cmp);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
   if (nmemb < 2 || size == 0) return;
   qsort_rec((char *)base, 0, (int)nmemb - 1, size, compar);
}

sighandler_t signal(int sig, sighandler_t handler)
{
   (void)sig;
   return handler;
}

int fseek(FILE *f, long off, int whence)
{
   int fd=ics_fdopen_fd(f);
   fflush(f);
   if (fd >= 0)
      return lseek(fd,off,whence) < 0 ? -1 : 0;
   dexsdk_systemcall(0x41, (long)f, (long)off, whence, 0, 0);
   return 0;
}

char *getenv(const char *name)
{
   static char buf[512];
   if (!dexsdk_systemcall(0x9F, (long)name, (long)buf, 0, 0, 0))
      return 0;
   return buf;
}

int machine_reboot(void)
{
   return dexsdk_systemcall(FXN_REBOOT, 0, 0, 0, 0, 0);
}

int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
   return (int)ics_sys(FXN_URING_SETUP, (long)entries, (long)p, 0, 0, 0);
}

int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
   return (int)ics_sys(FXN_URING_ENTER, fd, (long)to_submit,
                       (long)min_complete, (long)flags, 0);
}

static uint32_t uring_load_acquire(uint32_t *p)
{
   return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void uring_store_release(uint32_t *p, uint32_t v)
{
   __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags)
{
   struct io_uring_params p;
   char *base;
   int fd;
   if (!ring)
      return -1;
   memset(&p, 0, sizeof(p));
   p.flags = flags;
   fd = io_uring_setup(entries, &p);
   if (fd < 0)
      return -1;
   base = (char *)(unsigned long)p.sq_off.user_addr;
   ring->ring_fd = fd;
   ring->sq_entries = p.sq_entries;
   ring->cq_entries = p.cq_entries;
   ring->sq_head = (uint32_t *)(base + p.sq_off.head);
   ring->sq_tail = (uint32_t *)(base + p.sq_off.tail);
   ring->sq_mask = (uint32_t *)(base + p.sq_off.ring_mask);
   ring->sq_array = (uint32_t *)(base + p.sq_off.array);
   ring->sqes = (struct io_uring_sqe *)(base + p.resv[0]);
   ring->cq_head = (uint32_t *)(base + p.cq_off.head);
   ring->cq_tail = (uint32_t *)(base + p.cq_off.tail);
   ring->cq_mask = (uint32_t *)(base + p.cq_off.ring_mask);
   ring->cqes = (struct io_uring_cqe *)(base + p.cq_off.cqes);
   ring->sqe_tail = uring_load_acquire(ring->sq_tail);
   return 0;
}

void io_uring_queue_exit(struct io_uring *ring)
{
   if (ring && ring->ring_fd >= 0) {
      close(ring->ring_fd);
      ring->ring_fd = -1;
   }
}

struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
   unsigned tail, head, mask, idx;
   if (!ring)
      return 0;
   tail = ring->sqe_tail;
   head = uring_load_acquire(ring->sq_head);
   mask = *ring->sq_mask;
   if (tail - head >= ring->sq_entries)
      return 0;
   idx = ring->sq_array[tail & mask];
   memset(&ring->sqes[idx], 0, sizeof(struct io_uring_sqe));
   ring->sqe_tail = tail + 1;
   return &ring->sqes[idx];
}

int io_uring_submit(struct io_uring *ring)
{
   unsigned head, tail;
   if (!ring)
      return -1;
   head = uring_load_acquire(ring->sq_head);
   tail = ring->sqe_tail;
   if (tail == head)
      return 0;
   uring_store_release(ring->sq_tail, tail);
   return io_uring_enter(ring->ring_fd, tail - head, 0, 0);
}

int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr)
{
   unsigned head, tail;
   if (!ring)
      return -1;
   head = uring_load_acquire(ring->sq_head);
   tail = ring->sqe_tail;
   uring_store_release(ring->sq_tail, tail);
   return io_uring_enter(ring->ring_fd, tail - head, wait_nr,
                         IORING_ENTER_GETEVENTS);
}

int io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe)
{
   unsigned head, tail, mask;
   if (!ring || !cqe)
      return -1;
   head = uring_load_acquire(ring->cq_head);
   tail = uring_load_acquire(ring->cq_tail);
   if (head == tail) {
      if (io_uring_enter(ring->ring_fd, 0, 1, IORING_ENTER_GETEVENTS) < 0)
         return -1;
      head = uring_load_acquire(ring->cq_head);
      tail = uring_load_acquire(ring->cq_tail);
      if (head == tail)
         return -1;
   }
   mask = *ring->cq_mask;
   *cqe = &ring->cqes[head & mask];
   return 0;
}

void io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe)
{
   (void)cqe;
   if (ring)
      uring_store_release(ring->cq_head,
                 uring_load_acquire(ring->cq_head) + 1);
}

static char *ics_env_empty[] = { 0 };
char **environ = ics_env_empty;

DIR *opendir(const char *path)
{
   DIR *d;
   char *buf;
   long n;

   if (!path) {
      errno = EINVAL;
      return 0;
   }
   buf = (char *)malloc(4096);
   if (!buf)
      return 0;
   n = ics_sys(FXN_GETDENTS, (long)path, (long)buf, 4096, 0, 0);
   if (n < 0) {
      free(buf);
      return 0;
   }
   d = (DIR *)malloc(sizeof(DIR));
   if (!d) {
      free(buf);
      return 0;
   }
   d->packed = buf;
   d->off = 0;
   return d;
}

struct dirent *readdir(DIR *dir)
{
   const char *s;
   int n;

   if (!dir || !dir->packed)
      return 0;
   s = dir->packed + dir->off;
   if (s[0] == 0)
      return 0;
   n = (int)strlen(s);
   dir->de.d_ino = 1;
   if (n > 255)
      n = 255;
   memcpy(dir->de.d_name, s, (size_t)n);
   dir->de.d_name[n] = 0;
   dir->off += (int)strlen(s) + 1;
   return &dir->de;
}

int closedir(DIR *dir)
{
   if (!dir)
      return 0;
   free(dir->packed);
   free(dir);
   return 0;
}

int umask(int mask)
{
   (void)mask;
   return 022;
}

int getuid(void) { return 0; }
int geteuid(void) { return 0; }
int getgid(void) { return 0; }
int getegid(void) { return 0; }

/*
 * No uname/hostname facility in the kernel; report a fixed identity. The name
 * is always NUL-terminated and never exceeds len bytes.
 */
int gethostname(char *name, size_t len)
{
    static const char host[] = "icsos";
    size_t i;
    if (!name || len == 0) {
       errno = 22;
       return -1;
    }
    for (i = 0; host[i] != '\0' && i < len - 1; i++)
       name[i] = host[i];
    name[i] = '\0';
    return 0;
}

int dup2(int oldfd, int newfd)
{
   (void)oldfd;
   (void)newfd;
   errno = ENOSYS;
   return -1;
}

void perror(const char *s)
{
   if (s && s[0])
      printf("%s: errno=%d\n", s, errno);
   else
      printf("errno=%d\n", errno);
}

int pipe(int fd[2])
{
   (void)fd;
   errno = ENOSYS;
   return -1;
}

int flock(int fd, int op)
{
   (void)fd;
   (void)op;
   return 0;
}

int mkstemp(char *template)
{
   static int n;
   int i, len, fd;
   if (!template)
      return -1;
   len = (int)strlen(template);
   n++;
   for (i = len - 1; i >= 0 && i >= len - 6; i--) {
      if (template[i] == 'X')
         template[i] = '0' + (n % 10);
      n /= 10;
      if (n == 0)
         n = 1;
   }
   fd = open(template, O_RDWR | O_CREAT | O_TRUNC, 0600);
   return fd;
}

struct passwd *getpwnam(const char *name)
{
   (void)name;
   return 0;
}

struct passwd *getpwuid(uid_t uid)
{
   (void)uid;
   return 0;
}

int atexit(void (*fn)(void))
{
   (void)fn;
   return 0;
}

int kill(int pid, int sig)
{
   if (pid == getpid() || pid <= 0) {
      _exit(sig ? (128 + (sig & 127)) : 1);
   }
   (void)sig;
   return 0;
}

int fcntl(int fd, int cmd, ...)
{
   (void)fd;
   (void)cmd;
   return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
   (void)f;
   (void)buf;
   (void)mode;
   (void)size;
   return 0;
}

int fileno(FILE *f)
{
   if (!f)
      return -1;
   if (f == stdout)
      return 1;
   if (f == stderr)
      return 2;
   if (f == stdin)
      return 0;
   return 3;
}

int putc(int c, FILE *f)
{
   return fputc(c, f);
}

int ferror(FILE *f)
{
   (void)f;
   return 0;
}

double atof(const char *s)
{
   return strtod(s, 0);
}

char *ctime(const time_t *t)
{
   static char buf[32];
   (void)t;
   strcpy(buf, "Thu Jan  1 00:00:00 1970\n");
   return buf;
}

int putenv(char *string)
{
   char *eq;
   char name[128];
   int n;
   if (!string)
      return -1;
   eq = strchr(string, '=');
   if (!eq)
      return setenv(string, "", 1);
   n = (int)(eq - string);
   if (n <= 0 || n >= (int)sizeof(name))
      return -1;
   memcpy(name, string, (size_t)n);
   name[n] = 0;
   return setenv(name, eq + 1, 1);
}

char *getlogin(void)
{
   return 0;
}

int getloadavg(double loadavg[], int nelem)
{
   int i;
   if (!loadavg || nelem <= 0)
      return -1;
   for (i = 0; i < nelem && i < 3; i++)
      loadavg[i] = 0.0;
   return i > 0 ? i : -1;
}

pid_t vfork(void)
{
   errno = ENOSYS;
   return -1;
}

/* TinyCC emits calls to __builtin_alloca after host tcc -E; gcc will not
 * let us define that name. Stage script rewrites it to icsos_alloca. */
void *icsos_alloca(unsigned long n)
{
   if (n == 0)
      n = 1;
   return malloc(n);
}

/*
 * Path / memory / resource queries. Added for the in-OS toolchain
 * (GNU binutils: libbfd getpagesize, ld realpath, libiberty pathconf /
 * sysconf / getrlimit). Userspace-only: no new syscalls.
 */
#include <sys/resource.h>
#include <sys/param.h>

#define ICS_PAGE_SIZE 4096

int getpagesize(void)
{
   return ICS_PAGE_SIZE;
}

long sysconf(int name)
{
   switch (name) {
   case _SC_PAGESIZE:
      return ICS_PAGE_SIZE;
   case _SC_CLK_TCK:
      return 100;
   case _SC_NPROCESSORS_CONF:
   case _SC_NPROCESSORS_ONLN:
      return 1;   /* conservative; no user-visible cpu count syscall */
   default:
      return -1;
   }
}

long pathconf(const char *path, int name)
{
   (void)path;
   switch (name) {
   case _PC_PATH_MAX:
      return PATH_MAX;
   case _PC_NAME_MAX:
      return NAME_MAX;
   default:
      return -1;
   }
}

/* Canonicalise a path without following symlinks beyond the final
   component (ICS-OS has no symlinks yet). Resolves "." and ".." lexically. */
char *realpath(const char *path, char *resolved)
{
   char *out, *w;
   const char *p;
   size_t len;
   int nseg, i, segs[64];
   char names[64][256];
   int abs;

   if (!path || !*path) { errno = ENOENT; return 0; }
   abs = (path[0] == '/');

   nseg = 0;
   p = path;
   while (nseg < 64) {
      while (*p == '/') p++;
      if (!*p) break;
      w = names[nseg];
      len = 0;
      while (*p && *p != '/' && len < 255) w[len++] = *p++;
      w[len] = 0;
      if (len == 0) continue;
      if (len == 1 && w[0] == '.') continue;
      if (len == 2 && w[0] == '.' && w[1] == '.') {
         if (nseg > 0) nseg--;
         continue;
      }
      names[nseg][255] = 0;
      nseg++;
   }
   if (nseg >= 64) { errno = ENAMETOOLONG; return 0; }

   out = resolved ? resolved : (char *)malloc(PATH_MAX);
   if (!out) { errno = ENOMEM; return 0; }

   if (abs) {
       out[0] = '/';
       w = out + 1;
    } else {
       out[0] = 0;
       w = out;
    }
    for (i = 0; i < nseg; i++) {
       if (i > 0) *w++ = '/';
       len = strlen(names[i]);
       memcpy(w, names[i], len);
       w += len;
    }
    *w = 0;
    if (out[0] == 0) out[0] = '/';
   return out;
}

/* Per-process resource limits. ICS-OS keeps them in the SDK (no kernel
   backing); defaults are "unlimited" so tools that probe them (binutils
   stack-limit) behave. */
static struct rlimit ics_rlimits[16];
static int ics_rlimits_init = 0;

static void rlimits_init(void)
{
   int i;
   for (i = 0; i < 16; i++) {
      ics_rlimits[i].rlim_cur = RLIM_INFINITY;
      ics_rlimits[i].rlim_max = RLIM_INFINITY;
   }
   ics_rlimits[RLIMIT_NOFILE].rlim_cur = 256;
   ics_rlimits[RLIMIT_NOFILE].rlim_max = 256;
   ics_rlimits_init = 1;
}

int getrlimit(int resource, struct rlimit *rlim)
{
   if (!ics_rlimits_init) rlimits_init();
   if (!rlim || resource < 0 || resource > 15) { errno = EINVAL; return -1; }
   *rlim = ics_rlimits[resource];
   return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
   if (!ics_rlimits_init) rlimits_init();
   if (!rlim || resource < 0 || resource > 15) { errno = EINVAL; return -1; }
   ics_rlimits[resource] = *rlim;
   return 0;
}

/* POSIX signal sets. ICS-OS has no per-process hardware signal delivery;
   the set is tracked in the SDK so mask APIs are functional for binutils
   (libiberty sigsetmask.c, ld job control). */
static sigset_t ics_sigmask;

void sigemptyset(sigset_t *set)
{
   if (set) memset(set, 0, sizeof(*set));
}
void sigfillset(sigset_t *set)
{
   if (set) memset(set, 0xff, sizeof(*set));
}
void sigaddset(sigset_t *set, int signum)
{
   if (set && signum > 0 && signum <= 128)
      set->bits[signum / 64] |= 1UL << (signum % 64);
}
void sigdelset(sigset_t *set, int signum)
{
   if (set && signum > 0 && signum <= 128)
      set->bits[signum / 64] &= ~(1UL << (signum % 64));
}
int sigismember(const sigset_t *set, int signum)
{
   if (!set || signum <= 0 || signum > 128) return 0;
   return (set->bits[signum / 64] >> (signum % 64)) & 1;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
   if (oldset) *oldset = ics_sigmask;
   if (set) {
      if (how == SIG_BLOCK) {
         int i; for (i = 0; i < 4; i++) ics_sigmask.bits[i] |= set->bits[i];
      } else if (how == SIG_UNBLOCK) {
         int i; for (i = 0; i < 4; i++) ics_sigmask.bits[i] &= ~set->bits[i];
      } else { /* SIG_SETMASK */
         ics_sigmask = *set;
      }
   }
   return 0;
}
int raise(int sig)
{
   /* No delivery mechanism; a fatal self-raise terminates like kill. */
   if (sig > 0 && sig <= 128)
      _exit(128 + (sig & 127));
   return 0;
}

/* ---- extra POSIX helpers needed by host tools (binutils ar/objcopy, make) ---- */

/* lstat: same as stat (no symlinks in the ICS-OS VFS). */
int lstat(const char *path, struct stat *buf)
{
   return stat(path, buf);
}

/* chown: no ownership model; accept as no-op success. */
int chown(const char *path, uid_t uid, gid_t gid)
{
   (void)path; (void)uid; (void)gid;
   return 0;
}

/* utime: no timestamps preserved; accept as no-op success (POSIX allows the
   implementation to ignore the times). */
int utime(const char *path, const struct utimbuf *times)
{
   (void)path; (void)times;
   return 0;
}

/* mktemp: replace the trailing run of 'X' with a unique hex sequence
   (getpid + counter), returning a name not yet in use (POSIX). */
static int ics_mktmp_cnt;
char *mktemp(char *template)
{
   char *s, *e;
   int pid = getpid();
   static const char digits[] = "0123456789abcdef";
   if (!template) return 0;
   e = template + __builtin_strlen(template);
   s = e;
   while (s > template && s[-1] == 'X') s--; /* s = first X */
   if (s == e) return 0; /* no X run */
   for (;;) {
      int v = ((pid & 0xffff) << 4) + (ics_mktmp_cnt & 0xffff);
      char *p;
      ics_mktmp_cnt++;
      for (p = s; p < e; p++) { *p = digits[v & 15]; v >>= 4; }
      *e = '\0';
      if (access(template, 0) != 0)
          break; /* name is free */
    }
    return template;
 }

 /* ---- GCC front-end (cc1) support ---------------------------------- */
 /* Unlocked stdio variants: this libc has no per-stream locks, so the
    "unlocked" forms behave identically to the plain forms.  Keep these
    compatibility fallbacks weak because GCC's bundled libiberty and other
    GNU applications may provide their own implementations. */
 __attribute__((weak))
 FILE *fdopen_unlocked(int fd, const char *mode)
 {
    return fdopen(fd, mode);
 }

 __attribute__((weak))
 FILE *fopen_unlocked(const char *path, const char *mode)
 {
    return fopen(path, mode);
 }

 int setbuf(FILE *f, char *buf)
 {
    return setvbuf(f, buf, buf ? _IOFBF : _IONBF, buf ? (size_t)BUFSIZ : (size_t)1);
 }

 __attribute__((weak))
 void unlock_std_streams(void)
 {
    /* No per-stream locks; nothing to release. */
 }

 static int ics_tolower_i(int c)
 {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
 }

 __attribute__((weak))
 int strcasecmp(const char *a, const char *b)
 {
    while (*a && *b) {
       int ca = ics_tolower_i((unsigned char)*a);
       int cb = ics_tolower_i((unsigned char)*b);
       if (ca != cb) return ca - cb;
       a++; b++;
    }
    return ics_tolower_i((unsigned char)*a) - ics_tolower_i((unsigned char)*b);
 }

 __attribute__((weak))
 int strncasecmp(const char *a, const char *b, size_t n)
 {
    while (n && *a && *b) {
       int ca = ics_tolower_i((unsigned char)*a);
       int cb = ics_tolower_i((unsigned char)*b);
       if (ca != cb) return ca - cb;
       a++; b++; n--;
    }
    if (n) return ics_tolower_i((unsigned char)*a);
    return 0;
 }

 __attribute__((weak))
 const char *strsignal(int sig)
 {
    static char buf[64];
    snprintf(buf, sizeof buf, "Signal %d", sig);
    return buf;
 }

 int __popcountdi2(unsigned long long x)
 {
    int c = 0;
    while (x) { x &= x - 1ULL; c++; }
    return c;
 }

 static const char *ics_asctime_wday[] =
   { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
 static const char *ics_asctime_mon[] =
   { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

 char *asctime(const struct tm *t)
 {
    static char buf[64];
    if (!t) return 0;
    int w = (t->tm_wday >= 0 && t->tm_wday <= 6) ? t->tm_wday : 0;
    int m = (t->tm_mon  >= 0 && t->tm_mon  <= 11) ? t->tm_mon : 0;
    snprintf(buf, sizeof buf, "%.3s %.3s %2d %02d:%02d:%02d %d\n",
             ics_asctime_wday[w], ics_asctime_mon[m], t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900);
    return buf;
 }

 __attribute__((weak))
 void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *))
 {
    const char *lo = (const char *)base;
    const char *hi = lo + nmemb * size;
    while (lo < hi) {
        size_t mid = (hi - lo) / 2 / size;
        const char *midp = lo + mid * size;
        int c = compar(key, midp);
        if (c == 0) return (void *)midp;
        if (c < 0) hi = midp;
        else lo = midp + size;
    }
    return 0;
 }
