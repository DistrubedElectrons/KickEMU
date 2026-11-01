# KICK Emulator

A register-level emulator of the Behringer KICK (BE 0700-AAM) for developing and testing
the boot chain, kernel drivers, the KICK firmware and custom firmware on a computer.

Status: **in development, phase 1 (hardware layer)**. Nothing in this folder is ready to
boot a KICK firmware image yet.

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
```

See [boot/README.md](boot/README.md) (including the note on the NXP DDR firmware) and
[testimage/README.md](testimage/README.md). The device tree checklist for the models is
[docs/board-dt.md](docs/board-dt.md); the I2C board devices, SAI and SDMA models are described
in [docs/devices-i2c-audio.md](docs/devices-i2c-audio.md). The host application is added when it lands. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the plan.

## Configuration

`config/kickemu.json` holds every path, size and port used by the scripts and the host
application. Copy it and pass `--config <file>` to override.

## License

The emulator code is licensed under AGPL-3.0-or-later, the same as the rest of the
repository. QEMU code under `qemu/hw/kick` is also licensed GPL-2.0-or-later so it can be
built into QEMU. JUCE is used under its AGPLv3 option.
