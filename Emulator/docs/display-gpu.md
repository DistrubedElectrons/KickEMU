# Display path and GPU models

Models for the KICK display chain and the Vivante GPU cores of the i.MX8M Mini. All
sources are in `Emulator/qemu/hw/kick/`, tests in `Emulator/qemu/tests/`.

Sources used: the built board DT (`Emulator/out/boot/imx8mm-var-som-symphony.dts`,
`docs/board-dt.md`), varigit linux-imx 5.4.127 (`920c302c`) drivers, galcore 6.4.3.p1
(`Freescale/kernel-module-imx-gpu-viv` at `5c8eb7b0`, the version Buildroot builds) and the
etnaviv rnndb (`etna_viv/rnndb`: `cmdstream.xml`, `state.xml`, `state_hi.xml`,
`state_2d.xml`). The i.MX8MM Reference Manual was not available while writing these models;
where a reset value or bit was taken from another source it is marked in the code.

## What the board DT enables

```
LCDIF 0x32e00000 (SPI 5) -> Samsung DSIM 0x32e10000 (SPI 18)
  -> TI SN65DSI84 on I2C1 0x2c (compatible "ti,sn65dsi83", EN = GPIO2_IO11)
  -> LVDS panel, 1024x600, 33 MHz (display-timings in the bridge node)
```

There is no DSI panel and no DCS traffic on the KICK: the bridge is configured over I2C
by `drivers/gpu/drm/bridge/sn65dsi83/`, and it only attaches to the DSIM as a DSI
device to pass lanes and mode flags. The panel has no control interface. The GT911 touch
controller still reports 800x480 (`touchscreen-size-x/y`), which does not match the
1024x600 mode the display is driven with; the console follows the LCDIF programming.

The display mix block control at 0x32e28000 is used through the `dispmix-reset` nodes
(soft reset, clock enable, MIPI reset). The DSIM driver prints `failed to get blk_ctl`
on every boot because the 8MM DT has no `blk-ctl` phandle; the driver continues without
it, this is expected and not a missing model.

Power domains (`dispmix-pd`, `mipi-pd`, `gpumix-pd`) are `fsl,imx8m-pm-domain`, switched
by SiP SMC calls to ATF, which programs the GPC. The driver ignores the SMC result, so in
`boot=kernel` mode the domains work without anything from these models. The models here
are always powered; accesses to a domain the GPC has switched off are not faulted.

## Blocks

### `imx8mm-dispmix` (DISPLAY_BLK_CTRL)

| | |
|---|---|
| DT | `display-gpr@32e28000` (syscon), `dispmix-sft-rstn@32e28000`, `dispmix-clk-en@32e28004`, `dispmix-mipi-rst@32e28008` |
| Base / size | 0x32e28000 / 0x100 |
| IRQ, clocks | none / DISP_APB_ROOT |
| Driver | `drivers/reset/reset-dispmix.c` |
| QOM | GPIO outputs `sft-rstn[7]`, `clk-en[13]`, `mipi-rst[2]` (bit levels) |

Registers: 0x00 SFT_RSTN (bits 6:0, active low), 0x04 CLK_EN (bits 12:0), 0x08
GPR_MIPI_RESET_DIV (bits 17:16 MIPI S/M reset active low, 15:0 dividers kept). All reset
to 0. The rest of the 256 bytes is plain storage with LOG_UNIMP, since the 5.4 drivers do
not use the display GPR syscon on the 8MM.

### `imx8mm-lcdif` (eLCDIF)

| | |
|---|---|
| DT | `lcdif@32e00000`, `fsl,imx8mm-lcdif` (driver `imx-lcdif`, CRTC under `imx-drm`) |
| Base / size | 0x32e00000 / 0x10000 |
| IRQ | GIC SPI 5 |
| Clocks | `pix` (LCDIF_PIXEL), DISP_AXI, DISP_APB |
| QOM | `panel-width`/`panel-height` (console size before a mode is set, default 800x480), `fallback-refresh-hz` (60, used when the `pix` clock input is not connected), link `dma-mr` (default system memory), clock input `pix` |

