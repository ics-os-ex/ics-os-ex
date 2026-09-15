#include "softnet.h"
#include "tcp.h"
#include "dhcp.h"
#include "netif.h"
#include "inet_config.h"
#include "net_sync.h"

extern unsigned int ticks;
extern unsigned int time_count;
extern unsigned int createkthread(void *ptr, char *name, unsigned int stacksize);
extern void delay(unsigned int w);

static volatile int softnet_started;
static unsigned int softnet_ticks_done;
static unsigned int softnet_last_dhcp_secs;

void softnet_tick(void)
{
    struct netif *nif;
    struct inet_config cfg;

    nif = netif_default();
    if (!nif || !nif->configured)
        return;

    net_lock();
    tcp_timer(ticks);
    if (time_count != softnet_last_dhcp_secs) {
        softnet_last_dhcp_secs = time_count;
        cfg.ip = nif->ip;
        cfg.netmask = nif->netmask;
        cfg.gateway = nif->gateway;
        (void)dhcp_service(nif, &cfg, time_count, 50000);
    }
    softnet_ticks_done++;
    net_unlock();
}

static void softnet_thread(void)
{
    for (;;) {
        softnet_tick();
        delay(10); /* ~10ms at 200Hz ticks (delay is ms*2 ticks) */
    }
}

void softnet_init(void)
{
    if (softnet_started)
        return;
    softnet_started = 1;
    softnet_last_dhcp_secs = time_count;
    if (!createkthread((void *)softnet_thread, "softnet", 32768))
        softnet_started = 0;
}
