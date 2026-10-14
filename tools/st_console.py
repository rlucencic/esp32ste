#!/usr/bin/env python3
"""Talk to the emulator's USB-Serial-JTAG console (see main/console.h).

  st_console.py PORT log SECONDS          print the log for a while
  st_console.py PORT screenshot OUT.png   save the ST screen (needs macOS sips)
  st_console.py PORT lcd OUT.png          save the whole LCD (setup screen too)
  st_console.py PORT send "k 39" ...      send console commands
  st_console.py PORT info                 print the machine state
  st_console.py PORT reset                hard-reset the board, then log 10 s
"""
import base64
import os
import re
import subprocess
import sys
import tempfile
import time

import serial


BASE64_LINE = re.compile(rb"(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{4}|[A-Za-z0-9+/]{3}=|[A-Za-z0-9+/]{2}==)")


def is_native_usb(name):
    return "usbmodem" in name


def open_port(name):
    port = serial.Serial()
    port.port = name
    port.timeout = 0.2
    if is_native_usb(name):
        # USB-Serial-JTAG resets the chip when RTS=1 and DTR=0. The OS raises
        # both on open and pyserial then applies DTR before RTS, so keep DTR
        # high to avoid passing through the reset state.
        port.baudrate = 115200
        port.dtr = True
        port.rts = False
    else:
        # USB-to-UART (console moved there for the speaker, 921600 baud):
        # keep both lines as the OS left them, so the auto-reset circuit
        # (DTR -> IO0, RTS -> EN) stays idle.
        port.baudrate = 921600
        port.dtr = True
        port.rts = True
    port.open()
    return port


def log(port, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = port.read(4096)
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()


def screenshot(port, out_png, command=b"s"):
    port.reset_input_buffer()
    port.write(command + b"\n")
    # Collect everything first: the device drops data if we read too slowly
    data = bytearray()
    end = time.time() + 60
    while time.time() < end and b"STSCREEN END" not in data[-4096:]:
        data += port.read(65536)
    if b"STSCREEN END" not in data:
        sys.exit("timed out waiting for the screen dump")

    width = height = None
    chunks = []
    for line in bytes(data).split(b"\n"):
        line = line.strip()
        if line.startswith(b"STSCREEN END"):
            break
        if line.startswith(b"STSCREEN "):
            width, height = map(int, line.split()[1:3])
            chunks = []
        elif width is not None and BASE64_LINE.fullmatch(line):
            # Log lines from other tasks may land in between: skip them
            chunks.append(base64.b64decode(line))
    pixels = b"".join(chunks)
    if len(pixels) != width * height * 2:
        sys.exit(f"screen dump incomplete: {len(pixels)} of {width * height * 2} bytes")
    write_png(width, height, pixels, out_png)


def write_png(width, height, rgb565, out_png):
    rgb = bytearray()
    for i in range(0, len(rgb565), 2):
        p = (rgb565[i] << 8) | rgb565[i + 1]
        r, g, b = p >> 11, (p >> 5) & 0x3F, p & 0x1F
        rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    # Medium resolution pixels are twice as tall as wide
    rows = [rgb[y * width * 3:(y + 1) * width * 3] for y in range(height)]
    if width == 640 and height == 200:
        rows = [r for row in rows for r in (row, row)]
    with tempfile.NamedTemporaryFile(suffix=".ppm", delete=False) as f:
        f.write(b"P6\n%d %d\n255\n" % (width, len(rows)))
        f.write(b"".join(rows))
        ppm = f.name
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out_png],
                   check=True, stdout=subprocess.DEVNULL)
    os.unlink(ppm)
    print(f"{width}x{height} -> {out_png}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port = open_port(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "log":
        log(port, float(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif cmd == "screenshot":
        screenshot(port, sys.argv[3] if len(sys.argv) > 3 else "st_screen.png")
    elif cmd == "lcd":
        screenshot(port, sys.argv[3] if len(sys.argv) > 3 else "lcd.png", b"L")
    elif cmd == "send":
        for c in sys.argv[3:]:
            port.write(c.encode() + b"\n")
            time.sleep(0.1)
    elif cmd == "info":
        port.reset_input_buffer()
        port.write(b"i\n")
        end = time.time() + 2
        while time.time() < end:
            line = port.readline()
            if line.startswith(b"STINFO"):
                print(line.decode().strip())
                break
    elif cmd == "reset":
        if is_native_usb(sys.argv[1]):
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
            port.dtr = True
        else:
            port.dtr = True     # IO0 high: boot normally
            port.rts = False    # EN low: reset
            time.sleep(0.1)
            port.rts = True
        log(port, 10)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
