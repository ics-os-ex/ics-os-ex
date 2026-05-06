/*
  Name: usb_msc.c
  Description: USB Mass Storage (Bulk-Only Transport, SCSI transparent)
*/

#include "usb.h"
#include "../../stdlib/stdlib.h"
#include "../../stdlib/time.h"
#include "../../devmgr/dex32_devmgr.h"

// from usb_core.c
usb_device *usb_get_device(int index);
int usb_bulk_transfer(usb_device *dev, int ep, int max_packet, int dir_in, BYTE *data, int length);

#define MSC_CBW_SIGNATURE 0x43425355
#define MSC_CSW_SIGNATURE 0x53425355

typedef struct __attribute__((packed)) _msc_cbw {
    DWORD dCBWSignature;
    DWORD dCBWTag;
    DWORD dCBWDataTransferLength;
    BYTE bmCBWFlags;
    BYTE bCBWLUN;
    BYTE bCBWCBLength;
    BYTE CBWCB[16];
} msc_cbw;

typedef struct __attribute__((packed)) _msc_csw {
    DWORD dCSWSignature;
    DWORD dCSWTag;
    DWORD dCSWDataResidue;
    BYTE bCSWStatus;
} msc_csw;

typedef struct _usb_msc_device {
    usb_device *dev;
    int lun;
    DWORD block_size;
    DWORD total_blocks;
    int devmgr_id;
    char name[16];
} usb_msc_device;

static usb_msc_device msc_devices[USB_MAX_MSC_DEVICES];
static int msc_device_count = 0;

static int msc_find_free_slot()
{
    int i;
    for (i=0;i<USB_MAX_MSC_DEVICES;i++)
    {
        if (msc_devices[i].dev == 0) return i;
        if (msc_devices[i].dev && !msc_devices[i].dev->present) return i;
    }
    return -1;
}

static int msc_count_present()
{
    int i, count = 0;
    for (i=0;i<USB_MAX_MSC_DEVICES;i++)
    {
        if (msc_devices[i].dev && msc_devices[i].dev->present && msc_devices[i].dev->msc_registered)
            count++;
    }
    return count;
}

static int msc_command(usb_msc_device *msc, BYTE *cdb, int cdb_len, BYTE *data, DWORD data_len, int dir_in)
{
    msc_cbw cbw;
    msc_csw csw;
    DWORD tag = (DWORD)msc ^ time();

    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? 0x80 : 0x00;
    cbw.bCBWLUN = msc->lun;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    if (!usb_bulk_transfer(msc->dev, msc->dev->bulk_out.address, msc->dev->bulk_out.max_packet, USB_DIR_OUT, (BYTE*)&cbw, sizeof(cbw)))
        return 0;

    if (data_len > 0)
    {
        if (!usb_bulk_transfer(msc->dev, dir_in ? msc->dev->bulk_in.address : msc->dev->bulk_out.address,
                               dir_in ? msc->dev->bulk_in.max_packet : msc->dev->bulk_out.max_packet,
                               dir_in, data, data_len))
            return 0;
    }

    if (!usb_bulk_transfer(msc->dev, msc->dev->bulk_in.address, msc->dev->bulk_in.max_packet, USB_DIR_IN, (BYTE*)&csw, sizeof(csw)))
        return 0;

    if (csw.dCSWSignature != MSC_CSW_SIGNATURE || csw.dCSWTag != tag || csw.bCSWStatus != 0)
        return 0;

    return 1;
}

static int msc_inquiry(usb_msc_device *msc)
{
    BYTE cdb[6] = {0x12, 0, 0, 0, 36, 0};
    BYTE buf[36];
    return msc_command(msc, cdb, 6, buf, sizeof(buf), USB_DIR_IN);
}

static int msc_test_unit_ready(usb_msc_device *msc)
{
    BYTE cdb[6] = {0x00,0,0,0,0,0};
    return msc_command(msc, cdb, 6, 0, 0, USB_DIR_IN);
}

