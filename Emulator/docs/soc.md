# i.MX 8M Mini SoC and `kick` board models

The SoC (`fsl-imx8mm`, `hw/kick/fsl-imx8mm.c`) and the board (`kick`, `hw/kick/kick-board.c`)
of the KICK emulator. Sources for every value: the i.MX 8M Mini Reference Manual (IMX8MMRM),
`imx8mm.dtsi` / `imx8mm-var-som.dtsi` / `imx8mm-var-som-symphony.dts` of varigit `linux-imx`
(the pinned commit, with the KICK patches), and the drivers that program the blocks: U-Boot
(`arch/arm/mach-imx/imx8m`, `drivers/ddr/imx/imx8m`), imx-atf (`plat/imx/imx8m`) and Linux.
The RM PDF was not available while writing the models; where a reset value comes from
another source, the source file says so.

## Running

```sh
# kernel, HVF (daily use)
qemu-system-aarch64 -M kick,emmc=emmc -m 2G -accel hvf -cpu host \
    -kernel Image -dtb imx8mm-var-som-symphony.dtb [-initrd ...] -append "console=ttymxc3,115200" \
    -drive if=none,id=emmc,format=raw,file=emmc.img \
    -serial <uart1> -serial <uart2> -serial <uart3> -serial <uart4>

# U-Boot proper (u-boot.bin = u-boot-nodtb.bin + u-boot.dtb) at 0x40200000
qemu-system-aarch64 -M kick,boot-mode=uboot ... -bios u-boot.bin
#   or, as the host application does: -device loader,file=u-boot.bin,addr=0x40200000,cpu-num=0

# full chain from the BootROM (TCG only)
qemu-system-aarch64 -M kick,boot-mode=rom,emmc=emmc -accel tcg -cpu cortex-a53 ...
```

### Machine properties

| Property | Default | Meaning |
|---|---|---|
| `boot-mode` | `kernel` | `kernel` (-kernel/-dtb, QEMU PSCI over SMC), `uboot` (-bios or a loader device), `rom` (BootROM emulation, TCG). Named `boot-mode` because `boot` is QEMU's generic boot-order option. |
| `emmc`, `sd` | none | drive ids (`-drive if=none,id=...`) for the eMMC on uSDHC3 and the SD slot on uSDHC2 |
| `emmc-boot-partition-size` | 0 | eMMC boot partition size; the image then starts with boot0, boot1 |
| `rom-boot-device` | `emmc` | `boot-mode=rom`: `emmc` (MMC3) or `sd` (SD2) |
| `uboot-addr` | 0x40200000 | `boot-mode=uboot`: load and entry address for `-bios` |
| `soc-uid` | 0 | 64-bit unique ID fused into OCOTP (`/sys/devices/soc0/serial_number`) |
| `cv-chardev` | none | chardev for the CV/gate converter host protocol |
| `audiodev` | none | audiodev for SAI2 (generic machine property) |
| `panel-width`, `panel-height` | 1024, 600 | LCDIF scan-out size before the guest sets a mode |
| `power-key-press-ms` | 200 | how long `system_powerdown` holds the SNVS ON/OFF button |
| `gt911-reset-wired` | off | connect GPIO5_IO03 to the GT911 RSTB (see "Board") |
| `sdma-firmware` | none | the guest's `sdma-imx7d.bin`; its header gives the exact RAM script entry points |
| `sdma-infer-scripts` | on | derive the script at an unknown SDMA entry point (no firmware, or a firmware the host does not have) from the channel's DMA events and FIFO address |

Exception levels: `boot-mode=kernel/uboot` start the payload at EL2 under TCG (as ATF does on
the KICK) and at EL1 under HVF; `boot-mode=rom` starts SPL at EL3.

## Memory map

