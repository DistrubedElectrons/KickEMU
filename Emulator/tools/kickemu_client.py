#!/usr/bin/env python3
"""Client for the KickEmu automation interface (MCP over JSON lines).

Talks to a running instance through <run_dir>/automation.sock, or starts a
headless instance with --mcp and talks to it over stdio. Python standard
library only, so CI runners and scripts need nothing else.

    from kickemu_client import KickEmu
    with KickEmu.launch(["--profile", "selftest"]) as k:
        k.call("console_wait", pattern="login:", timeout_ms=60000)
        print(k.call("console_run", command="uname -a")["output"])
        k.screenshot("panel.png")

Command line:
    kickemu_client.py --socket /tmp/kemu/automation.sock status
    kickemu_client.py --socket ... call press control=play
    kickemu_client.py --launch -- --profile demo   tools
"""

import argparse
import base64
import json
import os
import socket
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def default_binary():
    if sys.platform == "darwin":
        return os.path.join(REPO, "Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu")
    return os.path.join(REPO, "Emulator/out/host/KickEmu_artefacts/Release/KickEmu")


class ToolError(RuntimeError):
    pass


class KickEmu:
    def __init__(self, reader, writer, process=None):
        self._reader, self._writer, self.process = reader, writer, process
        self._next_id = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "kickemu_client", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    # -- construction ---------------------------------------------------------------
    @classmethod
    def connect(cls, path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(path)
        f = s.makefile("rwb")
        return cls(f, f)

    @classmethod
    def launch(cls, args=(), binary=None, stderr=None):
        """Starts `KickEmu --mcp <args>`; the instance exits when the client closes."""
        p = subprocess.Popen([binary or default_binary(), "--mcp", *args], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=stderr)
        return cls(p.stdout, p.stdin, p)

    def close(self):
        try:
            self._writer.close()
        except OSError:
            pass
        if self.process:
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=10)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- protocol -------------------------------------------------------------------
    def _send(self, msg):
        self._writer.write((json.dumps(msg) + "\n").encode())
        self._writer.flush()

    def request(self, method, params=None):
        self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": self._next_id, "method": method, "params": params or {}})
        while True:
            line = self._reader.readline()
            if not line:
                raise ConnectionError("KickEmu closed the connection")
            msg = json.loads(line)
            if msg.get("id") == self._next_id:
                break
        if "error" in msg:
            raise ToolError(msg["error"]["message"])
        return msg["result"]

    def tools(self):
        return self.request("tools/list")["tools"]

    def call_raw(self, name, **arguments):
        """Returns (data, png_bytes or None); raises ToolError on a failed call."""
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        images = [base64.b64decode(c["data"]) for c in result["content"] if c["type"] == "image"]
        if result.get("isError"):
            raise ToolError(f"{name}: {texts[0] if texts else 'failed'}")
        return json.loads(texts[0]) if texts else None, images[0] if images else None

    def call(self, name, **arguments):
        return self.call_raw(name, **arguments)[0]

    def screenshot(self, path):
        data, png = self.call_raw("screenshot")
        with open(path, "wb") as f:
            f.write(png)
        return data


def _parse_value(text):
    try:
        return json.loads(text)
    except ValueError:
        return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", help="automation.sock of a running instance")
    ap.add_argument("--launch", action="store_true", help="start a headless instance (args after --)")
    ap.add_argument("--binary", help="KickEmu executable for --launch")
    ap.add_argument("command", help="tools | status | call <tool> key=value ...")
    ap.add_argument("rest", nargs="*")
    ns, extra = ap.parse_known_args()
    extra = [a for a in extra if a != "--"]

    if ns.socket:
        k = KickEmu.connect(ns.socket)
    elif ns.launch:
        k = KickEmu.launch(extra, binary=ns.binary, stderr=subprocess.DEVNULL)
    else:
        ap.error("need --socket or --launch")

    with k:
        if ns.command == "tools":
            for t in k.tools():
                print(f"{t['name']:16} {t['description']}")
        elif ns.command == "status":
            print(json.dumps(k.call("status"), indent=2))
        elif ns.command == "call":
            name, *kv = ns.rest
            arguments = {key: _parse_value(value) for key, value in (a.split("=", 1) for a in kv)}
            data, png = k.call_raw(name, **arguments)
            print(json.dumps(data, indent=2))
            if png and "path" not in arguments:
                print(f"(image: {len(png)} bytes PNG; pass path=... to save it)")
        else:
            ap.error("unknown command " + ns.command)


if __name__ == "__main__":
    main()
