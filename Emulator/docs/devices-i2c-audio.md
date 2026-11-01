# I2C board devices and audio path

Models in `Emulator/qemu/hw/kick/`, qtests in `Emulator/qemu/tests/`. All of them are
plain QOM devices; the SoC (`fsl-imx8mm`) and the board (`kick`) instantiate and wire them.
Hardware facts come from the built DT (`Emulator/out/boot/imx8mm-var-som-symphony.dts`,
summarised in `board-dt.md`), the vendor 5.4.127 drivers and the application code.

| Device | QOM type | Kconfig | Source |
|---|---|---|---|
| Goodix GT911 touch | `goodix-gt911` | `KICK_GOODIX_GT911` | `goodix_gt911.c` |
| PCF85263A RTC (DS1337 view) | `pcf85263` | `KICK_PCF85263` | `pcf85263.c` |
| AD5671R CV/Gate DAC | `ad5675r` | `KICK_AD5675R` | `ad5675r.c` |
| PCA9534 GPIO expander | `pca9534` | `KICK_PCA9534` | `pca9534.c` |
| SAI | `imx8mm-sai` | `KICK_IMX8MM_SAI` | `imx8mm_sai.c` |
| SDMA | `imx8mm-sdma` | `KICK_IMX_SDMA` | `imx_sdma.c` |
| qtest machine | `kick-periph-test` | `KICK_PERIPH_TEST` | `kick-periph-test.c` |

The SoC/board Kconfig entries should `select` the device symbols they instantiate.

Run the tests:

```
Emulator/qemu/build.sh
meson test -C Emulator/out/qemu-build --suite qtest-aarch64 'qtest-aarch64/kick-*'
```

## kick-periph-test machine

