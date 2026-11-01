#!/usr/bin/env python3
"""Talk to KickEmu's MCU socket as the guest would (Linux side of UART1).

Sends PktMgr frames ("MT", type, dir, cmd, size, data..., CRC-8/0x31) to
mcu.sock and prints what the emulated MCU sends back. Useful to test LED,
pad and slider rendering without a running guest. QEMU must not be connected
at the same time (the MCU socket takes one client).

Examples:
  mcu_send.py --socket $TMPDIR/kickemu/mcu.sock demo        # light everything up
  mcu_send.py --socket ... led 8 1                          # MAIN LED on
  mcu_send.py --socket ... pad 0 1 12                       # pad 1 on, colour 12
  mcu_send.py --socket ... listen                           # print MCU events
"""
import argparse
import os
import socket
import sys
import time

TYPE = {"system": 0, "button": 1, "led": 2, "pad": 3, "knobs": 4, "midi": 5, "cv": 6, "slider": 7}


def crc8(data):
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def frame(ptype, cmd, data, direction=0):
    body = bytes([ord("M"), ord("T"), ptype, direction, cmd, len(data) + 1] + [d & 0xFF for d in data])
    return body + bytes([crc8(body)])


def parse(buf):
    """Yields (type, dir, cmd, data) from MCU output (frame + 1 trailer byte)."""
    out = []
    while len(buf) >= 7:
        if buf[0:2] != b"MT":
            buf = buf[1:]
            continue
        size = buf[5]
        total = 6 + size + 1
        if len(buf) < total:
            break
        out.append((buf[2], buf[3], buf[4], list(buf[6:5 + size])))
        buf = buf[total:]
    return out, buf


def demo(sock, seconds):
    """Lights every LED, cycles pad colours and sweeps the slider."""
    send = lambda t, c, d: sock.sendall(frame(TYPE[t], c, d))
    send("system", 2, [2])                      # COMM_INIT
    send("system", 5, [0, 120])                 # BPM 120
    send("pad", 5, [4])                         # colour mode DIM_VELOCITY
    for led in range(0, 53):
        send("led", 0, [led, 1, 0])
    for led in range(56, 66):
        send("led", 0, [led, 1, 0])
    start = time.time()
    step = 0
    while time.time() - start < seconds:
        colours = [(i + step) % 15 for i in range(16)]
        send("pad", 3, [1] + colours)           # all pads, multi colour
        send("slider", 1, [int(50 + 50 * ((step % 20) / 10 - 1))])
        step += 1
        time.sleep(0.12)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--socket", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "kickemu", "mcu.sock"))
    p.add_argument("command", choices=["led", "blink", "pad", "padall", "slider", "mode", "bpm", "raw", "demo", "listen"])
    p.add_argument("args", nargs="*", type=int)
    p.add_argument("--seconds", type=float, default=5.0)
    a = p.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(a.socket)
    v = a.args
    if a.command == "led":
        s.sendall(frame(TYPE["led"], 0, v))
    elif a.command == "blink":
        s.sendall(frame(TYPE["led"], 1, v))
    elif a.command == "pad":
        s.sendall(frame(TYPE["pad"], 1, v))
    elif a.command == "padall":
        s.sendall(frame(TYPE["pad"], 2, v))
    elif a.command == "slider":
        s.sendall(frame(TYPE["slider"], 1, v))
    elif a.command == "mode":
        s.sendall(frame(TYPE["pad"], 5, v))
    elif a.command == "bpm":
        s.sendall(frame(TYPE["system"], 5, [v[0] >> 8, v[0] & 0xFF]))
    elif a.command == "raw":
        s.sendall(frame(v[0], v[1], v[2:]))
    elif a.command == "demo":
        demo(s, a.seconds)

    # Print what the MCU sends for a moment (or until Ctrl+C when listening).
    s.settimeout(0.2)
    buf = b""
    end = time.time() + (1e9 if a.command == "listen" else 0.5)
    try:
        while time.time() < end:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                msgs, buf = parse(buf + chunk)
                for t, d, c, data in msgs:
                    name = next((k for k, x in TYPE.items() if x == t), str(t))
                    print(f"{name:7s} cmd {c:2d} data {data}")
            except socket.timeout:
                pass
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