Register coverage:
- CTRL, CTRL1, CTRL2, VDCTRL0 with SET/CLR/TOG aliases; TRANSFER_COUNT, CUR_BUF, NEXT_BUF,
  TIMING, VDCTRL1-4, DVICTRL0-4, CSC, THRES, AS_*, SYNC_DELAY, PIGEONCTRL0-2 and
  PIGEON_n_0..2 as read/write registers; STAT (PRESENT, FIFO empty, BUSY while running),
  BM_ERROR_STAT, CRC_STAT read-only; bad offsets are guest errors.
- Reset values: CTRL 0xc0000000, CTRL1 0x000f0000, CTRL2 0x00200000, TRANSFER_COUNT
  0x00010000. Setting CTRL.SFTRST resets the block and sets CLKGATE; while SFTRST is set
  other writes are ignored.
- Interrupts VSYNC_EDGE (8), CUR_FRAME_DONE (9), UNDERFLOW (10), OVERFLOW (11) with enables
  12-15. The level is `CTRL1[11:8] & CTRL1[15:12]`.
- Timing: with RUN, DOTCLK_MODE and MASTER set a frame timer runs. Frame length
  VDCTRL1 lines (VSYNC_PERIOD_UNIT) times VDCTRL2 HSYNC_PERIOD pixels at the `pix` clock.
  At frame start NEXT_BUF is latched into CUR_BUF and VSYNC_EDGE is raised; CUR_FRAME_DONE
  follows after VERT_WAIT_CNT + VCOUNT lines. Clearing RUN stops after the current frame,
  RUN reads back as written.
- Scanout from CUR_BUF into a graphic console sized TRANSFER_COUNT (width cropped by
  PIGEON_12_0/1 as `lcdif_set_fb_hcrop()` programs it), using `framebuffer.c` dirty
  tracking. Formats from WORD_LENGTH/DF16/BYTE_PACKING/SHIFT/INPUT_SWIZZLE and the CTRL2
  line pattern: RGB565/BGR565, x/ARGB1555 and BGR variants, 18-bit unpacked, 24-bit packed,
  x/ARGB8888, x/ABGR8888, RGBx/RGBA8888. Alpha is ignored (no overlay).

Gaps: only the DOTCLK interface (MPU/VSYNC/DVI modes log LOG_UNIMP); the alpha surface
overlay (AS_CTRL enable logs LOG_UNIMP; the 5.4 driver never uses it); underflow, CSC,
CRC and 8-bit word length are not modelled. The console shows the LCDIF output regardless
of the DSIM/bridge state.

### `imx8mm-mipi-dsim` (Samsung DSIM)

| | |
|---|---|
| DT | `mipi_dsi@32e10000`, `fsl,imx8mm-mipi-dsim` (`sec_mipi_dsim-imx.c` + `bridge/sec-dsim.c`) |
| Base / size | 0x32e10000 / 0x400 |
| IRQ | GIC SPI 18 |
| Clocks | `cfg` (DSI_CORE, 266 MHz), `pll-ref` (DSI_PHY_REF, 12 MHz) |
| QOM | `version` (0x01060200, checked by the driver), `fallback-pll-ref-hz` (12000000), clock input `pll-ref`, link `dsi-peripheral` (object implementing `kick-dsi-peripheral`), GPIO input `reset-n` (active low, optional) |

Register coverage: VERSION; STATUS (PLLSTABLE, SWRSTRLS, TXREADYHSCLK, STOPSTATECLK,
STOPSTATEDAT from CONFIG.LANEEN); RGB_STATUS, CLKCTRL, TIMEOUT, CONFIG, ESCMODE (FORCEBTA
and TXTRIGGERRST self clearing), MDRESOL, MVPORCH, MHPORCH, MSYNC, SDRESOL; INTSRC (W1C) and
INTMSK (1 masks) driving the IRQ; PKTHDR, PAYLOAD, RXFIFO, FIFOTHLD, FIFOCTRL (EMPTY/FULL
flags, NINIT pointer resets), MEMACCHR, MULTI_PKT; PLLCTRL_1G, PLLCTRL, PLLCTRL1/2, PLLTMR;
PHYTIMING0-2; SWRST (SWRST resets registers and raises SWRSTRELEASE, FUNCRST the FIFOs).

Behaviour: writing PLLCTRL.PLLEN locks the PLL after PLLTMR reference clock cycles and
raises PLLSTABLE. Packets: payload words are queued, the PKTHDR write sends the packet
(short or long by data type) to the DSI peripheral and raises SFRPHFIFOEMPTY and, once
the payload FIFO is empty, SFRPLFIFOEMPTY. Read requests return the peripheral answer
through RXFIFO with RXDATDONE; without an answer TATOUT is raised. On the KICK no
peripheral answers DSI reads.

