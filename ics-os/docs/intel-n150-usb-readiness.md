# Intel N150 USB boot and working-storage readiness

## Goal

Boot ICS-OS on an Intel N150 laptop from a USB thumb drive and keep the same
FAT32 partition mounted at `/icsos` as writable working storage.

## Current qualification

The generated `ics-os-usb.img` is a DOS/MBR disk with one bootable FAT32
partition starting at LBA 2048. It contains GRUB for legacy BIOS and
`EFI/BOOT/BOOTX64.EFI` for 64-bit UEFI. Both paths load the ELF64 Multiboot2
kernel.

The following VirtualBox 7.1 checks pass with the image attached as a PIIX4 IDE
disk:

- BIOS GRUB boot, `hdp0p0` FAT32 root mount, file creation, `fsync`, poweroff,
  raw-image extraction, and byte-for-byte host comparison;
- UEFI GRUB boot and the same persistent-write/readback sequence;
- cleanup uses a disposable image and VM and never opens a physical drive.

Run these gates before flashing:

```bash
cd ics-os
make usb
make test-usb-storage
make test-usb-storage-xhci
make test-usb-storage-xhci-multi-controller
make test-usb-storage-xhci-msix
make test-usb-storage-xhci-msix-recovery
make test-usb-storage-xhci-vector-reservation
make test-usb-storage-xhci-poll
make test-usb-storage-xhci-high-bar
make test-usb-storage-xhci-recovery
make test-usb-storage-xhci-stall-recovery
make test-usb-storage-xhci-disconnect
make test-usb-storage-xhci-mounted-disconnect
make test-usb-storage-xhci-mounted-reconnect
make test-usb-storage-xhci-mounted-remount
make test-usb-storage-xhci-reconnect
make test-usb-storage-xhci-reconnect-mismatch
make test-usb-storage-xhci-reconnect-identity-mismatch
make test-usb-storage-xhci-no-device
make test-vbox-usb-image
make test-vbox-usb-image-efi
```

The QEMU UHCI gate boots the kernel from a separate CD, attaches a disposable
copy of the image only through `piix3-usb-uhci`, requires `usb0p0` to become the
root, performs a guest write and `fsync`, requires SCSI cache synchronization,
and compares the persisted bytes on the host. The QEMU q35 xHCI lane applies
the same contract through `qemu-xhci`. Its no-device lane requires bounded
probe failure, no `usb0` registration, no kernel fault, and continued console
operation. Firmware boot can therefore never be mistaken for USB success.

VirtualBox IDE validates firmware discovery, GRUB, kernel boot, FAT32 mounting,
allocation, block-cache writeback, and persistence. It does not validate that
the kernel can operate the laptop's physical USB controller.

## N150 qualification blockers

Intel N150-class laptops expose modern USB ports through an xHCI controller.
ICS-OS now has an MSI-X xHCI bring-up backend with polling fallback that passes
on QEMU q35. It
performs PCI class discovery, MMIO BAR sizing and register-range validation,
BIOS ownership handoff, controller/port reset, command and event rings,
32/64-byte contexts, scratchpad allocation, slot/address/configuration commands,
control and bulk transfers, BOT mass storage, FAT root I/O, and SCSI cache
synchronization.

This is not yet a production xHCI stack or proof of N150 compatibility. The
remaining blockers are:

1. Add concurrent per-controller USB device and frontend ownership. Ordered
   selection can initialize storage on any of up to eight discovered HCDs, and
   per-HCD MSI-X dispatch is implemented. Add a VT-d backend for translated IOVA
   mappings, non-coherent architecture maintenance, and IOMMU isolation.
   The generic domain control plane and device-scoped bounce mappings now cover
   policy bookkeeping and constrained non-identity streaming buffers, but generic
   translated mappings are not programmed into hardware. ACPI RSDP/XSDT/RSDT and
   checksum-validated DMAR/DRHD/device-scope discovery now passes under QEMU
   `intel-iommu`; root/context/page tables, invalidation, fault handling, and
   translation enablement remain absent.
   MSI-X completion, polling fallback, vector ownership, and handler-drain
   synchronization are implemented with dynamic device-vector allocation,
   domain bounds, and owner-checked software reservations. Firmware/ACPI
   reservation discovery, x2APIC/remapped MSI, general hardware interrupt-domain
   translation, affinity migration, interrupt storms, and physical routing remain
   unqualified.
