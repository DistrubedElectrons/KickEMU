# KICK Emulator Architecture

This document is the contract between the emulator components. If a component changes
an interface described here, it updates this file in the same commit.

## Goals

- Emulate the KICK (BE 0700-AAM) hardware **register-accurately** so the real boot chain
  (SPL, ATF, U-Boot), the vendor kernel and its drivers, the KICK firmware and custom
  firmware all run unmodified and show real hardware behaviour.
- Be fast enough for daily development on an Apple Silicon Mac (Linux hosts later).
- Show the device 1:1: screen, all buttons, pads, encoders, jog wheel, slider and LEDs,
  plus the rear and front connectors.
- Expose real I/O on the host: audio in/out, MIDI in/out (virtual MIDI ports), CV/gate
  monitoring, Ethernet (LINK), SD card and USB images, serial console.

## Hardware being emulated

The kernel is varigit `linux-imx` **5.4.127** (the pinned commit). The full enabled-node
checklist is [board-dt.md](board-dt.md).

Sources: `Linux/buildroot/configs/KTKick_defconfig`, the board files and patches in
`Linux/buildroot/board/freescale/imx8mm_var_som/`, the Variscite VAR-SOM-MX8M-MINI
device tree (`imx8mm-var-som.dtsi`, `imx8mm-var-som-symphony.dts` in the varigit kernel),
the NXP i.MX 8M Mini Reference Manual, and the application code.

| Real hardware | Linux view | Used by |
|---|---|---|
| NXP i.MX8M Mini, 4x Cortex-A53, GICv3 | aarch64 | everything |
| UART1 to the control-surface MCU, 230400 8N1 | `/dev/ttymxc0` | `Kick/modules/input_events` (PktMgr framing, `ComProtocol/PktMgr`) |
| UART3, 5-pin DIN MIDI | `/dev/ttymxc2` | `Kick/apps/midiPort` (ttymidi) |
| UART4, debug console | `/dev/ttymxc3` | SPL, ATF, U-Boot, kernel, getty |
| LCDIF -> Samsung DSIM -> TI SN65DSI84 DSI-to-LVDS bridge (I2C1 0x2c) -> LVDS panel, 1024x600 @ 33 MHz per the bridge timings in the DT | DRM/KMS, Weston | GUI |
| Goodix GT911 touch, I2C addr 0x14 | evdev multitouch | GUI |
| RTC at I2C3 0x68: the part is a PCF85263, but the symphony DTS overrides the patched node with `dallas,ds1337` (rtc-ds1307 driver), so the kernel talks DS1337 registers to it. Model what the driver uses. | `/dev/rtc0` | system |
| NXP PCA9534 GPIO expander, I2C2 0x20 (gpio-keys, hogs) | gpiochip, input | system |
| OmniVision OV5640 camera, I2C2 0x3c (MIPI CSI) | V4L2 | camera (later phase) |
| ROHM BD71847 PMIC (I2C1) | regulators | SPL, kernel |
| CV/Gate converter on I2C4, addr 0x0c: Analog Devices AD5671R, 8 ch 12-bit DAC, outputs only (`0x30\|ch` write and update, `0x90\|ch` readback setup, readback of the input registers) | `/dev/i2c-3` | `Kick/modules/hardware/source/kick_I2c.cpp` (class `CV`) |
| SAI2 (clock master, 12.288 MHz MCLK) + SDMA2 (events 2/3) to CoolAudio V4220M (no control bus, `linux,bt-sco` dummy). SDMA RAM firmware is under the NXP EULA, so the driver may run ROM scripts only. | ALSA card0, S32_LE 48 kHz | audio engine |
| uSDHC1 Wi-Fi SDIO | `mmc0` (5.4 numbers MMC hosts by probe order) | Wi-Fi |
| uSDHC2 SD card slot | `/dev/mmcblk1` | boot, file browser, update |
| uSDHC3 eMMC, 8-bit, CQHCI (registers at base+0x100), GPT layout from `genimage.cfg.emmc.partiton.table` | `/dev/mmcblk2` | boot, rootfs, KICK storage |
| ENET (FEC) + PHY, LINK jack | `eth0` | network |
| SNVS power key | input event, `KEY_STOP`/power | `kick_Hwpref.cpp` |
| OCOTP fuses, SoC ID | `/sys/devices/soc0/*` | `kick_Hwpref.cpp`, U-Boot |
| USB OTG gadget (FunctionFS `/dev/usb-utp0`, ACM `/dev/ttyGS0`) | PC mode / update | `kick_UsbFunctionFS.cpp` (later phase) |
| Vivante GC GPU | galcore | Weston G2D (not emulated, software rendering) |
| AI camera | V4L2 | gesture control (later phase) |

