# ICS-OS serial bridge — Waveshare ESP32-S3-LCD-1.47

A USB-to-serial debug coprocessor for booting ICS-OS on real hardware. The
board plugs into the host PC over its USB **Type-A** port (the ESP32-S3's
native USB — there is no bridge chip on this board) and captures the target
machine's **COM1 (0x3F8) @ 115200 8N1** boot log. Everything from kernel
handoff onward is printed there by ICS-OS; GRUB output depends on the image
(see "Why this works with ICS-OS").

```
host PC  <==USB CDC===>  ESP32-S3 (this board)  <==UART0 115200===>  target COM1
              |                    |
              |                    +-- 172x320 LCD: live scrolling console
              |                    +-- TF/SD card: persistent capture (/sdcard/icsos-*.log)
```

Every byte the target prints on COM1 TX appears, with no delay budget beyond
SPI row rendering, on:

1. the host's USB serial port (`/dev/ttyACM0`-style), so `minicom`/`screen`/
   `picocom`/PuTTY all work;
2. the onboard LCD (28 cols x 40 rows of 5x7 font, green-on-black, status bar
   on top);
3. a log file on the TF card (rotates at 512 KB, newest kept).

Keystrokes typed on the host's USB serial port are forwarded to the target's
COM1 RX, so the BIOS-image GRUB serial console (and any in-kernel serial
shell) stays reachable through the bridge.

## Why this works with ICS-OS

- `ics-os/kernel/hardware/chips/serial.c` drives COM1 (0x3F8) at 115200 8N1
  from the very start of `serial_init()` (`uart_hw_init`: divisor 1, 8N1,
  FIFO enabled). The kernel does raw port I/O, so this works on real
  hardware regardless of the firmware that loaded it.
- UEFI thumb-drive image: GRUB under UEFI cannot probe the legacy COM port
  (`mkusb-uefi.sh` deliberately does not — "serial port 'com0' isn't found"),
  so UEFI-firmware and GRUB messages only reach the display; **everything
  from kernel handoff onward reaches COM1**, which is exactly where ICS-OS
  boot problems show up (SMP bring-up, root mount, exec, faults).
- BIOS thumb-drive image (`mkusb.sh`): GRUB *does* use
  `serial --unit=0 --speed=115200` there, so GRUB output is captured as well.

## Hardware notes (verified against the Waveshare schematic + demo firmware)

| Item | Value |
|------|-------|
| SoC | ESP32-S3-R8N16: 16 MB flash, 8 MB octal PSRAM |
| USB port J1 (Type-A) | native USB: D- = GPIO19, D+ = GPIO20 (22 Ω R9/R10). No USB-UART bridge chip. Powers the board from a USB-A cable (5 V). |
| Reserved UART header (4× H2.8) | nets: `TXD`, `RXD`, `GND`, `3V3`. TXD → **R7 (499 Ω)** → U0TXD = **GPIO43**; RXD → **R6 (1 K)** → U0RXD = **GPIO44**. |
| LCD ST7789 172x320 | MOSI 45, SCLK 40, CS 42, DC 41, RST 39, BL 48 (SPI2/FSPI, 80 MHz, MODE0, portrait) |
| TF card (SD_MMC) | CMD 15, SCK 14, D0 16, D1 18, D2 17, D3 21 |
| RGB LED strip | GPIO38 (WS2812, 192 LEDs) — **left unlit** by this firmware |

### Soldering R6/R7 (required once)

The reserved UART header is **not populated at factory**: the wiki states the
reserved interface "can only be enabled by re-soldering the resistor". Populate
both:

- **R7 = 499 Ω** — in the `TXD` path (header → GPIO43)
- **R6 = 1 K** — in the `RXD` path (header → GPIO44)

A plain 0 Ω jumper also works for either; the series values are for
line-damping/ESD-ish behaviour. Without them the header is electrically dead
and the bridge will show the LCD/USB but never any target output.

## Wiring to the target machine

Target COM1 = 16550-style UART at 0x3F8. You need a COM1 header/cable on the
target (or a DB9 serial port adapter). A USB plug is not COM1; this board
cannot debug an N150-class laptop that has no serial port.

