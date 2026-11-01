#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Minimal stand-in for the KICK control-surface MCU on UART1.

Listens on a unix socket (QEMU connects with -serial unix:<path>), decodes
every PktMgr frame the firmware sends, answers the handshake like the MCU
(CHECK_COMM_STATUS [1] until COMM_INIT arrives) and can hit pads.

    mcu-monitor.py <socket> [--pads N] [--interval S] [--quiet]

--pads N sends N pad presses (pads 1..16 in turn) once the link is up,
--interval S apart. Prints one line per received frame unless --quiet,
and a summary per packet type at the end (Ctrl+C).
"""
import argparse
import collections
import os
import socket
import threading
import time

TYPES = ['SYSTEM', 'BUTTON', 'LED', 'PAD', 'KNOBS', 'MIDI', 'CV', 'SLIDER']


def crc8(data):
    crc = 0xff
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xff if crc & 0x80 else (crc << 1) & 0xff
    return crc


def frame(ptype, cmd, data, trailer=b'\n'):
    """MCU -> host frame, with the trailer byte the Linux reader expects."""
    body = bytes([ord('M'), ord('T'), ptype, 0, cmd, len(data) + 1]) + bytes(data)
    return body + bytes([crc8(body)]) + trailer


class Parser:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b'MT')
            if i < 0:
                del self.buf[:-1]
                return out
            del self.buf[:i]
            if len(self.buf) < 6:
                return out
            size = self.buf[5]
            total = 6 + size
            if len(self.buf) < total:
                return out
            pkt = bytes(self.buf[:total])
            del self.buf[:total]
            ok = crc8(pkt[:-1]) == pkt[-1]
            out.append((pkt[2], pkt[3], pkt[4], list(pkt[6:-1]), ok))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('socket')
    ap.add_argument('--pads', type=int, default=0)
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    if os.path.exists(args.socket):
        os.unlink(args.socket)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(args.socket)
    srv.listen(1)
    print('waiting for QEMU on', args.socket, flush=True)
    conn, _ = srv.accept()
    linked = threading.Event()
    stats = collections.Counter()

    def handshake():
        while not linked.is_set():
            conn.sendall(frame(0, 4, [1]))      # CHECK_COMM_STATUS [1]
            time.sleep(0.5)
        for n in range(args.pads):
            pad = n % 16
            conn.sendall(frame(3, 0, [pad, 1, 100, 0]))   # PAD_STATE pressed
            print('-> pad %d pressed' % (pad + 1), flush=True)
            time.sleep(0.1)
            conn.sendall(frame(3, 0, [pad, 0, 0, 0]))     # released
            time.sleep(args.interval)

    threading.Thread(target=handshake, daemon=True).start()
    parser = Parser()
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            for ptype, pdir, cmd, payload, ok in parser.feed(data):
                name = TYPES[ptype] if ptype < len(TYPES) else str(ptype)
                stats[(name, cmd, ok)] += 1
                if ptype == 0 and cmd == 2:                 # COMM_INIT
                    linked.set()
                if not args.quiet:
                    print('<- %-6s dir %d cmd %2d %s%s' % (
                        name, pdir, cmd, ' '.join('%d' % b for b in payload),
                        '' if ok else '  CRC ERROR'), flush=True)
    except KeyboardInterrupt:
        pass
    print('summary:')
    for (name, cmd, ok), n in sorted(stats.items()):
        print('  %-6s cmd %2d %s: %d' % (name, cmd, 'ok' if ok else 'BAD CRC', n))


if __name__ == '__main__':
    main()
