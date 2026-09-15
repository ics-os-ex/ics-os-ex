/*
 * Userland TCP echo client via Berkeley sockets.
 * Connects to QEMU SLIRP gateway 10.0.2.2:7778 (host tcp_echo_server).
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

int main(void)
{
   static const char payload[] = "ICS-SOCK!";
   char buf[32];
   struct sockaddr_in addr;
   int fd, n;

   printf("netecho: socket/connect/send/recv\n");
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return fail("socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(7778);
   addr.sin_addr = inet_addr("10.0.2.2");
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(fd);
      return fail("connect");
   }

   n = (int)send(fd, payload, sizeof(payload) - 1, 0);
   if (n != (int)(sizeof(payload) - 1)) {
      close(fd);
      return fail("send");
   }

   memset(buf, 0, sizeof(buf));
   n = (int)recv(fd, buf, sizeof(buf), 0);
   if (n != (int)(sizeof(payload) - 1) ||
       memcmp(buf, payload, sizeof(payload) - 1) != 0) {
      close(fd);
      return fail("recv");
   }

   close(fd);
   printf("NET_SOCK_OK\n");
   return 0;
}
