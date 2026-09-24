# Intel N150 USB boot and working-storage readiness

## Goal

Boot ICS-OS on an Intel N150 laptop from a USB thumb drive and keep the same
FAT32 partition mounted at `/icsos` as writable working storage.

## Current qualification

The generated `ics-os-usb.img` is a DOS/MBR disk with one bootable FAT32
partition starting at LBA 2048. It contains GRUB for legacy BIOS and
`EFI/BOOT/BOOTX64.EFI` for 64-bit UEFI. Both paths load the ELF64 Multiboot2
kernel.

For Intel N150-class laptops (64-bit UEFI, typically no CSM) flash
`ics-os-uefi.img` instead. That image is a GPT disk with a protective MBR and
one FAT32 EFI System Partition that holds `EFI/BOOT/BOOTX64.EFI`. Build it
with `make usb-etcher` (also writes `ics-os-uefi.img.zip` for Balena Etcher).
That image includes the dist toolchain (`gcc`/`cc1`, `as`/`ld`/`ar`/`objcopy`,
`make`, `tcc`, SDK runtime objects, ldscripts). `make test-usb-uefi-gpt`
boots that image under OVMF on q35 xHCI.

On the physical N150, GRUB may print `error: serial port 'com0' isn't found`.
That is expected (no COM0). After `Loading ICS-OS (multiboot2)...` the kernel
keeps the GOP linear framebuffer (`gfxterm` + `gfxpayload=keep`) and live-
blits the 80x25 console after the scheduler, including Intel-padded pitch.

Keyboard LEDs remain a boot breadcrumb. This laptop has no Scroll Lock LED,
so only Caps and Num count. Stage 0 first turns **both off** (clears firmware
Num Lock) then Caps on.

| Caps | Num | Last stage reached |
| --- | --- | --- |
| on | off | **0** kernel C, **1** IDT, **4** CPU info, **5** ext, **8** VT-d, **9** ports, **12** kbd, **13** LAPIC, **16** taskswitcher |
| off | on | **2** mem_init, **6** device mgr, **10** pci/nic, **14** process mgr |
| on | on | **3** console, **7** alloc, **11** API, **15** APs, or a kernel halt |

GOP is mapped write-combining after the scheduler Caps hold (identity if the
buffer is below 4 GiB, otherwise `KFB_BASE`). Treat the two columns as **Caps**
and **Num** (never Scroll Lock):

| Caps | Num | After scheduler |
| --- | --- | --- |
| on | on | GOP mapped; live 80x25 console |
| off | on | no framebuffer tag, **or** hung while mapping a high GOP |
| on | off | map still refused (buffer larger than 64 MiB) |

The 80x25 grid uses per-axis zoom from the Multiboot2 tag. A line
`FB WxH pitch=… zoom=XxY` is printed after GOP maps. 1920x1200 is 3x3 (fills
the panel). 1920x1080 is 3x2 (full width, letterbox top/bottom).

This image skips AP bring-up without COM1 (BSP only), skips the extra CPUID
in `smp_rdtscp_available`, and programs the LAPIC timer via x2APIC MSRs
(`lapic_present()`). virtio-blk probe on this laptop is bus 0 only and
skips empty/non-MF PCI functions. On no-COM1 laptops the virtio PCI scan
is skipped entirely (`virtio-blk: skip pci scan`) — even bus 0 alone could
freeze ~half the boots at `virtio-blk: pci bus 0`. Pico UART bridges still need a 16550 COM1 header; a USB
plug is not COM1. ICS-OS xHCI CDC-ACM host can take the console from a Pico
plugged in as a USB serial gadget (`USB_CDC_CONSOLE_OK`). The Pico Wi-Fi
HTTP API (`GET /v1`, `/health`, `/version`, `/status`, `/screen`, `/fb.ppm`, `POST /cmd`,
`/keys`, `/kexec`, `/reboot`) is the agent remote-debug path; telnet :23
forwards keystrokes over USB as well as UART.

`GET /health` does not need kernel RPC. It reports `pico=` (bridge firmware)
and `kernel=` (the `ICSOS_VER` bind stamp scraped from `/log`, or `none` if
the laptop booted an image older than that stamp). `compiled=` on that line
is `__DATE__`/`__TIME__` of the kernel object, so two dirty-tree etcher
builds are distinguishable even when git hash is unchanged.

```bash
curl http://192.168.0.174/health
curl http://192.168.0.174/version
```

### Remote kernel update (skip thumb-drive reflash)

Once the Pico bridge is up and CDC RPC answers `/status`, push a new
kernel over Wi-Fi instead of rebuilding `ics-os-uefi.img` and re-etching:

