# ICS-OS boot-log bridge for the Raspberry Pi Pico 2 W.
#
# Captures the target machine's COM1 (0x3F8) @ 115200 8N1 boot output on
# UART0 (RX = GPIO17), and/or USB CDC-ACM bytes when the Pico is plugged
# into the target as a USB serial gadget (ICS-OS xHCI host). Keeps a rolling
# copy in a littlefs file on the Pico's flash, and streams it over Wi-Fi:
#   * TCP  :23  -- live tail (replays the current buffer, then follows).
#                  Bytes you type back are forwarded to the target COM1 RX.
#   * HTTP :80  -- `curl http://<pico-ip>/` dumps the buffer; /health and
#                  /version for Pico firmware + kernel ICSOS_VER (from /log,
#                  no RPC); /reset to reboot the Pico.
#
# Wi-Fi mode (pick ONE -- the Pico W cannot run STA and AP together):
#   DEFAULT: AP  -- the Pico creates its own hotspot; works with no config.
#   Optional: STA -- connect to an existing network (set STA_SSID below).
#
# Flashing: copy this file to the Pico's flash as main.py (Thonny, or
# `ampy put main.py /main.py` over USB). It runs at boot.

import gc
import os
import sys
import time
import socket
import network
import machine
import hashlib
import ubinascii
from machine import UART, Pin

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

UART_ID   = 0
UART_TX   = 16        # Pico TX  -> target COM1 RX (optional, for input fwd)
UART_RX   = 17        # Pico RX  <- target COM1 TX (the boot log we capture)
BAUD      = 115200

# --- Wi-Fi: STA mode (connect to an existing network) -----------------------
# Leave STA_SSID == "" to use AP mode (the default, no config required).
STA_SSID     = "Dayo Family"
STA_PASSWORD = "elen4321"

# --- Wi-Fi: AP mode (the Pico becomes the hotspot) --------------------------
AP_SSID     = "ICSOS-DEBUG"
AP_PASSWORD = "icsos-debug"     # WPA2-PSK; must be 8..63 chars
AP_IP       = "10.42.0.1"       # fixed address on the AP subnet /28

# --- Storage -----------------------------------------------------------------
LOG_PATH  = "/boot.log"
LOG_MAX   = 512 * 1024          # cap the on-flash log (512 KB); rotate on full
BUF_MAX   = 16 * 1024           # in-RAM ring echoed to new TCP clients
FLUSH_EVERY = 256               # flush the log file to flash every N bytes

# --- Servers -----------------------------------------------------------------
TCP_PORT  = 23
HTTP_PORT = 80
HOSTNAME = "icsos-pico"

# Bump when the HTTP/RPC contract or USB IN retry policy changes so a
# laptop that still has yesterday's firmware is obvious from GET /health.
PICO_FW = "0.8.2-20260917"

# ---------------------------------------------------------------------------
# Hardware init
# ---------------------------------------------------------------------------

led  = Pin("LED", Pin.OUT)
uart = UART(UART_ID, BAUD, tx=Pin(UART_TX), rx=Pin(UART_RX))
# NOTE: rely on uart.any()/read(n) for non-blocking capture. Do NOT poll with
# a bare blocking read(); that stalls the Wi-Fi servers whenever the target is
# quiet.

# USB CDC stdin (host bulk OUT) when the Pico is a gadget on the target.
try:
    import uselect
    usb_poll = uselect.poll()
    usb_poll.register(sys.stdin, uselect.POLLIN)
except (ImportError, OSError, AttributeError):
    usb_poll = None

def usb_cdc_read():
    # Never block: a blocking stdin.read(64) stalls the Wi-Fi servers when
    # the USB host has DTR up but has not yet sent 64 bytes (N150 before
    # CDC bind, or a host that only writes a few glyphs).
    if usb_poll is None:
        return None
    try:
        if not usb_poll.poll(0):
            return None
    except OSError:
        return None
    chunks = b""
    try:
        buf = getattr(sys.stdin, "buffer", None)
        for _ in range(64):
            try:
                ev = usb_poll.poll(0)
            except OSError:
                break
            if not ev:
                break
            if buf is not None:
                b = buf.read(1)
            else:
                b = sys.stdin.read(1)
            if not b:
                break
            if isinstance(b, str):
                b = b.encode()
            chunks += b
        return chunks or None
    except OSError:
        return None