| Address | Size | Block | Model |
|---|---|---|---|
| 0x0000_0000 | 256 KiB | Boot ROM (incl. protected 4 KiB at 0x3f000) | ROM region, filled by the BootROM emulation |
| 0x0010_0000 | 32 KiB | CAAM secure RAM | RAM |
| 0x0018_0000 | 32 KiB | OCRAM_S | RAM |
| 0x007e_0000 | 128 KiB | M4 ITCM | RAM (SPL runs from 0x7e1000) |
| 0x0080_0000 | 128 KiB | M4 DTCM | RAM |
| 0x0090_0000 | 256 KiB | OCRAM | RAM (BL31 at 0x920000) |
| 0x0800_0000 | 256 MiB | FlexSPI memory | unimplemented |
| 0x1800_0000 | 128 MiB | PCIe memory | unimplemented |
| 0x2800_0000 | 16 MiB | A53 DAP | unimplemented |
| 0x3001_0000 | 64 KiB each | SAI1, SAI2, SAI3, SAI5 (0x30050000), SAI6 (0x30060000) | `imx8mm-sai` when built in |
| 0x3008_0000 / 0x3009_0000 | | MICFIL / SPDIF1 | unimplemented |
| 0x3020_0000 | 5 x 64 KiB | GPIO1..GPIO5 | `imx.gpio` (upstream) |
| 0x3026_0000 | 64 KiB | TMU | `imx8mm.tmu` |
| 0x3027_0000 | 64 KiB | ANA_OSC | unimplemented |
| 0x3028_0000 | 3 x 64 KiB | WDOG1..WDOG3 | `imx2.wdt` (upstream) |
| 0x302b_0000 / 0x302c_0000 | | SDMA3 / SDMA2 | `imx8mm-sdma` when built in |
| 0x302d_0000 | 3 x 64 KiB | GPT1..GPT3 | `imx7.gpt` (upstream) + per-GPT clock view |
| 0x3031_0000 | | ROMCP | unimplemented |
| 0x3033_0000 | 64 KiB | IOMUXC | `imx8mm.iomuxc` |
| 0x3034_0000 | 64 KiB | IOMUXC_GPR | `imx8mm.iomuxc-gpr` |
| 0x3035_0000 | 64 KiB | OCOTP_CTRL | `imx8mm.ocotp` |
| 0x3036_0000 | 64 KiB | CCM_ANALOG (ANATOP) | `imx8mm.anatop` |
| 0x3037_0000 | 64 KiB | SNVS_HP/LP | `imx8mm.snvs` |
| 0x3038_0000 | 64 KiB | CCM | `imx8mm.ccm` |
| 0x3039_0000 | 64 KiB | SRC | `imx8mm.src` |
| 0x303a_0000 | 64 KiB | GPC | `imx8mm.gpc` |
| 0x303b_0000 .. 0x303e_0000 | | SEMA1, SEMA2, RDC, CSU | unimplemented |
| 0x3066_0000 | 4 x 64 KiB | PWM1..PWM4 | unimplemented |
| 0x306a_0000 | 3 x 64 KiB | System counter read / compare / control frames | `imx8mm.sysctr` |
| 0x306e_0000 / 0x306f_0000 / 0x3070_0000 | | GPT6 / GPT5 / GPT4 | `imx7.gpt` |
| 0x3082_0000 | 3 x 64 KiB | ECSPI1..ECSPI3 | `imx.spi` (upstream) |
| 0x3086_0000 / 0x3089_0000 / 0x3088_0000 / 0x30a6_0000 | | UART1 / UART2 / UART3 / UART4 | `imx8mm-uart` |
| 0x3090_0000 | 256 KiB | CAAM | unimplemented |
| 0x30a2_0000 | 4 x 64 KiB | I2C1..I2C4 | `imx8mm-i2c` (upstream imx_i2c + NACK completion) |
| 0x30aa_0000 / 0x30ab_0000 | | MU A / B | unimplemented |
| 0x30b4_0000 | 3 x 64 KiB | uSDHC1..uSDHC3 | `imx8mm-usdhc` + `imx8mm.usdhc-cqhci` at +0x100 |
| 0x30bb_0000 | | FlexSPI | unimplemented |
| 0x30bd_0000 | | SDMA1 | `imx8mm-sdma` when built in |
| 0x30be_0000 | 64 KiB | ENET1 | `imx.enet` (upstream), MDIO answered by `ar8033-phy` |
| 0x3270_0000 | 1 MiB | NOC | unimplemented |
| 0x32e0_0000 | 64 KiB | LCDIF | `imx8mm-lcdif` |
| 0x32e1_0000 | 64 KiB | MIPI DSIM | `imx8mm-mipi-dsim` |
| 0x32e2_0000 / 0x32e3_0000 | | CSI1 / MIPI CSI | unimplemented |
| 0x32e2_8000 | 32 KiB | Display block control | `imx8mm-dispmix` |
| 0x32e4_0000 / 0x32e5_0000 | | USB OTG1 / OTG2 | unimplemented |
| 0x32f0_0000 / 0x32f8_0000 | | PCIe PHY / TZASC | unimplemented |
| 0x3300_0000 / 0x3300_2000 | | APBH DMA / GPMI | unimplemented |
| 0x3380_0000 | 4 MiB | PCIe | unimplemented |
| 0x3800_0000 / 0x3800_8000 | | GPU 3D / GPU 2D | `imx8mm-gpu3d` / `imx8mm-gpu2d` |
| 0x3830_0000 .. 0x3833_0000 | | VPU G1, G2, H1, VPU block control | unimplemented |
| 0x3880_0000 | 64 KiB | GIC-500 distributor | `arm-gicv3` |
| 0x3888_0000 | 512 KiB | GIC redistributors (4 x 128 KiB) | `arm-gicv3` |
| 0x3c00_0000 | 16 MiB | DDR PHY (APB) | `imx8mm.ddrc` MMIO 1 |
| 0x3d00_0000 | 1 MiB | DDR block control | unimplemented |
| 0x3d40_0000 | 4 MiB | DDR controller (uMCTL2) | `imx8mm.ddrc` MMIO 0 |
| 0x3d80_0000 | 4 MiB | DDR perf monitor | unimplemented |
| 0x4000_0000 | up to 4 GiB | DDR | RAM, size from `-m` (default 2 GiB) |