| Target COM1 | ESP32-S3 header pin |
|-------------|---------------------|
| **TX** (out) | `RXD` (GPIO44) |
| **RX** (in, optional) | `TXD` (GPIO43) |
| GND | `GND` |

Do **not** connect the header's `3V3` to the target — 3.3 V levels are
compatible but sharing power grounds between the two machines is not
required and the target's serial port is 3.3 V tolerant anyway. Keep the
cable short.

The target boots from its USB thumb drive as normal (UEFI). The ESP32-S3 is
powered from the host PC over its Type-A port (use a short USB-A → A cable;
the port on the board is Type-A on both the board side and the connector, so
any standard A-A patch cable works).

## Building

Requires PlatformIO (already installed on this host) and the `espressif32`
platform (installed). The board definition lives in `boards/` (it is the
stock ESP32-S3R8N16 config: `memory_type=qio_opi`, `variant=esp32_s3r8n16`,
CDC-on-boot).

```bash
cd extras/ics-esp32-serial-bridge
pio run                        # build
pio run -t upload              # flash
pio device monitor             # optional: monitor the bridge's own console
```

## Tests

The LCD line-buffer logic (`src/bridge_console.c`) is plain C and is linked
into the firmware, so it is covered by a host-native TAP test that compiles
that exact file — see `make test-bridge-console-unit` in `ics-os/`
(`ics-os/tests/bridge_console_unit.c`): commit/truncate/scroll/dirty
tracking, 19 cases. The ST7789 init sequence, pin map, and SD_MMC options are
copied from the Waveshare demo firmware (which is known to work on this
board); those paths need on-board verification.

## Flashing

1. Connect the board to the host PC (Type-A port).
2. `pio run -t upload`.
3. If the port is not auto-detected, force download mode: hold **BOOT**, tap
   **RESET**, release **BOOT**. (The board's USB port is the native
   USB-Serial-JTAG, so a normal `pio run -t upload` usually just works.)
4. After boot, the host gets a USB CDC serial port. Open it at **115200 8N1**
   (any baud works for CDC; 115200 matches the target side):

   ```bash
   # Linux
   picocom -b 115200 /dev/ttyACM0
   # or
   minicom -D /dev/ttyACM0 -b 115200
   # Windows: PuTTY -> Serial -> <COMx> -> 115200
   ```

5. Boot the target. Its COM1 output (UEFI → GRUB → kernel → userspace)
   streams to the host port, the LCD, and the TF card.

## What to look for

- The host port prints a one-time self-test banner (`ICS-OS serial bridge ...
  LCD ... ok | SD card: ok`) when the ESP32-S3 boots.
- The LCD shows a status bar (`ICS-OS BRIDGE SD:ok`) and the target's log
  scrolling underneath.
- If the host port is silent but the banner was printed: R6/R7 not soldered,
  or TX/RX swapped on the target side (swap and retest).
- If the LCD shows the banner but the host port is silent: check the USB
  cable/port and that the OS sees the CDC device.
- A full boot of the **UEFI** thumb-drive image shows kernel output only:
  kernel serial init → `Root mount [OK]` etc. (UEFI GRUB and OVMF do not
  print to COM1, by design in `mkusb-uefi.sh`). A full boot of the **BIOS**
  image additionally shows GRUB's serial output (`serial --unit=0`), exactly
  as on `make test-usb-uefi` / `make test-boot` in QEMU.

## TF card log

Each boot creates `/sdcard/icsos-<id>.log` on the card. The mount uses the
same options as the Waveshare demo (`SD_MMC.setPins(...)` + 1-bit MMC +
format-on-mount-failure), so a blank/unformatted card is initialized on first
use. The file holds the most recent 512 KB of target output (older bytes are
discarded when the size cap is hit). If the LCD status bar shows `SD:NO`,
check the card is a proper MMC/SD and reseated.

## Out of scope

- The RGB LED strip (GPIO38) is not driven.
- The LCD is display-only (no touch input on this board revision's touch
  controller in this firmware).
- This is a debug aid, not part of the ICS-OS image; the thumb-drive boot
  itself is unchanged.
