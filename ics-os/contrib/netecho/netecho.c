/*
 * Userland Berkeley socket smoke test:
 *   1) TCP client  → 10.0.2.2:7778  (NET_SOCK_OK)
 *   2) UDP sendto/recvfrom → 10.0.2.2:7777 (NET_SOCK_UDP_OK)
 *   3) TCP listen/accept on :7779 via QEMU hostfwd (NET_SOCK_LISTEN_OK)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static int fail(const char *msg)
{
   printf("netecho: FAIL %s errno=%d\n", msg, errno);
   printf("NET_SOCK_FAIL\n");
   return 1;
}

static int tcp_client(void)
{
   static const char payload[] = "ICS-SOCK!";
   char buf[32];
   struct sockaddr_in addr;
   int fd, n;

   printf("netecho: tcp connect/send/recv\n");
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return fail("tcp socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(7778);
   addr.sin_addr = inet_addr("10.0.2.2");
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(fd);
      return fail("tcp connect");
   }

   n = (int)send(fd, payload, sizeof(payload) - 1, 0);
   if (n != (int)(sizeof(payload) - 1)) {
      close(fd);
      return fail("tcp send");
   }

   memset(buf, 0, sizeof(buf));
   n = (int)recv(fd, buf, sizeof(buf), 0);
   if (n != (int)(sizeof(payload) - 1) ||
       memcmp(buf, payload, sizeof(payload) - 1) != 0) {
      close(fd);
      return fail("tcp recv");
   }

   close(fd);
   printf("NET_SOCK_OK\n");
   return 0;
}

static int udp_echo(void)
{
   static const char payload[] = "ICS-UDPS!";
   char buf[32];
   struct sockaddr_in dst, from;
   socklen_t fromlen;
   int fd, n;

   printf("netecho: udp sendto/recvfrom\n");
   fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0)
      return fail("udp socket");

   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = htons(7777);
   dst.sin_addr = inet_addr("10.0.2.2");
   n = (int)sendto(fd, payload, sizeof(payload) - 1, 0,
                   (struct sockaddr *)&dst, sizeof(dst));
   if (n != (int)(sizeof(payload) - 1)) {
      close(fd);
      return fail("udp sendto");
   }

   memset(buf, 0, sizeof(buf));
   fromlen = sizeof(from);
   n = (int)recvfrom(fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &fromlen);
   if (n != (int)(sizeof(payload) - 1) ||
       memcmp(buf, payload, sizeof(payload) - 1) != 0) {
      close(fd);
      return fail("udp recvfrom");
   }

   close(fd);
   printf("NET_SOCK_UDP_OK\n");
   return 0;
}

static int tcp_listen_echo(void)
{
   static const char expect[] = "ICS-LISN!";
   char buf[32];
   struct sockaddr_in addr, peer;
   socklen_t peerlen;
   int lfd, cfd, n;

   printf("netecho: tcp listen/accept on :7779\n");
   lfd = socket(AF_INET, SOCK_STREAM, 0);
   if (lfd < 0)
      return fail("listen socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(7779);
   addr.sin_addr = INADDR_ANY;
   if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(lfd);
      return fail("bind");
   }
   if (listen(lfd, 1) != 0) {
      close(lfd);
      return fail("listen");
   }

   printf("NET_SOCK_LISTEN_READY\n");
   peerlen = sizeof(peer);
   cfd = accept(lfd, (struct sockaddr *)&peer, &peerlen);
   if (cfd < 0) {
      close(lfd);
      return fail("accept");
   }

   memset(buf, 0, sizeof(buf));
   n = (int)recv(cfd, buf, sizeof(buf), 0);
   if (n != (int)(sizeof(expect) - 1) ||
       memcmp(buf, expect, sizeof(expect) - 1) != 0) {
      close(cfd);
      close(lfd);
      return fail("listen recv");
   }
   if ((int)send(cfd, buf, (size_t)n, 0) != n) {
      close(cfd);
      close(lfd);
      return fail("listen send");
   }

   close(cfd);
   close(lfd);
   printf("NET_SOCK_LISTEN_OK\n");
   return 0;
}

int main(void)
{
   if (tcp_client() != 0)
      return 1;
   if (udp_echo() != 0)
      return 1;
   if (tcp_listen_echo() != 0)
      return 1;
   return 0;
}
