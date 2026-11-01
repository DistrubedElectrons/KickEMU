# KICK board device tree: what the emulator must provide

Checklist for the SoC and device model work. Source: `imx8mm-var-som-symphony.dtb` built by
`Emulator/boot/build.sh` from varigit `linux-imx` 5.4.127 (commit pinned in
`KTKick_defconfig`) with the board patches in `Linux/buildroot/board/freescale/imx8mm_var_som/patches/linux/`.
The KICK boots this DTB unchanged (U-Boot only fixes up memory and the command line).

The node table at the end is generated; regenerate it after any kernel or patch change:

```sh
Emulator/boot/build.sh dtdoc
# writes <out>/board-dt-table.md and the decompiled <out>/imx8mm-var-som-symphony.dts
```

and replace everything below the "Generated node table" heading with the new file.
The other three DTBs built from the defconfig (`-legacy`, `-m4`, `-legacy-m4`) differ in
the SOM revision and the Cortex-M4 reservations; the KICK uses `imx8mm-var-som-symphony.dtb`.

## Boot essentials

| Item | Value | Emulator requirement |
|---|---|---|
| CPUs | 4x `arm,cortex-a53`, `enable-method = "psci"` | MPIDR 0..3 |
| PSCI | `arm,psci-1.0`, `method = "smc"` | `boot=kernel`/`uboot`: QEMU PSCI with SMC conduit; `boot=rom`: ATF BL31 at 0x920000 provides it |
| Memory | `/memory@40000000`: 0x40000000 + 2 GiB | default `memory_mb` 2048; U-Boot rewrites this from the DDR size |
| CMA | 320 MiB from the kernel config (`CONFIG_CMA_SIZE_MBYTES`), placed at the top of RAM | RAM must be contiguous from 0x40000000 |
| GIC | `arm,gic-v3`: distributor 0x38800000 (64 KiB), redistributors 0x38880000 (0xc0000, stride 0x20000), maintenance PPI 9 | 160 SPIs (`SPI n` = INTID n+32) |
| Arch timer | PPIs 13, 14, 11, 10 (secure, non-secure, virtual, hypervisor), active low | counter frequency 8 MHz (24 MHz / 3, as on silicon) |
| System counter | `nxp,sysctr-timer` at 0x306a0000, SPI 47 | broadcast timer used in idle; must count and interrupt |
| Console | `stdout-path` = UART4 (0x30a60000), `ttymxc3` | `earlycon=ec_imx6q,0x30a60000,115200` works as soon as the UART model exists |
| CPU idle | `cpu-pd-wait` idle state (PSCI suspend) | PSCI CPU_SUSPEND must return |

## Aliases that fix Linux device numbering

| Alias | Controller | Linux name | KICK use |
|---|---|---|---|
| serial0 | UART1 0x30860000, SPI 26 | ttymxc0 | MCU (230400 8N1) |
| serial1 | UART2 0x30890000, SPI 27 | ttymxc1 | unused |
| serial2 | UART3 0x30880000, SPI 28 | ttymxc2 | DIN MIDI |
| serial3 | UART4 0x30a60000, SPI 29 | ttymxc3 | console |
| i2c0 | I2C1 0x30a20000, SPI 35 | i2c-0 | PMIC, DSI bridge |
| i2c1 | I2C2 0x30a30000, SPI 36 | i2c-1 | PCA9534 expander, OV5640 camera |
| i2c2 | I2C3 0x30a40000, SPI 37 | i2c-2 | GT911 touch, RTC |
| i2c3 | I2C4 0x30a50000, SPI 38 | i2c-3 | CV/gate converter (no DT node, user space) |
| mmc0/1/2 | uSDHC1/2/3 | see below | |
| ethernet0 | FEC 0x30be0000 | eth0 | LINK jack |

UART1, UART3 and UART4 use SDMA1 (0x30bd0000) channels for RX/TX (`dmas`); the imx
serial driver only uses DMA when the port is opened with suitable settings, PIO must work.
UART2 has `uart-has-rtscts` in the DT.

## Board devices