def usb_cdc_write(data):
    if not data:
        return
    if isinstance(data, str):
        data = data.encode()
    buf = getattr(sys, "stdout", None)
    raw = getattr(buf, "buffer", None) if buf is not None else None
    try:
        if raw is not None:
            i = 0
            while i < len(data):
                raw.write(data[i:i + 64])
                i += 64
            flush = getattr(raw, "flush", None)
            if flush:
                flush()
        elif buf is not None:
            buf.write(data)
            flush = getattr(buf, "flush", None)
            if flush:
                flush()
    except OSError:
        pass

def usb_cdc_write_all(data, timeout_ms=5000):
    """Write every byte or fail; used for integrity-sensitive kexec data."""
    if isinstance(data, str):
        data = data.encode()
    stream = getattr(getattr(sys, "stdout", None), "buffer", None)
    if stream is None:
        return False
    start = time.ticks_ms()
    offset = 0
    while offset < len(data):
        try:
            chunk = data[offset:offset + 64]
            wrote = stream.write(chunk)
            if wrote is None:
                wrote = len(chunk)
            if wrote > 0:
                offset += wrote
                continue
        except OSError:
            pass
        if time.ticks_diff(time.ticks_ms(), start) >= timeout_ms:
            return False
        time.sleep_ms(2)
    flush = getattr(stream, "flush", None)
    if flush:
        flush()
    return True

rpc_seq = 1
rpc_buf = bytearray()
rpc_payload = bytearray()
rpc_need = 0
rpc_done = None   # (seq, status, payload) status 'ok'/'err'/None
rpc_line = bytearray()
last_rpc_line = b""
console_log = lambda d: None

def _rpc_next_seq():
    global rpc_seq
    n = rpc_seq
    rpc_seq = rpc_seq + 1
    if rpc_seq > 999999:
        rpc_seq = 1
    return n

def ingest_usb(data, log_fn):
    # Split console text from <RS>ICS RPC. Console goes to log_fn.
    global rpc_need, rpc_payload, rpc_done, rpc_line
    if not data:
        return
    i = 0
    while i < len(data):
        if rpc_need:
            n = rpc_need if rpc_need < (len(data) - i) else (len(data) - i)
            rpc_payload.extend(data[i:i + n])
            rpc_need -= n
            i += n
            continue
        b = data[i]
        i += 1
        if b == 0x1e:
            rpc_line = bytearray(b"\x1e")
            continue
        if len(rpc_line):
            rpc_line.append(b)
            if b == 0x0a:
                line = bytes(rpc_line)
                rpc_line = bytearray()
                _rpc_handle_line(line)
            elif len(rpc_line) > 256:
                rpc_line = bytearray()
            continue
        log_fn(bytes([b]))

def _rpc_handle_line(line):
    global rpc_need, rpc_payload, rpc_done
    # \x1eICS <seq> OK <n>\n  / END / ERR
    try:
        s = line.decode("ascii", "replace").strip("\r\n")
    except Exception:
        return
    if s[:1] == "\x1e":
        s = s[1:]
    parts = s.split(" ", 4)
    if len(parts) < 3 or parts[0] != "ICS":
        return
    try:
        seq = int(parts[1])
    except ValueError:
        return
    verb = parts[2]
    if verb == "OK":
        n = 0
        if len(parts) > 3:
            try:
                n = int(parts[3])
            except ValueError:
                n = 0
        rpc_payload = bytearray()
        rpc_need = n
        rpc_done = (seq, "ok-wait", rpc_payload)
    elif verb == "END":
        payload = bytes(rpc_payload)
        rpc_done = (seq, "ok", payload)
        rpc_payload = bytearray()
        rpc_need = 0
    elif verb == "ERR":
        msg = parts[3] if len(parts) > 3 else "err"
        if len(parts) > 4:
            msg = parts[3] + " " + parts[4]
        rpc_done = (seq, "err", msg.encode())
        rpc_need = 0
        rpc_payload = bytearray()

