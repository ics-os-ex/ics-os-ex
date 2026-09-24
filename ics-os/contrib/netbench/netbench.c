/*
 * Network performance microbench for ICS-OS (TCP/UDP over SLIRP).
 *
 * Measures:
 *   - TCP TX bulk to host sink (:7791)
 *   - TCP RX bulk from host source (:7792)
 *   - UDP echo round-trips (:7793)
 *   - TCP RTT ping-pong (:7791 after TX, or same sink with tiny payloads)
 *
 * Prints NETBENCH_* markers and kbps / us figures for the test gate.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define HOST           "10.0.2.2"
#define PORT_SINK      7791
#define PORT_SOURCE    7792
#define PORT_UDP       7793

#define TCP_BYTES      (2 * 1024 * 1024)
#define UDP_ROUNDS     500
#define RTT_ROUNDS     128
#define CHUNK          1024

static unsigned long long now_ms(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (unsigned long long)ts.tv_sec * 1000ULL +
          (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static int fail(const char *msg)
{
   printf("netbench: FAIL %s errno=%d\n", msg, errno);
   printf("NETBENCH_FAIL\n");
   return 1;
}

static int tcp_connect(unsigned short port)
{
   struct sockaddr_in addr;
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons(port);
   addr.sin_addr = inet_addr(HOST);
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(fd);
      return -1;
   }
   return fd;
}

static int bench_tcp_tx(unsigned long long *kbps_out)
{
   static unsigned char buf[CHUNK];
   unsigned long long t0, t1, ms, sent = 0;
   int fd, n, want;

   memset(buf, 0xA5, sizeof(buf));
   fd = tcp_connect(PORT_SINK);
   if (fd < 0)
      return fail("tcp_tx connect");

   t0 = now_ms();
   while (sent < TCP_BYTES) {
      want = (int)((TCP_BYTES - sent) < CHUNK ? (TCP_BYTES - sent) : CHUNK);
      n = (int)send(fd, buf, (size_t)want, 0);
      if (n <= 0) {
         close(fd);
         return fail("tcp_tx send");
      }
      sent += (unsigned long long)n;
   }
   /* Include UNA drain + FIN so kbps reflects delivered bytes, not buffer fill. */
   close(fd);
   t1 = now_ms();
   ms = (t1 > t0) ? (t1 - t0) : 1;
   *kbps_out = (sent * 8ULL) / ms; /* kilobits/sec */
   printf("NETBENCH_TCP_TX bytes=%llu ms=%llu kbps=%llu\n",
          sent, ms, *kbps_out);
   return 0;
}

static int bench_tcp_rx(unsigned long long *kbps_out)
{
   static unsigned char buf[CHUNK];
   unsigned long long t0, t1, ms, got = 0;
   int fd, n;

   fd = tcp_connect(PORT_SOURCE);
   if (fd < 0)
      return fail("tcp_rx connect");

   t0 = now_ms();
   while (got < TCP_BYTES) {
      n = (int)recv(fd, buf, sizeof(buf), 0);
      if (n < 0) {
         close(fd);
         return fail("tcp_rx recv");
      }
      if (n == 0) {
         close(fd);
         return fail("tcp_rx eof early");
      }
      got += (unsigned long long)n;
   }
   t1 = now_ms();
   close(fd);
   ms = (t1 > t0) ? (t1 - t0) : 1;
   *kbps_out = (got * 8ULL) / ms;
   printf("NETBENCH_TCP_RX bytes=%llu ms=%llu kbps=%llu\n",
          got, ms, *kbps_out);
   return 0;
}

