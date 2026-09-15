/*
 * Minimal TCP netcat for ICS-OS socket smoke tests.
 * Default: connect to 10.0.2.2:7778, send ICS-NC!, expect echo → NET_NC_OK.
 * Usage: nc [host] [port]
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static const char payload[] = "ICS-NC!";

static int fail(const char *msg)
{
   printf("nc: FAIL %s errno=%d\n", msg, errno);
   printf("NET_NC_FAIL\n");
   return 1;
}

static unsigned short parse_port(const char *s)
{
   unsigned int v = 0;
   if (!s || !*s)
      return 0;
   while (*s) {
      if (*s < '0' || *s > '9')
         return 0;
      v = v * 10u + (unsigned int)(*s - '0');
      if (v > 65535u)
         return 0;
      s++;
   }
   return (unsigned short)v;
}

int main(int argc, char **argv)
{
   const char *host = "10.0.2.2";
   unsigned short port = 7778;
   char buf[32];
   struct sockaddr_in addr;
   int fd, n;
   in_addr_t ip;

   if (argc >= 2 && argv[1] && argv[1][0])
      host = argv[1];
   if (argc >= 3) {
      port = parse_port(argv[2]);
      if (!port)
         return fail("bad port");
   }

   ip = inet_addr(host);
   if (ip == (in_addr_t)-1)
      return fail("bad host");

   printf("nc: connect %s:%u\n", host, (unsigned)port);
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return fail("socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(port);
   addr.sin_addr = ip;
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
   printf("NET_NC_OK\n");
   return 0;
}
