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
| Capture USB CDC gadget | Pico USB serial stdin (plug the Pico into the target; ICS-OS xHCI host) |
| Store a rolling log | `/boot.log` on the Pico's littlefs (cap 512 KB, keeps newest half on rotate) |
| Live tail | TCP **:23** — replays the buffer, then follows; typed bytes forwarded to UART **and** USB |
| Agent HTTP | **:80** — log, status, screen, framebuffer, cmd, keys, kexec, reboot |
| Wi-Fi | **AP mode by default** (own hotspot `ICSOS-DEBUG`) — or STA to an existing network |

No LCD, no TF card, no USB host port, no soldering. The Pico's flash *is* the
storage, and Wi-Fi *is* the transport. On machines without a 16550 COM1
header (Intel N150), plug the Pico into a USB port and let ICS-OS speak
CDC-ACM host; keep the UART wires for PCs that still have COM1.

## Hardware facts (verified, not guessed)

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

## USB gadget (no COM1 header)

Plug the Pico's USB into the target. MicroPython already presents a CDC-ACM
gadget. ICS-OS enumerates it as xHCI device 1 (MSC root stays device 0),
sets 115200 8N1 + DTR/RTS, and writes console bytes on bulk OUT. The Pico
firmware copies those stdin bytes into the same Wi-Fi/log path as UART0.

Look for `USB_CDC_CONSOLE_OK` then `ICSOS_VER release=... build=... ts=... compiled=...`
in `telnet`/`curl /log` after the kernel binds USB. `GET /health` reports
`pico=` (this firmware) and `kernel=` (that stamp from the RAM ring or
the flash log tail, or `none` if the laptop booted an older image). Then `USB_CDC_RX` once gadget TX is reaching the
host. UART capture still runs if the COM1 wires are attached. Kernel bulk
IN must already be posted when the Pico writes; otherwise MicroPython drops
the bytes. HTTP RPC retries the framed request every 400ms until the kernel
replies.

Local loop (Pico plugged into the build PC): `cd ics-os && make test-usb-cdc-pico`
passes the gadget into QEMU q35 xHCI with the MSC root and curls Wi-Fi
`/log` and `/status`. Emulated QEMU `usb-serial` (`test-usb-cdc-console`)
only covers host TX. VirtualBox USB filters can pass the same gadget, but
QEMU `usb-host` is the automated gate.

## Agent HTTP API (Pico :80)

Stable, scriptable surface for coding agents. Discover it with
`curl http://<pico-ip>/v1`. Kernel RPCs are framed (`0x1e ICS <seq> VERB`)
so they do not mix with the console log.

The STA address is assigned by DHCP and can change. Locate the bridge by
validating `/health` across the local IPv4 network instead of keeping a fixed
address:

```bash
cd ics-os
./scripts/discover-pico.py
./scripts/capture-wifi-hw.sh   # uses discovery when no IP is supplied
```

`discover-pico.py` prints the current address plus Pico/kernel versions and
rejects unrelated HTTP devices. A DHCP reservation remains useful but is not
required.

| Method | Path | Result |
|---|---|---|
| GET | `/health` | `pico=` firmware, `kernel=` ICSOS_VER from `/log` (no RPC), uptime, STA/AP, buffer size |
| GET | `/version` | `pico=` and `kernel=` only (`kernel=none` if the bind stamp is missing) |
| GET | `/log` | Console ring (RPC frames stripped) |
| GET | `/v1` | JSON catalog (`fw` plus endpoints) |
| GET | `/status` | Kernel: `cdc`, `usb_root`, framebuffer, `release`/`build`/`ts`, `klog`, cmdline |
| GET | `/screen` | 80×25 console text (best first look for an agent) |
| GET | `/fb.ppm` | Downscaled GOP screenshot (P6) |
| GET | `/dmesg` | Kernel log dump |
| POST | `/cmd` | Body is one `console_execute` line (`ls`, `ps`, `dmesg`, …) |
| POST | `/keys` | Raw keystrokes into the foreground tty (use for userland `sh`) |
| POST | `/kexec` | ELF64 body, stage and kexec (RAM) |
| POST | `/update` | New `main.py`; requires `X-SHA256`, atomically keeps `/main.py.bak`, then resets |
| POST | `/reboot` | Target `machine_reboot` |
| GET | `/reset` | Reboot the Pico only |

### Pico over-the-air update

After the initial USB installation, update the bridge over the LAN. The helper
discovers the current DHCP address, sends the firmware with its SHA-256, waits
for the atomic install/reset, and verifies that the Pico is discoverable again:

