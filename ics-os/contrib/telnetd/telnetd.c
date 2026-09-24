/*
 * Telnet-compatible remote shell daemon for ICS-OS.
 * Listens on :23, speaks NVT (refuses telnet options), remaps the accepted
 * socket onto stdin/stdout/stderr, then runs a line-oriented command shell
 * (echo / ifconfig / route / help / exit, plus execp of other apps).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define TELNETD_PORT 23
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251

int execp(char *fname, unsigned short mode, char *params);

static int fail(const char *msg)
{
   /* Markers must hit the serial console; call before remapping stdio. */
   printf("telnetd: FAIL %s errno=%d\n", msg, errno);
   printf("NET_TELNETD_FAIL\n");
   return 1;
}

static void sock_puts(int fd, const char *s)
{
   (void)send(fd, s, strlen(s), 0);
}

static int send_all(int fd, const void *buf, int n)
{
   const char *p = (const char *)buf;
   int off = 0;
   while (off < n) {
      int w = (int)send(fd, p + off, (size_t)(n - off), 0);
      if (w <= 0)
         return -1;
      off += w;
   }
   return 0;
}

/* Strip IAC sequences; reply WONT/DONT to WILL/DO. */
static int read_line(int fd, char *buf, int max)
{
   int n = 0;
   while (n < max - 1) {
      unsigned char c;
      int r = (int)recv(fd, &c, 1, 0);
      if (r <= 0)
         return (n > 0) ? n : -1;
      if (c == IAC) {
         unsigned char cmd, opt;
         if (recv(fd, &cmd, 1, 0) <= 0)
            return -1;
         if (cmd == IAC) {
            buf[n++] = (char)IAC;
            continue;
         }
         if (recv(fd, &opt, 1, 0) <= 0)
            return -1;
         if (cmd == WILL || cmd == DO) {
            unsigned char reply[3];
            reply[0] = IAC;
            reply[1] = (cmd == WILL) ? DONT : WONT;
            reply[2] = opt;
            (void)send_all(fd, reply, 3);
         }
         continue;
      }
      if (c == '\r')
         continue;
      if (c == '\n')
         break;
      if (c == 3) /* Ctrl-C */
         return -1;
      buf[n++] = (char)c;
   }
   buf[n] = 0;
   return n;
}

static void run_shell(int fd)
{
   char line[256];
   sock_puts(fd, "ICS-OS telnetd — type help or exit.\r\n");
   for (;;) {
      char *cmd;
      int n;
      sock_puts(fd, "telnet$ ");
      n = read_line(fd, line, sizeof(line));
      if (n < 0)
         break;
      cmd = line;
      while (*cmd == ' ')
         cmd++;
      if (cmd[0] == 0)
         continue;
      if (strncmp(cmd, "exit", 4) == 0)
         break;
      if (strncmp(cmd, "echo ", 5) == 0) {
         sock_puts(fd, cmd + 5);
         sock_puts(fd, "\r\n");
         continue;
      }
      if (strncmp(cmd, "help", 4) == 0) {
         sock_puts(fd, "commands: echo, ifconfig, route, help, exit\r\n");
         sock_puts(fd, "other names run /icsos/apps/<name>.exe\r\n");
         continue;
      }
      /* Avoid re-entering this daemon over the same session. */
      if (strncmp(cmd, "telnetd", 7) == 0) {
         sock_puts(fd, "telnetd: already running\r\n");
         continue;
      }
      {
         char name[128], path[192];
         int j = 0;
         while (cmd[j] && cmd[j] != ' ' && j < 127) {
            name[j] = cmd[j];
            j++;
         }
         name[j] = 0;
         strcpy(path, "/icsos/apps/");
         strcat(path, name);
         if (!strchr(name, '.'))
            strcat(path, ".exe");
         /* Child inherits remapped stdio sockets. */
         if (execp(path, 0, cmd) != 0)
            sock_puts(fd, "command failed\r\n");
      }
   }
}

int main(void)
{
   struct sockaddr_in addr, peer;
   socklen_t peerlen;
   int lfd, cfd;

   printf("telnetd: listen :%d\n", TELNETD_PORT);
   lfd = socket(AF_INET, SOCK_STREAM, 0);
   if (lfd < 0)
      return fail("socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(TELNETD_PORT);
   addr.sin_addr = INADDR_ANY;
   if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(lfd);
      return fail("bind");
   }
   if (listen(lfd, 1) != 0) {
      close(lfd);
      return fail("listen");
   }

   printf("NET_TELNETD_READY\n");
   peerlen = sizeof(peer);
   cfd = accept(lfd, (struct sockaddr *)&peer, &peerlen);
   if (cfd < 0) {
      close(lfd);
      return fail("accept");
   }
   close(lfd);

   if (dup2(cfd, 0) < 0 || dup2(cfd, 1) < 0 || dup2(cfd, 2) < 0) {
      close(cfd);
      return fail("dup2");
   }
   close(cfd);

   run_shell(0);

   close(0);
   close(1);
   close(2);
   printf("NET_TELNETD_OK\n");
   return 0;
}