static int msc_read_capacity(usb_msc_device *msc)
{
    BYTE cdb[10] = {0x25,0,0,0,0,0,0,0,0,0};
    BYTE buf[8];

    if (!msc_command(msc, cdb, 10, buf, sizeof(buf), USB_DIR_IN))
        return 0;

    DWORD last_lba = (buf[0]<<24) | (buf[1]<<16) | (buf[2]<<8) | buf[3];
    DWORD block_size = (buf[4]<<24) | (buf[5]<<16) | (buf[6]<<8) | buf[7];

    if (block_size == 0) return 0;

    msc->block_size = block_size;
    msc->total_blocks = last_lba + 1;
    return 1;
}

static int msc_read10(usb_msc_device *msc, DWORD lba, DWORD blocks, BYTE *buf)
{
    BYTE cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[7] = (blocks >> 8) & 0xFF;
    cdb[8] = blocks & 0xFF;

    return msc_command(msc, cdb, 10, buf, blocks * msc->block_size, USB_DIR_IN);
}

static int msc_write10(usb_msc_device *msc, DWORD lba, DWORD blocks, BYTE *buf)
{
    BYTE cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[7] = (blocks >> 8) & 0xFF;
    cdb[8] = blocks & 0xFF;

    return msc_command(msc, cdb, 10, buf, blocks * msc->block_size, USB_DIR_OUT);
}

static usb_msc_device *usb_msc_find_by_devid(int devid)
{
    int i;
    for (i=0;i<msc_device_count;i++)
    {
        if (msc_devices[i].devmgr_id == devid)
            return &msc_devices[i];
    }
    return 0;
}

static int usb_msc_read_block(int block, char *blockbuff, DWORD numblocks)
{
    int devid = devmgr_getcontext();
    usb_msc_device *msc = usb_msc_find_by_devid(devid);
    DWORD remaining = numblocks;
    DWORD lba = block;
    BYTE *buf = (BYTE*)blockbuff;
    DWORD max_blocks = 64;
    if (!msc) return 0;
    while (remaining)
    {
        DWORD chunk = (remaining > max_blocks) ? max_blocks : remaining;
        if (!msc_read10(msc, lba, chunk, buf))
            return 0;
        lba += chunk;
        buf += chunk * msc->block_size;
        remaining -= chunk;
    }
    return 1;
}

static int usb_msc_write_block(int block, char *blockbuff, DWORD numblocks)
{
    int devid = devmgr_getcontext();
    usb_msc_device *msc = usb_msc_find_by_devid(devid);
    DWORD remaining = numblocks;
    DWORD lba = block;
    BYTE *buf = (BYTE*)blockbuff;
    DWORD max_blocks = 64;
    if (!msc) return 0;
    while (remaining)
    {
        DWORD chunk = (remaining > max_blocks) ? max_blocks : remaining;
        if (!msc_write10(msc, lba, chunk, buf))
            return 0;
        lba += chunk;
        buf += chunk * msc->block_size;
        remaining -= chunk;
    }
    return 1;
}

static int usb_msc_get_block_size()
{
    int devid = devmgr_getcontext();
    usb_msc_device *msc = usb_msc_find_by_devid(devid);
    if (!msc) return 512;
    return msc->block_size;
}

static int usb_msc_total_blocks()
{
    int devid = devmgr_getcontext();
    usb_msc_device *msc = usb_msc_find_by_devid(devid);
    if (!msc) return 0;
    return msc->total_blocks;
}

static int usb_msc_register_device(usb_msc_device *msc, const char *name)
{
    devmgr_block_desc myblock;
    memset(&myblock, 0, sizeof(myblock));

    strcpy(myblock.hdr.name, name);
    strcpy(myblock.hdr.description, "USB Mass Storage Device");
    myblock.hdr.type = DEVMGR_BLOCK;
    myblock.hdr.size = sizeof(myblock);

    myblock.read_block = usb_msc_read_block;
    myblock.write_block = usb_msc_write_block;
    myblock.get_block_size = usb_msc_get_block_size;
    myblock.total_blocks = usb_msc_total_blocks;

    msc->devmgr_id = devmgr_register((devmgr_generic*)&myblock);
    return msc->devmgr_id;
}