Pads, knobs, buttons, LEDs, jog wheel, slider and CV/gate are behind the MCU and reach
Linux only as PktMgr packets on UART1. The MCU is emulated at the UART byte level by the
host application.

## Components

```
Emulator/
  docs/            this file and per-component notes
  config/          default configuration (kickemu.json); nothing is hardcoded in code
  qemu/            our QEMU sources: SoC, board, device models, qtests, integration patch
  boot/            builds the real boot chain and kernel from the board config (container)
  testimage/       small test rootfs and self-test scripts for the hardware layer
  host/            KickEmu host application (front panel, MCU emulator, MIDI bridge, launcher)
  firmware/        firmware pipeline: build from source, import .swu, import dumps (later)
  third_party/     git submodules (qemu, JUCE)
  out/             build output, gitignored
```

### 1. QEMU (`Emulator/qemu`, submodule `Emulator/third_party/qemu`)

QEMU is a git submodule pinned to an upstream release tag. Our code lives in
`Emulator/qemu/` and is never committed into the submodule. `Emulator/qemu/build.sh`
links `Emulator/qemu/hw/kick` into the submodule tree as `hw/kick`, applies
`Emulator/qemu/patches/*.patch` (small integration hooks only), then configures and
builds into `Emulator/out/qemu`. Every model lives under `hw/kick/` with its own
`meson.build` and `Kconfig` entries, and qtests under `Emulator/qemu/tests/`.

**SoC `fsl-imx8mm`** (`hw/kick/fsl-imx8mm.c`): the real i.MX8MM memory map, interrupt
numbers and clock tree, modelled on QEMU's `fsl-imx8mp`. Reuses upstream models where they
exist and are correct for the 8MM (imx_serial, imx_i2c, imx_spi, imx_gpio, imx-usdhc,
imx_fec, imx_gpt, wdt_imx2, snvs), and adds 8MM-specific ones (CCM, ANATOP, SRC, GPC,
IOMUXC/GPR, OCOTP, DDRC + DDR PHY, TMU, SDMA, SAI, LCDIF, MIPI DSIM, BootROM loader).
Unimplemented blocks are mapped as `unimplemented-device` so accesses are logged.

**Board `kick`** (`hw/kick/kick-board.c`): VAR-SOM-MX8M-MINI on the KICK carrier.
RAM size, eMMC/SD images, PMIC, GT911, RTC, PCA9534, SN65DSI84, the I2C4 0x0c device, ENET PHY,
panel and power key.

**Register accuracy rules:**
- Register layouts, reset values, W1C/W1S/self-clearing bits, status and interrupt
  behaviour follow the i.MX 8M Mini Reference Manual. Where the RM is ambiguous, follow
  what the Linux and U-Boot drivers rely on, and note it in the source.
- Every model has a qtest that checks reset values and the main state transitions.
- Unknown or unimplemented register accesses go to `qemu_log_mask(LOG_UNIMP, ...)`.
- Timing that drivers poll for (PLL lock, DDR training done, MMC busy) completes
  deterministically. Never hardcode delays that change guest behaviour.

**Boot modes, same machine:**

| Mode | Accelerator | Entry | Use |
|---|---|---|---|
| `boot=rom` | TCG | BootROM emulation reads `imx-boot` from eMMC/SD (offset 33 KiB), runs SPL in OCRAM, then ATF (EL3), U-Boot, kernel | boot chain, U-Boot, full fidelity |
| `boot=uboot` | TCG or HVF | U-Boot proper loaded at its link address, PSCI from QEMU | U-Boot work |
| `boot=kernel` | HVF (default) or TCG | `-kernel Image -dtb <board dtb>`, PSCI from QEMU | daily use, drivers, firmware |

With HVF the CPUs are `-cpu host` (the guest still sees the i.MX8MM DT and peripherals).

### 2. Boot chain and kernel (`Emulator/boot`)

Container builds of the exact components from `KTKick_defconfig`: varigit `linux-imx` at
the pinned commit with the board patches and `Linux-defconfig`, U-Boot (`uboot-imx`) with
its patches and SPL, `imx-atf` BL31, NXP DDR firmware and `imx-mkimage`, producing
`imx-boot`, `Image` and `imx8mm-var-som-symphony.dtb` in `Emulator/out/boot/`.
Repository URLs and commits come from the defconfig. They are not duplicated.
Outputs go to the main checkout's `Emulator/out/boot` (also when built from a worktree);
the full list, the eMMC image (`Emulator/boot/mkimage-emmc.sh` ->
`Emulator/out/images/emmc.img`) and the DDR firmware rules are in `Emulator/boot/README.md`.
Without the NXP DDR training firmware, `imx-boot` carries zero-filled training blobs and
is valid for the emulator only. The enabled DT nodes the models must provide are listed
in `docs/board-dt.md`.

