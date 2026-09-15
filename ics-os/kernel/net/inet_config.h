#ifndef ICSOS_NET_INET_CONFIG_H
#define ICSOS_NET_INET_CONFIG_H

/* QEMU SLIRP defaults for milestone-A selftest. */
#define NET_SLIRP_IP       0x0A00020Fu /* 10.0.2.15 */
#define NET_SLIRP_MASK     0xFFFFFF00u /* 255.255.255.0 */
#define NET_SLIRP_GATEWAY  0x0A000202u /* 10.0.2.2 */

struct inet_config {
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
};

void inet_config_defaults(struct inet_config *cfg);
void inet_config_from_cmdline(struct inet_config *cfg, const char *cmdline);

#endif