Reset values INTMSK 0xb337ffff, CLKCTRL 0x0000ffff, TIMEOUT 0x00ffffff, FIFOCTRL NINIT
bits set are the Samsung DSIM defaults, not checked against the 8MM RM. FIFO depths
(16 headers, 2 KiB payload, 256 bytes RX) are model choices. Video mode pixel streaming,
FRAMEDONE, ULPS and escape mode LP data are not modelled.

### `sn65dsi83` (TI SN65DSI84 bridge, I2C)

| | |
|---|---|
| DT | `sn65dsi84@2c` on `i2c@30a20000`, `ti,sn65dsi83`, 4 DSI lanes, 24 bpp, format 1, 1 LVDS channel |
| Address | 0x2c on I2C1 |
| QOM | `en-at-reset` (EN level before the GPIO drives it, default false), link `dsi-host` (the DSIM), GPIO input `enable` |

CSR coverage: 0x00-0x08 ID "DSI85" backwards and revision 0x01 (read-only), 0x09
SOFT_RESET (self clearing, reads 0, datapath only), 0x0A PLL_EN_STAT (bit 7, read-only) /
LVDS_CLK_RANGE / HS_CLK_SRC, 0x0B dividers, 0x0D PLL_EN, 0x10-0x3E DSI, LVDS and channel A/B
timing registers (read/write), 0xE0-0xE6 IRQ enable/mask and CHA/CHB error status (W1C;
PLL_UNLOCK stays set while the enabled PLL is unlocked). Register pointer auto increments.
The PLL locks when PLL_EN is set and, with HS_CLK_SRC = 1, the linked DSIM's HS clock runs.
With EN low the chip is in reset, its registers return to defaults and it NACKs.

Reset values other than the ID and 0x10 (0x26) are 0 and not checked against the
datasheet. The LVDS output, test pattern generator and IRQ pin are not modelled.

### `vivante-gpu`, `imx8mm-gpu3d`, `imx8mm-gpu2d`

| | 3D core | 2D core |
|---|---|---|
| DT | `gpu@38000000`, `fsl,imx8mm-gpu` (galcore.ko) | same node |
| Base / size | 0x38000000 / 0x8000 (`iobase_3d`) | 0x38008000 / 0x8000 (`iobase_2d`) |
| IRQ | GIC SPI 3 (`irq_3d`) | GIC SPI 25 (`irq_2d`) |
| Clocks | GPU3D_ROOT, GPU_BUS_ROOT, GPU_AHB | GPU2D_ROOT, GPU_BUS_ROOT, GPU_AHB |
| Type | `imx8mm-gpu3d` | `imx8mm-gpu2d` |
| Model / rev | GC600 0x4653 (GCNanoUltra) | GC520 0x5341 (GC520L, NXP calls it GC320) |
| Product / ECO / customer | 0x70005 / 0 / 0 | 0x5202 / 0 / 0 |

Identification source: galcore matches model, revision, product, ECO and customer ID
exactly against its feature database and refuses the core otherwise. The database has two
rows for each core: `gc7000nanoultra_4_6_5_rc3a` (customer 0) and `_rc3e` (customer
0x102), `gc520l_5341_rc1b` (customer 0) and `_rc1c` (customer 0x204). The customer 0 rows
are used; 0x204 is the GC520L variant mainline etnaviv lists, which is the one on the
i.MX8MP. **This choice should be confirmed on a real KICK** (`dmesg`/
`/sys/kernel/debug/gc/info`); all IDs are QOM properties (`chip-model`, `chip-revision`,
`chip-date`, `chip-time`, `product-id`, `eco-id`, `customer-id`, `chip-features`,
`minor-features0..6`) so they can be changed without code changes. The feature registers
are rebuilt from the database row by `Emulator/qemu/scripts/gen-vivante-ids.py`
(the REG_* columns are the bits of HI_CHIP_FEATURE and MINOR_FEATURE_0..6). CHIP_DATE and
CHIP_TIME are not in the database and read 0.

Other QOM properties: `wait-poll-min-ns`/`wait-poll-max-ns` (10 us / 1 ms, WAIT/LINK
re-fetch interval, doubled while the FE idles), `fe-batch` (4096 commands per timer
callback), link `dma-mr`.

