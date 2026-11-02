#!/usr/bin/env python3
"""Smoke test: boot the bare-metal demo headless and drive it through MCP.

Checks the whole automation path end to end: QEMU starts, the console prints
the banner, the MCU link comes up, the panel is scanned out, panel input and
MIDI reach the guest, the SD slot answers, and the machine powers off.

    Emulator/tools/smoke_demo.py [--binary KickEmu] [--screenshot out.png]

Exit status 0 on success; failures print what was expected and what came back.
"""

import argparse
import os
import struct
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kickemu_client import KickEmu, ToolError  # noqa: E402


def check(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")
    print(f"ok   {message}")


def png_distinct_colours(png):
    """Rough non-blank check without third-party modules: count distinct pixels
    in the first rows of an 8-bit RGB/RGBA PNG (JUCE writes those)."""
    pos, idat, width, colour = 8, b"", 0, 0
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        data = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, _, _, colour = struct.unpack(">IIBB", data[:10])
        elif kind == b"IDAT":
            idat += data
        pos += 12 + length
    raw = zlib.decompress(idat)
    bpp = 4 if colour == 6 else 3
    stride = 1 + width * bpp
    seen, prev = set(), bytearray(width * bpp)
    for row in range(0, min(len(raw) // stride, 600)):
        line = bytearray(raw[row * stride + 1:(row + 1) * stride])
        f = raw[row * stride]
        for i in range(len(line)):   # undo the PNG filters
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        seen.update(bytes(line[i:i + 3]) for i in range(0, len(line), bpp * 7))
    return len(seen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--screenshot", help="save the panel here")
    ap.add_argument("--timeout", type=int, default=180, help="boot timeout in seconds (TCG is slow)")
    ns = ap.parse_args()

    run_dir = tempfile.mkdtemp(prefix="kemu-", dir="/tmp")
    args = ["--profile", "demo", "--run-dir", run_dir, "--set", "qemu.network=false"]
    log = open(os.path.join(run_dir, "kickemu.log"), "w")
    try:
        with KickEmu.launch(args, binary=ns.binary, stderr=log) as k:
            check(len(k.tools()) >= 30, "MCP lists the automation tools")
            m = k.call("console_wait", pattern="KICK bare-metal demo", since=0, timeout_ms=ns.timeout * 1000)
            check("demo" in m["line"], f"console banner: {m['line'].strip()}")

            deadline = time.time() + ns.timeout
            while time.time() < deadline:
                st = k.call("status")
                if st["mcu"] == "linked" and st["display"]["width"]:
                    break
                time.sleep(0.5)
            check(st["mcu"] == "linked", f"MCU link up (accel {st['accel']})")
            check((st["display"]["width"], st["display"]["height"]) == (1024, 600), "panel scanned out at 1024x600")

            frame = st["display"]["frame"]
            k.call("screen_wait", frame=frame, timeout_ms=30000)
            data, png = k.call_raw("screenshot")
            colours = png_distinct_colours(png)
            check(colours > 4, f"panel shows a picture ({colours} distinct colours sampled)")
            if ns.screenshot:
                with open(ns.screenshot, "wb") as f:
                    f.write(png)

            k.call("press", control="play")
            k.call("press", control="pad1", velocity=100)
            k.call("turn", control="jog", steps=2)
            k.call("midi_send", bytes="90 3c 64")
            check(True, "panel input and MIDI accepted")

            sd = k.call("sd_status")
            check(sd["inserted"] is False and sd.get("qemu_medium") is False, "SD slot present and empty")

            st = k.call("machine_stop")
            check(st["qemu"] == "stopped", "machine powers off")
    except (ToolError, ConnectionError) as e:
        log.close()
        print(open(os.path.join(run_dir, "kickemu.log")).read()[-4000:], file=sys.stderr)
        raise SystemExit(f"FAIL: {e}")
    print("smoke test passed")


if __name__ == "__main__":
    main()
