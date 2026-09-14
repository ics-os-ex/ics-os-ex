# Pico 2 W serial bridge for ICS-OS

A **no-solder** replacement for the ESP32-LCD debug bridge, built for a
Raspberry Pi **Pico 2 W** (RP2350 + CYW43439 Wi-Fi). It captures the target
machine's **COM1 (0x3F8) @ 115200 8N1** boot output, keeps a rolling copy on
the Pico's own flash (littlefs), and **streams it over Wi-Fi** — so you can
watch a boot from across the room with no cable to a host PC.

## What it does

| Function | Where |
|---|---|
| Capture target COM1 TX | UART0, **RX = GPIO17** (TX = GPIO16) |
| Store a rolling log | `/boot.log` on the Pico's littlefs (cap 512 KB, keeps newest half on rotate) |
| Live tail | TCP **:23** — replays the buffer, then follows; typed bytes forwarded to target COM1 RX |
| Grab the buffer | HTTP **:80** — `curl http://<ip>/` (also `/health`, `/reset`) |
| Wi-Fi | **AP mode by default** (own hotspot `ICSOS-DEBUG`) — or STA to an existing network |

No LCD, no TF card, no USB host port, no soldering. The Pico's flash *is* the
storage, and Wi-Fi *is* the transport.

## Hardware facts (verified, not guessed)

- **Pico 2 W** = RP2350 + Infineon CYW43439 (2.4 GHz 802.11n).
- **UART0** default pins GPIO0/1; **alternate GPIO16(TX)/GPIO17(RX)** used here
  to keep GPIO0 (a boot/flash pin) free. Both are on the standard 40-pin header.
- **Wi-Fi**: MicroPython `network.WLAN` — the CYW43439 **cannot run STA and AP
  at the same time**, so the firmware picks one (AP by default).
- **Storage**: 4 MB flash; the standard `RPI_PICO2_W` MicroPython image's
  littlefs is **~1.4 MB**. Check on-device with `os.statvfs('/')`. A boot log
  is KB–low-100 KB, so 1.4 MB holds one full log with room (or several with
  rotation).

## Wiring (target → Pico)

Only three wires, from the target machine's serial header:

| Target COM1 | → Pico |
|---|---|
| **TX** | **GPIO17** (UART0 RX) |
| **RX** (optional) | GPIO16 (UART0 TX) — for keyboard/GRUB input forwarding |
| **GND** | GND |

Do **not** wire the Pico's 3V3 to the target's 5 V TX line — the Pico's UART
is 3.3 V. If your target COM1 is 5 V single-ended, add a 3.3 V level shifter
or a simple 1 K / 2 K divider on the TX→RX path. (ICS-OS on a PC header is
usually 3.3 V, in which case direct is fine.)

## Flashing

1. Load the `RPI_PICO2_W` MicroPython firmware (UF2) if not already present.
2. Copy `main.py` to the Pico's flash:
   - Thonny: open `main.py` → *File → Save as → Raspberry Pi Pico → main.py*, or
   - CLI: `ampy -p /dev/ttyACM0 put main.py /main.py`
3. Reboot. It runs at boot and prints the banner on the Pico's USB console
   (visible with `ampy -p /dev/ttyACM0 monitor` or any USB serial terminal).

## Using it

**AP mode (default — no network setup):**
1. Power the Pico. It creates a Wi-Fi hotspot `ICSOS-DEBUG` (password
   `icsos-debug`).
2. Connect your laptop/phone to it. The Pico hands out `10.42.0.x` and is at
   `10.42.0.1`.
3. `curl http://10.42.0.1/` → dump the captured buffer.
   `telnet 10.42.0.1 23` (or `nc 10.42.0.1 23`) → live tail; type to forward to
   the target.

**STA mode (use your LAN):** set `STA_SSID` / `STA_PASSWORD` in `main.py`,
re-flash, reboot. The Pico joins your network and you reach it at the IP it
prints.

## Notes / limitations

- **One stream client at a time** on :23 (serial is single-reader); multiple
  HTTP readers are fine.
- **1.4 MB littlefs** is the practical cap for the log (set `LOG_MAX` lower,
  e.g. 256 KB, if you want more margin). Rotate keeps the most recent half.
- If the Pico's flash is nearly full the log file may fail to grow — the
  Wi-Fi stream keeps working regardless.
- **Needs a real 16550 COM1 header.** A USB plug on the target is not COM1.
  This bridge cannot debug an N150-class laptop that has no serial port.
- Boot before the kernel enables its serial console is not captured (same
  limitation as the ESP32 bridge): a dead UEFI/GRUB stage shows nothing, which
  is itself a useful signal.

## Tests

The pure Python logic (buffer ring + log rotation) is ordinary MicroPython and
is exercised on-device at boot. There is no host-side TAP for the Pico firmware
because `machine`/`network` are device-only; the shared line-buffer algorithm
used by the *ESP32* bridge is covered by `make test-bridge-console-unit` in
`ics-os/`.