```bash
cd ics-os
make -C kernel bzImage
./scripts/remote-kexec.sh    # or curl --data-binary @kernel/Kernel64.bin http://PICO/kexec
```

Flow: Pico `POST /kexec` → CDC `KEXEC` → `kexec_load_mem` → ACK →
`kexec_reboot`. The image runs from RAM; `/vmdex` on the USB ESP is
**not** rewritten during this path (MSC persist wedged N150 mid-reboot).
Cold boot still needs `make usb-etcher` (or a future quiet file-PUT)
until persist is restored. Cap 8 MiB; allow a few minutes for transfer.
Verify with `/status` (`kexeced=1`, `compiled=`) after the target comes
back.

### Capture laptop Wi-Fi / NIC PCI IDs

There is still no Wi-Fi driver. To collect the exact PCI IDs, BARs, and
capability list for eventual support, flash an image that includes the
`pciwifi` / `pci` console commands, then dump over the Pico bridge:

```bash
cd ics-os
make usb-etcher          # flash ics-os-uefi.img
# after CONSOLE_READY + CDC STATUS ok:
./scripts/capture-wifi-hw.sh 192.168.0.174
# or interactively:  pciwifi   then   pci
```

`pciwifi` (alias `wifi`) prints only PCI class `0x02` (network) and
`0x0D` (wireless) with `WIFI_HW_BEGIN`/`END` markers. `pci` dumps every
present function (empty slots skipped — same N150-safe policy as xHCI).
The capture script saves `wifi-hw.txt`, `pci-hw.txt`, `/health`, and
`/status` under `wifi-hw-capture-<UTC>/`. Do not auto-run this from
`autoexec.bat` on ADL-N.

**Captured on this N150 (2026-09-15):** one Wi-Fi NIC at `PCI 1:0.0`
`10ec:c821` — Realtek **RTL8821CE** 802.11ac (`class=0280`, BAR2
`0x80500000`, MSI+PCIe caps). Linux reference driver: `rtw88_8821ce`.
Artifacts: `ics-os/wifi-hw-capture-n150/`. No ethernet-class NIC on PCI.

### RTL8821CE driver (bring-up)

ICS-OS includes a Dual-BSD/GPL-derived rtw88 subset under
`kernel/hardware/wifi/rtw88/` plus a thin `wifi_dev` framework
(`kernel/net/wifi.c`). After root mount the kernel probes the card,
powers the MAC, and loads `/icsos/firmware/rtw88/rtw8821c_fw.bin`
(linux-firmware redistributable blob staged from `base/firmware/`).

Boot markers to look for (serial / Pico `/log`):

| Marker | Meaning |
|--------|---------|
| `RTL8821CE_PROBE_OK` | PCI found, BAR2 MMIO live (`SYS_CFG1` readable) |
| `RTL8821CE_POWER_OK` | `card_enable_flow_8821c` power-on succeeded |
| `RTL8821CE_FW_OK` | Firmware downloaded; `REG_MCUFW_CTRL` matches `FW_READY` |
| `RTL8821CE_FW_MISSING` | Firmware file not on ESP |
| `RTL8821CE_FW_FAIL` | Download / checksum / ready poll failed |
| `RTL8821CE_PHY_OK` | MAC/AGC/BB/RF tables loaded and applied |
| `WIFI_REGISTER wlan0` | `wifi_dev` registered |

Console: `wifistat` (driver state), `wifiscan` (2.4 GHz passive scan), and
`pciwifi` (PCI dump). Firmware, efuse, RX descriptors, file-backed PHY tables,
and reference RF18 20 MHz channel tuning are implemented. Association, secured
station operation, TX, and Wi-Fi/netif integration remain incomplete. A scan is
qualified only when N150 serial reports `WIFI_SCAN_DONE bss=>0`; QEMU cannot
model this device.