static int bench_udp(unsigned long long *pps_out)
{
   static unsigned char payload[64];
   static unsigned char buf[128];
   struct sockaddr_in dst, from;
   socklen_t fromlen;
   unsigned long long t0, t1, ms;
   int fd, i, n, ok = 0;

   fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0)
      return fail("udp socket");
   memset(&dst, 0, sizeof(dst));
   dst.sin_family = AF_INET;
   dst.sin_port = htons(PORT_UDP);
   dst.sin_addr = inet_addr(HOST);
   memset(payload, 0x5A, sizeof(payload));

   t0 = now_ms();
   for (i = 0; i < UDP_ROUNDS; i++) {
      payload[0] = (unsigned char)(i & 0xFF);
      payload[1] = (unsigned char)((i >> 8) & 0xFF);
      if ((int)sendto(fd, payload, sizeof(payload), 0,
                      (struct sockaddr *)&dst, sizeof(dst)) !=
          (int)sizeof(payload))
         continue;
      fromlen = sizeof(from);
      n = (int)recvfrom(fd, buf, sizeof(buf), 0,
                        (struct sockaddr *)&from, &fromlen);
      if (n == (int)sizeof(payload) && buf[0] == payload[0] &&
          buf[1] == payload[1])
         ok++;
   }
   t1 = now_ms();
   close(fd);
   if (ok < UDP_ROUNDS / 2)
      return fail("udp echo");
   ms = (t1 > t0) ? (t1 - t0) : 1;
   *pps_out = ((unsigned long long)ok * 1000ULL) / ms;
   printf("NETBENCH_UDP ok=%d/%d ms=%llu pps=%llu\n",
          ok, UDP_ROUNDS, ms, *pps_out);
   return 0;
}

static int bench_rtt(unsigned long long *us_out)
{
   static unsigned char msg[32];
   static unsigned char buf[32];
   unsigned long long t0, t1, total_ms = 0;
   int fd, i, n, ok = 0;

   fd = tcp_connect(PORT_SINK);
   if (fd < 0)
      return fail("rtt connect");
   memset(msg, 0x11, sizeof(msg));
   for (i = 0; i < RTT_ROUNDS; i++) {
      msg[0] = (unsigned char)i;
      t0 = now_ms();
      if ((int)send(fd, msg, sizeof(msg), 0) != (int)sizeof(msg))
         break;
      n = (int)recv(fd, buf, sizeof(buf), 0);
      t1 = now_ms();
      if (n != (int)sizeof(msg))
         break;
      total_ms += (t1 > t0) ? (t1 - t0) : 0;
      ok++;
   }
   close(fd);
   if (ok < RTT_ROUNDS / 2)
      return fail("rtt");
   /* Millisecond clock: report average ms * 1000 as approximate us. */
   *us_out = (total_ms * 1000ULL) / (unsigned long long)ok;
   printf("NETBENCH_RTT rounds=%d avg_us=%llu\n", ok, *us_out);
   return 0;
}

int main(void)
{
   unsigned long long tcp_tx = 0, tcp_rx = 0, udp_pps = 0, rtt_us = 0;

   printf("netbench: tcp=%uKB udp=%d rtt=%d host=%s\n",
          TCP_BYTES / 1024, UDP_ROUNDS, RTT_ROUNDS, HOST);

   if (bench_tcp_tx(&tcp_tx) != 0)
      return 1;
   if (bench_tcp_rx(&tcp_rx) != 0)
      return 1;
   if (bench_udp(&udp_pps) != 0)
      return 1;
   if (bench_rtt(&rtt_us) != 0)
      return 1;

   /*
    * Floors for QEMU SLIRP + TCG with pipelined TCP (2 KiB flight, 1 KiB
    * MSS) over 2 MiB transfers. Host-side sink timing on this path is
    * typically ~30–80 Mbit/s (matches common SLIRP quotes). Guest kbps
    * includes close/drain for TX. Floors catch regressions with margin.
    */
   if (tcp_tx < 10000) {
      printf("netbench: TCP TX too slow (%llu kbps < 10000)\n", tcp_tx);
      printf("NETBENCH_FAIL\n");
      return 1;
   }
   if (tcp_rx < 10000) {
      printf("netbench: TCP RX too slow (%llu kbps < 10000)\n", tcp_rx);
      printf("NETBENCH_FAIL\n");
      return 1;
   }
   if (udp_pps < 100) {
      printf("netbench: UDP pps too low (%llu < 100)\n", udp_pps);
      printf("NETBENCH_FAIL\n");
      return 1;
   }
   if (rtt_us > 500000) { /* 500 ms average (ms clock quantization) */
      printf("netbench: RTT too high (%llu us > 500000)\n", rtt_us);
      printf("NETBENCH_FAIL\n");
      return 1;
   }

   printf("NETBENCH_PASS tcp_tx_kbps=%llu tcp_rx_kbps=%llu udp_pps=%llu rtt_us=%llu\n",
          tcp_tx, tcp_rx, udp_pps, rtt_us);
   return 0;
}