Register coverage (byte offsets):
- 0x000 CLOCK_CONTROL: SOFT_RESET resets the core (FE, MMU, states, interrupts),
  FSCALE_CMD_LOAD self clears, IDLE_3D/IDLE_2D read 1, reset 0x00010900.
- 0x004 IDLE_STATE: 0x7fffffff, FE bit clear while the FE runs (also in the WAIT/LINK loop,
  as `gckHARDWARE_QueryIdle()` expects).
- 0x008/0x00c AXI_CONFIG/STATUS (STATUS records read/write errors), 0x010
  INTR_ACKNOWLEDGE (read to clear; bit 30 MMU exception, bit 31 AXI error, bits 0-29
  events), 0x014 INTR_ENBL.
- Identification 0x018-0x034, 0x074, 0x084, 0x088, 0x094, 0x0a0, 0x0a8, 0x0dc, 0x0e8;
  0x0d4 BLT_INTR reads 0.
- 0x100-0x10c PM power/module controls, module status (reads 0), pulse eater.
- 0x180 MMUv2 SAFE_ADDRESS, 0x184 CONFIGURATION (masked fields), 0x188 STATUS (read to
  clear, 4 bits per unit: FE 0, pixel/2D engine 1), 0x18c CONTROL (enable), 0x190-0x19c
  EXCEPTION_ADDR. The secure MMU registers 0x388/0x3a4/0x3a8 log guest errors (these cores
  have no SECURITY_AHB).
- 0x654 FE command address, 0x658 command control (ENABLE starts the FE), 0x664 DMA
  address (address of the current command; END address + 8 after END), 0x668/0x66c current
  command words, 0x65c/0x660 debug state read 0.
- All other offsets below 0x1000 are kept as storage (MC base addresses, debug selects,
  galcore tuning registers); the 0x1000-0x7fff window logs LOG_UNIMP.

MMUv2: 1K mode (256 x 16 MiB master entries, which galcore 6.4 uses) and 4K mode
(1024 x 4 MiB); 4K, 64K, 1M and 16M slave pages; writable bit; 40-bit physical addresses
from STLB bits 11:4. Not-present master or slave entries and writes to read-only pages
raise the MMU exception interrupt, latch status and address, and the access goes to the
safe page. The model keeps no TLB, so table changes take effect immediately (galcore always
flushes). The FE fetches through the MMU once CONTROL.ENABLE is set; galcore programs
CONFIGURATION/SAFE_ADDRESS through LOAD_STATE 0x0061/0x0060 as `_ProgramMMUStates()` does.

Front end: LOAD_STATE (count 0 = 1024, states below 0x400 alias the MMU registers),
END (optional event), NOP, DRAW_2D, DRAW_PRIMITIVES, DRAW_INDEXED_PRIMITIVES,
DRAW_INSTANCED, WAIT, LINK, STALL, CALL/RETURN (one level), CHIP_SELECT (chip 0). Unknown
opcodes (including WAIT_FENCE, which these cores do not have) stop the FE with a guest
error. GL states with side effects: PIPE_SELECT, EVENT (raises the event bit), SEMAPHORE,
cache and MMU flushes (no-ops, nothing is outstanding). Execution is synchronous in a
virtual-clock timer, so STALL and semaphores complete at once and events fire after all
previous work.

2D engine (`vivante_de.c`, PE2.0 states 0x1200-0x13ff, 0x3400):
- commands CLEAR (CLEAR_PIXEL_VALUE32), LINE, BIT_BLT, BIT_BLT_REVERSED (bottom-up),
  STRETCH_BLT (16.16 factors), HOR/VER/ONE_PASS_FILTER_BLT and VR_CONFIG starts;
- formats A/X4R4G4B4, A/X1R5G5B5, R5G6B5, A/X8R8G8B8, A8 with ARGB/RGBA/ABGR/BGRA swizzles
  as source and destination; INDEX8 (INDEX_COLOR_TABLE32), YUY2, UYVY, NV12, NV16, YV12 as
  source with BT.601/BT.709 (PE_CONTROL) and UV swap;
- linear and 4x4 tiled surfaces; clipping; relative or absolute source origin;
- ROP2 (pattern or source), ROP3, ROP4 with the transparency mask; 8x8 mono pattern
  (PATTERN_LOW/HIGH with FG/BG colours) or colour pattern from memory;
