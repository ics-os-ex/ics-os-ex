#ifndef ICSOS_VIRTIO_H
#define ICSOS_VIRTIO_H

#include "../../types.h"

/* OASIS virtio 1.2 — PCI modern transport + virtio-blk. */

#define VIRTIO_VENDOR_ID          0x1AF4
#define VIRTIO_DEV_BLK_TRANS      0x1001
#define VIRTIO_DEV_BLK_MODERN     0x1042
#define VIRTIO_DEV_NET_TRANS      0x1000
#define VIRTIO_DEV_NET_MODERN     0x1041
#define VIRTIO_ID_BLOCK           2
#define VIRTIO_ID_NET             1

#define VIRTIO_NET_F_MAC          5
#define VIRTIO_NET_F_STATUS       16
#define VIRTIO_NET_S_LINK_UP      1

/* With VIRTIO_F_VERSION_1 the header always includes num_buffers. */
struct virtio_net_hdr {
   u8 flags;
   u8 gso_type;
   u16 hdr_len;
   u16 gso_size;
   u16 csum_start;
   u16 csum_offset;
   u16 num_buffers;
} __attribute__((packed));

struct virtio_net_config {
   u8 mac[6];
   u16 status;
} __attribute__((packed));


#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG    5

#define VIRTIO_PCI_CAP_VNDR       0x09
#define PCI_CAP_ID_MSIX           0x11

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1        32
#define VIRTIO_F_ACCESS_PLATFORM  33

#define VIRTIO_BLK_F_SIZE_MAX     1
#define VIRTIO_BLK_F_SEG_MAX      2
#define VIRTIO_BLK_F_RO           5
#define VIRTIO_BLK_F_BLK_SIZE     6
#define VIRTIO_BLK_F_FLUSH        9
#define VIRTIO_BLK_F_DISCARD      13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

#define VIRTQ_DESC_F_NEXT         1
#define VIRTQ_DESC_F_WRITE        2

#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4

#define VIRTIO_BLK_S_OK           0
#define VIRTIO_BLK_S_IOERR        1
#define VIRTIO_BLK_S_UNSUPP       2

#define VIRTIO_MSI_NO_VECTOR      0xFFFF

#define VIRTIO_QUEUE_MAX          128
#define VIRTIO_BLK_SECTOR_SIZE    512

struct virtq_desc {
   u64 addr;
   u32 len;
   u16 flags;
   u16 next;
} __attribute__((packed));

struct virtq_avail {
   u16 flags;
   u16 idx;
   u16 ring[VIRTIO_QUEUE_MAX];
} __attribute__((packed));

struct virtq_used_elem {
   u32 id;
   u32 len;
} __attribute__((packed));

struct virtq_used {
   u16 flags;
   u16 idx;
   struct virtq_used_elem ring[VIRTIO_QUEUE_MAX];
} __attribute__((packed));

struct virtio_blk_req {
   u32 type;
   u32 reserved;
   u64 sector;
} __attribute__((packed));

struct virtio_pci_common_cfg {
   u32 device_feature_select;
   u32 device_feature;
   u32 driver_feature_select;
   u32 driver_feature;
   u16 msix_config;
   u16 num_queues;
   u8  device_status;
   u8  config_generation;
   u16 queue_select;
   u16 queue_size;
   u16 queue_msix_vector;
   u16 queue_enable;
   u16 queue_notify_off;
   u64 queue_desc;
   u64 queue_driver;
   u64 queue_device;
} __attribute__((packed));

#endif