2. Add hubs, multiple devices, USB 2/3 protocol and endpoint-companion coverage,
   controlled root replacement, and complete surprise-removal safety. A bounded
   polling monitor now covers direct-attached late first attachment and repeated
   remove/add, but does not implement a general USB topology or remount policy.
3. Add sustained and concurrent I/O and broader deterministic fault injection.
   BOT reset, bulk-endpoint stall recovery, bounded whole-controller fallback,
   and cancellation of one active direct-attached transfer are covered in QEMU,
   but do not constitute a complete removal and recovery policy.
4. Qualify 64-byte contexts, nonzero scratchpad counts, firmware handoff, the
   exact N150 controller, every intended port, and the target thumb drive on
   physical hardware.

USB bus-address translation now goes through the shared checked identity-DMA
contract, which rejects invalid alignment, overflow, ranges beyond the 32-bit
device mask, and subranges outside their owner. Its coherent allocator supplies
aligned, zeroed, releasable heap storage and transactionally unwinds partial xHCI
allocation. Every xHCI ring, context, scratchpad, and controller table uses that
allocator, and coherent ordering no longer flushes the whole CPU cache. This
narrows but does not retire blocker 1: registers, rings, DMA regions, device and
recovery state, and IRQ resources now live in `xhci_hcd` objects. Lifecycle,
command, control, bulk, context, scratchpad, recovery, hotplug, event, IRQ, and
DMA helpers receive that object explicitly, with no compatibility field aliases.
PCI discovery records up to eight controllers, and dedicated assembly stubs
route MSI-X by HCD with bind-before-unmask and unbind-after-drain ordering.
`test-usb-storage-xhci-multi-controller` discovers two QEMU controllers and
leaves HCD 0 empty, then validates selection, MSI-X, and persistent storage on
HCD 1. The shared USB frontend still owns only one active HCD and device.
Concurrent controllers, translated IOVA DMA,
non-coherent architecture cache
maintenance, and IOMMU-backed isolation remain open. Direct-attached bulk and control
buffers now have explicit transfer direction and operation-scoped map/unmap
lifetimes across success, timeout, stall, and disconnect. Streaming mappings
support up to 32 segments with transactional unwind; xHCI bulk submission emits
bounded chained segment TRBs. `test-usb-storage-xhci-sg` forces real BOT traffic
through that path and requires MSI-X, root mount, cache sync, and host readback.
The HCD-owned streaming policy can instead force aligned bounce buffers; map and
unmap copy according to transfer direction and enforce the device mask.
`test-usb-storage-xhci-bounce` requires successful bulk traffic in both
directions plus the same durable storage contract. Coherent rings and contexts
remain identity-mapped. `test-usb-storage-xhci-vtd-discovery` boots q35 with
QEMU `intel-iommu`, requires parsed DMAR and DRHD diagnostics, then proves MSI-X,
USB-root mount, cache synchronization, and host-visible persistence while VT-d
translation remains disabled.

MSI-X table entry 0 targets the BSP using a dynamically allocated vector from
the device-vector pool; waiters consume event TRBs outside
hard IRQ context and retain bounded polling. The shared registry rejects vector
collisions and waits for active handlers after xHCI masks the interrupter, halts
the controller, disables MSI-X, and masks the table entry. The combined recovery
gate proves initial claim plus three release/reclaim cycles. This is emulator
evidence, not qualification of the N150 firmware/APIC interrupt route.
`make test-usb-storage-xhci-vector-reservation` holds vector 66 under a separate
platform owner and requires xHCI to use vector 67 for initial setup and all three
recoveries. This validates software reservation exclusion, not discovery of
firmware-reserved vectors.
The device domain now validates and composes the xAPIC MSI address and data used
by xHCI and virtio-blk. This removes duplicated raw LAPIC encoding but does not
qualify x2APIC destinations, interrupt remapping, or N150 firmware routing.

Controller BARs above 4 GiB are mapped through the bounded `KMMIO_BASE`
kernel window and are exercised by `make test-usb-storage-xhci-high-bar`.
That QEMU gate does not replace physical N150 BAR-placement qualification.