def rpc_send(verb, args=b"", payload=b""):
    global last_rpc_line
    seq = _rpc_next_seq()
    if isinstance(verb, str):
        verb = verb.encode()
    if isinstance(args, str):
        args = args.encode()
    if isinstance(payload, str):
        payload = payload.encode()
    line = b"\x1eICS " + str(seq).encode() + b" " + verb
    if args:
        line += b" " + args
    line += b"\n"
    # Never stash large payloads for retry — a Kernel64.bin (~0.7–2 MiB)
    # doubled the heap and OOM'd the Pico mid-/kexec.
    if payload and len(payload) > 256:
        last_rpc_line = b""
        usb_cdc_write(line)
        usb_cdc_write(payload)
    else:
        last_rpc_line = line + payload
        usb_cdc_write(last_rpc_line)
    return seq

def rpc_send_kexec_header(nbytes, crc32):
    """Start KEXEC; body must be streamed separately. No retransmit."""
    global last_rpc_line
    seq = _rpc_next_seq()
    line = (b"\x1eICS " + str(seq).encode() + b" KEXEC "
            + str(nbytes).encode() + b" " + str(crc32).encode() + b"\n")
    last_rpc_line = b""
    usb_cdc_write(line)
    return seq

def http_stream_kexec(c, req, hdrs):
    """POST /kexec: wait for kernel 'ready', then pace the ELF body."""
    global buf, last_rpc_line
    n = 0
    cl = hdrs.get(b"content-length", b"0")
    try:
        n = int(cl)
    except ValueError:
        n = 0
    if n <= 0 or n > 8 * 1024 * 1024:
        http_reply(c, 400, "Bad Request", b"bad content-length\r\n")
        return
    try:
        expected_crc = int(hdrs.get(b"x-crc32", b""))
    except (ValueError, TypeError):
        http_reply(c, 400, "Bad Request", b"missing x-crc32\r\n")
        return
    buf = bytearray()
    gc.collect()
    split = req.find(b"\r\n\r\n")
    pending = req[split + 4:] if split >= 0 else b""
    if len(pending) > n:
        pending = pending[:n]
    seq = rpc_send_kexec_header(n, expected_crc)
    # Phase 1: kernel malloc + OK "ready" (do not send body yet).
    r = rpc_wait(seq, 60000)
    if r is None:
        http_reply(c, 504, "Gateway Timeout", b"kexec ready timeout\r\n")
        return
    if r[1] == "err":
        http_reply(c, 502, "Bad Gateway", r[2] + b"\r\n")
        return
    if r[2] != b"ready":
        http_reply(c, 502, "Bad Gateway", b"expected ready ack\r\n")
        return
    sent = 0
    c.settimeout(60000)
    try:
        while sent < n:
            if pending:
                chunk = pending[:64]
                pending = pending[64:]
            else:
                want = n - sent
                if want > 64:
                    want = 64
                try:
                    chunk = c.recv(want)
                except OSError:
                    chunk = b""
                if not chunk:
                    http_reply(c, 400, "Bad Request", b"short body\r\n")
                    return
            if not usb_cdc_write_all(chunk):
                http_reply(c, 503, "Service Unavailable", b"USB CDC write timeout\r\n")
                return
            sent += len(chunk)
            # Keep well under host CDC IN capacity.
            # N150 xHCI/CDC acknowledges Pico writes before all four queued
            # USB packets reach the host. Pace one max-packet at a time.
            time.sleep_ms(10)
            d = usb_cdc_read()
            if d:
                ingest_usb(d, console_log)
            if (sent & 0xffff) == 0:
                gc.collect()
                print("kexec tx %d/%d\r\n" % (sent, n))
        # Phase 2: persist + kexec_reboot ACK ("done").
        r = rpc_wait(seq, 180000)
    finally:
        last_rpc_line = b""
    if r is None:
        http_reply(c, 504, "Gateway Timeout", b"kexec timeout\r\n")
    elif r[1] == "err":
        http_reply(c, 502, "Bad Gateway", r[2] + b"\r\n")
    else:
        http_reply(c, 200, "OK", b"kexec staged\r\n")


