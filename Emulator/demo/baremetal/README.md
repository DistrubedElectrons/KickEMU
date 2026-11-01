# KICK bare-metal demo

A small firmware that runs directly on the i.MX 8M Mini of the KICK, without U-Boot or
Linux. It is the first program that ran on the emulated `kick` machine and doubles as a
reference for writing your own firmware for the KICK: every driver in it programs the SoC
registers the way the i.MX8MM Reference Manual, ATF and Linux do, nothing in it depends on
the emulator.

What it does:

- Console banner and a system report on UART4 (the debug console, 115200 8N1).
- Reads the SoC identity the way U-Boot and Linux do: DIGPROG from CCM_ANALOG, unique ID,
  variant and MAC from the OCOTP fuses, PLL rates, die temperature from the TMU, CPU count
  through PSCI.
- GICv3 set up from scratch, GPT1 as a 100 Hz tick interrupt.
- Scans the four I2C buses (PMIC 0x4b and the DSI bridge 0x2c on I2C1, GPIO expander 0x20
  on I2C2, touch 0x14 and RTC 0x68 on I2C3, CV converter 0x0c on I2C4).
- Toggles GPIO4_IO25 (the codec mute line) once per second and reads it back.
- Talks to the control-surface MCU on UART1 at 230400 baud with the real KICK protocol
  (`ComProtocol/PktMgr` framing, the packets of `Kick/modules/contracts/dcl`): the
  `COMM_INIT` handshake, a rainbow wave across the 4x4 pads, a chase over the key LEDs, the
  jog ring spinning (emulator LED ids 56-61), the LINK LEDs counting beats (62-65) and a
  slider level sweep. A pad hit reported by the MCU flashes that pad.
- Powers the display mix through the GPC, clocks and starts the eLCDIF and draws an
  800x480 screen: the KICK wordmark wiping in, then live system facts, counters, the last
  pad hit and a mirror of the pad colours.

## Layout

| File | Contents |
|---|---|
| `src/start.S` | entry, EL2 to EL1 drop, exception vectors |
| `src/mmu.c` | identity map with caches, cache clean for the LCDIF |
| `src/clock.c` | CCM clock roots and gates, PLL rate from CCM_ANALOG |
| `src/uart.c`, `src/console.c`, `src/fmt.c` | i.MX UART, console, printf |
| `src/gic.c`, `src/gpt.c` | GICv3 and the GPT tick |
| `src/i2c.c`, `src/gpio.c` | I2C master (polled), GPIO |
| `src/soc_info.c`, `src/psci.c` | SoC identification, TMU, PSCI |
| `src/lcdif.c`, `src/gfx.c`, `src/screen.c` | display bring-up, drawing, screen layout |
| `src/pktmgr.c`, `src/panel.c` | PktMgr protocol port and the light show |
| `src/main.c` | bring-up order and main loop |
| `tools/mcu-monitor.py` | a stand-in MCU for testing UART1 without the host application |

The 8x8 font is Daniel Hepper's public domain `font8x8_basic.h`.

## Build

Needs clang with the AArch64 target and an ELF lld (`brew install llvm lld`; a Rust
toolchain's `rust-lld` is used when `ld.lld` is missing) and CMake 3.22:

```sh
cmake -S Emulator/demo/baremetal -B Emulator/out/demo -G Ninja
cmake --build Emulator/out/demo
```

This produces `Emulator/out/demo/kick-demo.elf` (and `kick-demo.bin`). Load address,
framebuffer address and size, tick rate and baud rates are CMake cache variables
(`-DKICK_FB_ADDR=...`, see `CMakeLists.txt`).

## Run

Under TCG or HVF, with the console on the terminal:

```sh
Emulator/out/qemu/bin/qemu-system-aarch64 -M kick -m 2G -accel hvf -cpu host \
    -kernel Emulator/out/demo/kick-demo.elf \
    -serial null -serial null -serial null -serial stdio
```

(`-accel tcg -cpu cortex-a53` for TCG.) Add `-display cocoa` to see the screen.

With the KickEmu host application (front panel, screen, MCU emulator) select the demo as
the kernel, the host acts as the MCU on UART1.

Without the host application, `tools/mcu-monitor.py` decodes the frames the demo sends and
plays the MCU, including pad hits:

```sh
python3 Emulator/demo/baremetal/tools/mcu-monitor.py mcu.sock --pads 8 &
Emulator/out/qemu/bin/qemu-system-aarch64 -M kick -m 2G -accel hvf -cpu host \
    -kernel Emulator/out/demo/kick-demo.elf -display none \
    -serial unix:mcu.sock -serial null -serial null -serial stdio
```

## Notes for your own firmware

- QEMU starts the image at EL2 under TCG and EL1 under HVF; on the KICK, ATF hands over at
  EL2. `start.S` shows the EL2 to EL1 hand-over (HCR_EL2, CNTHCTL_EL2, ICC_SRE_EL2).
- Without EL3 firmware the GIC runs with one security state: use Group 1, affinity routing.
- Enable the clock root and the CCGR gate of every block before touching it (`clock.c`);
  on the hardware an ungated block does not answer.
- The display mix must be powered through the GPC (and the ADB400 handshake completed)
  before the LCDIF registers respond.
- Clean the data cache over the framebuffer after drawing: the LCDIF reads DDR directly.