typedef struct __attribute__((packed)) _usb_partition_table {
    BYTE active_flag;
    BYTE chs_info1[3];
    BYTE type;
    BYTE chs_end[3];
    DWORD startlba;
    DWORD sector_size;
} usb_partition_table;

typedef struct __attribute__((packed)) _usb_partition_mbr {
    BYTE program[446];
    usb_partition_table tables[4];
    BYTE mbr_magic[2];
} usb_partition_mbr;

static usb_msc_device *msc_part_parent[USB_MAX_MSC_DEVICES];
static DWORD msc_part_start[USB_MAX_MSC_DEVICES];
static DWORD msc_part_size[USB_MAX_MSC_DEVICES];
static char msc_block_names[USB_MAX_MSC_DEVICES][16];
static int msc_block_dev_ids[USB_MAX_MSC_DEVICES];
static int msc_block_count = 0;

static int usb_msc_read_block_partition(int block, char *blockbuff, DWORD numblocks)
{
    int ctx = devmgr_getcontext();
    int i;
    for (i=0;i<msc_block_count;i++)
    {
        if (msc_block_dev_ids[i] == ctx)
        {
            usb_msc_device *msc = msc_part_parent[i];
            DWORD remaining = numblocks;
            DWORD lba = msc_part_start[i] + block;
            BYTE *buf = (BYTE*)blockbuff;
            DWORD max_blocks = 16;
            while (remaining)
            {
                DWORD chunk = (remaining > max_blocks) ? max_blocks : remaining;
                if (!msc_read10(msc, lba, chunk, buf))
                    return 0;
                lba += chunk;
                buf += chunk * msc->block_size;
                remaining -= chunk;
            }
            return 1;
        }
    }
    return 0;
}

static int usb_msc_write_block_partition(int block, char *blockbuff, DWORD numblocks)
{
    int ctx = devmgr_getcontext();
    int i;
    for (i=0;i<msc_block_count;i++)
    {
        if (msc_block_dev_ids[i] == ctx)
        {
            usb_msc_device *msc = msc_part_parent[i];
            DWORD remaining = numblocks;
            DWORD lba = msc_part_start[i] + block;
            BYTE *buf = (BYTE*)blockbuff;
            DWORD max_blocks = 16;
            while (remaining)
            {
                DWORD chunk = (remaining > max_blocks) ? max_blocks : remaining;
                if (!msc_write10(msc, lba, chunk, buf))
                    return 0;
                lba += chunk;
                buf += chunk * msc->block_size;
                remaining -= chunk;
            }
            return 1;
        }
    }
    return 0;
}

static int usb_msc_total_blocks_partition()
{
    int ctx = devmgr_getcontext();
    int i;
    for (i=0;i<msc_block_count;i++)
        if (msc_block_dev_ids[i] == ctx)
            return msc_part_size[i];
    return 0;
}

static int usb_msc_get_block_size_partition()
{
    int ctx = devmgr_getcontext();
    int i;
    for (i=0;i<msc_block_count;i++)
        if (msc_block_dev_ids[i] == ctx)
            return msc_part_parent[i]->block_size;
    return 512;
}

static void usb_msc_register_partitions(usb_msc_device *msc, const char *basename)
{
    usb_partition_mbr *mbr = (usb_partition_mbr*)malloc(sizeof(usb_partition_mbr));
    int i;

    if (!msc_read10(msc, 0, 1, (BYTE*)mbr))
    {
        free(mbr);
        return;
    }

    for (i=0;i<4;i++)
    {
        if (mbr->tables[i].type != 0)
        {
            devmgr_block_desc part;
            memset(&part, 0, sizeof(part));
            sprintf(part.hdr.name, "%sp%d", basename, i);
            sprintf(part.hdr.description, "USB partition %d", i);
            part.hdr.type = DEVMGR_BLOCK;
            part.hdr.size = sizeof(part);
            part.read_block = usb_msc_read_block_partition;
            part.write_block = usb_msc_write_block_partition;
            part.total_blocks = usb_msc_total_blocks_partition;
            part.get_block_size = usb_msc_get_block_size_partition;

            int devid = devmgr_register((devmgr_generic*)&part);
            if (msc_block_count < USB_MAX_MSC_DEVICES)
            {
                strcpy(msc_block_names[msc_block_count], part.hdr.name);
                msc_block_dev_ids[msc_block_count] = devid;
                msc_part_parent[msc_block_count] = msc;
                msc_part_start[msc_block_count] = mbr->tables[i].startlba;
                msc_part_size[msc_block_count] = mbr->tables[i].sector_size;
                msc_block_count++;
            }
        }
    }

    free(mbr);
}

