#ifndef ICSOS_VIRTIO_NET_H
#define ICSOS_VIRTIO_NET_H

void virtio_net_init(void);
void virtio_net_irq(void);
void virtio_net_poll(void);
int  virtio_net_present(void);

#endif