```bash
./ics-os/scripts/update-pico.py
```

The Pico compiles the candidate before replacing `/main.py`, rejects a missing
or incorrect digest, and retains `/main.py.bak`. This endpoint is intended for
the trusted lab LAN; the bridge API does not yet authenticate clients.

### Remote kernel update (no thumb-drive reflash)

Build the kernel on the host, then push it through the Pico. The target
loads the ELF into the kexec staging area, ACKs, and jumps via
`kexec_reboot` (RAM only). ESP `/vmdex` is **not** rewritten here — FAT
persist during CDC kexec wedged N150; cold boot still needs etcher until
a quiet persist path returns.

```bash
cd ics-os
make -C kernel bzImage
./scripts/remote-kexec.sh                 # default Pico 192.168.0.174
# or:
curl --max-time 240 --data-binary @kernel/Kernel64.bin \
  http://192.168.0.174/kexec
curl http://192.168.0.174/status          # expect kexeced=1, new compiled=
```

Limits: image ≤ 8 MiB on the target; firmware `pico=0.7-*` waits for the
kernel's OK `ready` after `KEXEC <nbytes>`, then paces 256 B every 5 ms,
then waits for OK `done` before returning HTTP 200. Older bridges either
OOMed (`0.4`), flooded CDC IN (`0.5`/`0.6`), or skipped the ready
handshake. Allow ~4 minutes for transfer.
`BOOTX64.EFI` / GRUB stay on the stick. First-time ESP layout still needs
`make usb-etcher`.

If `/health` resets or hangs after a failed kexec: power-cycle the Pico
(or `curl http://PICO/reset` when it still answers), confirm
`pico=0.7-…`, then retry. A wedged target may need the laptop power
button if CDC/console stalls mid-transfer.

Examples:

```bash
curl http://192.168.0.174/v1
curl http://192.168.0.174/health
curl http://192.168.0.174/version
curl http://192.168.0.174/status
curl http://192.168.0.174/screen
curl -s http://192.168.0.174/fb.ppm > /tmp/n150.ppm
curl -H 'Content-Type: text/plain' --data 'ls' http://192.168.0.174/cmd
curl --data $'ls\r' http://192.168.0.174/keys
curl --data-binary @ics-os/kernel/Kernel64.bin http://192.168.0.174/kexec
```

Telnet `:23` still tails the log. Typed bytes go to USB (and UART if wired)
so you can drive `sh$` remotely after the tty remainder/ONLCR fix.

### Further agent capabilities (not in this firmware yet)

These are the next pieces for hands-off N150 bring-up:

- **Boot generation / crash snapshot** — Pico flash keeps last panic SCREEN +
  `dmesg` across target reboot.
- **Watchdog ping** — kernel STATUS age; Pico HTTP 503 if the target missed
  PING, so agents distinguish "quiet" from "dead".
- **File GET/PUT** — `/icsos/...` over the RPC so agents can drop tests
  without kexec.
- **Structured KTAP** — `POST /cmd` returns captured stdout + exit, not just
  the live log.
- **Symbolize** — host-side `Kernel64.sym` lookup for RIP in STATUS/fault.
- **Auth token** — unguessable header on a lab LAN.
- **GDB stub** — later; do not block console RPC on it.
- **Test trigger** — `exectest` / `posixio` as RPC verbs with timeouts.
- **LED/GOP health** — Caps/Num + `fbconsole_active` in STATUS (partially
  there).
- **Multi-client HTTP** — already OK; keep :23 single-reader.

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
3. `curl http://10.42.0.1/v1` → endpoint catalog.
   `curl http://10.42.0.1/screen` → 80×25 text.
   `curl http://10.42.0.1/log` → console ring.
   `telnet 10.42.0.1 23` (or `nc`) → live tail; type to forward to USB/UART.

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
- **COM1 UART and USB CDC gadget both work.** A USB plug on the target is
  not COM1; ICS-OS CDC-ACM host is the N150 path. Keep the UART wiring for
  machines that still have a 16550 header.
- Boot before the kernel enables its serial or USB console is not captured
  (same limitation as the ESP32 bridge): a dead UEFI/GRUB stage shows
  nothing, which is itself a useful signal.

## Tests

The pure Python logic (buffer ring + log rotation) is ordinary MicroPython and
is exercised on-device at boot. There is no host-side TAP for the Pico firmware
because `machine`/`network` are device-only; the shared line-buffer algorithm
used by the *ESP32* bridge is covered by `make test-bridge-console-unit` in
`ics-os/`.