`make test-usb-storage-xhci-recovery` deterministically drops three bulk
doorbells. It requires two repeat controller resets, a forced recovery
initialization failure that offlines the device, explicit restoration, sector equality after each
successful recovery, and the normal guest-write/host-readback persistence
contract. This retires the unbounded timeout state as an emulator blocker; it
does not qualify unplug or physical-controller recovery.

`make test-usb-storage-xhci-stall-recovery` sends a malformed CBW that makes
QEMU report a real xHCI Stall Error. It requires a BOT reset, clear-halt and
Reset Endpoint/Set TR Dequeue recovery for each stalled bulk endpoint, then
verifies the retried sector. A second stall drops the selective retry doorbell and must
escalate exactly once through controller reset/re-enumeration before sector
equality and durable host readback pass. This qualifies the direct-attached
BOT path in QEMU, not hubs, removal races, or the physical N150 controller.

`make test-usb-storage-xhci-disconnect` uses QMP to remove the storage device
after a bulk doorbell has been rung and before its event is consumed. The
active wait must terminate when Port Status `CCS` clears, mark storage offline
without a transfer timeout or controller reset, reject a second read
immediately, avoid registering the removed device, and continue to the console.
This covers a deterministic in-flight direct-device removal in QEMU. It does
not cover dirty mounted filesystems, device-object retirement, repeated
removal, hubs, or physical electrical and firmware behavior.

`make test-usb-storage-xhci-mounted-disconnect` runs after `usb0p0` is
registered and mounted as the FAT root. It deliberately creates a dirty cache
page, removes storage during another active transfer, requires clean and dirty
cache identities for `usb0` and its partitions to be invalidated, reports the
discarded dirty page, and rejects a reread which would otherwise be satisfied
from stale cache. The namespace remains mounted but fail-closed. Transparent
remount is not supported. Disconnect now quarantines the parent and partition
device registrations from new lookup while mounted references remain pinned
for fail-closed teardown.
`make test-usb-storage-xhci-mounted-reconnect`
now publishes a fresh parent and partition generation after re-enumeration. It
proves the old mounted partition callback remains offline while the replacement
is discoverable and readable as raw storage.

`make test-usb-storage-xhci-mounted-remount` exercises explicit recovery of the
non-root `/icsos` namespace after same-media identity validation. A process
working in `/icsos/boot` must cause remount rejection without namespace
detachment. After that workdir is released, VFS removes the stale mount, claims
the replacement generation, mounts FAT again, and resolves `/icsos/vmdex`.
Open files and descendant workdirs are never forced closed or relocated. Actual
`vfs_root` replacement and transparent namespace recovery remain unsupported.

`make test-usb-storage-xhci-hotplug` exercises the BSP-pinned 100 ms runtime
monitor through two automatic direct-device remove/add cycles. Attachment is
debounced across three stable samples, each accepted replacement publishes a
fresh raw block generation, and stale mounted callbacks remain offline. The
monitor never remounts a VFS namespace. Failed replacement validation remains
latched until a physical detach; the identity-mismatch target verifies this
with a same-size FAT image whose volume serial changed.

`make test-usb-storage-xhci-late-attach` boots with an empty xHCI controller,
waits for normal console startup, attaches the first USB storage device over
QMP, and requires automatic parent and partition publication. Stable geometry
and volume identity become mandatory only after that first medium establishes
the replacement contract.

Reconnect compares the complete MBR partition geometry and standard volume
identity for every partition: FAT12/16/32 and exFAT volume serials, ext4 UUIDs,
or ISO9660 volume identifiers. Unrecognized filesystems remain available on
initial attachment as raw block devices but are not eligible for reconnect.
`make test-usb-storage-xhci-reconnect-identity-mismatch` attaches a same-size
copy with only its FAT volume serial changed and requires fail-closed rejection.

`make test-usb-storage-xhci-reconnect` follows the same in-flight removal with
QMP `blockdev-add` and `device_add`. The replacement may attach to a different
root port, so reconnect readiness scans all root ports before resetting the
controller and repeating descriptor, endpoint, BOT, and SCSI enumeration. The
gate requires unchanged capacity and block size plus sector equality after a
restored raw read. This qualifies explicit direct-device re-enumeration in
QEMU, while the hotplug targets qualify the bounded polling monitor. Neither
provides transparent recovery of a mounted filesystem and its cached state. The mismatch target attaches a smaller
replacement, requires completed enumeration followed by geometry rejection,
and keeps storage offline. Geometry prevents obvious substitution but is not a
complete stable media-identity policy.

