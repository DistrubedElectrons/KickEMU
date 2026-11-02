# Automation, headless runs and MCP

Every KickEmu instance, with or without a window, serves the same set of automation tools:
power, console, screen, front panel, touch, MIDI, CV, SD slot, audio files, USB and raw QMP.
They are reachable three ways:

| Transport | How | Use |
|---|---|---|
| MCP on stdio | `KickEmu --mcp [--profile ...]` | coding agents (Claude Code, Cursor, any MCP client) |
| MCP on a unix socket | `<run_dir>/automation.sock` (JSON lines) | scripts, CI, attaching an agent to a running GUI |
| Smoke test flags | `KickEmu --expect <regex> --timeout <s>` | one-line boot tests |

`KickEmu` below is `Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu`
on macOS and `Emulator/out/host/KickEmu_artefacts/Release/KickEmu` on Linux.

## Headless smoke test

```sh
KickEmu --profile selftest --run-dir /tmp/kemu-ci --expect 'kick-selftest: .* passed' --timeout 300
```

Exit codes: 0 match, 2 invalid pattern, 3 timeout, 4 QEMU exited first, 1 start failure.
The console is mirrored to stdout, the host log goes with it. Use one `--run-dir` per
instance so parallel runs do not share sockets (and set `--set qemu.ssh_host_port=0` or a
distinct port). Headless runs use `audiodev=none` unless one is set explicitly.

## MCP for agents

Claude Code (`.mcp.json` in the project, or `claude mcp add`):

```json
{
  "mcpServers": {
    "kickemu": {
      "command": "Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu",
      "args": ["--mcp", "--profile", "selftest", "--run-dir", "/tmp/kemu-agent"]
    }
  }
}
```

The machine starts with the server (pass `--no-start` to start it with `machine_start`).
Closing stdin quits the instance. stdout carries only the protocol; the log goes to stderr.

To give an agent a GUI instance you are already running, bridge its socket:

```json
{ "command": "socat", "args": ["STDIO", "UNIX-CONNECT:/tmp/kickemu/automation.sock"] }
```

## Tools

| Tool | Arguments | Result |
|---|---|---|
| `status` | | machine state, uptime, accel, MCU link, console/MIDI/CV/display state, command line |
| `machine_start`, `machine_stop`, `machine_restart` | | state after the change |
| `power_button` | | SNVS power key (clean shutdown request) |
| `console_read` | `since`, `max_bytes` | text since an offset, `next` offset |
| `console_write` | `text`, `enter` | bytes sent |
| `console_wait` | `pattern` (regex), `timeout_ms`, `since` | match, line, `next` |
| `console_run` | `command`, `prompt`, `timeout_ms` | command output (waits for the prompt) |
| `screenshot` | `path` | PNG image of the 1024x600 panel, frame number |
| `screen_wait` | `frame`, `timeout_ms` | next frame number |
| `panel_controls` | | control ids, types, labels |
| `press` | `control`, `action` (tap/down/up/long), `hold_ms`, `velocity` | buttons, jog keys, pads, jog push |
| `turn` | `control` (p1..p4, jog), `steps` | encoder steps |
| `slider` | `percent` | touch slider |
| `panel_state` | | LEDs, pad colours/states, slider, BPM, MCU link |
| `touch`, `swipe` | pixel coordinates, `action`, `slot` | multitouch on the screen |
| `midi_send`, `midi_read` | hex bytes / `since` | MIDI IN / MIDI OUT |
| `cv_read`, `cv_set` | `channel`, `volts` | CV/Gate outputs and inputs |
| `sd_insert`, `sd_eject`, `sd_status` | `path` | hot insert and eject on the front slot |
| `audio_wav_in`, `audio_wav_out`, `audio_status` | `path`, `loop` | WAV file into the input, recording of the output |
| `measure` | `source`, `window_ms` | multimeter: DC, RMS, AC RMS, peak, min/max, frequency (audio also dBFS) |
| `spectrum` | `source`, `fft_size`, `window_ms`, `peaks`, `min_db` | strongest spectral peaks (Hann, Welch average, dBFS) |
| `midi_monitor` | `enable`, `clear`, `since`, `max` | decoded MIDI in both directions (`in` = to the KICK, `out` = from it) |
| `instrument_view` | `view` (scope/spectrum/meter/midi), `path`, sources, timebase | PNG of an instrument page, rendered offscreen |
| `usb_attach`, `usb_detach`, `usb_list` | `device`, `port`, `path`, ids | virtual or passed-through USB devices |
| `log_read` | `since`, `source` | host log |
| `qmp`, `hmp` | raw command | escape hatch (snapshots: `hmp savevm s1` / `loadvm s1`) |

Instrument sources: `audio_l`, `audio_r`, `audio_mid` are the SAI2 playback (the `audio-tap`
stream of host-interface.md, 48 kHz, full scale = 1.0); `cv1`..`cv8` are the CV/GATE outputs 1..8
as printed on the rear panel (`cv_read` channels 0..7), in volts, sampled and held at 1 kHz on
the host clock because the converter only reports changes. Audio and CV run on different clocks
(guest time vs host time), so the scope aligns them only approximately. A reading's `age_ms` says
how long ago the source last delivered data; the guest sends no audio frames while nothing plays.
The same instruments are in the GUI under the side panel's Measure tab (Window opens a resizable
copy).

Console offsets are absolute byte offsets of the console text (escape sequences removed).
Pass a returned `next` as `since` to see only what came after. `console_wait` without
`since` only looks at output that arrives after the call.

## Scripts and CI

`Emulator/tools/kickemu_client.py` is a dependency-free Python client:

```python
from kickemu_client import KickEmu
with KickEmu.launch(["--profile", "selftest", "--run-dir", "/tmp/kemu-ci"]) as k:
    k.call("console_wait", pattern="login:", since=0, timeout_ms=120000)
    print(k.call("console_run", command="cat /proc/cpuinfo | head -3")["output"])
    k.screenshot("panel.png")
```

```sh
Emulator/tools/kickemu_client.py --socket /tmp/kickemu/automation.sock call press control=play
Emulator/tools/kickemu_client.py --launch tools -- --profile demo
```
