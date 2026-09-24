/*
 * route — show / set the default gateway (ICS-OS single route).
 *
 *   route
 *   route add default gw 10.0.2.2
 */
#include <stdio.h>
#include <string.h>
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
      printf("route: get failed errno=%d\n", errno);
      printf("NET_ROUTE_FAIL\n");
      return 1;
   }
   printf("Kernel IP routing table\n");
   printf("Destination     Gateway         Genmask         Flags Iface\n");
   printf("0.0.0.0         ");
   print_dotted(info.gateway);
   printf("     0.0.0.0         UG    %s\n",
          info.name[0] ? info.name : "eth0");
   printf("NET_ROUTE_OK\n");
   return 0;
}

int main(int argc, char **argv)
{
   unsigned int gw;

   if (argc <= 1)
      return show();

   /* route add default gw A.B.C.D */
   if (argc >= 5 && strcmp(argv[1], "add") == 0 &&
       strcmp(argv[2], "default") == 0 && strcmp(argv[3], "gw") == 0) {
      gw = (unsigned int)ntohl(inet_addr(argv[4]));
      if (netcfg_set_gw(gw) != 0) {
         printf("route: set gw failed errno=%d\n", errno);
         printf("NET_ROUTE_FAIL\n");
         return 1;
      }
      return show();
   }

   printf("usage: route | route add default gw <ip>\n");
   return 1;
}