### I2C1 (i2c-0, 0x30a20000)
- `pmic@4b`: `rohm,bd71847`, IRQ GPIO2_IO08 active low. I2C driver `rohm-bd718x7` (MFD)
  with the cells `bd718xx-pmic` (regulators BUCK1..6, LDO1..6), `bd718xx-clk` and a
  `gpio-keys` power button `bd718xx-pwrkey` fed by the PMIC interrupt. SPL (via U-Boot's PMIC code) and the kernel both access it.
- `sn65dsi84@2c`: `ti,sn65dsi83` DSI-to-LVDS bridge, `enable-gpios` GPIO2_IO11. Driver
  `CONFIG_DRM_I2C_SN65DSI83=y`. It carries the panel timings (see Display).

### I2C2 (i2c-1, 0x30a30000)
- `gpio@20`: `nxp,pca9534` GPIO expander, IRQ GPIO1_IO07 falling. Hogs: pins 4..7
  driven low (usb3_sel, enet_rst, som_vsel, enet_sel). `gpio-keys` Back/Home/Menu use
  pins 1..3 (active low).
- `ov5640_mipi1@3c`: `ovti,ov5640_mipi` camera (phase 3).

### I2C3 (i2c-2, 0x30a40000)
- `touchscreen@14`: `goodix,gt911`, IRQ GPIO5_IO04 falling (`irq-gpios` GPIO5_IO04,
  `reset-gpios` GPIO5_IO03 active low), `touchscreen-size` 800x480. The goodix driver is
  patched (`003.goodix.c.patch`); the model must follow the reset and INT handling of
  the patched driver.
- `rtc@68`: **`dallas,ds1337`**, driver `rtc-ds1307`. The symphony DTS overrides the
  `nxp,pcf85263` node added by `001.imx8mm_dtsi.patch` (the DTS is applied after the
  DTSI), and `CONFIG_RTC_DRV_PCF85363` is not enabled. The KICK hardware has a PCF85263,
  so on the device Linux speaks the DS1337 register layout to a PCF85263. A
  register-accurate PCF85263 model will show the same behaviour as the hardware.

### I2C4 (i2c-3, 0x30a50000)
- No child nodes. The CV/gate converter at 0x0c is opened from user space
  (`/dev/i2c-3`, `I2C_SLAVE` 0x0c): 3-byte writes `0x30|ch, v>>4, (v<<4)&0xf0` set the
  12-bit DAC, a 3-byte write `0x90|ch, 0, 0` selects a channel and a 2-byte read returns
  the ADC value (big endian, 12-bit, 0..5 V).

### SD/eMMC
| Node | Width | Notes | Linux |
|---|---|---|---|
| uSDHC1 0x30b40000, SPI 22 | 4 | `non-removable`, SDIO `brcm,bcm4329-fmac` Wi-Fi child | mmc0 (SDIO, no card on the KICK expected) |
| uSDHC2 0x30b50000, SPI 23 | 4 | SD slot, `cd-gpios` GPIO1_IO10 active low, vmmc `regulator-usdhc2` (GPIO4_IO22) | mmc1 -> `/dev/mmcblk1` |
| uSDHC3 0x30b60000, SPI 24 | 8 | eMMC, `non-removable` (no HS200/HS400 properties in the DT) | mmc2 -> `/dev/mmcblk2` |

The 5.4 MMC core ignores the `mmcN` aliases; host indices are handed out in probe order,
which is DT order unless a probe defers. All three controllers must be present for the
`mmcblk1`/`mmcblk2` names the KICK root= and fstab rely on.
The esdhc driver enables **CQHCI** on uSDHC3 (i.MX8MM `ESDHC_FLAG_CQHCI`): the CQHCI
register block at base + 0x100 must exist. On stock QEMU (`-M imx8mp-evk`) the kernel dies
with a synchronous external abort in `cqhci_init` reading it (see "Stock QEMU check").

### Ethernet
- FEC 0x30be0000, SPIs 118/119/120, `phy-mode = "rgmii"`, `phy-reset-gpios` GPIO1_IO09
  active low, PHY supply `regulator-eth-phy` (GPIO2_IO09).
- MDIO PHY at address 4, `ethernet-phy-ieee802.3-c22` with `at803x,*` properties; the
  kernel uses the AT803x driver (`CONFIG_AT803X_PHY=y`) when the PHY ID matches
  (AR8031/AR8033), otherwise the generic PHY driver.

### Audio
- SAI2 0x30020000, SPI 96: `fsl,sai-synchronous-rx`, `fsl,sai-mclk-direction-output`,
  MCLK1 = `IMX8MM_CLK_SAI2_ROOT` from AUDIO_PLL1 at 12.288 MHz. SAI2 is bit clock and
  frame master. DMA: SDMA2 (0x302c0000, SPI 103) events 2 (RX) and 3 (TX).
- `sound-v4220m`: `simple-audio-card`, I2S, mclk-fs 256, CPU DAI SAI2, codec `codec1`
  (`linux,bt-sco`; `002.fsl_sai_codec.patch` widens it to 2 channels, 32..192 kHz,
  S16/S20_3/S24/S32_LE, and patches `fsl_sai.c`, `simple-card.c`, `soc-core.c`).
- `leds` (`gpio-leds`): `codecreset` GPIO4_IO24 and `codecmute` GPIO4_IO25, default on.
- Modules: `snd-soc-fsl-sai`, `snd-soc-simple-card`, `snd-soc-bt-sco`, `imx-sdma` are `=m`
  and are loaded by modalias.
- SDMA firmware (`imx/sdma/sdma-imx7d.bin`) is part of NXP firmware-imx and not in the
  test image; the driver then runs with the SDMA ROM scripts only. The SDMA model must
  support the ROM script entry points used for SAI (mcu_2_app / app_2_mcu style).

### Display
- Pipeline (graph links): LCDIF 0x32e00000 (SPI 5, `fsl,imx8mm-lcdif`, bound by
  `imx-lcdif-crtc` under `imx-drm`) -> MIPI DSIM 0x32e10000 (SPI 18,
  `fsl,imx8mm-mipi-dsim`, `CONFIG_DRM_IMX_SEC_DSIM`) -> SN65DSI84 on I2C1 0x2c -> LVDS.
- Mode: the bridge's `display-timings` give **1024x600**, 33 MHz pixel clock (patched in
  `001.imx8mm_dtsi.patch`). The touch controller is configured for 800x480.
- DSIM needs the display block control (`display-gpr@32e28000`, `syscon`) and the
  `dispmix-reset` nodes (soft reset / clock enable / MIPI reset registers at 0x32e28000,
  0x32e28004, 0x32e28008). Power domain `dispmix-pd` and `mipi-pd` go through the GPC via
  ATF SiP calls (`fsl,imx8m-pm-domain`), so BL31 or the PSCI/SiP emulation must accept them.
- Backlight: `pwm-backlight` on PWM1 (0x30660000, SPI 81), period 1 ms, default level 80/100.
- fbdev: `CONFIG_DRM_FBDEV_EMULATION=y` gives `/dev/fb0` on top of DRM.

### GPU
- `gpu@38000000`: `fsl,imx8mm-gpu`, 3D at 0x38000000 (SPI 3), 2D at 0x38008000 (SPI 25),
  power domain `gpumix-pd`, clocks GPU3D/GPU2D/GPU_BUS/GPU_AHB. The in-tree galcore is
  disabled (`CONFIG_MXC_GPU_VIV` off); `galcore.ko` from Buildroot's
  `kernel-module-imx-gpu-viv` binds to this node.

### Power key and inputs
- `snvs-powerkey` in SNVS 0x30370000, SPI 4, `fsl,sec-v4.0-pwrkey` -> input device
  `30370000.snvs:snvs-powerkey` (driver `snvs_pwrkey`, `linux,keycode` 116 = `KEY_POWER`).
- The SNVS RTC (`snvs-rtc-lp`) is disabled in this DT.

