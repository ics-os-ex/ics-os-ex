/*
 * ifconfig — show / set the default NIC address (ICS-OS single-iface).
 *
 *   ifconfig
 *   ifconfig eth0 10.0.2.15 netmask 255.255.255.0
 *   ifconfig eth0 up|down
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>

static void print_dotted(unsigned int host)
{
   printf("%u.%u.%u.%u",
          (host >> 24) & 0xFF, (host >> 16) & 0xFF,
          (host >> 8) & 0xFF, host & 0xFF);
}

static int show(void)
{
   struct netcfg_info info;
   if (netcfg_get(&info) != 0) {
      printf("ifconfig: get failed errno=%d\n", errno);
      printf("NET_IFCONFIG_FAIL\n");
      return 1;
   }
   printf("%s: flags=%s mtu=1500\n",
          info.name[0] ? info.name : "eth0",
          info.link_up ? "UP" : "DOWN");
   printf("        inet ");
   print_dotted(info.ip);
   printf("  netmask ");
   print_dotted(info.netmask);
   printf("\n");
   printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
          info.mac[0], info.mac[1], info.mac[2],
          info.mac[3], info.mac[4], info.mac[5]);
   printf("        gateway ");
   print_dotted(info.gateway);
   printf("  configured=%u\n", info.configured);
   printf("NET_IFCONFIG_OK\n");
   return 0;
}

static unsigned int parse_ip(const char *s)
{
   return (unsigned int)ntohl(inet_addr(s));
}

int main(int argc, char **argv)
{
   unsigned int ip = 0, mask = 0, gw = 0;
   int i, do_up = 0, do_down = 0, have_addr = 0;

   if (argc <= 1)
      return show();

   for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "up") == 0) {
         do_up = 1;
      } else if (strcmp(argv[i], "down") == 0) {
         do_down = 1;
      } else if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
         mask = parse_ip(argv[++i]);
         have_addr = 1;
      } else if (strcmp(argv[i], "gw") == 0 && i + 1 < argc) {
         gw = parse_ip(argv[++i]);
         have_addr = 1;
      } else if (strchr(argv[i], '.') != 0) {
         ip = parse_ip(argv[i]);
         have_addr = 1;
      }
      /* iface name (eth0/eth1) ignored — single default NIC */
   }

   if (do_down) {
      if (netcfg_set_down() != 0) {
         printf("NET_IFCONFIG_FAIL\n");
         return 1;
      }
   }
   if (have_addr) {
      if (netcfg_set_addr(ip, mask, gw) != 0) {
         printf("NET_IFCONFIG_FAIL\n");
         return 1;
      }
   }
   if (do_up) {
      if (netcfg_set_up() != 0) {
         printf("NET_IFCONFIG_FAIL\n");
         return 1;
      }
   }
   return show();
}