Every peripheral window also has an `unimplemented-device` at the lowest priority: accesses
to blocks without a model, and to parts of a slot a model does not decode, are logged with
`-d unimp` and read as zero instead of aborting.

## Interrupts (GIC SPI numbers; INTID = SPI + 32)

| SPI | Source | | SPI | Source |
|---|---|---|---|---|
| 2 | SDMA1 | | 47 | System counter compare 0 (48: compare 1) |
| 3 | GPU 3D | | 50 | SAI3 |
| 4 | SNVS ON/OFF button | | 51 | GPT5 / GPT6 (OR) |
| 5 | LCDIF | | 52..55 | GPT4, GPT3, GPT2, GPT1 |
| 10 | WDOG3 | | 64/65 .. 72/73 | GPIO1..GPIO5 (pins 0-15 / 16-31) |
| 18 | MIPI DSIM | | 78, 79 | WDOG1, WDOG2 |
| 19 | SNVS RTC (alarm) | | 87 | GPC |
| 22..24 | uSDHC1..uSDHC3 (+ their CQHCI) | | 89 | SRC |
| 25 | GPU 2D | | 90 | SAI5 / SAI6 (OR) |
| 26..29 | UART1..UART4 | | 95, 96 | SAI1, SAI2 |
| 31..33 | ECSPI1..ECSPI3 | | 103 | SDMA2 |
| 34 | SDMA3 | | 120 | ENET1 MAC (queue 0 and other events) |
| 35..38 | I2C1..I2C4 | | 121 | ENET1 IEEE 1588 timer |

PPIs: generic timer secure 13, non-secure 14, virtual 11, hypervisor 10; GIC maintenance 9;
PMU 7 (`imx8mm.dtsi`). Not wired: TMU interrupt (not in the DT), GPIO1 per-pin INT0..7
(SPI 56..63, the upstream GPIO model has no per-pin outputs), SNVS security violation (20).

DMA requests: UART1/3/4 to SDMA1 events 22/23, 26/27, 28/29 (RX/TX); SAI1/2/3/5/6 to SDMA2
events 0/1, 2/3, 4/5, 8/9, 10/11.

## What is modelled

### New register models (`hw/kick`)