QEMU now provides passing UHCI and q35 xHCI persistence lanes. Emulation is
useful for driver development but does not replace testing the exact laptop
controller, firmware, ports, and thumb drive. VirtualBox does not provide a
useful UHCI compatibility lane for this image.

## Removable-filesystem policy

ICS-OS currently supports writable FAT12/16/32, writable ext4, ISO9660 for CD
media, devfs, and a FAT16-formatted RAM disk. It does not currently implement
exFAT. The proposed default physical USB layout is a small FAT32 EFI/boot
partition plus a separate exFAT work partition after the exFAT driver and its
corruption/removal gates exist. FAT32 remains necessary for broad UEFI firmware
compatibility; exFAT must not become the only firmware-visible partition.

exFAT is preferred for a host-interoperable large work volume because it
supports large files and devices without FAT32's 4 GiB file limit. ext4 remains
the preferred native writable option when Linux/ICS-OS interoperability and
stronger filesystem semantics matter more than direct Windows/macOS access.
See `docs/exfat-adoption-plan.md` for the required implementation and QA gates.

## Other laptop gaps

These do not all block initial serial/VGA boot, but they block a practical or
production-grade N150 laptop experience:

| Area | Current gap | Minimum qualification |
|---|---|---|
| Firmware | UEFI works in VirtualBox; Secure Boot is unsupported | Disable Secure Boot initially; later sign a measured boot chain and define key/update policy |
| ACPI | Modern topology, interrupt routing, power, battery, lid, and sleep support is incomplete | Parse required ACPI tables, validate APIC routing, orderly poweroff, thermal and battery reporting |
| Graphics | Legacy VGA is the current display path | GOP framebuffer handoff or native Intel graphics modesetting; resolution and console tests |
| Input | Legacy keyboard/mouse assumptions | xHCI HID keyboard/touchpad support or laptop-specific PS/2 validation |
| Internal storage | IDE and virtio paths do not cover typical NVMe hardware | NVMe queues, DMA, MSI-X, flush/FUA, timeout/reset, and power-loss tests |
| Networking | RTL8139 does not match typical N150 laptop Ethernet/Wi-Fi | Driver for the exact PCI/USB NIC; Wi-Fi also needs firmware, regulatory, authentication, and crypto support |
| Audio | No modern laptop audio stack | PCI/HDA or SoundWire support for the exact hardware |
| Reliability | FAT has no journal and current recovery coverage is limited | Clean shutdown, durable metadata ordering, corruption detection/repair, removal and power-loss testing |
| Security | No production Secure Boot/IOMMU posture | IOMMU-backed DMA isolation, least-privilege drivers, signed updates, and threat-model validation |

## Capacity and flashing

The default image is intentionally 128 MiB for fast emulator testing. It will
not automatically occupy a 16 GB drive. Build a larger image below the drive's
actual byte capacity, leaving margin for vendor size differences. For example:

```bash
cd ics-os
ICSOS_USB_SIZE_MB=14000 make usb
```

Do not select a size larger than the target device. Check the device immediately
before flashing and ensure none of its partitions are mounted:

```bash
lsblk -o NAME,PATH,SIZE,MODEL,SERIAL,TRAN,MOUNTPOINTS
sudo umount /dev/sdX1
sudo dd if=ics-os-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Replace `/dev/sdX` with the whole thumb drive, never a partition. All data on
that drive is destroyed. Do not flash while the VirtualBox gates fail.

## Physical acceptance gate

The image is not N150-ready until testing on the target laptop proves all of the
following through serial or persistent structured logs:

- Secure Boot state and UEFI boot path are recorded;
- xHCI ownership transfer succeeds and the boot stick is enumerated;
- `/icsos` mounts from USB, not an emulator-only IDE fallback;
- create, overwrite, `fsync`, reboot, and byte-for-byte readback pass;
- repeated multi-megabyte I/O passes without timeout, corruption, or DMA fault;
- unplug during idle and active I/O fails safely without use-after-free;
- controller reset and media reattach recover predictably;
- SMP contention and sustained I/O complete without faults;
- clean shutdown flushes data, and injected power loss has a documented recovery
  result.

Until the physical hardware gate passes, retain the image as an
emulator-qualified BIOS/UEFI USB image, not as a production-ready N150 USB
installation.