def rpc_wait(seq, timeout_ms=8000):
    global rpc_done
    t0 = time.ticks_ms()
    last_tx = t0
    while time.ticks_diff(time.ticks_ms(), t0) < timeout_ms:
        d = usb_cdc_read()
        if d:
            ingest_usb(d, console_log)
        if rpc_done and rpc_done[0] == seq and rpc_done[1] in ("ok", "err"):
            r = rpc_done
            rpc_done = None
            return r
        now = time.ticks_ms()
        if last_rpc_line and time.ticks_diff(now, last_tx) > 400:
            usb_cdc_write(last_rpc_line)
            last_tx = now
        time.sleep_ms(5)
        gc.collect()
    return None

def blink(n=2, ms=120):
    for _ in range(n):
        led.on();  time.sleep_ms(ms)
        led.off(); time.sleep_ms(ms)

# ---------------------------------------------------------------------------
# Wi-Fi (STA or AP, not both -- the CYW43439 cannot run them together)
# ---------------------------------------------------------------------------

def wifi_ap():
    ap = network.WLAN(network.AP_IF)
    ap.config(essid=AP_SSID, password=AP_PASSWORD)
    ap.active(True)
    ap.ifconfig((AP_IP, "255.255.255.240", AP_IP, AP_IP))
    return ap

def wifi_sta():
    sta = network.WLAN(network.STA_IF)
    try:
        network.hostname(HOSTNAME)
    except (AttributeError, OSError):
        pass
    sta.active(True)
    if not sta.isconnected():
        sta.connect(STA_SSID, STA_PASSWORD)
    t0 = time.ticks_ms()
    while not sta.isconnected():
        if time.ticks_ms() - t0 > 15000:
            raise OSError("STA connect timeout to %r" % STA_SSID)
        time.sleep_ms(100)
    return sta

if STA_SSID:
    try:
        wlan = wifi_sta(); MODE = "sta"
    except OSError as e:
        print("STA failed (%s); falling back to AP." % e)
        wlan = wifi_ap(); MODE = "ap"
else:
    wlan = wifi_ap(); MODE = "ap"