int usb_msc_init()
{
    int i;
    msc_device_count = 0;
    msc_block_count = 0;
    memset(msc_devices, 0, sizeof(msc_devices));

    for (i=0;i<USB_MAX_DEVICES; i++)
    {
        usb_device *dev = usb_get_device_slot(i);
        if (!dev || !dev->present) continue;

        if (dev->bulk_in.address == 0 || dev->bulk_out.address == 0)
            continue;

        int slot = msc_find_free_slot();
        if (slot < 0) break;
        usb_msc_device *msc = &msc_devices[slot];
        memset(msc, 0, sizeof(usb_msc_device));
        msc->dev = dev;
        msc->lun = 0;

        if (!msc_inquiry(msc))
            continue;
        if (!msc_test_unit_ready(msc))
            continue;
        if (!msc_read_capacity(msc))
            continue;

        sprintf(msc->name, "usb%d", slot);
        usb_msc_register_device(msc, msc->name);

        if (msc_block_count < USB_MAX_MSC_DEVICES)
        {
            strcpy(msc_block_names[msc_block_count], msc->name);
            msc_block_dev_ids[msc_block_count] = msc->devmgr_id;
            msc_part_parent[msc_block_count] = msc;
            msc_part_start[msc_block_count] = 0;
            msc_part_size[msc_block_count] = msc->total_blocks;
            msc_block_count++;
        }

        usb_msc_register_partitions(msc, msc->name);
        msc_device_count++;
        dev->msc_registered = 1;
    }

    msc_device_count = msc_count_present();

    return msc_device_count;
}

void usb_msc_rescan()
{
    int i;
    for (i=0;i<USB_MAX_DEVICES; i++)
    {
        usb_device *dev = usb_get_device_slot(i);
        if (!dev || !dev->present || dev->msc_registered)
            continue;

        if (dev->bulk_in.address == 0 || dev->bulk_out.address == 0)
            continue;

        int slot = msc_find_free_slot();
        if (slot < 0) break;
        usb_msc_device *msc = &msc_devices[slot];
        memset(msc, 0, sizeof(usb_msc_device));
        msc->dev = dev;
        msc->lun = 0;

        if (!msc_inquiry(msc))
            continue;
        if (!msc_test_unit_ready(msc))
            continue;
        if (!msc_read_capacity(msc))
            continue;

        sprintf(msc->name, "usb%d", slot);
        usb_msc_register_device(msc, msc->name);

        if (msc_block_count < USB_MAX_MSC_DEVICES)
        {
            strcpy(msc_block_names[msc_block_count], msc->name);
            msc_block_dev_ids[msc_block_count] = msc->devmgr_id;
            msc_part_parent[msc_block_count] = msc;
            msc_part_start[msc_block_count] = 0;
            msc_part_size[msc_block_count] = msc->total_blocks;
            msc_block_count++;
        }

        usb_msc_register_partitions(msc, msc->name);
        msc_device_count++;
        dev->msc_registered = 1;
    }

    msc_device_count = msc_count_present();
}

int usb_msc_get_block_count()
{
    return msc_block_count;
}

const char *usb_msc_get_block_name(int index)
{
    if (index < 0 || index >= msc_block_count) return 0;
    return msc_block_names[index];
}

int usb_msc_get_block_deviceid(int index)
{
    if (index < 0 || index >= msc_block_count) return -1;
    return msc_block_dev_ids[index];
}

