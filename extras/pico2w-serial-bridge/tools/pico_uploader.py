#!/usr/bin/env python3
"""Upload a MicroPython file to a Pico 2 W over its USB-CDC serial port.

Usage:
    python3 pico_uploader.py /dev/ttyACM0 main.py /main.py
    python3 pico_uploader.py /dev/ttyACM0 --ls          # list remote files
    python3 pico_uploader.py /dev/ttyACM0 --reset       # reset the Pico

Uses the MicroPython raw-REPL protocol (Ctrl-A / Ctrl-C / Ctrl-D) and ships
file content as base64 so arbitrary bytes are safe. No ampy required.

Note: on the Pico W CDC port, flushInput() BEFORE a read discards the prompt
that just arrived, so reads are done as sleep-then-read_all() only.
"""
import base64
import sys
import time
import serial

REPL_PROMPT = b">>>"
RAW_ENTER = b"\x01"   # Ctrl-A
RAW_RUN   = b"\x04"   # Ctrl-D
INTERRUPT = b"\x03"   # Ctrl-C


def _read_after(s, delay):
    time.sleep(delay)
    return s.read_all()


def enter_normal_repl(s, settle=1.5):
    """Get to a normal '>>>' prompt (the Pico may be mid-boot or mid-line).

    CDC quirk: do NOT flushInput() before a read -- it drops the prompt that
    just arrived. Interrupt any partial line with Ctrl-C, then CR.
    """
    time.sleep(settle)
    s.flushInput()
    for _ in range(15):
        s.write(INTERRUPT)          # abort any partial/secondary line
        _read_after(s, 0.15)
        s.write(b"\r")
        d = _read_after(s, 0.5)
        if REPL_PROMPT in d:
            return True
    return False


def send_line(s, line, settle=0.5):
    """Send one cooked-REPL line; return the response bytes."""
    s.write(line if line.endswith(b"\r\n") else line + b"\r\n")
    return _read_after(s, settle)


def raw_exec(s, program, settle=1.5):
    """Run a program in raw REPL; return its stdout (and any error trace)."""
    _read_after(s, 0.2)
    s.write(RAW_ENTER)
    _read_after(s, 0.3)
    s.write(program)
    _read_after(s, settle)
    s.write(RAW_RUN)
    out = b""
    for _ in range(15):
        chunk = _read_after(s, 0.25)
        if chunk:
            out += chunk
        if b"Traceback" in out:
            break
    # back to normal REPL
    s.write(INTERRUPT)
    _read_after(s, 0.3)
    s.write(b"\r")
    _read_after(s, 0.3)
    return out


def upload(s, local_path, remote_path):
    with open(local_path, "rb") as f:
        data = f.read()
    b64 = base64.b64encode(data).decode("ascii")
    lines = ["import ubinascii", "d=b''"]
    for i in range(0, len(b64), 200):
        lines.append("d+=b'%s'" % b64[i:i + 200])
    lines.append("f=open('%s','wb')" % remote_path)
    lines.append("f.write(ubinascii.a2b_base64(d))")
    lines.append("f.close()")
    lines.append("print('WROTE_BYTES', len(d))")
    program = ("\n".join(lines) + "\r").encode("ascii")
    out = raw_exec(s, program, settle=max(1.5, len(data) // 4000))
    print(out.decode("utf-8", "replace"))
    return b"WROTE_BYTES" in out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    port = sys.argv[1]
    s = serial.Serial(port, 115200, timeout=1)
    try:
        if sys.argv[2] == "--reset":
            if not enter_normal_repl(s):
                print("ERROR: no '>>>' prompt on %s" % port); sys.exit(1)
            print(send_line(s, b"import machine; machine.reset()").decode())
            print("reset sent")
            return
        if sys.argv[2] == "--ls":
            if not enter_normal_repl(s):
                print("ERROR: no '>>>' prompt on %s" % port); sys.exit(1)
            print(send_line(s, b"import os; print(os.listdir())").decode())
            return
        if len(sys.argv) >= 4:
            if not enter_normal_repl(s):
                print("ERROR: no '>>>' prompt on %s (wrong port or not MicroPython)" % port)
                sys.exit(1)
            print("REPL OK. Uploading %s -> %s" % (sys.argv[2], sys.argv[3]))
            ok = upload(s, sys.argv[2], sys.argv[3])
            print("verify (stat: [ino, mode, nlink, uid, gid, size, ...]):")
            statcmd = ("import os; print(os.stat('%s'))" % sys.argv[3]).encode()
            print(send_line(s, statcmd).decode())
            print("upload", "OK" if ok else "FAILED")
            sys.exit(0 if ok else 1)
        print(__doc__)
    finally:
        s.close()


if __name__ == "__main__":
    main()
