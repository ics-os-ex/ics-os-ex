#ifndef ICSOS_NET_SOFTNET_H
#define ICSOS_NET_SOFTNET_H

/* Background softnet: TCP RTO + DHCP T1/T2 service. */
void softnet_init(void);
void softnet_tick(void); /* callable from tests without waiting on the thread */

#endif
