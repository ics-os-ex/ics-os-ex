/*
 * Concurrent socket stress under SMP (intended with user-smp cmdline).
 * Forks NWORKERS children; each runs TCP and UDP echo rounds against the
 * QEMU SLIRP host helpers on :7778 / :7777. Parent reaps and asserts.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define NWORKERS     4
#define TCP_ROUNDS   6
#define UDP_ROUNDS   4

static int fail(const char *msg)
{
   printf("netstress: FAIL %s errno=%d\n", msg, errno);
   printf("NETSTRESS_FAIL\n");
   return 1;
}

static int tcp_round(int id, int round)
{
   char payload[32];
   char buf[32];
   struct sockaddr_in addr;
   int fd, n, want;
   int attempt;

   snprintf(payload, sizeof(payload), "T%d-R%d", id, round);
   want = (int)strlen(payload);

   for (attempt = 0; attempt < 2; attempt++) {
      fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
         return -1;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(7778);
      addr.sin_addr = inet_addr("10.0.2.2");
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
         close(fd);
         continue;
      }
      if ((int)send(fd, payload, (size_t)want, 0) != want) {
         close(fd);
         continue;
      }
      memset(buf, 0, sizeof(buf));
      n = (int)recv(fd, buf, sizeof(buf), 0);
      close(fd);
      if (n == want && memcmp(buf, payload, (size_t)want) == 0)
         return 0;
   }
   return -1;
}

static int udp_round(int id, int round)
{
   char payload[32];
   char buf[32];
   struct sockaddr_in dst, from;
   socklen_t fromlen;
   int fd, n, want;
   int attempt;

   snprintf(payload, sizeof(payload), "U%d-R%d", id, round);
   want = (int)strlen(payload);

   for (attempt = 0; attempt < 2; attempt++) {
      fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0)
         return -1;
      memset(&dst, 0, sizeof(dst));
      dst.sin_family = AF_INET;
      dst.sin_port = htons(7777);
      dst.sin_addr = inet_addr("10.0.2.2");
      if ((int)sendto(fd, payload, (size_t)want, 0,
                      (struct sockaddr *)&dst, sizeof(dst)) != want) {
         close(fd);
         continue;
      }
      memset(buf, 0, sizeof(buf));
      fromlen = sizeof(from);
      n = (int)recvfrom(fd, buf, sizeof(buf), 0,
                        (struct sockaddr *)&from, &fromlen);
      close(fd);
      if (n == want && memcmp(buf, payload, (size_t)want) == 0)
         return 0;
   }
   return -1;
}

static int worker(int id)
{
   int r;
   for (r = 0; r < TCP_ROUNDS; r++) {
      if (tcp_round(id, r) != 0) {
         printf("netstress: worker %d tcp round %d fail\n", id, r);
         return 1;
      }
   }
   for (r = 0; r < UDP_ROUNDS; r++) {
      if (udp_round(id, r) != 0) {
         printf("netstress: worker %d udp round %d fail\n", id, r);
         return 1;
      }
   }
   return 0;
}

int main(void)
{
   pid_t pids[NWORKERS];
   int i, st, failed = 0;

   printf("netstress: workers=%d tcp_rounds=%d udp_rounds=%d\n",
          NWORKERS, TCP_ROUNDS, UDP_ROUNDS);

   for (i = 0; i < NWORKERS; i++) {
      pid_t p = fork();
      if (p < 0)
         return fail("fork");
      if (p == 0)
         _exit(worker(i) == 0 ? 0 : 1);
      pids[i] = p;
   }

   for (i = 0; i < NWORKERS; i++) {
      if (waitpid(pids[i], &st, 0) != pids[i])
         return fail("waitpid");
      if (st != 0)
         failed++;
   }

   if (failed) {
      printf("netstress: %d workers failed\n", failed);
      printf("NETSTRESS_FAIL\n");
      return 1;
   }
   printf("NETSTRESS_PASS\n");
   return 0;
}