| Block | Coverage | qtest |
|---|---|---|
| **CCM** `imx8mm.ccm` | GPR0, PLL_CTRL0..38, CCGR0..511, all clock root slices with TARGET/POST/PRE/ACCESS; SET/CLR/TOG aliases everywhere; clock root frequencies from the 8MM root table (generated from the slice list, `tools/gen-imx8mm-clock-roots.py`) and the ANATOP PLLs; per-GPT clock views so GPTn counts at GPTn_CLK_ROOT | `kick-clock-test`: reset values, aliases, GPT1 counting at a programmed root |
| **CCM_ANALOG** `imx8mm.anatop` | all 10 PLLs (1443X fractional, 1416X integer): GNRL_CTL, DIV/FDIV, SSCG, MNIT; LOCK follows RST at once, read-only; bypass and output gates in the rate; DIGPROG 0x824110 (property) | reset, lock |
| **SRC** `imx8mm.src` | all reset control registers, SRSR (W1C, POR), SBMR1/2 read-only from properties, SISR/SIMR + IRQ, GPR1..10, DDRC resets; A53 secondary core release through A53RCR1 with the entry in GPR (ATF PSCI path) | `kick-system-test` |
| **GPC** `imx8mm.gpc` | full register file; CPU and PU power-up/down triggers self-clear, PGC status reflects the domain power state (PCR gates power-down), ADB400 handshake (ACK = SYNC << 18) | power domain up/down, handshake |
| **IOMUXC / GPR** | register files 0x014..0x540 / GPR0..22 | read back |
| **OCOTP** `imx8mm.ocotp` | CTRL with SET/CLR/TOG, BUSY/ERROR, RELOAD_SHADOWS; TIMING, DATA (program with the 0x3e77 key, fuses only 0->1, ERROR without key), READ_CTRL/READ_FUSE_DATA, SW_STICKY, SCS; 256 shadow words. Fuses from properties: unique ID (TESTER0/1), TESTER3/4, BOOT_CFG0..3, MAC0 (set to the NIC MAC by the board), raw `fuse-override` pairs; survive guest resets | SoC ID, MAC, programming, reload, reset |
| **TMU** `imx8mm.tmu` | TER, TIER, TIDR (W1C), the three thresholds, TRITSR/TRATSR from the `temperature` property (run-time settable, default 45 C), calibration space as storage | enable, qom-set temperature, threshold |
| **SNVS** `imx8mm.snvs` | HP and LP real time counters (47-bit, 32.768 kHz, run/stop, settable), LPTAR alarm with timer and SPI 19, LPSR W1C, ON/OFF button (GPIO input `power-key` and `button-pressed` property; HPSR.BTN, LPSR.SPO, SPI 4), power off with LPCR DP_EN+TOP, LPGPR | counter, button, alarm |
| **System counter** `imx8mm.sysctr` | read, compare (2 comparators with timer-driven ISTAT and IRQ) and control frames (EN, HDBG, FCREQ, CNTCV writable, CNTFID0/1) | count rate, compare IRQ, IMASK, stop |
| **uSDHC CQHCI** | the CQHCI 5.10 register block at +0x100 (idle engine: halt, status W1C, read-only IDs) | `kick-system-test` |
| **DDR controller + PHY** `imx8mm.ddrc` | uMCTL2 register file with STAT/operating mode, DFIMISC/DFISTAT, SWCTL/SWSTAT, MRSTAT, DBGCAM, scrubber; PHY APB space as storage (firmware images read back), PMU mailbox, training start reports "pass" | via `boot-mode=rom` |
| **BD71847 PMIC** `bd71847` | full 8-bit register file, auto-increment, REGLOCK (VREG/PWRSEQ), SWRESET, IRQ/MIRQ with active-low INTB on GPIO2_IO08, read-only status | `kick-board-test` |
| **AR8033 PHY** `ar8033-phy` | MDIO clause 22 at address 4 (ID 0x004d/0xd074), link from the NIC, 1000/full resolution, specific status, interrupt status, MMD and debug port | ID, link, debug port, reset |
| **UART** `imx8mm-uart` | 32 entry TX/RX FIFOs with line timing from the UARTn clock root, RFDIV, UBIR/UBMR and the frame format; TRDY/RRDY at TXTL/RXTL, TXFE/TXDC/RDR, UTS flags, OVRRUN on entry 32; ageing timer (AGTIM after 8 idle frames below RXTL) and idle detect (UCR1.ICD); all USR1/USR2 interrupt sources with their UCR1..4 enables; SDMA requests RXDMAEN&RRDY, ATDMAEN&AGTIM, IDDMAEN&IDLE, TXDMAEN&TRDY; SRST resets only what the RM lists; UTS.LOOP; non-blocking backend with flow control (nothing is lost). Characters are scheduled by due time so a late host timer catches up (`catch-up-us`) | `kick-uart-test` |
| **SDMA** `imx8mm-sdma` | channel 0 boot commands, contexts, ROM script semantics (ap_2_ap, app/shp_2_mcu, mcu_2_app/shp, uart(sh)_2_mcu with ageing/idle close), RAM entry points from the firmware header, script inference for unknown entry points | `kick-sdma-test`, `kick-uart-test` |
| **BootROM** (`kick-bootrom.c`) | IVT at 33 KiB, boot data, image placed in TCM/OCRAM (as a ROM blob, reloaded on reset), ROM boot info at 0x9e8 for SPL, CPU 0 entry in EL3 | `boot-mode=rom` |