IP   = wlan.ifconfig()[0]
MODE_SSID = STA_SSID if MODE == "sta" else AP_SSID
BANNER = ("==== ICS-OS serial bridge (Pico 2 W) ====\r\n"
          "  fw %s\r\n"
          "  target COM1 @ %d 8N1 on UART0 (rx=GPIO%d)\r\n"
          "  USB CDC gadget stdin %s\r\n"
          "  log file %s (cap %d KB)\r\n"
          "  stream: telnet <ip> 23   http: http://%s/\r\n"
          "  wifi: %s  ssid=%s  ip=%s\r\n"
          % (PICO_FW, BAUD, UART_RX,
             "on" if usb_poll else "off",
             LOG_PATH, LOG_MAX // 1024, IP, MODE, MODE_SSID, IP))
print(BANNER)

# ---------------------------------------------------------------------------
# Rolling log: in-RAM ring buffer + file on flash
# ---------------------------------------------------------------------------

buf = bytearray()          # tail ring: last BUF_MAX bytes, replayed on connect
try:
    log = open(LOG_PATH, "ab")   # kept open; flushed periodically + on rotate
except OSError:
    log = None                    # FS hiccup at boot -- stream over Wi-Fi only
pending = 0                # bytes written to `log` since last flush

def log_banner_once():
    # Mark the start of this Pico session so an old log is distinguishable.
    if log is None:
        return
    try:
        # No strftime in MicroPython -- use uptime ticks instead.
        log.write(("-- ICS-OS bridge session pico=%s (uptime %d ms) --\r\n"
                   % (PICO_FW, time.ticks_ms())).encode())
        log.flush()
    except OSError:
        pass

def buf_append(data):
    global buf
    buf += data
    if len(buf) > BUF_MAX:
        del buf[: len(buf) - BUF_MAX]

def file_append(data):
    global pending
    if log is None:
        return
    try:
        log.write(data)
        pending += len(data)
        if pending >= FLUSH_EVERY:
            log.flush()
            pending = 0
        if os.stat(LOG_PATH)[6] >= LOG_MAX:
            rotate()
    except OSError:
        pass   # flash full / FS hiccup -- keep streaming over Wi-Fi regardless

def rotate():
    # Keep the most recent half; restart from the top. `log` is always left
    # open (re-opened in a finally) so a failed read-back cannot wedge logging.
    global log, pending
    try:
        if log is not None:
            try:
                log.close()
            except OSError:
                pass
        old = open(LOG_PATH, "rb").read()
        open(LOG_PATH, "wb").write(old[-(LOG_MAX // 2):])
    except OSError:
        pass
    finally:
        try:
            log = open(LOG_PATH, "ab")
        except OSError:
            log = None
    pending = 0

client = None
log_banner_once()

def _console_log(d):
    buf_append(d)
    file_append(d)
    if client is not None:
        try:
            client.send(d)
        except OSError:
            pass

console_log = _console_log

# ---------------------------------------------------------------------------
# HTTP (short-lived, handled synchronously)
# ---------------------------------------------------------------------------

def http_reply(c, code, msg, body, ctype="text/plain"):
    if isinstance(body, str):
        body = body.encode()
    hdr = ("HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n"
           % (code, msg, ctype, len(body)))
    try:
        c.send(hdr.encode())
        if body:
            c.send(body)
    except OSError:
        pass
    try:
        c.close()
    except OSError:
        pass

V1 = (
    "{\n"
    "  \"device\": \"pico2w-icsos-debug\",\n"
    "  \"fw\": \"" + PICO_FW + "\",\n"
    "  \"endpoints\": {\n"
    "    \"GET /health\": \"pico= fw, kernel= ICSOS_VER from /log (no RPC)\",\n"
    "    \"GET /version\": \"pico firmware and kernel ICSOS_VER from /log\",\n"
    "    \"GET /log\": \"console ring (RPC frames stripped)\",\n"
    "    \"GET /status\": \"kernel STATUS (cdc, usb_root, fb, release, build)\",\n"
    "    \"GET /screen\": \"80x25 console text dump\",\n"
    "    \"GET /fb.ppm\": \"downscaled GOP screenshot (P6)\",\n"
    "    \"GET /dmesg\": \"kernel log dump via DMESG\",\n"
    "    \"POST /keys\": \"raw keystrokes into the tty\",\n"
    "    \"POST /cmd\": \"kernel console_execute line\",\n"
    "    \"POST /reboot\": \"machine_reboot\",\n"
    "    \"POST /kexec\": \"ELF64 body, stage and kexec\",\n"
    "    \"POST /update\": \"main.py body with X-SHA256; atomic install + reset\",\n"
    "    \"GET /reset\": \"reboot the Pico only\"\n"
    "  }\n"
    "}\n"
)

def _icsos_ver_line(data):
    i = data.rfind(b"ICSOS_VER ")
    if i < 0:
        return None
    line = data[i:]
    n = line.find(b"\n")
    if n < 0:
        n = line.find(b"\r")
    if n >= 0:
        line = line[:n]
    if len(line) > 160:
        line = line[:160]
    return line.decode("ascii", "replace")

def kernel_ver_from_buf():
    # No RPC: scrape the bind stamp from the RAM ring, then the flash log
    # tail (survives a Pico reset while the laptop is still frozen).
    # kernel=none means the laptop booted an image older than ICSOS_VER,
    # or CDC TX died before bind.
    line = _icsos_ver_line(bytes(buf))
    if line:
        return line
    try:
        sz = os.stat(LOG_PATH)[6]
        f = open(LOG_PATH, "rb")
        if sz > 4096:
            f.seek(sz - 4096)
        data = f.read()
        f.close()
    except OSError:
        return "none"
    line = _icsos_ver_line(data)
    return line if line else "none"

def version_body():
    return ("pico=%s\r\nkernel=%s\r\n" % (PICO_FW, kernel_ver_from_buf()))

def _http_headers(req):
    hdrs = {}
    parts = req.split(b"\r\n")
    for line in parts[1:]:
        if not line:
            break
        if b":" in line:
            k, v = line.split(b":", 1)
            hdrs[k.strip().lower()] = v.strip()
    return hdrs

def _http_body(c, req, hdrs):
    n = 0
    cl = hdrs.get(b"content-length", b"0")
    try:
        n = int(cl)
    except ValueError:
        n = 0
    split = req.find(b"\r\n\r\n")
    body = req[split + 4:] if split >= 0 else b""
    while len(body) < n:
        try:
            chunk = c.recv(n - len(body) if n - len(body) < 512 else 512)
        except OSError:
            break
        if not chunk:
            break
        body += chunk
    if n and len(body) > n:
        body = body[:n]
    return body

def _hex_keys(body):
    h = []
    for b in body:
        h.append("%02x" % (b if isinstance(b, int) else ord(b)))
    return "".join(h)

def handle_http(c):
    try:
        c.settimeout(2000)
        req = c.recv(512)
    except OSError:
        try:
            c.close()
        except OSError:
            pass
        return
    if not req:
        try:
            c.close()
        except OSError:
            pass
        return
    line = req.split(b"\r\n", 1)[0].decode("ascii", "replace")
    parts = line.split(" ")
    method = parts[0] if parts else "GET"
    path = parts[1].split("?", 1)[0] if len(parts) > 1 else "/"
    hdrs = _http_headers(req)

    if path == "/health":
        try:
            lsz = os.stat(LOG_PATH)[6]
        except OSError:
            lsz = -1
        body = ("up 1\r\npico=%s\r\nkernel=%s\r\nhostname=%s\r\nmode=%s\r\nip=%s\r\n"
                "buf=%d bytes\r\nlog=%d bytes\r\n"
                % (PICO_FW, kernel_ver_from_buf(), HOSTNAME, MODE, IP,
                   len(buf), lsz)).encode()
        http_reply(c, 200, "OK", body)
        return
    if path == "/version":
        http_reply(c, 200, "OK", version_body().encode())
        return
    if path == "/reset":
        http_reply(c, 200, "OK", b"rebooting\r\n")
        time.sleep_ms(50)
        machine.reset()
        return
    if path in ("/", "/log", "/boot.log"):
        http_reply(c, 200, "OK", bytes(buf))
        return
    if path == "/v1":
        http_reply(c, 200, "OK", V1, "application/json")
        return

    if path in ("/status", "/screen", "/fb.ppm", "/fb", "/dmesg") and method == "GET":
        verb = "STATUS"
        args = b""
        ctype = "text/plain"
        timeout = 8000
        if path == "/screen":
            verb = "SCREEN"
        elif path in ("/fb.ppm", "/fb"):
            verb = "FB"
            args = b"8"
            ctype = "image/x-portable-pixmap"
            timeout = 20000
        elif path == "/dmesg":
            verb = "DMESG"
        seq = rpc_send(verb, args)
        r = rpc_wait(seq, timeout)
        if r is None:
            http_reply(c, 504, "Gateway Timeout", b"kernel rpc timeout\r\n")
        elif r[1] == "err":
            http_reply(c, 502, "Bad Gateway", r[2] + b"\r\n")
        else:
            http_reply(c, 200, "OK", r[2], ctype)
        return

    if method == "POST" and path in ("/keys", "/cmd", "/reboot", "/kexec", "/update"):
        if path == "/kexec":
            http_stream_kexec(c, req, hdrs)
            return
        body = _http_body(c, req, hdrs)
        if path == "/update":
            expected = hdrs.get(b"x-sha256", b"").decode().lower()
            actual = ubinascii.hexlify(hashlib.sha256(body).digest()).decode()
            if not expected or expected != actual:
                http_reply(c, 400, "Bad Request", b"sha256 mismatch\r\n")
                return
            try:
                compile(body, "main.py", "exec")
                with open("/main.py.new", "wb") as update:
                    update.write(body)
                    update.flush()
                try:
                    os.remove("/main.py.bak")
                except OSError:
                    pass
                os.rename("/main.py", "/main.py.bak")
                os.rename("/main.py.new", "/main.py")
            except Exception as e:
                try:
                    os.remove("/main.py.new")
                except OSError:
                    pass
                try:
                    os.stat("/main.py")
                except OSError:
                    try:
                        os.rename("/main.py.bak", "/main.py")
                    except OSError:
                        pass
                http_reply(c, 400, "Bad Request", ("update failed: %s\r\n" % e).encode())
                return
            http_reply(c, 200, "OK", ("updated sha256=%s\r\n" % actual).encode())
            time.sleep_ms(100)
            machine.reset()
            return
        if path == "/reboot":
            seq = rpc_send("REBOOT")
            rpc_wait(seq, 3000)
            http_reply(c, 200, "OK", b"rebooting target\r\n")
            return
        if path == "/keys":
            seq = rpc_send("KEYS", _hex_keys(body))
        elif path == "/cmd":
            seq = rpc_send("CMD", body.strip())
        r = rpc_wait(seq, 8000)
        if r is None:
            http_reply(c, 504, "Gateway Timeout", b"kernel rpc timeout\r\n")
        elif r[1] == "err":
            http_reply(c, 502, "Bad Gateway", r[2] + b"\r\n")
        else:
            http_reply(c, 200, "OK", r[2] if r[2] else b"ok\r\n")
        return

    http_reply(c, 404, "Not Found",
               b"try / /health /version /v1 /status /screen /fb.ppm /log "
               b"POST /cmd /keys /kexec /reboot\r\n")

# ---------------------------------------------------------------------------
# Listeners (non-blocking)
# ---------------------------------------------------------------------------

def listener(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(1)
    s.settimeout(0)   # non-blocking accept
    return s

tcp_listen  = listener(TCP_PORT)
http_listen = listener(HTTP_PORT)

print("  listening: tcp %d, http %d -- ready\r\n" % (TCP_PORT, HTTP_PORT))

last_ping = time.ticks_ms()

# ---------------------------------------------------------------------------
# Main loop: UART capture + servers, all non-blocking
# ---------------------------------------------------------------------------

while True:
    # --- UART capture (the heart of the bridge) ----------------------------
    n = uart.any()
    if n:
        d = uart.read(n)
        buf_append(d)
        file_append(d)
        if client is not None:
            try:
                client.send(d)
            except OSError:
                try:
                    client.close()
                except OSError:
                    pass
                client = None

    d = usb_cdc_read()
    if d:
        ingest_usb(d, console_log)

    # --- HTTP accept (short-lived) -----------------------------------------
    try:
        c, _ = http_listen.accept()
        handle_http(c)
    except OSError:
        pass

    # --- stream accept ------------------------------------------------------
    if client is None:
        try:
            c, _ = tcp_listen.accept()
            c.settimeout(0)          # non-blocking recv/send
            c.send(bytes(buf))       # replay history, then follow live
            client = c
        except OSError:
            pass

    # --- stream client: forward typed input + detect disconnect ------------
    if client is not None:
        try:
            kb = client.recv(64)
        except OSError:
            kb = None                # EAGAIN: no input pending
        if kb is None:
            pass
        elif kb == b"":
            client.close(); client = None   # peer closed
        else:
            uart.write(kb)
            usb_cdc_write(kb)

    if time.ticks_diff(time.ticks_ms(), last_ping) > 200:
        usb_cdc_write(b"\x1eICS 0 PING\n")
        last_ping = time.ticks_ms()

    time.sleep_ms(1)
    gc.collect()