### Other enabled SoC blocks the kernel probes at boot
Every row in the table below gets probed. Blocks the KICK does not use still need at
least a register window that does not abort and reads its reset values:
CCM 0x30380000 and ANATOP 0x30360000 (clock tree), IOMUXC 0x30330000 and GPR 0x30340000,
OCOTP 0x30350000 (SoC ID, MAC address, speed grade), SRC 0x30390000, GPIO1..5,
TMU 0x30260000, WDOG1 0x30280000, SDMA1/2/3, ECSPI1 (MCP251xFD CAN at CS1, spidev at CS2),
MU 0x30aa0000, CAAM 0x30900000 (job rings), CSI bridge and MIPI CSI (camera), USB OTG1/2
(0x32e40000, 0x32e50000), PCIe 0x33800000 + PHY 0x32f00000, APBH DMA 0x33000000,
VPU G1/G2/H1 (0x38300000, 0x38310000, 0x38320000), DDR PMU 0x3d800000 and `busfreq`
(DRAM frequency switching through ATF SiP calls).

## Stock QEMU check

`Image` + this DTB + the test initramfs on QEMU 11.0 `-M imx8mp-evk` (the closest stock
machine; same UART, GIC and many peripheral addresses):

```
qemu-system-aarch64 -M imx8mp-evk -m 2G -smp 4 -display none \
  -kernel Image -dtb imx8mm-var-som-symphony.dtb -initrd initramfs.cpio.gz \
  -append "console=ttymxc3,115200 earlycon=ec_imx6q,0x30a60000,115200" \
  -serial null -serial null -serial null -serial stdio
```

reaches: earlycon on UART4, PSCI 1.1 over SMC, GICv3 with 160 SPIs and 4 redistributors,
all four CPUs online, 2 GiB RAM with 320 MiB CMA, initramfs unpacked, ttymxc0..3
registered at the expected addresses, `snvs-powerkey` input registered. Failures there,
all expected because the 8MP machine lacks 8MM blocks:

- `stmp_reset_block ... module reset timeout`, `mxs-dma ... error -110` (APBH DMA 0x33000000)
- `imx_busfreq: probe of busfreq failed with error -22`
- `imx_sec_dsim_drv 32e10000.mipi_dsi: failed to get blk_ctl` plus a WARN in
  `sec_mipi_dsim_bind` (display GPR / dispmix block control)
- synchronous external abort in `cqhci_init` from `sdhci_esdhc_imx_probe` -> panic
  (uSDHC CQHCI registers at +0x100)

These are the first items for the `fsl-imx8mm` SoC model.

## Generated node table: imx8mm-var-som-symphony.dtb

Model: `Variscite VAR-SOM-MX8M-MINI on Symphony-Board 1.4a and above`, compatible: `variscite,dart-mx8mm, fsl,imx8mm`

### Aliases

| alias | node |
|---|---|
| ethernet0 | `/soc@0/bus@30800000/ethernet@30be0000` |
| gpio0 | `/soc@0/bus@30000000/gpio@30200000` |
| gpio1 | `/soc@0/bus@30000000/gpio@30210000` |
| gpio2 | `/soc@0/bus@30000000/gpio@30220000` |
| gpio3 | `/soc@0/bus@30000000/gpio@30230000` |
| gpio4 | `/soc@0/bus@30000000/gpio@30240000` |
| i2c0 | `/soc@0/bus@30800000/i2c@30a20000` |
| i2c1 | `/soc@0/bus@30800000/i2c@30a30000` |
| i2c2 | `/soc@0/bus@30800000/i2c@30a40000` |
| i2c3 | `/soc@0/bus@30800000/i2c@30a50000` |
| mmc0 | `/soc@0/bus@30800000/mmc@30b40000` |
| mmc1 | `/soc@0/bus@30800000/mmc@30b50000` |
| mmc2 | `/soc@0/bus@30800000/mmc@30b60000` |
| sai1 | `/soc@0/bus@30000000/sai@30010000` |
| sai2 | `/soc@0/bus@30000000/sai@30020000` |
| sai3 | `/soc@0/bus@30000000/sai@30030000` |
| sai5 | `/soc@0/bus@30000000/sai@30050000` |
| sai6 | `/soc@0/bus@30000000/sai@30060000` |
| serial0 | `/soc@0/bus@30800000/serial@30860000` |
| serial1 | `/soc@0/bus@30800000/serial@30890000` |
| serial2 | `/soc@0/bus@30800000/serial@30880000` |
| serial3 | `/soc@0/bus@30800000/serial@30a60000` |
| spi0 | `/soc@0/bus@30800000/spi@30820000` |
| spi1 | `/soc@0/bus@30800000/spi@30830000` |
| spi2 | `/soc@0/bus@30800000/spi@30840000` |

### /chosen

- `stdout-path` = `/soc@0/bus@30800000/serial@30a60000`

### Memory

- `/memory@40000000`: 0x40000000+0x80000000
- `/reserved-memory`: no regions

### Graph links (ports/endpoints)

- `/soc@0/bus@30800000/i2c@30a20000/sn65dsi84@2c` <-> `/soc@0/bus@32c00000/mipi_dsi@32e10000`
- `/soc@0/bus@30800000/i2c@30a30000/ov5640_mipi1@3c` <-> `/soc@0/bus@32c00000/mipi_csi@32e30000`
- `/soc@0/bus@32c00000/lcdif@32e00000` <-> `/soc@0/bus@32c00000/mipi_dsi@32e10000`
- `/soc@0/bus@32c00000/csi1_bridge@32e20000` <-> `/soc@0/bus@32c00000/mipi_csi@32e30000`

### Enabled nodes

Interrupts: `SPI n (GIC m)` is the GIC shared peripheral interrupt n, i.e. GIC INTID m.
Clocks are the `IMX8MM_CLK_*` indices of the CCM node. Nodes are listed in tree order;
devices sharing one interrupt or address range appear separately.

