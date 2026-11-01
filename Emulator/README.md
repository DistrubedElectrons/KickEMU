# KICK Emulator

A register-level emulator of the Behringer KICK (BE 0700-AAM) for developing and testing
the boot chain, kernel drivers, the KICK firmware and custom firmware on a computer.

Status: **0.1, hardware layer working**. The vendor kernel 5.4.127 boots to a shell under
HVF in about 1.5 s and passes the board self-test; the bare-metal demo drives the screen and
the front panel; the KICK application (KT_Kick) runs on a Debian 12 root file system with
its screen, touch, MCU link, audio and network (see "Running the KICK application").

## How it works

- `qemu/` adds an `fsl-imx8mm` SoC and the `kick` board to QEMU, with register-level models
  of the i.MX8M Mini peripherals and the KICK board devices. Details are in
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- The same machine runs under TCG (full boot chain from the BootROM, including SPL and ATF)
  or under HVF on Apple Silicon (direct U-Boot or kernel boot at near-native speed).
- `host/` is the KickEmu application: a 1:1 front panel with the screen, pads, buttons,
  encoders, jog wheel and LEDs, the MCU emulator for the control surface, and a MIDI
  bridge with virtual MIDI ports.

## Requirements (macOS)

- Xcode command line tools, CMake 3.22 or newer, Ninja, Meson, pkg-config
- Homebrew: `glib pixman dtc libslirp`
- A container runtime (OrbStack or Docker) for the kernel and boot chain builds

## Building

```sh
git submodule update --init --recursive Emulator/third_party
Emulator/qemu/build.sh            # QEMU with the KICK models, into Emulator/out/qemu
Emulator/boot/build.sh            # kernel, DTBs, galcore, ATF, U-Boot, imx-boot (container)
Emulator/testimage/build.sh       # test initramfs and ext4 rootfs with kick-selftest
Emulator/boot/mkimage-emmc.sh     # eMMC image with the KICK partition layout

# Host application (KickEmu) and its tests, into Emulator/out/host
cmake -S Emulator/host -B Emulator/out/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Emulator/out/host
ctest --test-dir Emulator/out/host --output-on-failure
open Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app
```

See [boot/README.md](boot/README.md) (including the note on the NXP DDR firmware),
[testimage/README.md](testimage/README.md) and [host/README.md](host/README.md) (running
KickEmu, keyboard map, settings). The device tree checklist for the models is
[docs/board-dt.md](docs/board-dt.md). The models are described in
[docs/display-gpu.md](docs/display-gpu.md) and [docs/devices-i2c-audio.md](docs/devices-i2c-audio.md).
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the overall design.

## Running the KICK application

`rootfs/build.sh` builds a Debian 12 arm64 root file system from prebuilt packages with
KT_Kick (the community arm64 build of the KICK app from
[KickEm-Tools/0700-AAM](https://github.com/KickEm-Tools/0700-AAM), AGPL-3.0), X on the
framebuffer and the KICK's own service files, and writes it into the eMMC image. KickEmu
boots it with the `kick-app` profile; the app screen shows after about 17 s under HVF.

```sh
Emulator/rootfs/build.sh                  # needs the boot build, a container runtime and gh
Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu --profile kick-app
```

Details, the start sequence compared with the KICK, and licenses: [rootfs/README.md](rootfs/README.md).

## Configuration

`config/kickemu.json` holds every path, size and port used by the scripts and the host
application. Copy it and pass `--config <file>` to override.

## License

The emulator code is licensed under AGPL-3.0-or-later, the same as the rest of the
repository. QEMU code under `qemu/hw/kick` is also licensed GPL-2.0-or-later so it can be
built into QEMU. JUCE is used under its AGPLv3 option.
