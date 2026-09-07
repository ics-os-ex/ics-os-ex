# exFAT adoption plan

## Decision

exFAT should become the default **work-volume** filesystem for physical USB
images when direct access from ICS-OS, Windows, and macOS is required. It must
not replace the FAT32 EFI/boot partition. The target image layout is:

1. A small FAT32 partition containing GRUB, EFI boot files, the kernel, and
   recovery assets.
2. An exFAT partition using the remaining capacity, mounted as `/work`.

ext4 remains the preferred native work filesystem when stronger validation and
Linux interoperability are more important than direct host portability.

## Existing filesystem roles

| Filesystem | Current role | Policy |
|---|---|---|
| FAT12/16/32 | Writable boot/root media and legacy disks | Keep; FAT32 remains the firmware partition |
| ext4 | Writable virtio work disk with host `e2fsck` validation | Keep as the native writable option |
| ISO9660 | CD/live-media root | Keep read-only media role |
| devfs | VFS projection of registered devices | Keep; not an on-disk format |
| FAT16 RAM disk | Volatile compiler/test scratch storage | Keep; exFAT adds no benefit here |
| exFAT | Not implemented | Add as a separate VFS filesystem driver |

## Required implementation

The driver must follow the Microsoft exFAT 1.00 specification rather than
sharing FAT12/16/32 on-disk assumptions. At minimum it must implement:

- 512-byte through 4096-byte logical sectors and checked 64-bit arithmetic;
- main and backup boot-region validation, including boot checksums and field
  bounds before allocating memory or reading derived locations;
- FAT chain traversal plus contiguous `NoFatChain` streams;
- allocation bitmap discovery, validation, allocation, and free accounting;
- compressed and uncompressed up-case tables with checksum validation;
- UTF-16 file names, name hashes, and checksummed file/stream/name entry sets;
- valid-data-length behavior, timestamps and UTC offsets, volume dirty/media
  failure flags, and preservation of reserved and unknown benign entries;
- ordered metadata updates, `fsync`, unmount flush, read-only fallback for
  unsupported or inconsistent volumes, and fail-closed surprise removal.

TexFAT is out of scope for the first driver. A two-FAT TexFAT volume must not be
silently mounted writable as ordinary exFAT.

## Delivery gates

1. Add host-native parser/checksum tests using valid, truncated, overflowed,
   cyclic, cross-linked, and checksum-corrupt structures.
2. Add a read-only VFS driver and compare directory/file reads against images
   produced by `mkfs.exfat` and checked by `fsck.exfat`.
3. Add create, extend, truncate, rename, unlink, mkdir, and large-file writes;
   require clean host `fsck.exfat` and byte-for-byte readback after each case.
4. Add full/fragmented media, 512/4096-byte sectors, `NoFatChain` conversion,
   Unicode/case-folding, dirty-volume, and unknown-entry tests.
5. Reuse the USB timeout, stall, disconnect, mounted-cache, reconnect, mismatch,
   SMP, and persistence lanes. Inject removal at every metadata write-ordering
   boundary and verify the resulting volume is either consistent or detected
   as dirty, never silently accepted as clean.
6. Only then change the image builder and `/work` auto-mount preference from
   FAT to exFAT. Keep an ext4 image option and a FAT32-only compatibility image.

Geometry is not media identity. Replacement device-manager generations, cache
invalidation, and exFAT volume-serial comparison are available. Automatic
remount still requires VFS quiescing of open files and working directories plus
an explicit namespace recovery policy.