A qtest-only machine with a powered-off Cortex-A53 (qtest memory accesses go through the
first CPU's address space) and the peripherals at their i.MX8MM addresses: I2C2/3/4
(`imx.i2c`, buses `i2c-bus.0/1/2`), SAI2 at 0x30020000 (`/machine/sai2`, MCLK1 12.288 MHz),
SDMA2 at 0x302c0000 (`/machine/sdma2`), SAI2 `dma-rx`/`dma-tx` wired to SDMA2 events 2/3,
64 MiB RAM at 0x40000000. I2C slaves are added with `-device ...,bus=i2c-bus.N`. Once the
`kick` machine is merged, the tests can move to it.

## Goodix GT911

- DT: `goodix,gt911`, I2C3 (0x30a40000, SPI 37) address 0x14, IRQ GPIO5_IO04 falling,
  `irq-gpios` GPIO5_IO04, `reset-gpios` GPIO5_IO03 active low, touchscreen-size 800x480.
  Driver `goodix.c` with `003.goodix.c.patch` (reads the config, never writes one).
- Registers: 16 bit big-endian address, auto-increment. 0x8040 command (0 read coords,
  5 screen off), 0x8041 ESD check, 0x8046 command check, 0x8047..0x8100 config (186 bytes:
  X/Y output max, touch number, Module_Switch1 trigger, refresh rate, channel maps, checksum
  at 0x80FF = two's complement of the byte sum, Config_Fresh 0x8100 = 1), 0x8140 "911\0",
  0x8144 firmware version, 0x8146/0x8148 resolution, 0x814A vendor id, 0x814E status
  (bit 7 buffer ready, bits 3:0 points), 0x814F.. 8 byte points (track id, x, y, size).
  A host config write is taken when Config_Fresh is written as 1 with a valid checksum.
- Behaviour: a report every scan period (5 + Refresh_Rate ms, 10 ms by default) while
  fingers are down plus one empty report after release; a new report only after the host
  wrote 0 to 0x814E. INT per Module_Switch1[1:0]: rising/falling edge pulses of
  `int-pulse-us`, low/high levels held until 0x814E is cleared. While RSTB is low the chip
  does not acknowledge; at the rising edge of RSTB the level on `int-sense` selects the
  address (high 0x14, low 0x5d), as `goodix_reset()` does. Command 5 sleeps until a rising
  INT.
- Input: QEMU input handler for MTT (slots 0..4 = track ids), ABS + BTN left/touch
  (single contact, ignored once MTT events were seen). Axes 0..0x7fff are scaled to the
  config X/Y output maximum.
- Coordinate space: the DT says 800x480 while the display is 1024x600. The real chip reports
  in its own configured output range regardless of the display; the model does the same with
  `width`/`height` (default 800x480). The host maps the 1024x600 screen to the 0..0x7fff
  QEMU axes, the model maps those to 0..799/0..479, and the guest driver scales as on the
  device. Set `width=1024,height=600` to emulate a panel configured for the display size.
- Properties: `width` (800), `height` (480), `max-contacts` (5), `int-trigger` (1 falling),
  `touch-size` (0x20), `fw-version` (0x1060), `vendor-id` (0), `int-pulse-us` (100).
- GPIOs: out `int`, in `int-sense`, in `reset` (see `goodix_gt911.h`).
- Wiring (as in `kick_board_wire_gt911()` on feature/emu-soc):

```c
dev = DEVICE(i2c_slave_create_simple(kick_i2c_bus(s, 3), "goodix-gt911", 0x14));
qdev_connect_gpio_out_named(dev, "int", 0, qdev_get_gpio_in(kick_gpio(s, 5), 4));
qdev_connect_gpio_out(kick_gpio(s, 5), 4, qdev_get_gpio_in_named(dev, "int-sense", 0));
qdev_connect_gpio_out(kick_gpio(s, 5), 3, qdev_get_gpio_in_named(dev, "reset", 0));
```

- qtest `kick-gt911-test`: ID/version, config defaults and checksum, config properties,
  config write, touch sequence (INT pulses, buffer handshake, periodic reports, 2 fingers,
  release), pointer input, level trigger, reset NACK and address select.
- Gaps: the config block is a model default (field meanings per the programming guide, not a
  dump of the KICK panel's flash); the capacitive key byte, gesture/proximity modes and
  real-time commands other than 0 and 5 are LOG_UNIMP; no pressure from QEMU events (size is
  the `touch-size` property). `int-sense` only reflects what the SoC GPIO drives while the
  pin is an output (QEMU's imx_gpio does not model the bidirectional pad).

## RTC: PCF85263A with DS1337 view

- DT: node `rtc@68` on I2C3, compatible **`dallas,ds1337`** (the symphony DTS overrides the
  `nxp,pcf85263` node of `001.imx8mm_dtsi.patch`), driver `rtc-ds1307`;
  `CONFIG_RTC_DRV_PCF85363` is not set. No interrupt in the DT.
- Discrepancy: the board carries a PCF85263A, the shipped kernel speaks the DS1337 register
  layout to it. On real hardware the DS1337 driver would read the 100ths register as
  seconds, the seconds as minutes, and so on. `register-map` selects the behaviour:
  - `ds1337` (default, per the coordinator's decision): DS1337 register view on the RTC time
    base, so `/dev/rtc0` and `hwclock` work with the shipped kernel. 0x00..0x06 time (12/24 h,
    century bit), 0x07..0x0A alarm 1 and 0x0B..0x0D alarm 2 with A*Mx masks and DY/DT,
    0x0E control (nEOSC freezes the clock and sets OSF, INTCN, A2IE, A1IE; reset 0x18),
    0x0F status (OSF/A2F/A1F, write 0 to clear). Pointer wraps at 0x0F. Time writes are
    applied at the end of the I2C transfer (bulk writes as the driver does them) and restart
    the seconds at .0. INTA/INTB follow INTCN (alarm 1 on INTA, alarm 2 on SQW/INTB).
  - `pcf85263`: the native PCF85263A RTC-mode map (100ths, STOP/CPR set sequence of
    `rtc-pcf85363`, alarms 1/2 with enables, periodic interrupt, Pin_IO INTA/INTB routing,
    ILP pulse/level, flags written 0 to clear, resets CPR/CTS/SR). This reproduces the real
    board including the DS1337 driver misreading it.
- Time: `rtc_clock` plus a guest offset (QEMU RTC conventions: `-rtc base=,clock=`), year
  00..99 = 2000..2099 (+100 with the DS1337 century bit). Battery backed: OSF/OS clear at
  reset, the offset survives system reset.
- Properties: `register-map` (`ds1337`), `int-pulse-us` (15625, PCF ILP=0 pulses).
- GPIOs: out `inta`, `intb` (active low). Not connected on KICK (no IRQ in the DT).
- Wiring: `i2c_slave_create_simple(kick_i2c_bus(s, 3), "pcf85263", 0x68);` (as on
  feature/emu-soc). Add `qdev_prop_set_string(dev, "register-map", "pcf85263")` via
  `i2c_slave_new` to reproduce the hardware mismatch.
- qtest `kick-pcf85263-test`: DS1337 view read at a fixed base date, bulk set, year
  rollover, day counter, 12 h mode, alarm 1 every second on INTA, OSF on oscillator stop;
  native view STOP/CPR set, frozen while stopped, alarm 1 with INTA level output, flag
  clearing, and the DS1337 layout seen on the native map.
- Gaps: DS1337 square wave (INTCN=0) and PCF stopwatch mode, timestamps, watchdog, battery
  switch-over and offset calibration are stored but LOG_UNIMP. Writing an invalid BCD date
  is normalised instead of counting like the silicon.

## CV/Gate converter: AD5671R

- Part identification from `kick_I2c.cpp`: 3 byte frames `[cmd<<4 | ch, MSB, LSB]`, `0x30|ch`
  = "write to and update DAC channel n" with the 12 bit code left aligned, `0x90|ch` = "set up
  readback", 16 byte read = 8 input registers MSB first, auto-incrementing. Eight channels,
  12 bit, address 0b00011A1A0 = 0x0c, 0..5 V (2.5 V reference, gain 2): Analog Devices
  **AD5671R** (12 bit member of the AD5671R/AD5675R octal I2C nanoDAC+ family).
- Bus: I2C4 (0x30a50000, SPI 38), address 0x0c, no DT node (user space `/dev/i2c-3`).
- Commands: 0 NOP, 1 write input (LDAC dependent), 2 update DAC from input, 3 write and
  update, 4 power down (2 bits per channel), 5 LDAC mask, 6 software reset (data 0x1234),
  7 reference setup (DB0 disables), 9 readback. Every command byte points the readback at its
  channel; reads return MSB, LSB and move to the next channel (7 wraps to 0).
- Output: JSON lines on the `chardev` (protocol in ARCHITECTURE.md), on every output change
  and for all channels when the socket connects. Host input lines are discarded (DAC only).
- Properties: `chardev`, `resolution` (12; 16 = AD5675R), `vref-mv` (2500), `vref-ext-mv`
  (0, used when the internal reference is disabled), `gain` (2), `ldac-level` (false: LDAC
  tied low).
- GPIOs: in `ldac`, in `reset` (active low).
- Wiring (as in `kick_board_wire_cv()` on feature/emu-soc): `i2c_slave_new("ad5675r", 0x0c)`,
  `qdev_prop_set_chr(dev, "chardev", chr)`, realize on `kick_i2c_bus(s, 4)`.
- qtest `kick-ad5675r-test`: frames as sent by `setVoltage`/`setVoltageOn`, JSON output and
  volts, `readAllchannelVoltage`/`readVoltage` readback, LDAC held high + update command,
  power down, software reset.
- Gaps: the gate outputs are the same DAC channels driven to full scale by the application;
  output settling time is not modelled.

## PCA9534 GPIO expander

- DT: `nxp,pca9534` on I2C2 (0x30a30000, SPI 36) address 0x20, IRQ GPIO1_IO07 falling;
  gpio-keys Back/Home/Menu on pins 1..3 (active low), hogs on pins 4..7 (output low).
- Registers: 0 Input (read only, pin levels XOR polarity), 1 Output (reset 0xFF),
  2 Polarity (0x00), 3 Configuration (0xFF, 1 = input); the command byte selects the
  register for the whole transfer (no auto-increment). INT (open drain, low) is asserted
  while an input pin differs from its level at the last Input read, cleared by reading
  Input or by the pin returning.
- Inputs: 8 unnamed GPIO inputs (board wiring) and the run time QOM property
  `input-levels` (uint8, `qom-set`) for host buttons; default all high (board pull-ups).
  Outputs: 8 unnamed GPIO outputs with the pin levels, named `int`.
- Wiring:

```c
dev = DEVICE(i2c_slave_create_simple(kick_i2c_bus(s, 2), "pca9534", 0x20));
qdev_connect_gpio_out_named(dev, "int", 0, qdev_get_gpio_in(kick_gpio(s, 1), 7));
```

- Compared with feature/emu-soc (31752b5c / 50886bee, upstream `pca9554`): that model has no
  INT output. `gpio-keys` on pca953x pins is interrupt driven (`gpiod_to_irq` through the
  pca953x irqchip, `CONFIG_GPIO_PCA953X_IRQ=y`), so without INT the Back/Home/Menu keys never
  reach Linux. This model adds INT, polarity inversion, the no-auto-increment command
  semantics and a host input path, with a qtest. The coordinator picks one at merge.
- qtest `kick-pca9534-test`: reset values, outputs and polarity, button press through QOM,
  INT assert/clear by read and by the pin returning, output pins not interrupting.

## SAI (i.MX8MM)

- DT: `fsl,imx8mm-sai` (`fsl,imx8mq-sai` soc data: register offset 8, 128 word FIFOs, 8 data
  lines). SAI2 0x30020000, SPI 96, `fsl,sai-synchronous-rx` (TX synchronous to RX; RX makes
  BCLK/FS), clock master, MCLK1 = SAI2_ROOT 12.288 MHz, DMA SDMA2 events 2 (rx) / 3 (tx).
  Driver modules `snd-soc-fsl-sai`, `snd-soc-simple-card`, `snd-soc-bt-sco`.
- Registers: VERID (0x03010000), PARAM (SPF 32 slots, WPF 128 words, DLN), TCSR/RCSR
  (TE, STOPE, DBGE, BCE, FR, SR, WSF/SEF/FEF W1C, FWF/FRF live, IE bits, FWDE/FRDE),
  xCR1 watermark, xCR2 SYNC/BCS/BCI/MSEL/BCP/BCD/BYP/DIV, xCR3 CFR/CE/WDFL, xCR4
  FCONT/FCOMB/FPACK/FRSZ/SYWD/CHMOD/MF/FSE/ONDEM/FSP/FSD, xCR5 WNW/W0W/FBT, TDR0..7 / RDR0..7,
  TFR/RFR (WFP 23:16, RFP 7:0 with wrap bit), TMR/RMR, MCTL, MDIV, with write masks.
- Data path: the frame engine runs at BCLK / bits per frame of the clock side (MSEL source,
  DIV or BYP), on the virtual clock in `tick-us` batches; masked words do not touch the FIFO;
  underrun/overflow set FEF. Data line 0, words 0/1 go to/come from the audiodev as S32
  stereo (MSB first, right aligned words of W0W/WNW bits). If a FIFO runs dry while its DMA
  request is pending, the batch is resumed right after the DMA bottom half had its turn, so
  host scheduling jitter does not turn into underruns.
- DMA requests: GPIO outs `dma-tx`, `dma-rx` = (FRDE && FRF) || (FWDE && FWF), low during SR.
  IRQ: sysbus 0 = OR of enabled flags of both directions.
- Properties: `audiodev`, `verid`, `datalines` (8), `fifo-words` (128), `max-slots` (32),
  `bus-clk-hz`, `mclk1-hz`, `mclk2-hz`, `mclk3-hz` (0 = no clock; the SoC must set them from
  its clock tree, SAI2 needs `mclk1-hz` = 12288000), `ext-frame-rate` (48000, BCD=0 slave
  mode), `tick-us` (500), `buffer-ms` (100).
- Wiring (SoC, before realizing the optional devices / after for the GPIOs):

```c
DeviceState *sai2 = fsl_imx8mm_get_optional(s, "sai2");
DeviceState *sdma2 = fsl_imx8mm_get_optional(s, "sdma2");

qdev_prop_set_uint32(sai2, "mclk1-hz", 12288000);   /* from the CCM SAI2 root */
/* after realize: RX request -> event 2, TX request -> event 3 */
qdev_connect_gpio_out_named(sai2, "dma-rx", 0,
                            qdev_get_gpio_in_named(sdma2, "events", 2));
qdev_connect_gpio_out_named(sai2, "dma-tx", 0,
                            qdev_get_gpio_in_named(sdma2, "events", 3));
```

  Other SAIs (imx8mm.dtsi): SAI1 SDMA2 events 0/1, SAI3 4/5, SAI5 8/9, SAI6 10/11 (rx/tx).
  The board passes `audiodev` (already done in `kick_board_setup_sai2()`).
- qtest `kick-sai-test`: reset values and write masks, FIFO pointers, FRF/FWF, DMA request
  level, FR, 48 kHz pacing, underrun FEF and IRQ, W1C, TE off, synchronous capture,
  overflow, SR, sync needs the clock side, playback samples verified through `-audiodev wav`.
- Gaps (LOG_UNIMP): FIFO combine and packing, LSB first/odd FBT, timestamp/bit counter
  registers (VERID has no timestamp feature), SEF never set, more than 2 slots to the
  audiodev, data lines other than 0 are shifted but not routed.

## SDMA (i.MX8MM, ROM scripts)

- DT: `fsl,imx8mm-sdma`, `fsl,imx8mq-sdma`. SDMA1 0x30bd0000 SPI 2, SDMA2 0x302c0000 SPI 103,
  SDMA3 0x302b0000 SPI 34. Driver `imx-sdma` (module). The RAM firmware
  `imx/sdma/sdma-imx7d.bin` is not in the image, so the driver uses the ROM script table
  `sdma_script_imx7d` only.
- Approach: no SDMA core instruction set emulation (the ROM code is not public). Channel 0
  implements the ROM boot commands; other channels run the semantics of the ROM script found
  at the context PC:

  | PC | Script | Used by |
  |---|---|---|
  | 644 | ap_2_ap | memcpy (IMX_DMATYPE_MEMORY) |
  | 685 | app_2_mcu | SSI/SAI/EXT/CSPI rx |
  | 749 | mcu_2_app | UART/SSI/SAI tx |
  | 819 | uart_2_mcu | UART rx |
  | 893 | shp_2_mcu | **SAI2 rx** (DT type 2 = MMC), ESAI, shared SSI |
  | 962 | mcu_2_shp | **SAI2 tx** |
  | 1034 | uartsh_2_mcu | shared UART rx |

  Any other PC (RAM scripts such as mcu_2_ecspi or mcu_2_sai, or spdif) aborts the BD with
  BD_RROR and an interrupt and is logged LOG_UNIMP.
- Registers: MC0PTR, INTR (W1C, IRQ while non zero), STOP_STAT (W1 clears HE), HSTART
  (W1 sets HE; channel 0 completes before the write returns), EVTOVR, DSPOVR (reset all
  ones), HOSTOVR, EVTPEND (W1C), DSPENBL, RESET (core reset, resched), EVTERR (clear on read),
  INTRMASK, PSW (current channel/priority), EVTERRDBG, CONFIG (reset 3), LOCK, ONCE_* (stored,
  commands LOG_UNIMP), EVT_MIRROR/EVT_MIRROR2 (event levels), ILLINSTADDR (reset 1),
  CHN0ADDR (reset 0x50, SMSZ selects 32 or 24 word contexts), XTRIG_CONF1/2, CHNPRI0..31,
  CHNENBL0..47.
- Memory: CCB array at MC0PTR (16 bytes per channel: current BD, base BD); BDs 12 bytes
  (EXTD) or 8; internal RAM from DM 0x800 (`ram-size`, PM address = 2 x DM address, even PM
  address = upper half), contexts at DM 0x800 + ch x 32. Channel 0 commands: C0_SETDM (1),
  C0_GETDM (2), C0_SETPM (4), C0_GETPM (8), C0_SETCTX ((ch<<3)|7), C0_GETCTX ((ch<<3)|3).
- Scheduling: eligible = (HE|HOSTOVR) & (EP|EVTOVR) & (DSPENBL|DSPOVR), priority != 0,
  highest priority first, in a bottom half. Events are level inputs: a rising edge sets EP
  for the channels in CHNENBL (EVTERR if EP was set), EP stays set while the request is
  asserted. Peripheral scripts move gReg[7] bytes per request between the BD buffer and the
  FIFO register gReg[6] using the BD command width (0 = 32 bit, 1/2/3 bytes); a burst stops at
  the end of a BD. Completed BDs: DONE cleared, count = received bytes for per-to-memory,
  BD_INTR raises INTR, WRAP returns to the CCB base BD, no CONT ends the channel. A BD still
  owned by the CPU makes the channel wait; it is rechecked every `bd-poll-us` while requested.
- Properties: `dma-mr` (link, bus master view; the SoC sets it), `ram-size` (16 KiB),
  `bd-poll-us` (100).
- GPIOs: in `events` (48). IRQ: sysbus 0.
- Wiring: created by the SoC as optional device `imx8mm-sdma` (names `sdma1..3`), `dma-mr`
  linked to system memory (done in `fsl_imx8mm_realize_optional()`); request lines as in the
  SAI snippet.
- qtest `kick-sdma-test`: reset values; the driver's init sequence; C0_SETPM/GETPM/GETDM and
  context load/read back; ap_2_ap memcpy over two chained BDs with INTR, IRQ and W1C;
  mcu_2_shp feeding SAI2 through event 3 (bursts of 6 words until above the watermark,
  period interrupts, steady state at 48 kHz without underrun, waiting at a BD the CPU has not
  returned and resuming after it is re-armed); unknown script PC; event overrun and EVTERR.
- Gaps: no ISA emulation, so RAM scripts do not run (not needed without the firmware); script
  state is not saved in the context, so a context restore restarts the script; OnCE, DSP side
  and cross triggers are not modelled; uart_2_mcu/uartsh_2_mcu (read URXD while CHARRDY,
  close the BD early when fewer characters arrive, BD_RROR on URXD.ERR) are implemented but
  untested against a UART (see below).

## Open issues for the SoC/board

- UART DMA: UART1 (SDMA1 events 22/23) and UART3 (26/27) have `dmas` in the DT and are not
  consoles, so `imx.c` enables DMA for them (MCU link and MIDI). QEMU's `imx_serial` has no
  DMA request outputs. Either the UART model gets RX/TX request outputs (RX: data ready or
  aging, TX: FIFO below TXTL) wired to SDMA1, or receive on those ports may stall once the
  driver switches to DMA. This needs a check with the real kernel.
- SAI clocks: the SAI needs `mclk1-hz` (and `bus-clk-hz` if MSEL 0 is used) from the clock
  tree; without it TE logs "no usable bit clock" and the frame engine does not run.
- Rebase the qtests onto the `kick` machine once it is merged, replacing `kick-periph-test`.
