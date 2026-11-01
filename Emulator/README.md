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
```

The remaining build steps (boot chain, test image, host application) are added as each
component lands. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the plan.

## Configuration

`config/kickemu.json` holds every path, size and port used by the scripts and the host
application. Copy it and pass `--config <file>` to override.

## License

The emulator code is licensed under AGPL-3.0-or-later, the same as the rest of the
repository. QEMU code under `qemu/hw/kick` is also licensed GPL-2.0-or-later so it can be
built into QEMU. JUCE is used under its AGPLv3 option.
