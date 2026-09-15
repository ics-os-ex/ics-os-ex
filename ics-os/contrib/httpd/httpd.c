/*
 * Minimal HTTP/1.0 server for ICS-OS socket + hostfwd smoke test.
 * Listens on :8080, serves one GET, prints NET_HTTPD_OK.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define HTTPD_PORT 8080

static const char response[] =
   "HTTP/1.0 200 OK\r\n"
   "Content-Type: text/plain\r\n"
   "Content-Length: 18\r\n"
   "Connection: close\r\n"
   "\r\n"
   "ICS-OS HTTP OK\r\n";

static int fail(const char *msg)
{
   printf("httpd: FAIL %s errno=%d\n", msg, errno);
   printf("NET_HTTPD_FAIL\n");
   return 1;
}

int main(void)
{
   struct sockaddr_in addr, peer;
   socklen_t peerlen;
   char buf[256];
   int lfd, cfd, n;

   printf("httpd: listen :%d\n", HTTPD_PORT);
   lfd = socket(AF_INET, SOCK_STREAM, 0);
   if (lfd < 0)
      return fail("socket");

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(HTTPD_PORT);
   addr.sin_addr = INADDR_ANY;
   if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(lfd);
      return fail("bind");
   }
   if (listen(lfd, 1) != 0) {
      close(lfd);
      return fail("listen");
   }

   printf("NET_HTTPD_READY\n");
   peerlen = sizeof(peer);
   cfd = accept(lfd, (struct sockaddr *)&peer, &peerlen);
   if (cfd < 0) {
      close(lfd);
      return fail("accept");
   }

   memset(buf, 0, sizeof(buf));
   n = (int)recv(cfd, buf, sizeof(buf) - 1, 0);
   if (n < 3 || buf[0] != 'G' || buf[1] != 'E' || buf[2] != 'T') {
      close(cfd);
      close(lfd);
      return fail("recv GET");
   }

   if ((int)send(cfd, response, sizeof(response) - 1, 0) !=
       (int)(sizeof(response) - 1)) {
      close(cfd);
      close(lfd);
      return fail("send");
   }

   close(cfd);
   close(lfd);
   printf("NET_HTTPD_OK\n");
   return 0;
}