### Upstream models used, and the 8MM differences fixed in wrappers

| Block | Upstream model | 8MM difference handled |
|---|---|---|
| UART1..4 | not `imx.serial` | replaced by `imx8mm-uart` (the upstream one has no TX FIFO, no line timing and blocks on the backend, which breaks SDMA mode) |
| I2C1..4 | `imx.i2c` | `imx8mm-i2c`: I2SR.IIF is set at the end of a NACKed byte, as on hardware (upstream leaves it clear, so absent devices time out instead of failing with -ENXIO) |
| uSDHC1..3 | `imx-usdhc` | `imx8mm-usdhc`: SYS_CTRL[3:0] read as 1 (U-Boot does not set them, the generic core then refuses commands); INT_STATUS_EN resets to 0x117f013f; transfer-complete after R1b busy (eMMC CMD6 under Linux); no SDMA buffer boundary pauses (BLK_ATT has no boundary field) |
| GPT1..6 | `imx7.gpt` | per-instance CCM clock view (`imx8mm.gpt-clock`) |
| GPIO1..5 | `imx.gpio` | as is (edge select, high/low combined IRQs) |
| WDOG1..3 | `imx2.wdt` | as is, pretimeout enabled |
| ECSPI1..3 | `imx.spi` | as is |
| ENET1 | `imx.enet` | built-in LAN9118-style PHY disabled; `ar8033-phy` answers MDIO through an MMFR window and lets the MAC complete the frame |
| SNVS | not `imx7.snvs` | replaced by `imx8mm.snvs` (the upstream one has no button, alarm or status) |

### Board (`kick`)

- RAM at 0x4000_0000, `-m` (default 2 GiB, max 4 GiB).
- `serial_hd(0..3)` -> UART1..UART4.
- eMMC (`emmc` device, `emmc` property) on uSDHC3, SD card on uSDHC2.
- I2C1: BD71847 PMIC 0x4b (INTB -> GPIO2_IO08), SN65DSI84 bridge 0x2c (EN <- GPIO2_IO11,
  DSI host = DSIM). I2C2: PCA9534 0x20 (INT -> GPIO1_IO07). I2C3: GT911 0x14 (INT ->
  GPIO5_IO04, INT sense <- GPIO5_IO04), RTC 0x68 (`pcf85263`, DS1337 view). I2C4: CV/gate
  converter 0x0c (`ad5675r`, `cv-chardev`). The I2C pads (GPIO5_IO14..21) read high as
  GPIO after every reset (SOM pull-ups; SPL bus recovery checks them).
- GT911 RSTB: the DT declares `reset-gpios = <&gpio5 3 GPIO_ACTIVE_LOW>`, so the patched
  goodix driver ends `goodix_reset()` with GPIO5_IO03 low. A controller whose RSTB is on that
  pin would stay in reset, yet touch works on the device, so the line is not connected by
  default (`gt911-reset-wired=on` connects it).
- ENET1 PHY: AR8033 at MDIO address 4; the OCOTP MAC fuses carry the NIC's MAC.
- SNVS ON/OFF button: `system_powerdown` presses it for `power-key-press-ms`; also
  `qom-set /machine/soc/snvs button-pressed true|false`.
- SAI2: `audiodev`, MCLK1 12.288 MHz (`sai-mclk-hz` SoC property).
- LCDIF panel size `panel-width/height`.

## Boot modes

