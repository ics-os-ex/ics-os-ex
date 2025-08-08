/*
  Name: exFAT filesystem driver
  Copyright: 
  Author: Generated for ICS-OS exFAT support
  Date: 31/07/25
  Description: This is the implementation of the exFAT filesystem based on the
  "Microsoft exFAT File System Specification" version 1.00 released by 
  Microsoft Corporation.
  
    ICS-OS Educational Operating System
    Copyright (C) 2025

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
*/

#ifndef EXFAT_H
#define EXFAT_H

#include "../vfs/vfs_core.h"

// Integer types supplied by global headers; avoid redefinition here.

// exFAT constants
#define EXFAT_SIGNATURE 0x4146544558 // "EXFAT"
#define EXFAT_SECTOR_SIZE 512
#define EXFAT_CLUSTER_SIZE_MIN 512
#define EXFAT_CLUSTER_SIZE_MAX 33554432 // 32MB

// exFAT entry types
#define EXFAT_TYPE_UNUSED 0x00
#define EXFAT_TYPE_ALLOCATION_BITMAP 0x81
#define EXFAT_TYPE_UPCASE_TABLE 0x82
#define EXFAT_TYPE_VOLUME_LABEL 0x83
#define EXFAT_TYPE_FILE 0x85
#define EXFAT_TYPE_VOLUME_GUID 0xA0
#define EXFAT_TYPE_STREAM_EXTENSION 0xC0
#define EXFAT_TYPE_FILE_NAME 0xC1
#define EXFAT_TYPE_END_OF_DIRECTORY 0x00

// exFAT attributes
#define EXFAT_ATTR_READ_ONLY 0x01
#define EXFAT_ATTR_HIDDEN 0x02
#define EXFAT_ATTR_SYSTEM 0x04
#define EXFAT_ATTR_DIRECTORY 0x10
#define EXFAT_ATTR_ARCHIVE 0x20

#pragma pack(1)

// exFAT Boot Sector structure
typedef struct _exfat_boot_sector {
    uint8_t  jump_boot[3];         // Jump instruction
    uint8_t  fs_name[8];           // "EXFAT   "
    uint8_t  must_be_zero[53];     // Must be zero
    uint64_t partition_offset;     // Partition offset in sectors
    uint64_t volume_length;        // Volume length in sectors
    uint32_t fat_offset;           // FAT offset in sectors
    uint32_t fat_length;           // FAT length in sectors
    uint32_t cluster_heap_offset;  // Cluster heap offset in sectors
    uint32_t cluster_count;        // Number of clusters
    uint32_t first_cluster_of_root_directory; // First cluster of root directory
    uint32_t volume_serial_number; // Volume serial number
    uint16_t file_system_revision; // File system revision
    uint16_t volume_flags;         // Volume flags
    uint8_t  bytes_per_sector_shift; // Bytes per sector shift (power of 2)
    uint8_t  sectors_per_cluster_shift; // Sectors per cluster shift (power of 2)
    uint8_t  number_of_fats;       // Number of FATs
    uint8_t  drive_select;         // Drive select
    uint8_t  percent_in_use;       // Percent in use
    uint8_t  reserved[7];          // Reserved
    uint8_t  boot_code[390];       // Boot code
    uint16_t boot_signature;       // Boot signature (0xAA55)
} exfat_boot_sector;

// exFAT Directory Entry structure
typedef struct _exfat_directory_entry {
    uint8_t entry_type;            // Entry type
    uint8_t secondary_count;       // Number of secondary entries
    uint16_t set_checksum;         // Set checksum
    uint16_t file_attributes;      // File attributes
    uint16_t reserved1;            // Reserved
    uint32_t create_timestamp;     // Create timestamp
    uint32_t last_modified_timestamp; // Last modified timestamp
    uint32_t last_accessed_timestamp; // Last accessed timestamp
    uint8_t  create_10ms_increment; // Create 10ms increment
    uint8_t  last_modified_10ms_increment; // Last modified 10ms increment
    uint8_t  create_utc_offset;    // Create UTC offset
    uint8_t  last_modified_utc_offset; // Last modified UTC offset
    uint8_t  last_accessed_utc_offset; // Last accessed UTC offset
    uint8_t  reserved2[7];         // Reserved
} exfat_directory_entry;

// exFAT Stream Extension Directory Entry
typedef struct _exfat_stream_extension_entry {
    uint8_t entry_type;            // Entry type (0xC0)
    uint8_t general_secondary_flags; // General secondary flags
    uint8_t reserved1;             // Reserved
    uint8_t name_length;           // Name length
    uint16_t name_hash;            // Name hash
    uint16_t reserved2;            // Reserved
    uint64_t valid_data_length;    // Valid data length
    uint32_t reserved3;            // Reserved
    uint32_t first_cluster;        // First cluster
    uint64_t data_length;          // Data length
} exfat_stream_extension_entry;

// exFAT File Name Directory Entry
typedef struct _exfat_file_name_entry {
    uint8_t entry_type;            // Entry type (0xC1)
    uint8_t general_secondary_flags; // General secondary flags
    uint16_t file_name[15];        // File name (UTF-16)
} exfat_file_name_entry;

#pragma pack()

// Function prototypes
int exfat_register(const char *name);
int exfat_mount_root(vfs_node *mountpoint, int id);
int exfat_mountdirectory(vfs_node *directory, int id);
int exfat_openfileEX(vfs_node *f, char *buf, DWORD start, DWORD end, int device_id);
int exfat_writefileEX(vfs_node *f, char *buf, int start, int end, int device_id);
int exfat_createfileEX(vfs_node *node, int device_id);
int exfat_deletefile(vfs_node *node, int device_id);
DWORD exfat_getbytesperblock(int id);
int exfat_validatefilename(const char *filename);
void exfat_rewritefileEX(vfs_node *f, int id);
int exfat_modifyattb(vfs_node *f, const int attb, int id);

#endif // EXFAT_H