- source colour key (single or DirectFB range), destination colour key;
- alpha blending with all eight factors, global alpha (replace/scale), inversed alpha,
  premultiply of source/destination, global premultiply, demultiply;
- masked state writes for ROT_ANGLE, PE_CONTROL, PE_TRANSPARENCY, PE_CONFIG, VR_CONFIG,
  VR_CONFIG_EX, BW_CONFIG;
- source and destination rotation (ROT_ANGLE, or the PE1.0 ROTATION bit as 90 degrees)
  and mirroring (ROT_ANGLE mirrors, CONFIG.MIRROR_BLT).

2D gaps and approximations: filter blits sample bilinearly and ignore FILTER_KERNEL taps;
the rotation convention (ROT90 = logical picture turned 90 degrees clockwise) and the
mono pattern bit order follow the description in `state_2d.xml` and have not been compared
against hardware or libg2d output; monochrome and masked stream sources (DRAW_2D inline
data), super tiling, multi-source blits (the GC520L lacks the feature), dithering, YUV or
indexed destinations, CLEAR_BYTE_MASK and the DEC/TS compression states are not
supported and log LOG_UNIMP.

3D core: the 3D command stream is accepted and all states are stored, draws are skipped
with LOG_UNIMP per draw and a one-time warning. galcore's dummy draw and flop-reset
command buffers complete because their events still fire.

#### Plan for 3D execution

The GC600 uses the pre-Halti shader ISA etnaviv documents in `isa.xml` (four 32-bit words
per instruction, up to 256 instructions, 64 temporaries, 320 uniforms). A software path
would be:
1. Decode the vertex fetch states (FE_VERTEX_ELEMENT_CONFIG, FE_VERTEX_STREAM_*,
   FE_INDEX_STREAM_*) and DRAW_PRIMITIVES/DRAW_INDEXED_PRIMITIVES into vertex arrays.
2. Interpret the vertex shader (VS_INST_MEM, VS_UNIFORMS, VS_INPUT/OUTPUT) per vertex.
3. Primitive assembly, clipping, viewport (PA_VIEWPORT_*), culling (PA_CONFIG), then a
   tiled half-space rasteriser generating fragments with varyings (GL_VARYING_*).
4. Interpret the pixel shader (PS_INST_MEM, PS_UNIFORMS) with texture sampling from
   TE_SAMPLER_* (linear, tiled and supertiled layouts, ETC1/DXT formats).
5. PE: depth/stencil (PE_DEPTH_*, PE_STENCIL_*), blending (PE_ALPHA_*), colour write in the
   render target layout, tile status and fast clear (TS_*), and the RS resolve engine
   (RS_*) that galcore uses to detile into scanout buffers.
6. Validate against etnaviv's piglit/deqp runs on the same IDs before relying on it.
The RS resolve of step 5 alone would already cover most EGL buffer copies.

## Wiring

These are the connections the SoC (`fsl-imx8mm.c`) and the board (`kick-board.c`) need.
`kick-display-bench.c` shows the same wiring without IRQs and clocks.

```c
/* SoC: display mix, LCDIF, DSIM. */
object_initialize_child(obj, "dispmix", &s->dispmix, TYPE_IMX8MM_DISPMIX);
object_initialize_child(obj, "lcdif", &s->lcdif, TYPE_IMX8MM_LCDIF);
object_initialize_child(obj, "dsim", &s->dsim, TYPE_IMX8MM_MIPI_DSIM);
object_initialize_child(obj, "gpu3d", &s->gpu3d, TYPE_IMX8MM_GPU3D);
object_initialize_child(obj, "gpu2d", &s->gpu2d, TYPE_IMX8MM_GPU2D);

sysbus_realize(SYS_BUS_DEVICE(&s->dispmix), &error_abort);
sysbus_mmio_map(SYS_BUS_DEVICE(&s->dispmix), 0, 0x32e28000);

sysbus_realize(SYS_BUS_DEVICE(&s->lcdif), &error_abort);
sysbus_mmio_map(SYS_BUS_DEVICE(&s->lcdif), 0, 0x32e00000);
sysbus_connect_irq(SYS_BUS_DEVICE(&s->lcdif), 0, qdev_get_gpio_in(gic, 5));
/* optional: qdev_connect_clock_in(DEVICE(&s->lcdif), "pix", <CCM LCDIF_PIXEL>); */

sysbus_realize(SYS_BUS_DEVICE(&s->dsim), &error_abort);
sysbus_mmio_map(SYS_BUS_DEVICE(&s->dsim), 0, 0x32e10000);
sysbus_connect_irq(SYS_BUS_DEVICE(&s->dsim), 0, qdev_get_gpio_in(gic, 18));
/* optional: qdev_connect_clock_in(DEVICE(&s->dsim), "pll-ref", <CCM DSI_PHY_REF>); */
/* optional: MIPI_DSI_I_PRESET (sft-rstn 5) holds the DSIM in reset when low. */
qdev_connect_gpio_out_named(DEVICE(&s->dispmix), "sft-rstn", 5,
                            qdev_get_gpio_in_named(DEVICE(&s->dsim), "reset-n", 0));

sysbus_realize(SYS_BUS_DEVICE(&s->gpu3d), &error_abort);
sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpu3d), 0, 0x38000000);
sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpu3d), 0, qdev_get_gpio_in(gic, 3));
sysbus_realize(SYS_BUS_DEVICE(&s->gpu2d), &error_abort);
sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpu2d), 0, 0x38008000);
sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpu2d), 0, qdev_get_gpio_in(gic, 25));
```

