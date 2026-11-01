# KickEmu

The host application of the KICK emulator. It shows the device, launches QEMU and provides
all of its I/O:

- **Front panel**: the top view drawn as vector graphics from `resources/layout.json`
  (millimetres measured on `docs/images`), with a rear and a front view. Pads, keys,
  encoders, the jog wheel, the touch slider and every LED are live.
- **Display and touch**: an RFB 3.8 client on `vnc.sock` draws the guest framebuffer into
  the bezel (following the framebuffer size, 1024x600 on the KICK); clicks and drags on the
  screen become QMP `input-send-event` multitouch (`mtt`) and absolute (`abs`) events.
- **MCU emulator**: the control-surface MCU on `mcu.sock` (UART1), framed by the real
  `ComProtocol/PktMgr` code.
- **MIDI**: UART3 (`midi.sock`) to CoreMIDI virtual ports "KICK MIDI Out" / "KICK MIDI In",
  optionally also to an existing MIDI device.
- **CV/Gate**: `cv.sock` JSON lines; output voltages on the rear view, inputs settable.
- **Consoles**: UART4 (`console.sock`) and UART2 in terminal tabs.
- **Launcher**: builds the QEMU command line from the configuration, starts, stops and
  restarts QEMU, shows its output in the log.

The interfaces are described in [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

## Build

Requirements: macOS with Xcode command line tools, CMake 3.22+, Ninja.

```sh
git submodule update --init --depth 1 Emulator/third_party/JUCE
cmake -S Emulator/host -B Emulator/out/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Emulator/out/host
ctest --test-dir Emulator/out/host --output-on-failure
```

The app is `Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app`. Version, git branch
and commit are compiled in at build time (`--version`, Settings tab).
`-DKICKEMU_BUILD_APP=OFF` builds only the core library and the tests.

## Run

```sh
KickEmu=Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu

$KickEmu                                  # kick machine, settings from Emulator/config/kickemu.json
$KickEmu --profile virt-test              # stock QEMU virt + virtio GPU/multitouch (no kick machine needed)
$KickEmu --set qemu.accel=tcg --set qemu.boot=rom
$KickEmu --print-command                  # show the QEMU command line
$KickEmu --headless                       # no window; log and console on stdout, exits with QEMU
$KickEmu --help
```

| Option | Meaning |
|---|---|
| `--config <file>` | configuration file (default `Emulator/config/kickemu.json`, found from the app location) |
| `--profile <name>` | merge `profiles.<name>` from the config |
| `--set key.path=value` | override a config value (JSON values allowed), repeatable |
| `--headless` | no window |
| `--no-start` | open the window without starting QEMU |
| `--print-command` | print the QEMU command line and exit |
| `--view top\|rear\|front` | start in a view |
| `--screenshot <png>` / `--screenshot-delay <ms>` | save a 2x image of the window and quit |

The UART, MIDI and CV sockets are created by KickEmu before QEMU starts and live as long as
the app, so QEMU can be restarted (or crash) without losing anything; QEMU reconnects.

## Using the panel

- **Pads**: click; the closer to the centre, the higher the velocity (a force-sensitive
  trackpad or pen sets velocity by pressure). While held, moving up (or pressing harder)
  sends aftertouch when the guest enabled it.
- **Keys**: click. Hold SHIFT (or the keyboard Shift key) for the second function printed
  below a key (MAIN -> GRID, BANK A -> E, ...).
- **Encoders P1-P4**: drag up/down or scroll.
- **Jog wheel**: drag around the centre or scroll to turn; click (or hold) to push. The four
  keys around it are undo, copy, minus and plus.
- **Slider**: drag on the strip; 0 % at the bottom.
- **MASTER / REC**: analog on the device (audio path, not the MCU); they keep their
  position but are not reported to the guest. Double-click centres them.
- **Screen**: click and drag to touch; hold Option for a two-finger pinch mirrored around
  the screen centre. Every pointer source (e.g. a touchscreen) is its own contact.
- **Rear view**: MIDI activity LEDs on the DIN jacks, CV voltages under the CV jacks
  (channel n and n+4), network link LED on the LINK jack. Click a CV or MIDI jack to open
  the I/O tab. Hover any connector for details.

### Keyboard

Click the device first. Keys follow the KICK desktop simulator's keyboard map
(`Kick/modules/contracts/utils/keyboard/keyboard_detector.cpp`):

| Keys | Controls |
|---|---|
| `Z X C V` | pads 1-4 (bottom row) |
| `A S D F` | pads 5-8 |
| `Q W E R` | pads 9-12 |
| `1 2 3 4` | pads 13-16 |
| `T Y U I O` | MAIN, BROWSE, TRACK MIX, TRACK MUTE, NEXT SEQ |
| `G H J K L` | STEP SEQ, TC, SAMPLER, SAMPLE EDIT, PROG EDIT |
| `F1`-`F4` | BANK A-D |
| `F5` `F6` `F7` | TAP TEMPO, NOTE REPEAT, MENU |
| `F8` `F9` `F10` | FULL LEVEL, 16 LEVELS, ERASE |
| `F12` `F11` `Esc` `Space` `/` | RECORD, OVERDUB, STOP, PLAY, PLAY START |
| `P` | LINK |
| `Shift` | SHIFT |
| `Up` / `Down`, `Return` | jog wheel turn, push |
| `Backspace` `\` `-` `=` | undo, copy, minus, plus |

The map lives in `resources/layout.json` (`key` on each control) and is listed in the
Settings tab and in each control's tooltip.

App shortcuts: `Cmd+P` presentation mode, `Cmd+Shift+S` snapshot, `Cmd+L` side panel,
`Cmd+1/2/3` views, `Cmd+0` fit, `Cmd+R` restart QEMU, `Cmd+.` stop QEMU.

### Presentation and snapshots

Presentation mode shows only the device on a plain background (colour in the Settings tab,
default `host.ui.presentation_background`). Snapshots are PNGs of the current view at
`host.ui.snapshot_scale` (2x) in `host.ui.snapshot_dir` (`~/Pictures/KickEmu`).

## Settings

Emulator settings come from the JSON config (nothing is hardcoded). The `host` section:

| Key | Meaning |
|---|---|
| `host.autostart` | start QEMU when the window opens |
| `host.sockets.*` | socket file names inside `run_dir` |
| `host.chardev_reconnect_ms` | `reconnect-ms` of QEMU's socket chardevs |
| `host.connect_retry_ms` | QMP/VNC connect retry interval |
| `host.qemu_quit_timeout_ms` | how long QMP `quit` may take before QEMU is killed |
| `host.ui.*` | scale presets, px/mm at 100 %, LED fade and blink timing, terminal font, snapshot settings, `layout_file` to use an external layout.json |
| `host.mcu.frame_trailer` | bytes appended to every MCU frame (see ARCHITECTURE.md) |
| `host.mcu.comm_init_interval_ms`, `keepalive_interval_ms` | handshake timing |
| `host.mcu.sw_version` | version reported with `EN_SYS_GET_SW_VER` |
| `host.mcu.led_radio_groups` | LEDs that are mutually exclusive (banks, transport) |
| `host.mcu.button_led_colour` | `fixed` (key colours from layout.json) or `palette` (LED colour id > 0 picks a pad palette colour) |
| `host.mcu.jog_ring_default` | ring when the guest does not drive it: `connected`, `always`, `off` |
| `host.mcu.pad_velocity_min/max`, `aftertouch_*`, `long_press_ms`, `encoder_drag_px_per_step`, `jog_drag_degrees_per_step`, `wheel_steps_per_notch`, `tx_frame_gap_us` | input behaviour |
| `host.touch.*` | axis range, contacts, pinch modifier, whether to send `abs` events |
| `host.cv.*` | channel count and DAC/ADC scale |
| `host.pad_palette` | pad colours by `PadColourIds` (from `Kick/assets/Default/theme.xml`) |

User settings (window layout, view, scale, side panel, MIDI device bindings, CV input
voltages, pot positions, presentation colour) are stored in
`~/Library/Application Support/KickEmu/KickEmu.settings`.

## Driving the MCU without a guest

`tools/mcu_send.py` connects to `mcu.sock` the way the guest's UART does and sends PktMgr
frames, e.g. `tools/mcu_send.py demo` lights every LED and cycles the pads,
`tools/mcu_send.py led 8 1` turns the MAIN LED on, `tools/mcu_send.py listen` prints the
events the panel sends. Only one client can be attached, so stop QEMU first.

## Layout

```
host/
  CMakeLists.txt      JUCE added via add_subdirectory, core library + app + tests
  resources/          layout.json: geometry (mm), key map, LED ids and colours, theme
  src/config/         Config (JSON, profiles, overrides), Layout
  src/emu/            sockets, McuProtocol/McuEmulator, QEMU launcher, QMP, RFB,
                      MIDI/CV/console bridges, terminal model (no GUI code)
  src/panel/          one component per control type, TopPanel, Rear/Front, DeviceView
  src/app/            Session, main window, title bar, side panel, settings, CLI
  tests/              JUCE UnitTest suite (KickEmuTests)
  tools/mcu_send.py   guest-side MCU test client
```
