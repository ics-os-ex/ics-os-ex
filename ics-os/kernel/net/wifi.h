#ifndef ICSOS_NET_WIFI_H
#define ICSOS_NET_WIFI_H

#include "ieee80211.h"

struct wifi_dev;

struct wifi_ops {
    int (*start)(struct wifi_dev *wdev);
    int (*stop)(struct wifi_dev *wdev);
    int (*scan)(struct wifi_dev *wdev);
    int (*connect)(struct wifi_dev *wdev, const char *ssid);
};

struct wifi_dev {
    char name[16];
    enum wifi_state state;
    unsigned char addr[WIFI_ADDR_LEN];
    void *priv;
    const struct wifi_ops *ops;
    struct wifi_dev *next;
};

int wifi_register(struct wifi_dev *wdev);
void wifi_unregister(struct wifi_dev *wdev);
struct wifi_dev *wifi_default(void);
struct wifi_dev *wifi_find(const char *name);
const char *wifi_state_str(enum wifi_state st);
void wifi_dump(void);

#endif
