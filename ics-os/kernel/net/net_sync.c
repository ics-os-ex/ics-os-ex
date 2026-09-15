#include "net_sync.h"

extern int smp_cpu_id(void);

static spinlock_t net_biglock;
static volatile int net_owner = -1;
static volatile int net_depth;
static spin_irq_flags_t net_irq_flags;
static int net_inited;

static void net_sync_init(void)
{
    if (net_inited)
        return;
    spin_init(&net_biglock);
    net_owner = -1;
    net_depth = 0;
    net_inited = 1;
}

void net_lock(void)
{
    int cpu;

    if (!net_inited)
        net_sync_init();
    cpu = smp_cpu_id();
    if (net_owner == cpu && net_depth > 0) {
        net_depth++;
        return;
    }
    {
        spin_irq_flags_t f = spin_lock_irqsave(&net_biglock);
        net_owner = cpu;
        net_depth = 1;
        net_irq_flags = f;
    }
}

void net_unlock(void)
{
    if (net_depth <= 0)
        return;
    if (--net_depth > 0)
        return;
    net_owner = -1;
    spin_unlock_irqrestore(&net_biglock, net_irq_flags);
}

int net_lock_held(void)
{
    return net_owner == smp_cpu_id() && net_depth > 0;
}