| node | compatible | reg | interrupts | clocks | other |
|---|---|---|---|---|---|
| `/` | variscite,dart-mx8mm, fsl,imx8mm |  |  |  |  |
| `/cpus/idle-states/cpu-pd-wait` | arm,idle-state |  |  |  |  |
| `/cpus/cpu@0` | arm,cortex-a53 | 0x0 |  | IMX8MM_CLK_ARM | on cpus; enable-method=psci |
| `/cpus/cpu@1` | arm,cortex-a53 | 0x1 |  | IMX8MM_CLK_ARM | on cpus; enable-method=psci |
| `/cpus/cpu@2` | arm,cortex-a53 | 0x2 |  | IMX8MM_CLK_ARM | on cpus; enable-method=psci |
| `/cpus/cpu@3` | arm,cortex-a53 | 0x3 |  | IMX8MM_CLK_ARM | on cpus; enable-method=psci |
| `/cpus/l2-cache0` | cache |  |  |  |  |
| `/opp-table` | operating-points-v2 |  |  |  |  |
| `/clock-osc-32k` | fixed-clock |  |  |  |  |
| `/clock-osc-24m` | fixed-clock |  |  |  |  |
| `/clock-ext1` | fixed-clock |  |  |  |  |
| `/clock-ext2` | fixed-clock |  |  |  |  |
| `/clock-ext3` | fixed-clock |  |  |  |  |
| `/clock-ext4` | fixed-clock |  |  |  |  |
| `/psci` | arm,psci-1.0 |  |  |  | method=smc |
| `/pmu` | arm,armv8-pmuv3 |  | PPI 7 high |  |  |
| `/power-domains` | simple-bus |  |  |  |  |
| `/power-domains/hsiomix-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_USB1_CTRL_ROOT |  |
| `/power-domains/pcie-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_PCIE1_ROOT |  |
| `/power-domains/usbotg1-pd` | fsl,imx8m-pm-domain |  |  |  |  |
| `/power-domains/usbotg2-pd` | fsl,imx8m-pm-domain |  |  |  |  |
| `/power-domains/gpumix-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_GPU_BUS_ROOT, IMX8MM_CLK_GPU_AHB, IMX8MM_CLK_GPU2D_ROOT, IMX8MM_CLK_GPU3D_ROOT |  |
| `/power-domains/vpumix-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_VPU_DEC_ROOT |  |
| `/power-domains/vpug1-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_VPU_G1_ROOT |  |
| `/power-domains/vpug2-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_VPU_G2_ROOT |  |
| `/power-domains/vpuh1-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_VPU_H1_ROOT |  |
| `/power-domains/dispmix-pd` | fsl,imx8m-pm-domain |  |  | IMX8MM_CLK_DISP_ROOT, IMX8MM_CLK_DISP_AXI_ROOT, IMX8MM_CLK_DISP_APB_ROOT |  |
| `/power-domains/mipi-pd` | fsl,imx8m-pm-domain |  |  |  |  |
| `/timer` | arm,armv8-timer |  | PPI 13 low; PPI 14 low; PPI 11 low; PPI 10 low |  |  |
| `/usbphynop1` | usb-nop-xceiv |  |  | main_clk=IMX8MM_CLK_USB_PHY_REF |  |
| `/usbphynop2` | usb-nop-xceiv |  |  | main_clk=IMX8MM_CLK_USB_PHY_REF |  |
| `/busfreq` | fsl,imx_busfreq |  | SPI 74 (GIC 106) high; SPI 75 (GIC 107) high; SPI 76 (GIC 108) high; SPI 77 (GIC 109) high | dram_pll=IMX8MM_DRAM_PLL_OUT, dram_alt_src=IMX8MM_CLK_DRAM_ALT, dram_apb_src=IMX8MM_CLK_DRAM_APB, dram_apb_pre_div=IMX8MM_CLK_DRAM_APB, dram_core=IMX8MM_CLK_DRAM_CORE, dram_alt_root=IMX8MM_CLK_DRAM_ALT_ROOT, sys_pll1_40m=IMX8MM_SYS_PLL1_40M, sys_pll1_100m=IMX8MM_SYS_PLL1_100M, sys_pll2_333m=IMX8MM_SYS_PLL2_333M, noc_div=IMX8MM_CLK_NOC, ahb_div=IMX8MM_CLK_AHB, main_axi_src=IMX8MM_CLK_MAIN_AXI, osc_24m=IMX8MM_CLK_24M, sys_pll1_800m=IMX8MM_SYS_PLL1_800M, dram_pll_div=IMX8MM_DRAM_PLL |  |
| `/soc@0` | simple-bus |  |  |  |  |
| `/soc@0/caam-sm@100000` | fsl,imx6q-caam-sm | 0x00100000+0x8000 |  |  |  |
| `/soc@0/bus@30000000` | fsl,aips-bus, simple-bus |  |  |  |  |
| `/soc@0/bus@30000000/sai@30020000` | fsl,imx8mm-sai, fsl,imx8mq-sai | 0x30020000+0x10000 | SPI 96 (GIC 128) high | bus=IMX8MM_CLK_SAI2_IPG, mclk0=IMX8MM_CLK_DUMMY, mclk1=IMX8MM_CLK_SAI2_ROOT, mclk2=IMX8MM_CLK_DUMMY, mclk3=IMX8MM_CLK_DUMMY | rx=dma-controller@302c0000(2 2 0); tx=dma-controller@302c0000(3 2 0); fsl,sai-synchronous-rx; fsl,sai-mclk-direction-output; assigned-clock-rates=12288000 |
| `/soc@0/bus@30000000/gpio@30200000` | fsl,imx8mm-gpio, fsl,imx35-gpio | 0x30200000+0x10000 | SPI 64 (GIC 96) high; SPI 65 (GIC 97) high | IMX8MM_CLK_GPIO1_ROOT |  |
| `/soc@0/bus@30000000/gpio@30210000` | fsl,imx8mm-gpio, fsl,imx35-gpio | 0x30210000+0x10000 | SPI 66 (GIC 98) high; SPI 67 (GIC 99) high | IMX8MM_CLK_GPIO2_ROOT |  |
| `/soc@0/bus@30000000/gpio@30220000` | fsl,imx8mm-gpio, fsl,imx35-gpio | 0x30220000+0x10000 | SPI 68 (GIC 100) high; SPI 69 (GIC 101) high | IMX8MM_CLK_GPIO3_ROOT |  |
| `/soc@0/bus@30000000/gpio@30230000` | fsl,imx8mm-gpio, fsl,imx35-gpio | 0x30230000+0x10000 | SPI 70 (GIC 102) high; SPI 71 (GIC 103) high | IMX8MM_CLK_GPIO4_ROOT |  |
| `/soc@0/bus@30000000/gpio@30230000/cam_buf_hog` | (child of fsl,imx8mm-gpio) |  |  |  | hog GPIO4_IO05 active-high output-low |
| `/soc@0/bus@30000000/gpio@30240000` | fsl,imx8mm-gpio, fsl,imx35-gpio | 0x30240000+0x10000 | SPI 72 (GIC 104) high; SPI 73 (GIC 105) high | IMX8MM_CLK_GPIO5_ROOT |  |
| `/soc@0/bus@30000000/tmu@30260000` | fsl,imx8mm-tmu | 0x30260000+0x10000 |  | IMX8MM_CLK_TMU_ROOT |  |
| `/soc@0/bus@30000000/watchdog@30280000` | fsl,imx8mm-wdt, fsl,imx21-wdt | 0x30280000+0x10000 | SPI 78 (GIC 110) high | IMX8MM_CLK_WDOG1_ROOT |  |
| `/soc@0/bus@30000000/dma-controller@302c0000` | fsl,imx8mm-sdma, fsl,imx8mq-sdma | 0x302c0000+0x10000 | SPI 103 (GIC 135) high | ipg=IMX8MM_CLK_SDMA2_ROOT, ahb=IMX8MM_CLK_SDMA2_ROOT |  |
| `/soc@0/bus@30000000/dma-controller@302b0000` | fsl,imx8mm-sdma, fsl,imx8mq-sdma | 0x302b0000+0x10000 | SPI 34 (GIC 66) high | ipg=IMX8MM_CLK_SDMA3_ROOT, ahb=IMX8MM_CLK_SDMA3_ROOT |  |
| `/soc@0/bus@30000000/pinctrl@30330000` | fsl,imx8mm-iomuxc | 0x30330000+0x10000 |  |  |  |
| `/soc@0/bus@30000000/iomuxc-gpr@30340000` | fsl,imx8mm-iomuxc-gpr, fsl,imx6q-iomuxc-gpr, syscon | 0x30340000+0x10000 |  |  |  |
| `/soc@0/bus@30000000/efuse@30350000` | fsl,imx8mm-ocotp, syscon | 0x30350000+0x10000 |  | IMX8MM_CLK_OCOTP_ROOT |  |
| `/soc@0/bus@30000000/anatop@30360000` | fsl,imx8mm-anatop, syscon, simple-bus | 0x30360000+0x10000 |  |  |  |
| `/soc@0/bus@30000000/caam_secvio` | fsl,imx6q-caam-secvio |  | SPI 20 (GIC 52) high |  |  |
| `/soc@0/bus@30000000/caam-snvs@30370000` | fsl,imx6q-caam-snvs | 0x30370000+0x10000 |  | ipg=IMX8MM_CLK_SNVS_ROOT |  |
| `/soc@0/bus@30000000/snvs@30370000` | fsl,sec-v4.0-mon, syscon, simple-mfd | 0x30370000+0x10000 |  |  |  |
| `/soc@0/bus@30000000/snvs@30370000/snvs-powerkey` | fsl,sec-v4.0-pwrkey |  | SPI 4 (GIC 36) high | snvs=IMX8MM_CLK_SNVS_ROOT |  |
| `/soc@0/bus@30000000/clock-controller@30380000` | fsl,imx8mm-ccm | 0x30380000+0x10000 |  | osc_32k=clock-osc-32k, osc_24m=clock-osc-24m, clk_ext1=clock-ext1, clk_ext2=clock-ext2, clk_ext3=clock-ext3, clk_ext4=clock-ext4 | assigned-clock-rates=0 400000000 400000000 750000000 594000000 393216000 361267200 |
| `/soc@0/bus@30000000/reset-controller@30390000` | fsl,imx8mm-src, fsl,imx8mq-src, syscon | 0x30390000+0x10000 | SPI 89 (GIC 121) high |  |  |
| `/soc@0/bus@30400000` | fsl,aips-bus, simple-bus |  |  |  |  |
| `/soc@0/bus@30400000/pwm@30660000` | fsl,imx8mm-pwm, fsl,imx27-pwm | 0x30660000+0x10000 | SPI 81 (GIC 113) high | ipg=IMX8MM_CLK_PWM1_ROOT, per=IMX8MM_CLK_PWM1_ROOT |  |
| `/soc@0/bus@30400000/timer@306a0000` | nxp,sysctr-timer | 0x306a0000+0x20000 | SPI 47 (GIC 79) high | per=clock-osc-24m |  |
| `/soc@0/bus@30800000` | fsl,aips-bus, simple-bus |  |  |  |  |
| `/soc@0/bus@30800000/spi@30820000` | fsl,imx8mm-ecspi, fsl,imx51-ecspi | 0x30820000+0x10000 | SPI 31 (GIC 63) high | ipg=IMX8MM_CLK_ECSPI1_ROOT, per=IMX8MM_CLK_ECSPI1_ROOT | cs-gpios=GPIO1_IO14 active-low; cs-gpios=GPIO1_IO00 active-low; cs-gpios=GPIO2_IO19 active-low; rx=dma-controller@30bd0000(0 7 1); tx=dma-controller@30bd0000(1 7 2) |
| `/soc@0/bus@30800000/spi@30820000/can@1` | microchip,mcp251xfd | 0x1 | GPIO1_IO05 low | can0_osc | on spi@30820000; microchip,rx-int-gpios=GPIO2_IO12 active-low |
| `/soc@0/bus@30800000/spi@30820000/spidev@2` | var,spidev | 0x2 |  |  | on spi@30820000 |
| `/soc@0/bus@30800000/serial@30860000` | fsl,imx8mm-uart, fsl,imx6q-uart | 0x30860000+0x10000 | SPI 26 (GIC 58) high | ipg=IMX8MM_CLK_UART1_ROOT, per=IMX8MM_CLK_UART1_ROOT | rx=dma-controller@30bd0000(22 4 0); tx=dma-controller@30bd0000(23 4 0) |
| `/soc@0/bus@30800000/serial@30880000` | fsl,imx8mm-uart, fsl,imx6q-uart | 0x30880000+0x10000 | SPI 28 (GIC 60) high | ipg=IMX8MM_CLK_UART3_ROOT, per=IMX8MM_CLK_UART3_ROOT | rx=dma-controller@30bd0000(26 4 0); tx=dma-controller@30bd0000(27 4 0) |
| `/soc@0/bus@30800000/serial@30890000` | fsl,imx8mm-uart, fsl,imx6q-uart | 0x30890000+0x10000 | SPI 27 (GIC 59) high | ipg=IMX8MM_CLK_UART2_ROOT, per=IMX8MM_CLK_UART2_ROOT |  |
| `/soc@0/bus@30800000/crypto@30900000` | fsl,sec-v4.0 | 0x30900000+0x40000 | SPI 91 (GIC 123) high | aclk=IMX8MM_CLK_AHB, ipg=IMX8MM_CLK_IPG_ROOT |  |
| `/soc@0/bus@30800000/crypto@30900000/jr@1000` | fsl,sec-v4.0-job-ring | 0x00001000+0x1000 | SPI 105 (GIC 137) high |  |  |
| `/soc@0/bus@30800000/crypto@30900000/jr@2000` | fsl,sec-v4.0-job-ring | 0x00002000+0x1000 | SPI 106 (GIC 138) high |  |  |
| `/soc@0/bus@30800000/crypto@30900000/jr@3000` | fsl,sec-v4.0-job-ring | 0x00003000+0x1000 | SPI 114 (GIC 146) high |  |  |
| `/soc@0/bus@30800000/i2c@30a20000` | fsl,imx8mm-i2c, fsl,imx21-i2c | 0x30a20000+0x10000 | SPI 35 (GIC 67) high | IMX8MM_CLK_I2C1_ROOT | scl-gpios=GPIO5_IO14 active-high; sda-gpios=GPIO5_IO15 active-high |
| `/soc@0/bus@30800000/i2c@30a20000/sn65dsi84@2c` | ti,sn65dsi83 | 0x2c |  |  | on i2c@30a20000; enable-gpios=GPIO2_IO11 active-high |
| `/soc@0/bus@30800000/i2c@30a20000/pmic@4b` | rohm,bd71847 | 0x4b | GPIO2_IO08 low |  | on i2c@30a20000 |
| `/soc@0/bus@30800000/i2c@30a30000` | fsl,imx8mm-i2c, fsl,imx21-i2c | 0x30a30000+0x10000 | SPI 36 (GIC 68) high | IMX8MM_CLK_I2C2_ROOT | scl-gpios=GPIO5_IO16 active-high; sda-gpios=GPIO5_IO17 active-high |
| `/soc@0/bus@30800000/i2c@30a30000/ov5640_mipi1@3c` | ovti,ov5640_mipi | 0x3c |  | csi_mclk=IMX8MM_CLK_CLKO1 | on i2c@30a30000; pwn-gpios=GPIO5_IO12 active-high; rst-gpios=GPIO1_IO13 active-low |
| `/soc@0/bus@30800000/i2c@30a30000/gpio@20` | nxp,pca9534 | 0x20 | GPIO1_IO07 falling |  | on i2c@30a30000 |
| `/soc@0/bus@30800000/i2c@30a30000/gpio@20/usb3_sel_hog` | (child of nxp,pca9534) |  |  |  | hog i2c@30a30000/gpio@20.4 active-high output-low |
| `/soc@0/bus@30800000/i2c@30a30000/gpio@20/enet_rst_hog` | (child of nxp,pca9534) |  |  |  | hog i2c@30a30000/gpio@20.5 active-high output-low |
| `/soc@0/bus@30800000/i2c@30a30000/gpio@20/som_vsel_hog` | (child of nxp,pca9534) |  |  |  | hog i2c@30a30000/gpio@20.6 active-high output-low |
| `/soc@0/bus@30800000/i2c@30a30000/gpio@20/enet_sel_hog` | (child of nxp,pca9534) |  |  |  | hog i2c@30a30000/gpio@20.7 active-high output-low |
| `/soc@0/bus@30800000/i2c@30a40000` | fsl,imx8mm-i2c, fsl,imx21-i2c | 0x30a40000+0x10000 | SPI 37 (GIC 69) high | IMX8MM_CLK_I2C3_ROOT | scl-gpios=GPIO5_IO18 active-high; sda-gpios=GPIO5_IO19 active-high |
| `/soc@0/bus@30800000/i2c@30a40000/touchscreen@14` | goodix,gt911 | 0x14 | GPIO5_IO04 falling |  | on i2c@30a40000; irq-gpios=GPIO5_IO04 active-high; reset-gpios=GPIO5_IO03 active-low; touchscreen-size-x=800; touchscreen-size-y=480 |
| `/soc@0/bus@30800000/i2c@30a40000/rtc@68` | dallas,ds1337 | 0x68 |  |  | on i2c@30a40000 |
| `/soc@0/bus@30800000/i2c@30a50000` | fsl,imx8mm-i2c, fsl,imx21-i2c | 0x30a50000+0x10000 | SPI 38 (GIC 70) high | IMX8MM_CLK_I2C4_ROOT | scl-gpios=GPIO5_IO20 active-high; sda-gpios=GPIO5_IO21 active-high |
| `/soc@0/bus@30800000/serial@30a60000` | fsl,imx8mm-uart, fsl,imx6q-uart | 0x30a60000+0x10000 | SPI 29 (GIC 61) high | ipg=IMX8MM_CLK_UART4_ROOT, per=IMX8MM_CLK_UART4_ROOT | rx=dma-controller@30bd0000(28 4 0); tx=dma-controller@30bd0000(29 4 0) |
| `/soc@0/bus@30800000/mu@30aa0000` | fsl,imx8mm-mu, fsl,imx6sx-mu | 0x30aa0000+0x10000 | SPI 88 (GIC 120) high | mu=IMX8MM_CLK_MU_ROOT |  |
| `/soc@0/bus@30800000/mmc@30b40000` | fsl,imx8mm-usdhc, fsl,imx7d-usdhc | 0x30b40000+0x10000 | SPI 22 (GIC 54) high | ipg=IMX8MM_CLK_IPG_ROOT, ahb=IMX8MM_CLK_NAND_USDHC_BUS, per=IMX8MM_CLK_USDHC1_ROOT | bus-width=4; non-removable; assigned-clock-rates=400000000 |
| `/soc@0/bus@30800000/mmc@30b40000/wifi@1` | brcm,bcm4329-fmac | 0x1 |  |  | on mmc@30b40000 |
| `/soc@0/bus@30800000/mmc@30b50000` | fsl,imx8mm-usdhc, fsl,imx7d-usdhc | 0x30b50000+0x10000 | SPI 23 (GIC 55) high | ipg=IMX8MM_CLK_IPG_ROOT, ahb=IMX8MM_CLK_NAND_USDHC_BUS, per=IMX8MM_CLK_USDHC2_ROOT | cd-gpios=GPIO1_IO10 active-low; bus-width=4 |
| `/soc@0/bus@30800000/mmc@30b60000` | fsl,imx8mm-usdhc, fsl,imx7d-usdhc | 0x30b60000+0x10000 | SPI 24 (GIC 56) high | ipg=IMX8MM_CLK_IPG_ROOT, ahb=IMX8MM_CLK_NAND_USDHC_BUS, per=IMX8MM_CLK_USDHC3_ROOT | bus-width=8; non-removable; assigned-clock-rates=400000000 |
| `/soc@0/bus@30800000/dma-controller@30bd0000` | fsl,imx8mm-sdma, fsl,imx8mq-sdma | 0x30bd0000+0x10000 | SPI 2 (GIC 34) high | ipg=IMX8MM_CLK_SDMA1_ROOT, ahb=IMX8MM_CLK_AHB |  |
| `/soc@0/bus@30800000/ethernet@30be0000` | fsl,imx8mm-fec, fsl,imx8mq-fec, fsl,imx6sx-fec | 0x30be0000+0x10000 | SPI 118 (GIC 150) high; SPI 119 (GIC 151) high; SPI 120 (GIC 152) high | ipg=IMX8MM_CLK_ENET1_ROOT, ahb=IMX8MM_CLK_ENET1_ROOT, ptp=IMX8MM_CLK_ENET_TIMER, enet_clk_ref=IMX8MM_CLK_ENET_REF, enet_out=IMX8MM_CLK_ENET_PHY_REF | phy-reset-gpios=GPIO1_IO09 active-low; phy-mode=rgmii; assigned-clock-rates=0 0 125000000 100000000 |
| `/soc@0/bus@30800000/ethernet@30be0000/mdio/ethernet-phy@4` | ethernet-phy-ieee802.3-c22 | 0x4 |  |  | on mdio |
| `/soc@0/bus@32c00000` | fsl,aips-bus, simple-bus |  |  |  |  |
| `/soc@0/bus@32c00000/lcdif@32e00000` | fsl,imx8mm-lcdif | 0x32e00000+0x10000 | SPI 5 (GIC 37) high | pix=IMX8MM_CLK_LCDIF_PIXEL, disp-axi=IMX8MM_CLK_DISP_AXI_ROOT, disp-apb=IMX8MM_CLK_DISP_APB_ROOT |  |
| `/soc@0/bus@32c00000/mipi_dsi@32e10000` | fsl,imx8mm-mipi-dsim | 0x32e10000+0x400 | SPI 18 (GIC 50) high | cfg=IMX8MM_CLK_DSI_CORE, pll-ref=IMX8MM_CLK_DSI_PHY_REF | assigned-clock-rates=266000000 12000000 |
| `/soc@0/bus@32c00000/csi1_bridge@32e20000` | fsl,imx8mm-csi, fsl,imx8mq-csi, fsl,imx6s-csi | 0x32e20000+0x1000 | SPI 16 (GIC 48) high | disp-axi=IMX8MM_CLK_DISP_AXI_ROOT, csi_mclk=IMX8MM_CLK_CSI1_ROOT, disp_dcic=IMX8MM_CLK_DISP_APB_ROOT |  |
| `/soc@0/bus@32c00000/mipi_csi@32e30000` | fsl,imx8mm-mipi-csi | 0x32e30000+0x1000 | SPI 17 (GIC 49) high | mipi_clk=IMX8MM_CLK_CSI1_CORE, phy_clk=IMX8MM_CLK_CSI1_PHY_REF, disp_axi=IMX8MM_CLK_DISP_AXI_ROOT, disp_apb=IMX8MM_CLK_DISP_APB_ROOT | bus-width=4 |
| `/soc@0/bus@32c00000/display-gpr@32e28000` | fsl, imx8mm-iomuxc-gpr, syscon | 0x32e28000+0x100 |  |  |  |
| `/soc@0/bus@32c00000/display-subsystem` | fsl,imx-display-subsystem |  |  |  |  |
| `/soc@0/bus@32c00000/usb@32e40000` | fsl,imx8mm-usb, fsl,imx7d-usb | 0x32e40000+0x200 | SPI 40 (GIC 72) high | usb1_ctrl_root_clk=IMX8MM_CLK_USB1_CTRL_ROOT |  |
| `/soc@0/bus@32c00000/usbmisc@32e40200` | fsl,imx8mm-usbmisc, fsl,imx7d-usbmisc | 0x32e40200+0x200 |  |  |  |
| `/soc@0/bus@32c00000/usb@32e50000` | fsl,imx8mm-usb, fsl,imx7d-usb | 0x32e50000+0x200 | SPI 41 (GIC 73) high | usb1_ctrl_root_clk=IMX8MM_CLK_USB1_CTRL_ROOT |  |
| `/soc@0/bus@32c00000/usbmisc@32e50200` | fsl,imx8mm-usbmisc, fsl,imx7d-usbmisc | 0x32e50200+0x200 |  |  |  |
| `/soc@0/bus@32c00000/pcie-phy@32f00000` | fsl,imx7d-pcie-phy | 0x32f00000+0x10000 |  |  |  |
| `/soc@0/dma-controller@33000000` | fsl,imx7d-dma-apbh, fsl,imx28-dma-apbh | 0x33000000+0x2000 | gpmi0: SPI 12 (GIC 44) high; gpmi1: SPI 12 (GIC 44) high; gpmi2: SPI 12 (GIC 44) high; gpmi3: SPI 12 (GIC 44) high | IMX8MM_CLK_NAND_USDHC_BUS_RAWNAND_CLK |  |
| `/soc@0/pcie@33800000` | fsl,imx8mm-pcie, snps,dw-pcie | dbi 0x33800000+0x400000, config 0x1ff00000+0x80000 | msi: SPI 122 (GIC 154) high; dma: SPI 127 (GIC 159) high | pcie=IMX8MM_CLK_PCIE1_ROOT, pcie_aux=IMX8MM_CLK_PCIE1_AUX, pcie_phy=IMX8MM_CLK_PCIE1_PHY, pcie_bus=pcie0-refclk | reset-gpio=GPIO4_IO21 active-low; assigned-clock-rates=10000000 100000000 250000000 |
| `/soc@0/interrupt-controller@38800000` | arm,gic-v3 | 0x38800000+0x10000, 0x38880000+0xc0000 | PPI 9 high |  |  |
| `/soc@0/ddr-pmu@3d800000` | fsl,imx8mm-ddr-pmu, fsl,imx8m-ddr-pmu | 0x3d800000+0x400000 | SPI 98 (GIC 130) high |  |  |
| `/dispmix-reset` | simple-bus |  |  |  |  |
| `/dispmix-reset/dispmix-sft-rstn@32e28000` | fsl,imx8mm-dispmix-sft-rstn | 0x32e28000+0x4 |  | disp_apb_root_clk=IMX8MM_CLK_DISP_APB_ROOT |  |
| `/dispmix-reset/dispmix-clk-en@32e28004` | fsl,imx8mm-dispmix-clk-en | 0x32e28004+0x4 |  | disp_apb_root_clk=IMX8MM_CLK_DISP_APB_ROOT |  |
| `/dispmix-reset/dispmix-mipi-rst@32e28008` | fsl,imx8mm-dispmix-mipi-rst | 0x32e28008+0x4 |  | disp_apb_root_clk=IMX8MM_CLK_DISP_APB_ROOT |  |
| `/lcdif-resets/lcdif-clk-enable` | lcdif,clk-enable |  |  |  |  |
| `/mipi-dsi-resets/dsi-soft-resetn` | dsi,soft-resetn |  |  |  |  |
| `/mipi-dsi-resets/dsi-clk-enable` | dsi,clk-enable |  |  |  |  |
| `/mipi-dsi-resets/dsi-mipi-reset` | dsi,mipi-reset |  |  |  |  |
| `/vpu_h1@38320000` | nxp,imx8mm-hantro-h1 | regs_hantro_h1 0x38320000+0x10000 | irq_hantro_h1: SPI 30 (GIC 62) high | clk_hantro_h1=IMX8MM_CLK_VPU_H1_ROOT, clk_hantro_h1_bus=IMX8MM_CLK_VPU_DEC_ROOT | assigned-clock-rates=600000000 800000000 |
| `/vpu_g1@38300000` | nxp,imx8mm-hantro | regs_hantro 0x38300000+0x100000 | irq_hantro: SPI 7 (GIC 39) high | clk_hantro=IMX8MM_CLK_VPU_G1_ROOT, clk_hantro_bus=IMX8MM_CLK_VPU_DEC_ROOT | assigned-clock-rates=600000000 800000000 |
| `/vpu_g2@38310000` | nxp,imx8mm-hantro | regs_hantro 0x38310000+0x100000 | irq_hantro: SPI 8 (GIC 40) high | clk_hantro=IMX8MM_CLK_VPU_G2_ROOT, clk_hantro_bus=IMX8MM_CLK_VPU_DEC_ROOT | assigned-clock-rates=600000000 800000000 |
| `/gpu@38000000` | fsl,imx8mm-gpu, fsl,imx6q-gpu | iobase_3d 0x38000000+0x8000, iobase_2d 0x38008000+0x8000, phys_baseaddr 0x40000000+0x80000000, contiguous_mem 0x00000000+0x8000000 | irq_3d: SPI 3 (GIC 35) high; irq_2d: SPI 25 (GIC 57) high | gpu3d_clk=IMX8MM_CLK_GPU3D_ROOT, gpu3d_shader_clk=IMX8MM_CLK_DUMMY, gpu3d_axi_clk=IMX8MM_CLK_GPU_BUS_ROOT, gpu3d_ahb_clk=IMX8MM_CLK_GPU_AHB, gpu2d_clk=IMX8MM_CLK_GPU2D_ROOT, gpu2d_axi_clk=IMX8MM_CLK_GPU_BUS_ROOT, gpu2d_ahb_clk=IMX8MM_CLK_GPU_AHB | assigned-clock-rates=0 0 0 400000000 1000000000 |
| `/audio_vdd` | regulator-fixed |  |  |  |  |
| `/regulator-eth-phy` | regulator-fixed |  |  |  | gpio=GPIO2_IO09 active-high |
| `/sound-v4220m` | simple-audio-card |  |  |  |  |
| `/codec1` | linux,bt-sco |  |  | mclk=IMX8MM_CLK_SAI2_ROOT |  |
| `/can0_osc` | fixed-clock |  |  |  |  |
| `/backlight` | pwm-backlight |  |  |  |  |
| `/gpio-keys` | gpio-keys |  |  |  |  |
| `/gpio-keys/back` | (child of gpio-keys) |  |  |  | gpios=i2c@30a30000/gpio@20.1 active-low; label=Back; linux,code=158 |
| `/gpio-keys/home` | (child of gpio-keys) |  |  |  | gpios=i2c@30a30000/gpio@20.2 active-low; label=Home; linux,code=102 |
| `/gpio-keys/menu` | (child of gpio-keys) |  |  |  | gpios=i2c@30a30000/gpio@20.3 active-low; label=Menu; linux,code=139 |
| `/leds` | gpio-leds |  |  |  |  |
| `/leds/codecreset` | (child of gpio-leds) |  |  |  | gpios=GPIO4_IO24 active-high; gpios=GPIO4_IO24 active-high; label=codecreset |
| `/leds/codecmute` | (child of gpio-leds) |  |  |  | gpios=GPIO4_IO25 active-high; gpios=GPIO4_IO25 active-high; label=codecmute |
| `/pcie0-refclk` | fixed-clock |  |  |  |  |
| `/regulator-usdhc2` | regulator-fixed |  |  |  | gpio=GPIO4_IO22 active-high |
| `/regulator-usb-otg2-vbus` | regulator-fixed |  |  |  | gpio=GPIO5_IO01 active-high |
| `/extcon_usb1` | linux,extcon-usb-gpio |  |  |  | id-gpio=GPIO1_IO11 active-high |

128 enabled nodes with a compatible string.