**Console after Root mount:** older etcher images could leave GOP stuck on
`Root mount [OK]` because `usb_cdc_pump` ran on the init thread before the
console existed (with or without the Pico). Current images print
`CONSOLE_READY` after a light autoexec (PATH/SDK only — **no** boot-time
`copy` into `/ramdisk`, which hung on ADL-N MSC while CDC was pumping),
start xHCI hotplug only after that, leave Caps Lock off for tmux keys, and
drop to a kernel prompt (type `sh` for the POSIX shell). Dist/etcher
`gcc.exe` links SDK `.o` files from `/icsos/apps` when `/ramdisk` has none.
Large apps (`vim.exe` ~2 MiB) stream from USB; if the console freezes,
**Ctrl-C** aborts the wait, **F4** force-kills the foreground, **C-b c**
opens a fresh kernel prompt. Pico `/status` must stay up across repeated
`ls` / MSC: do **not** Stop-EP the posted CDC IN at each BOT (Intel
ADL-N wedges CDC RX after a few cancel cycles). Event-ring stash keeps
MSC waits from stealing CDC completions. After each MSC sector, only
**arm/take** CDC IN (`usb_cdc_after_msc`) — a full 40k-spin
`usb_cdc_pump` per block hung `hello.exe` stream loads. ELF stream
loads call `usb_cdc_bulk_io_begin/end` (quiesce CDC; take completed IN
only — do not abandon a live TRB). After each MSC sector unlock,
`usb_cdc_after_msc` re-arms IN so Pico `/status` survives repeated `ls`.

**Next on this laptop:** qualify physical xHCI USB root and writable `/icsos`.
Intel ADL-N xHCI is `8086:54ed` at `PCI 0:20.0`, BAR `0x6001100000` (64 KiB,
above 4 GiB), 34 scratchpads, 16 ports. A previous flash mapped it and saw
CCS on ports 4/5/8, then `control timeout request=6` on the first bound
device (GET_DESCRIPTOR) and hotplug `device reconnect failed`. This image restores the PORTSC reset that enabled ports 4/5 (no PRC-clear),
posts control TDs the way Intel xHCI requires (Setup Chain clear; first TRB
cycle inverted until Data/Status are written), and tries every CCS port.
Look for `usb: vid=` / `MSC on port` / `[OK]`. `ccs=0x99` is ports 1, 4, 5, 8.

Num Lock during USB probe; Caps+Num returns after it finishes. Do not
re-enable APs until MADT is parsed.

`make test-vbox-uefi-gpt` and `make test-vbox-uefi-gpt-bios` boot it in
VirtualBox EFI and BIOS. `make test-bochs-uefi-gpt` boots it under Bochs
BIOS (i386-pc GRUB lives in the GPT gap so ESP remains partition 0).

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
| Graphics | GOP handoff via GRUB `gfxterm` + `gfxpayload=keep`; kernel late-maps write-combining GOP after the scheduler on no-COM1; QEMU still maps early for `FBCONSOLE_PASS` | Physical N150 1920x1200 panel must show a centered `ICS-OS` bar after Caps+Num; `test-usb-uefi-gpt` still only proves OVMF GOP |
| Input | Legacy keyboard/mouse assumptions | xHCI HID keyboard/touchpad support or laptop-specific PS/2 validation |
| Internal storage | IDE and virtio paths do not cover typical NVMe hardware | NVMe queues, DMA, MSI-X, flush/FUA, timeout/reset, and power-loss tests |
| Networking | N150 has Realtek RTL8821CE (`10ec:c821` at `1:0.0`); bring-up driver in-tree | Prove `RTL8821CE_FW_OK` on hardware; then TX/RX + 802.11 assoc/regdb |
| Audio | No modern laptop audio stack | PCI/HDA or SoundWire support for the exact hardware |
| Reliability | FAT has no journal and current recovery coverage is limited | Clean shutdown, durable metadata ordering, corruption detection/repair, removal and power-loss testing |
| Security | No production Secure Boot/IOMMU posture | IOMMU-backed DMA isolation, least-privilege drivers, signed updates, and threat-model validation |

## Capacity and flashing

The default image is intentionally 128 MiB for fast emulator testing. It will
not automatically occupy a 16 GB drive. Build a larger image below the drive's
actual byte capacity, leaving margin for vendor size differences. For example:

```bash
cd ics-os
ICSOS_USB_SIZE_MB=14000 make usb-etcher
```

Do not select a size larger than the target device.

### Balena Etcher (N150)

1. On the laptop firmware setup, disable Secure Boot and enable USB boot.
2. Build `make usb-etcher` and open `ics-os-uefi.img` or `ics-os-uefi.img.zip`
   in Balena Etcher. Select the whole thumb drive, then Flash.
3. Etcher writes a raw GPT disk image. Do not unzip onto the stick as files.
4. Boot from the USB device (UEFI, not legacy/CSM).

Alternatively, check the device immediately before flashing and ensure none of
its partitions are mounted:

```bash
lsblk -o NAME,PATH,SIZE,MODEL,SERIAL,TRAN,MOUNTPOINTS
sudo umount /dev/sdX1
sudo dd if=ics-os-uefi.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Replace `/dev/sdX` with the whole thumb drive, never a partition. All data on
that drive is destroyed. Do not flash while `make test-usb-uefi-gpt` fails.

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
