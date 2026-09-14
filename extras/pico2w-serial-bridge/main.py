# ICS-OS boot-log bridge for the Raspberry Pi Pico 2 W.
#
# Captures the target machine's COM1 (0x3F8) @ 115200 8N1 boot output on
# UART0 (RX = GPIO17), keeps a rolling copy in a littlefs file on the Pico's
# flash, and streams it over Wi-Fi:
#   * TCP  :23  -- live tail (replays the current buffer, then follows).
#                  Bytes you type back are forwarded to the target COM1 RX.
#   * HTTP :80  -- `curl http://<pico-ip>/` dumps the buffer; /health for a
#                  one-line status; /reset to reboot the Pico.
#
# Wi-Fi mode (pick ONE -- the Pico W cannot run STA and AP together):
#   DEFAULT: AP  -- the Pico creates its own hotspot; works with no config.
#   Optional: STA -- connect to an existing network (set STA_SSID below).
#
# Flashing: copy this file to the Pico's flash as main.py (Thonny, or
# `ampy put main.py /main.py` over USB). It runs at boot.

import gc
import os
import time
import socket
import network
import machine
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

# ---------------------------------------------------------------------------
# Hardware init
# ---------------------------------------------------------------------------

led  = Pin("LED", Pin.OUT)
uart = UART(UART_ID, BAUD, tx=Pin(UART_TX), rx=Pin(UART_RX))
# NOTE: rely on uart.any()/read(n) for non-blocking capture. Do NOT poll with
# a bare blocking read(); that stalls the Wi-Fi servers whenever the target is
# quiet.

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
          "  target COM1 @ %d 8N1 on UART0 (rx=GPIO%d)\r\n"
          "  log file %s (cap %d KB)\r\n"
          "  stream: telnet <ip> 23   http: http://%s/\r\n"
          "  wifi: %s  ssid=%s  ip=%s\r\n"
          % (BAUD, UART_RX, LOG_PATH, LOG_MAX // 1024, IP, MODE, MODE_SSID, IP))
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
        log.write(("-- ICS-OS bridge session (uptime %d ms) --\r\n"
                   % time.ticks_ms()).encode())
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

log_banner_once()

# ---------------------------------------------------------------------------
# HTTP (short-lived, handled synchronously)
# ---------------------------------------------------------------------------

def http_reply(c, code, msg, body):
    try:
        c.send("HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
               % (code, msg, len(body), body))
    except OSError:
        pass
    try:
        c.close()
    except OSError:
        pass

def handle_http(c):
    try:
        c.settimeout(500)
        req = c.recv(256)
    except OSError:
        c.close()
        return
    if not req:
        c.close()
        return
    line = req.split(b"\r\n", 1)[0].decode("ascii", "replace")
    parts = line.split(" ")
    path = parts[1].split("?", 1)[0] if len(parts) > 1 else "/"
    if path == "/health":
        try:
            lsz = os.stat(LOG_PATH)[6]
        except OSError:
            lsz = -1
        body = ("up 1\r\nmode=%s\r\nip=%s\r\nbuf=%d bytes\r\nlog=%d bytes\r\n"
                % (MODE, IP, len(buf), lsz)).encode()
        http_reply(c, 200, "OK", body)
    elif path == "/reset":
        http_reply(c, 200, "OK", b"rebooting\r\n")
        time.sleep_ms(50)
        machine.reset()
    elif path in ("/", "/log", "/boot.log"):
        http_reply(c, 200, "OK", bytes(buf))
    else:
        http_reply(c, 404, "Not Found", b"try /  /health  /reset\r\n")

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
client = None         # single live TCP tail client (serial = one reader)

print("  listening: tcp %d, http %d -- ready\r\n" % (TCP_PORT, HTTP_PORT))

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
            uart.write(kb)           # operator -> target COM1 RX

    time.sleep_ms(1)
    gc.collect()