Connecting the dispmix reset line means the guest must deassert it (the vendor reset
driver does) before the DSIM answers; leave it out to keep the DSIM always out of reset.

```c
/* Board: SN65DSI84 on I2C1, EN from GPIO2_IO11, linked to the DSIM. */
I2CSlave *br = i2c_slave_new(TYPE_SN65DSI83, 0x2c);
object_property_set_link(OBJECT(br), "dsi-host", OBJECT(&soc->dsim), &error_abort);
i2c_slave_realize_and_unref(br, soc->i2c[0].bus, &error_abort);
qdev_connect_gpio_out(DEVICE(&soc->gpio[1]), 11,
                      qdev_get_gpio_in_named(DEVICE(br), "enable", 0));
```

The board sets the LCDIF `panel-width`/`panel-height` to the size the host application
should see before the kernel sets a mode (the DT mode is 1024x600).

Kconfig symbols: `IMX8MM_DISPMIX`, `IMX8MM_LCDIF`, `IMX8MM_MIPI_DSIM`, `SN65DSI83`
(selects `IMX8MM_MIPI_DSIM`), `VIVANTE_GPU`; the SoC selects them.

## Tests

`kick-display-bench` is a qtest-only machine (one idle Cortex-A53, RAM at 0x40000000) that
maps dispmix, LCDIF, DSIM, I2C1 with the SN65DSI84 and both GPU cores at their i.MX8MM
addresses.

| Test | Covers |
|---|---|
| `kick-lcdif-test` | reset values, SET/CLR/TOG, soft reset, frame IRQs and NEXT_BUF flip, scanout of XRGB8888, ABGR8888 and cropped RGB565 patterns checked through QMP `screendump` |
| `kick-dsim-test` | DSIM reset values, the `sec-dsim` enable sequence (PLL lock IRQ, lane states, FIFO init, HS clock), long/short packets and a read timing out; SN65DSI84 EN/NACK, ID, probe/configure/start-stream sequence, PLL lock from the DSIM HS clock, W1C errors |
| `kick-gpu-test` | ID registers of both cores, galcore reset sequence, WAIT/LINK loop with events and a commit, idle/DMA address semantics, MMUv2 setup and translation, both exception kinds, skipped 3D draw completing, 2D clear with clipping, blit with RGB565 to ARGB8888 conversion, ROP, SRC_OVER blending, stretch, rotation and 2D access through the MMU |

```sh
meson test -C Emulator/out/qemu-build --suite qtest-aarch64 'qtest-aarch64/kick-*'
```

## Open issues

- Not yet run against the real kernel and galcore: the `kick` machine does not exist yet.
  Once it does, boot `Emulator/out/boot/Image` with `galcore.ko` and check the probe
  (`[galcore]: Feature database is not found` means the ID choice above is wrong).
- Customer ID choice for both GPU cores (see above).
- Unverified reset values (DSIM, SN65DSI84) and 2D rotation/mirror conventions.
- libg2d's exact command streams were not available, so the 2D state usage is covered by
  the rnndb, not by captured traces. A trace of `g2d_blit`/`g2d_clear` from a real board
  would allow a direct check.
