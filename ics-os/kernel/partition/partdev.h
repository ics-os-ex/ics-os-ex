/*
  Name: partdev.h
  Description: Generic partition block device layer shared by the ATA (IDE) and
  USB mass-storage drivers.

  A partition is a window [startlba, endlba) into a parent whole-disk block
  device. The driver supplies raw accessors (which carry any per-driver
  serialization such as the USB io-lock or the IDE bridge call); this layer
  supplies the devmgr-facing callbacks that do context lookup, LBA offset and
  overflow-safe bounds checking. The same layer serves both MBR and GPT
  partitions, preserving the hdpNpX / usb0pX naming contract.
*/
#ifndef PARTDEV_H
#define PARTDEV_H

/* Global partition cap. 192 pushed the kernel BSS to the 4MiB user-ELF wall;
   64 keeps generous headroom for boot+data+test images while freeing BSS. */
#define PARTDEV_MAX           64
#define PARTDEV_MAX_PER_DISK  32
#define PARTDEV_MAX_DISKS     16

#define PARTDEV_TABLE_NONE 0
#define PARTDEV_TABLE_MBR  1
#define PARTDEV_TABLE_GPT  2

/* Driver-specific raw access at an absolute LBA on the parent device.
   `parent_deviceid` is the devmgr id of the whole-disk device. Returns 1 on
   success, 0/-1 on failure. Must apply any per-driver serialization. */
typedef int partdev_raw_fn(int parent_deviceid, u64 lba, char *buf, DWORD nblocks);

typedef struct partdev_entry {
    int mydeviceid;
    int parent_deviceid;
    u64 startlba;
    u64 endlba;
    int block_size;
    int read_only;
    partdev_raw_fn *read_raw;
    partdev_raw_fn *write_raw;
    char type_name[32];
    int entry_index;    /* GPT entry number (sparse) or MBR slot */
    u64 attrs;          /* GPT partition attributes (bit 2 = bootable); 0 for MBR */
    char pname[37];     /* GPT UTF-16-derived label (ASCII); empty for MBR */
} partdev_entry;

typedef struct partdev_disk {
    int parent_deviceid;
    char disk_name[20];
    int table_type;     /* PARTDEV_TABLE_* */
    unsigned char disk_guid[16];
    int used_backup;    /* GPT: backup header was used */
    int gpt_entries;    /* GPT: number of non-empty entries */
} partdev_disk;

/* Register a partition window into parent_deviceid. Returns the new devmgr
   device id, or -1 when the per-disk cap (32) or the global cap is reached. */
int partdev_register(int parent_deviceid, const char *name, const char *description,
                     int block_size, u64 startlba, u64 endlba,
                     partdev_raw_fn read_raw, partdev_raw_fn write_raw,
                     int read_only, const char *type_name, int entry_index,
                     u64 attrs, const char *pname);

/* Remove all partition windows registered under parent_deviceid. The devmgr
   device ids are retired by the owner (the driver quiesces them); this only
   drops the partition metadata so the generic callbacks stop matching. */
void partdev_remove(int parent_deviceid);

/* Record per-disk table information for diagnostics / the 'partitions' cmd. */
void partdev_set_disk(int parent_deviceid, const char *disk_name, int table_type,
                      const unsigned char disk_guid[16], int used_backup, int gpt_entries);

/* Diagnostics access. */
int  partdev_count(void);
const partdev_entry *partdev_get(int index);
int  partdev_is_child(int deviceid, int parent_deviceid);
int  partdev_disk_count(void);
const partdev_disk *partdev_disk_get(int index);

/* Number of partitions registered under parent_deviceid. */
int  partdev_count_children(int parent_deviceid);

/* Device id of the first partition under parent_deviceid, or -1 if none. */
int  partdev_first_child(int parent_deviceid);

/* Core partition I/O keyed on an explicit device id (the devmgr callbacks
   pass devmgr_getcontext(); drivers/tests may call directly). Returns the
   raw accessor's result, or 0 when the partition is unknown/out of bounds. */
int  partdev_partition_read(int deviceid, u64 block, char *buf, DWORD nblocks);
int  partdev_partition_write(int deviceid, u64 block, char *buf, DWORD nblocks);

/* Format a 16-byte GPT GUID into the canonical 8-4-4-4-12 lowercase string
   (first three fields little-endian, last two big-endian). buf must be >=37. */
void partdev_format_guid(const unsigned char *guid, char *buf, int buflen);

#endif
