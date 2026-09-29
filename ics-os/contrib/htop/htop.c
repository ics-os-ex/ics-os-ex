/*
 * htop: a small ICS-OS process/system monitor.
 *
 * Modes:
 *   (no args)      interactive full-screen monitor
 *   --version      print version
 *   --dump         print a plain-text process/system snapshot
 *   --frame        render one full-screen frame
 *   --selftest     validate the icsos_* stats/kill APIs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/icsos.h>

#define HTOP_VERSION "0.1"
#define MAXPROC 256
#define ROWS 25
#define COLS 80

typedef struct {
   int active;
   struct icsos_procinfo p;
   unsigned long long prev_cpu;
   unsigned int cpu10;
} htop_proc_t;

static htop_proc_t procs[MAXPROC];
static struct icsos_procinfo rawprocs[MAXPROC];
static int vis[MAXPROC];
static int nproc = 0;
static int nvis = 0;
static int sel = 0;
static int sort_mode = 1;
static unsigned long long prev_systicks = 0;
static int have_prev = 0;
static char filter[64];
static int filter_active = 0;

static int htop_fail(const char *msg)
{
   printf("htop: FAIL %s errno=%d\n", msg, errno);
   printf("HTOP_FAIL\n");
   return 1;
}

static int htop_expect(int cond, const char *msg)
{
   if (!cond)
      return htop_fail(msg);
   return 0;
}

static void htop_write_str(const char *s)
{
   size_t n = strlen(s);
   while (n > 0) {
      ssize_t w = write(1, s, n);
      if (w <= 0)
         break;
      s += w;
      n -= (size_t)w;
   }
}

static void htop_strcpy(char *dst, const char *src, int n)
{
   int i;
   if (!src)
      src = "";
   for (i = 0; i < n - 1 && src[i]; i++)
      dst[i] = src[i];
   dst[i] = 0;
}

static void htop_mem_str(unsigned long long pages, char *buf, int n)
{
   unsigned long long kb = pages * 4;
   if (kb >= 1024)
      snprintf(buf, n, "%luM", (unsigned long)(kb / 1024));
   else
      snprintf(buf, n, "%luK", (unsigned long)kb);
}

static char htop_state_char(unsigned int st)
{
   if (st & ICSOS_ST_DYING)
      return 'Z';
   if (st & ICSOS_ST_RUNNING)
      return 'R';
   if (st & ICSOS_ST_BLOCKED)
      return 'S';
   return '.';
}

static int htop_match(const struct icsos_procinfo *p)
{
   int i;
   if (!filter_active)
      return 1;
   for (i = 0; filter[i]; i++) {
      char c = filter[i];
      char s = p->name[i];
      if (c >= 'A' && c <= 'Z')
         c += 32;
      if (s >= 'A' && s <= 'Z')
         s += 32;
      if (c != s)
         return 0;
   }
   return 1;
}

static int htop_cmp(const void *a, const void *b)
{
   const htop_proc_t *x = (const htop_proc_t *)a;
   const htop_proc_t *y = (const htop_proc_t *)b;
   if (sort_mode == 1) {
      if (x->cpu10 != y->cpu10)
         return (x->cpu10 > y->cpu10) ? -1 : 1;
   }
   if (x->p.pid != y->p.pid)
      return (x->p.pid > y->p.pid) ? -1 : 1;
   return 0;
}

static int htop_update(struct icsos_sysinfo *si)
{
   int total, n, i, j, compact;
   int seen[MAXPROC];
   unsigned long long sys_delta = 0;

   if (icsos_sysinfo(si) != 0)
      return -1;

   total = icsos_proc_list(rawprocs, MAXPROC);
   if (total < 0)
      return -1;
   n = (total < MAXPROC) ? total : MAXPROC;

   for (i = 0; i < nproc; i++)
      seen[i] = 0;

   if (have_prev && si->total_cpu_ticks >= prev_systicks)
      sys_delta = si->total_cpu_ticks - prev_systicks;

   for (i = 0; i < n; i++) {
      htop_proc_t *slot = 0;

      for (j = 0; j < nproc; j++) {
         if (procs[j].active && procs[j].p.pid == rawprocs[i].pid) {
            slot = &procs[j];
            seen[j] = 1;
            break;
         }
      }

      if (!slot) {
         if (nproc >= MAXPROC)
            continue;
         slot = &procs[nproc++];
         slot->active = 1;
         slot->prev_cpu = rawprocs[i].totalcputime;
         slot->cpu10 = 0;
      }

      {
         unsigned long long delta = rawprocs[i].totalcputime;
         if (rawprocs[i].totalcputime < slot->prev_cpu)
            delta = 0;
         else
            delta = rawprocs[i].totalcputime - slot->prev_cpu;

         if (have_prev && sys_delta > 0)
            slot->cpu10 = (unsigned int)((delta * 1000) / sys_delta);
         else
            slot->cpu10 = 0;
      }

      slot->p = rawprocs[i];
      slot->prev_cpu = rawprocs[i].totalcputime;
   }

   compact = 0;
   for (i = 0; i < nproc; i++) {
      if (procs[i].active && seen[i]) {
         if (compact != i)
            procs[compact] = procs[i];
         compact++;
      }
   }
   nproc = compact;

   qsort(procs, nproc, sizeof(htop_proc_t), htop_cmp);

   have_prev = 1;
   prev_systicks = si->total_cpu_ticks;
   return 0;
}

static void htop_refresh_visible(void)
{
   int i;
   nvis = 0;
   for (i = 0; i < nproc; i++)
      if (htop_match(&procs[i].p))
         vis[nvis++] = i;
   if (sel >= nvis)
      sel = (nvis > 0) ? nvis - 1 : 0;
   if (sel < 0)
      sel = 0;
}

static void htop_print_row(int row, const char *s)
{
   char line[COLS + 1];
   char esc[16];
   int len, i;

   snprintf(line, COLS, "%-79s", s);
   len = (int)strlen(line);
   for (i = len; i < COLS - 1; i++)
      line[i] = ' ';
   line[COLS - 1] = 0;

   snprintf(esc, sizeof(esc), "\x1b[%d;1H", row + 1);
   htop_write_str(esc);
   htop_write_str(line);
   if (row != ROWS - 1)
      htop_write_str("\n");
}

static void htop_render(const struct icsos_sysinfo *si)
{
   char line[COLS + 1];
   char mem[16], freemem[16], usedmem[16];
   unsigned int uptime_sec = 0, h = 0, m = 0, s = 0, running = 0;
   int i, v, row;

   if (si->hz > 0) {
      uptime_sec = si->uptime_ticks / si->hz;
      h = uptime_sec / 3600;
      m = (uptime_sec % 3600) / 60;
      s = uptime_sec % 60;
   }

   for (i = 0; i < nvis; i++)
      if (procs[vis[i]].p.state & ICSOS_ST_RUNNING)
         running++;

   sprintf(line, "htop %s - ICS-OS    Uptime: %02lu:%02lu:%02lu   CPUs: %u   Tasks: %u (%lu running)",
           HTOP_VERSION, (unsigned long)h, (unsigned long)m, (unsigned long)s,
           si->ncpu, si->total_procs, (unsigned long)running);
   htop_print_row(0, line);

   htop_mem_str(si->used_pages, usedmem, sizeof(usedmem));
   htop_mem_str(si->free_pages, freemem, sizeof(freemem));
   htop_mem_str(si->total_pages, mem, sizeof(mem));
   sprintf(line, "Mem: %s/%s  Free: %s   Total CPU ticks: %lu   Sort: %s",
           usedmem, mem, freemem,
           (unsigned long)si->total_cpu_ticks,
           sort_mode ? "CPU" : "PID");
   htop_print_row(1, line);
   htop_print_row(2, "");
   htop_print_row(3, "  PID USER    PRI   S CPU%      TIME    RES COMMAND");

   row = 4;
   v = 0;
   for (i = 0; i < nvis && row < 23; i++, v++) {
      const htop_proc_t *p = &procs[vis[i]];
      char res[16], cpupct[16];
      unsigned long long time_sec = p->p.totalcputime / 100;

      htop_mem_str(p->p.rss_pages, res, sizeof(res));
      sprintf(cpupct, "%lu.%lu",
              (unsigned long)(p->cpu10 / 10),
              (unsigned long)(p->cpu10 % 10));

      if (i == sel)
         sprintf(line, "* %4lu %-7s %3u %c %5s %6lu %6s %s",
                 (unsigned long)p->p.pid, p->p.name,
                 p->p.priority, htop_state_char(p->p.state), cpupct,
                 (unsigned long)time_sec, res, p->p.name);
      else
         sprintf(line, "  %4lu %-7s %3u %c %5s %6lu %6s %s",
                 (unsigned long)p->p.pid, p->p.name,
                 p->p.priority, htop_state_char(p->p.state), cpupct,
                 (unsigned long)time_sec, res, p->p.name);
      htop_print_row(row, line);
      row++;
   }
   for (; row < 23; row++)
      htop_print_row(row, "");

   if (filter_active)
      sprintf(line, "Filter: %s  (backspace edit, enter apply, esc clear)", filter);
   else
      sprintf(line, "F5 refresh  F9/K kill  / filter  r sort cpu  n sort pid  q quit");
   htop_print_row(23, line);
   htop_print_row(24, "ICS-OS htop");
}

static int htop_raw_save(struct termios *saved, struct termios *raw)
{
   if (tcgetattr(0, saved))
      return -1;
   *raw = *saved;
   raw->c_iflag &= ~(unsigned int)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                   INLCR | IGNCR | ICRNL);
   raw->c_oflag &= ~(unsigned int)OPOST;
   raw->c_lflag &= ~(unsigned int)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG);
   raw->c_cflag &= ~(unsigned int)(CSIZE | PARENB);
   raw->c_cflag |= (unsigned int)CS8;
   raw->c_cc[VMIN] = 1;
   raw->c_cc[VTIME] = 0;
   if (tcsetattr(0, TCSANOW, raw))
      return -1;
   return 0;
}

static void htop_raw_restore(const struct termios *saved)
{
   tcsetattr(0, TCSANOW, saved);
}

static int htop_filter_prompt(void)
{
   int len = 0;
   filter[0] = 0;
   filter_active = 1;
   htop_write_str("\x1b[24;1H/ ");
   while (1) {
      char ch = 0;
      ssize_t r = read(0, &ch, 1);
      if (r <= 0 || ch == 3 || ch == 13 || ch == 10)
         break;
      if (ch == 127 || ch == 8) {
         if (len > 0) {
            len--;
            filter[len] = 0;
         }
      } else if (ch >= 32 && ch < 127 && len < 63) {
         filter[len++] = ch;
         filter[len] = 0;
      }
      htop_write_str("\x1b[24;1H/ ");
      htop_write_str(filter);
      htop_write_str("\x1b[K");
   }
   if (!filter[0])
      filter_active = 0;
   htop_refresh_visible();
   return 0;
}

static int htop_key(void)
{
   fd_set rfds;
   struct timeval tv;
   int r;
   char ch = 0;

   FD_ZERO(&rfds);
   FD_SET(0, &rfds);
   tv.tv_sec = 0;
   tv.tv_usec = 500000;
   r = select(1, &rfds, 0, 0, &tv);
   if (r <= 0)
      return 0;
   if (read(0, &ch, 1) != 1)
      return 1;

   if (ch == 'q' || ch == 'Q' || ch == 3)
      return 1;
   if (ch == 'j' || ch == 'J') {
      if (sel < nvis - 1)
         sel++;
      return 0;
   }
   if (ch == 'k' || ch == 'K') {
      if (sel > 0)
         sel--;
      return 0;
   }
   if (ch == 'r' || ch == 'R') {
      sort_mode = 1;
      qsort(procs, nproc, sizeof(htop_proc_t), htop_cmp);
      htop_refresh_visible();
      return 0;
   }
   if (ch == 'n' || ch == 'N') {
      sort_mode = 0;
      qsort(procs, nproc, sizeof(htop_proc_t), htop_cmp);
      htop_refresh_visible();
      return 0;
   }
   if (ch == 'g' || ch == 'G') {
      sel = 0;
      return 0;
   }
   if (ch == '/') {
      htop_filter_prompt();
      return 0;
   }
   if (ch == '9' || ch == 'x' || ch == 'X') {
      if (nvis > 0) {
         htop_proc_t *p = &procs[vis[sel]];
         if (p->p.state & ICSOS_ST_KERNEL) {
            htop_write_str("\x1b[24;1HCannot kill a kernel process              \n");
         } else if (icsos_kill(p->p.pid, SIGTERM) == 0) {
            char msg[64];
            sprintf(msg, "Sent SIGTERM to pid %u", p->p.pid);
            htop_write_str("\x1b[24;1H");
            htop_write_str(msg);
            htop_write_str("\n");
         }
      }
      return 0;
   }
   if (ch == 27) {
      struct timeval tv0;
      tv0.tv_sec = 0;
      tv0.tv_usec = 0;
      FD_ZERO(&rfds);
      FD_SET(0, &rfds);
      if (select(1, &rfds, 0, 0, &tv0) > 0)
         (void)read(0, &ch, 1);
      if (select(1, &rfds, 0, 0, &tv0) > 0)
         (void)read(0, &ch, 1);
      return 0;
   }
   return 0;
}

static int htop_interactive(void)
{
   struct termios saved, raw;
   struct icsos_sysinfo si;
   int rc;

   if (htop_raw_save(&saved, &raw))
      return htop_fail("tcsetattr(raw)");
   tcflush(0, TCIFLUSH);
   htop_write_str("\x1b[?1049h");
   htop_write_str("\x1b[?25l");

   while (1) {
      if (htop_update(&si) != 0) {
         htop_raw_restore(&saved);
         htop_write_str("\x1b[?25h");
         htop_write_str("\x1b[?1049l");
         return htop_fail("refresh");
      }
      htop_refresh_visible();
      htop_write_str("\x1b[2J");
      htop_write_str("\x1b[H");
      htop_render(&si);
      rc = htop_key();
      if (rc)
         break;
   }

   htop_raw_restore(&saved);
   htop_write_str("\x1b[?25h");
   htop_write_str("\x1b[?1049l");
   return 0;
}

static int htop_dump(void)
{
   struct icsos_sysinfo si;
   int total, n, i;

   if (icsos_sysinfo(&si) != 0)
      return htop_fail("sysinfo");
   total = icsos_proc_list(rawprocs, MAXPROC);
   if (total < 0)
      return htop_fail("proc_list");
   n = (total < MAXPROC) ? total : MAXPROC;
   if (icsos_proc_list(rawprocs, n) != total)
      return htop_fail("proc_list fill");

   printf("htop %s - ICS-OS dump\n", HTOP_VERSION);
   printf("uptime=%lu s hz=%u ncpu=%u procs=%u\n",
          (unsigned long)(si.uptime_ticks / (si.hz ? si.hz : 1)),
          si.hz, si.ncpu, si.total_procs);
   printf("mem used=%lu free=%lu total=%lu pages cpu_ticks=%lu\n",
          (unsigned long)si.used_pages,
          (unsigned long)si.free_pages,
          (unsigned long)si.total_pages,
          (unsigned long)si.total_cpu_ticks);
   printf("PID PPID STATE PRI CPU CPU%% TIME RES NAME\n");
   for (i = 0; i < n; i++) {
      char res[16];
      htop_mem_str(rawprocs[i].rss_pages, res, sizeof(res));
      printf("%u %u %c %u %u %s %lu %s %s\n",
             rawprocs[i].pid,
             rawprocs[i].ppid,
             htop_state_char(rawprocs[i].state),
             rawprocs[i].priority,
             (rawprocs[i].on_cpu == 0xFFFFFFFFu) ? 0 : rawprocs[i].on_cpu,
             "0.0",
             (unsigned long)(rawprocs[i].totalcputime / 100),
             res,
             rawprocs[i].name);
   }
   printf("HTOP_DUMP_OK\n");
   return 0;
}

static int htop_frame(void)
{
   struct icsos_sysinfo si;
   if (htop_update(&si) != 0)
      return htop_fail("frame update");
   htop_refresh_visible();
   htop_write_str("\x1b[2J");
   htop_write_str("\x1b[H");
   htop_render(&si);
   return 0;
}

static int htop_selftest(void)
{
   struct icsos_sysinfo si;
   int total, n, i, found = 0;
   int mypid = getpid();

   printf("htop: selftest begin\n");
   if (htop_expect(icsos_sysinfo(&si) == 0, "sysinfo"))
      return 1;
   if (htop_expect(si.hz > 0, "hz"))
      return 1;
   if (htop_expect(si.ncpu >= 1, "ncpu"))
      return 1;
   if (htop_expect(si.total_pages > si.free_pages, "memory counters"))
      return 1;
   if (htop_expect(si.total_procs >= 1, "total_procs"))
      return 1;
   if (htop_expect(si.used_pages == si.total_pages - si.free_pages, "used_pages"))
      return 1;

   total = icsos_proc_list(0, 0);
   if (htop_expect(total >= 1, "proc_list total"))
      return 1;
   n = (total < MAXPROC) ? total : MAXPROC;
   if (htop_expect(icsos_proc_list(rawprocs, n) == total, "proc_list fill"))
      return 1;

   for (i = 0; i < n; i++) {
      if (rawprocs[i].pid == (unsigned int)mypid) {
         found = 1;
         break;
      }
   }
   if (htop_expect(found, "self pid in proc_list"))
      return 1;
   if (htop_expect(rawprocs[i].name[0] != 0, "self name"))
      return 1;
   if (htop_expect((rawprocs[i].state & ICSOS_ST_KERNEL) == 0, "self is user"))
      return 1;

   if (htop_expect(icsos_kill(mypid, 0) == 0, "kill self sig=0"))
      return 1;
   if (htop_expect(icsos_kill(0x7fffffff, 0) == -1, "kill bad pid"))
      return 1;
   if (htop_expect(errno == ESRCH, "bad pid errno"))
      return 1;

   printf("htop: selftest all checks passed\n");
   printf("HTOP_SELFTEST_PASS\n");
   printf("HTOP_PASS\n");
   return 0;
}

int main(int argc, char *argv[])
{
   int i;
   for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--version") == 0) {
         printf("htop %s (ICS-OS)\n", HTOP_VERSION);
         return 0;
      }
      if (strcmp(argv[i], "--dump") == 0)
         return htop_dump();
      if (strcmp(argv[i], "--frame") == 0)
         return htop_frame();
      if (strcmp(argv[i], "--selftest") == 0)
         return htop_selftest();
   }
   if (isatty(0) && isatty(1))
      return htop_interactive();
   return htop_frame();
}
