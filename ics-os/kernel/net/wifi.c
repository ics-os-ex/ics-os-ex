#include "wifi.h"

extern int printf(const char *fmt, ...);
extern void *memset(void *s, int c, unsigned int n);
extern int strcmp(const char *a, const char *b);

static struct wifi_dev *wifi_list;
static struct wifi_dev *wifi_def;

static int wifi_ops_stub_start(struct wifi_dev *wdev)
{
    if (!wdev)
        return -1;
    if (wdev->state < WIFI_STATE_INIT)
        wdev->state = WIFI_STATE_INIT;
    return 0;
}

static int wifi_ops_stub_stop(struct wifi_dev *wdev)
{
    if (!wdev)
        return -1;
    wdev->state = WIFI_STATE_DOWN;
    return 0;
}

static int wifi_ops_stub_scan(struct wifi_dev *wdev)
{
    if (!wdev)
        return -1;
    /* Stub: association/scan not implemented yet. */
    wdev->state = WIFI_STATE_SCANNING;
    return -1;
}

static int wifi_ops_stub_connect(struct wifi_dev *wdev, const char *ssid)
{
    (void)ssid;
    if (!wdev)
        return -1;
    return -1;
}

const struct wifi_ops wifi_stub_ops = {
    wifi_ops_stub_start,
    wifi_ops_stub_stop,
    wifi_ops_stub_scan,
    wifi_ops_stub_connect
};

const char *wifi_state_str(enum wifi_state st)
{
    switch (st) {
    case WIFI_STATE_DOWN: return "DOWN";
    case WIFI_STATE_INIT: return "INIT";
    case WIFI_STATE_SCANNING: return "SCANNING";
    case WIFI_STATE_AUTHENTICATING: return "AUTH";
    case WIFI_STATE_ASSOCIATING: return "ASSOC";
    case WIFI_STATE_CONNECTED: return "CONNECTED";
    case WIFI_STATE_ERROR: return "ERROR";
    default: return "?";
    }
}

int wifi_register(struct wifi_dev *wdev)
{
    if (!wdev || !wdev->name[0])
        return -1;
    if (!wdev->ops)
        wdev->ops = &wifi_stub_ops;
    wdev->next = wifi_list;
    wifi_list = wdev;
    if (!wifi_def)
        wifi_def = wdev;
    if (wdev->state < WIFI_STATE_INIT)
        wdev->state = WIFI_STATE_INIT;
    printf("WIFI_REGISTER %s state=%s\n", wdev->name, wifi_state_str(wdev->state));
    return 0;
}

void wifi_unregister(struct wifi_dev *wdev)
{
    struct wifi_dev **pp;
    if (!wdev)
        return;
    for (pp = &wifi_list; *pp; pp = &(*pp)->next) {
        if (*pp == wdev) {
            *pp = wdev->next;
            break;
        }
    }
    wdev->next = 0;
    if (wifi_def == wdev)
        wifi_def = wifi_list;
    printf("WIFI_UNREGISTER %s\n", wdev->name);
}

struct wifi_dev *wifi_default(void)
{
    return wifi_def;
}

struct wifi_dev *wifi_find(const char *name)
{
    struct wifi_dev *w;
    if (!name)
        return 0;
    for (w = wifi_list; w; w = w->next) {
        if (strcmp(w->name, name) == 0)
            return w;
    }
    return 0;
}

void wifi_dump(void)
{
    struct wifi_dev *w;
    int n = 0;
    for (w = wifi_list; w; w = w->next) {
        printf("wifi %s state=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               w->name, wifi_state_str(w->state),
               w->addr[0], w->addr[1], w->addr[2],
               w->addr[3], w->addr[4], w->addr[5]);
        n++;
    }
    if (!n)
        printf("wifi: no devices registered\n");
}