### 3. Test image (`Emulator/testimage`)

A minimal aarch64 rootfs (busybox, `evtest`, `i2c-tools`, `alsa-utils`, `mmc-utils`,
`memtester`, `devmem2`) plus `kick-selftest`, which exercises every emulated peripheral
through the real drivers. It validates the hardware layer before any firmware image is used.

### 4. Host application (`Emulator/host`, target `KickEmu`)

C++17, CMake, JUCE (submodule `Emulator/third_party/JUCE`). Responsibilities:

- **Launcher:** builds the QEMU command line from the configuration, creates the run
  directory and sockets, starts and stops QEMU, shows its log. Shows branch and commit
  in `--help` and in the about box.
- **Front panel:** draws the device 1:1 to scale (top view with the screen in its bezel;
  rear and front connector strips). It must match `docs/images/kick-front.webp` and
  `docs/images/views.png`. Every control is interactive by mouse, trackpad and keyboard.
- **Display:** RFB (VNC) client on the QEMU VNC unix socket, rendered inside the bezel.
- **Touch:** mouse and trackpad events on the screen are sent as QMP `input-send-event`
  multitouch (`mtt`) and absolute (`abs`) events. The GT911 model consumes them.
- **MCU emulator:** implements the MCU side of the PktMgr protocol on the UART1 socket,
  following `Kick/modules/contracts/dcl` and `ComProtocol/PktMgr`.
- **MIDI bridge:** UART3 socket byte stream <-> host MIDI, with virtual ports
  "KICK MIDI Out" / "KICK MIDI In" (CoreMIDI; ALSA sequencer on Linux).
- **Console:** UART4 socket in a terminal pane.

### Host <-> QEMU interfaces

All sockets live in the run directory (`run_dir` in the config). It is kept short because
unix socket paths are limited to 104 bytes on macOS. The host app creates the UART
listening sockets first. QEMU connects as a client with reconnect, so either side can
restart.

| Socket | QEMU option | Guest side | Content |
|---|---|---|---|
| `mcu.sock` | `-chardev socket,id=uart1,path=...,reconnect-ms=500 -serial chardev:uart1` | UART1, `/dev/ttymxc0` | raw UART bytes, PktMgr frames |
| `uart2.sock` | `id=uart2`, second `-serial` | UART2, `/dev/ttymxc1` | raw bytes |
| `midi.sock` | `id=uart3`, third `-serial` | UART3, `/dev/ttymxc2` | raw MIDI bytes |
| `console.sock` | `id=uart4`, fourth `-serial` | UART4, `/dev/ttymxc3` | console |
| `qmp.sock` | `-qmp unix:...,server=on,wait=off` (QEMU is server) | n/a | QMP JSON |
| `vnc.sock` | `-display vnc=unix:...` (QEMU is server) | panel | RFB 3.8 |
| `cv.sock` | `-chardev socket,id=cv,path=...,reconnect-ms=500`, board property `cv-chardev=cv` | CV/Gate converter | JSON lines, see below |

`serial_hd(0..3)` map to UART1..UART4 in that order.

Audio: the SAI2 model plays to and records from a QEMU audiodev (`coreaudio` on macOS),
selected with the machine property `audiodev`. Ethernet: `-nic user` with SSH forwarding.
eMMC and SD: raw images through `-drive if=none,id=emmc` / `id=sd` and the machine
properties `emmc` / `sd`.

### CV/Gate socket protocol

One JSON object per line, UTF-8, `\n` terminated.

- QEMU to host, on every change of a channel output: `{"ch":0,"dac":2048,"volts":2.500}`.
  `volts` = 2.5 V x 2 x code / 4096 (0..5 V, the scale of `kick_I2c.cpp`); it is 0 while the
  channel is powered down. The state of all 8 channels is sent when the socket connects.
- Host to QEMU: nothing. The converter is an AD5671R DAC without inputs (the `0x90` reads in
  `kick_I2c.cpp` read back the DAC registers), so there is no input channel to drive. Lines
  sent by the host are read and discarded.

### Touch conventions

QEMU absolute axes run 0..0x7fff. The GT911 model scales them to its configured panel size
(default 800x480, as `touchscreen-size-x/y` in the board DT, even though the display is
1024x600; keep both configurable and reproduce the real mapping) and reports up to 5 contacts.

## Phases

1. **Hardware layer:** SoC and board models, boot chain and kernel builds, test image,
   host app with front panel, display, touch, MCU emulator and MIDI bridge. Validated with
   the test image only.
2. **Firmware:** build the KICK rootfs and application from this repository; import
   official `.swu` updates and device dumps; assemble eMMC images with the real partition
   layout; boot them.
3. **Extras:** USB OTG gadget, camera, GPU stub, CV/gate scope, Linux host support.