- **kernel**: QEMU loads Image/DTB/initrd, provides PSCI 1.1 over SMC, removes
  `cpu-idle-states` from the DTB (QEMU's CPU_SUSPEND only waits) and, under HVF, the fixed
  8 MHz `clock-frequency` of the arch timer (the host counter runs at 24 MHz). The system
  counter starts enabled (ATF would enable it).
- **uboot**: U-Boot proper (`u-boot-nodtb.bin` + `u-boot.dtb`) from `-bios` at `uboot-addr`,
  or placed by `-device loader,...,cpu-num=0`; PSCI from QEMU. Works under TCG and HVF; it
  prints on UART1 (the U-Boot console setting of the built DART config).
- **rom** (TCG): the BootROM emulation reads `imx-boot` at 33 KiB from the eMMC (or SD),
  places SPL at 0x7e0bc0..., records MMC3 as the boot device and starts it at EL3. Verified:
  SPL runs (console on UART4), configures clocks and the PMIC, completes the LPDDR4 init and
  three training runs against the DDRC/PHY model, initialises the eMMC and loads the FIT
  (U-Boot + BL31) from sector 0x300, BL31 runs and returns to U-Boot proper at EL2
  (0x40200000). **Not working yet:** U-Boot proper produces no output in this mode and the
  CPU is later found back in SPL code (a reset loop after the hand-over); not yet analysed.

## HVF

`-accel hvf -cpu host` works with the SoC: userspace GICv3 (ICC system registers trapped),
the host generic timer at 24 MHz (the DT frequency is removed), PSCI over SMC (CPU_ON for all
four cores), all MMIO models. The vendor kernel boots to the test image shell in about 1.5 s
and the bare-metal demo runs with GIC/GPT interrupts, LCDIF scan-out and the MCU link.
Limits: EL1 only (no EL2/EL3), so `boot-mode=rom` needs TCG; no PMU (`hw perfevents: failed
to probe PMU`); the system counter (SYS_CTR) model runs at 8 MHz on QEMU's virtual clock while
the CPU counter is the host's, so they do not match under HVF.

Patch `0003-hvf-align-dirty-log-protection-to-host-pages.patch`: HVF write-protects dirty
logged memory in host pages (16 KiB), and QEMU asserted when a logged section (an LCDIF
framebuffer, e.g. 800x480x4 bytes) did not cover whole host pages. The patch rounds the range
to host pages; the fault handler already works per host page.

## Known gaps

- Clock tree: rates are computed for roots and PLLs; reset values of roots and gates are
  "enabled on input 0" (the RM table was not available); POST_ROOT/PRE_ROOT are plain storage.
- IOMUXC: no pin routing; reset values 0.
- GPC: domain power state is tracked, but powering a domain off does not reset or isolate the
  blocks in it.
- NXP SiP services (`IMX_SIP_*`) are not provided by QEMU's PSCI: power domain calls return
  "not supported" (the 5.4 driver ignores it), `imx_busfreq` fails to probe with -22. With
  `boot-mode=rom` the real ATF provides them.
- APBH DMA (mxs-dma probe fails with a reset timeout), CAAM (job rings fail), PWM, USB,
  PCIe, VPU, CSI, MU, FlexSPI, ANA_OSC, DDR block control: unimplemented-device only.
- TMU interrupt and GPIO1 per-pin interrupts not wired; SNVS long-press power off and
  security monitor not modelled; HP RTC alarm not modelled.
- uSDHC: eMMC EXT_CSD reads through `mmc extcsd read` (MMC_IOC_CMD) fail in kick-selftest;
  CQHCI task execution not implemented (QEMU's eMMC does not advertise CMDQ).
- DDR: training is not simulated (always passes); the scrubber does not write the pattern.
- BootROM: no DCD, HAB, plugin, USB serial download, FlexSPI/NAND, or ROM API; frequencies in
  the ROM boot info are 0.
- U-Boot proper (DART config) finds no SOM EEPROM (I2C1 0x52 is not modelled) and reports
  512 MiB DRAM; it expects its PHY at MDIO address 0.
- Audio capture: `kick-selftest` hangs in `arecord` (SAI2 RX through SDMA2 event 2); the SAI
  RX FIFO overflows from boot on (`Receive overflow detected`). To be looked at with the audio
  models.

## Tests

`meson test -C Emulator/out/qemu-build --suite qtest-aarch64 'qtest-aarch64/kick-*'`; the SoC
work adds `kick-clock-test`, `kick-ocotp-test`, `kick-system-test`, `kick-board-test` and
`kick-uart-test` (UART registers and timing, UART DMA through SDMA1 including inferred
scripts).
