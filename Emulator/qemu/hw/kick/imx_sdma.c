/*
 * i.MX Smart DMA controller (SDMA v2, i.MX 8M Mini)
 *
 * Model of the SDMA as seen by the Linux imx-sdma driver for
 * "fsl,imx8mq-sdma" (i.MX 8M Mini SDMA1/2/3). Reference: i.MX 8M Mini
 * Reference Manual, chapter "Smart Direct Memory Access Controller (SDMA)",
 * and drivers/dma/imx-sdma.c of the vendor 5.4 kernel.
 *
 * Approach: the SDMA RISC core is not emulated instruction by instruction.
 * Its ROM (boot script and the standard scripts) is not public, so an ISA
 * interpreter could not run the scripts the driver uses anyway. Instead:
 *
 *  - Channel 0 implements the ROM boot script command set (C0_SETDM,
 *    C0_GETDM, C0_SETPM, C0_GETPM, C0_SETCTX, C0_GETCTX) on the internal
 *    RAM, so firmware download, context load and context save work as on
 *    hardware and are visible through GETDM/GETPM.
 *  - Other channels execute the semantics of the ROM script found at the
 *    channel context PC, using the script addresses of the i.MX 7D/8M SDMA
 *    ROM (Linux sdma_script_imx7d): ap_2_ap, app_2_mcu, mcu_2_app,
 *    uart_2_mcu, shp_2_mcu, mcu_2_shp, uartsh_2_mcu. These are all the
 *    scripts the KICK kernel selects: SAI2 (DT peripheral type 2 = MMC) uses
 *    shp_2_mcu/mcu_2_shp, UARTs uart_2_mcu/mcu_2_app, memcpy ap_2_ap.
 *  - With the RAM firmware (sdma-imx7d.bin, as on the KICK rootfs) the
 *    driver takes every entry point from the firmware header, and the
 *    5.4 kernel has no ROM entry for uart_2_mcu at all (it is a v3 field).
 *    The "firmware" property names the same file; its header is read for
 *    the RAM entry points (sdma_fw_scripts), which then run the semantics
 *    of the matching script. The RAM code itself is not interpreted.
 *  - A PC that is not one of those (for example mcu_2_ecspi, or a RAM
 *    script without the "firmware" property) is logged as LOG_UNIMP and
 *    the transfer is terminated with BD_RROR and an interrupt, so the
 *    driver reports an error instead of hanging.
 *
 * Script semantics (per context: gReg[6] peripheral FIFO address, gReg[7]
 * watermark in bytes; per BD: count, buffer address, command = access
 * width 0:32 1:8 2:16 3:24 bit):
 *  - memory to peripheral: on each DMA request write watermark bytes from
 *    the buffer to the FIFO register, per access width;
 *  - peripheral to memory: on each DMA request read watermark bytes;
 *  - UART receive: read URXD while CHARRDY is set, up to the watermark;
 *    a request from the UART ageing timer or idle detect drains the FIFO,
 *    closes the buffer early and clears USR1.AGTIM / USR2.IDLE;
 *  - ap_2_ap: copy count bytes from buffer to extended buffer address.
 * A completed BD gets DONE cleared (and its count set to the received byte
 * count for peripheral to memory); BD_INTR raises the channel bit in INTR;
 * BD_WRAP continues at the base BD of the channel control block; a BD
 * without BD_CONT ends the channel (HE cleared). When the next BD is still
 * owned by the CPU the channel waits for it and rechecks every bd-poll-us
 * while its request is asserted.
 *
 * Scheduling: a channel is eligible when (HE | HOSTOVR) & (EP | EVTOVR)
 * & (DSPENBL | DSPOVR) and its priority is not 0, highest priority first.
 * Events are level inputs. EP is set on a rising edge of an event enabled
 * in CHNENBL and stays set while the request is asserted (the peripheral
 * keeps it high until its FIFO is serviced). A rising edge while EP is
 * already set is an overrun and sets EVTERR. Scripts run from a bottom
 * half so that peripheral accesses never re-enter a device that is raising
 * its request from its own MMIO handler.
 *
 * Not modelled: the OnCE debug interface (registers are stored, commands
 * are LOG_UNIMP), the DSP side, cross triggers (XTRIG_CONF stored),
 * script state across a context save/restore (a restored context restarts
 * its script from the context PC).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "imx_sdma.h"

/* Host register offsets */
#define SDMA_MC0PTR         0x000
#define SDMA_INTR           0x004
#define SDMA_STOP_STAT      0x008
#define SDMA_HSTART         0x00c
#define SDMA_EVTOVR         0x010
#define SDMA_DSPOVR         0x014
#define SDMA_HOSTOVR        0x018
#define SDMA_EVTPEND        0x01c
#define SDMA_DSPENBL        0x020
#define SDMA_RESET          0x024
#define SDMA_EVTERR         0x028
#define SDMA_INTRMASK       0x02c
#define SDMA_PSW            0x030
#define SDMA_EVTERRDBG      0x034
#define SDMA_CONFIG         0x038
#define SDMA_LOCK           0x03c
#define SDMA_ONCE_ENB       0x040
#define SDMA_ONCE_DATA      0x044
#define SDMA_ONCE_INSTR     0x048
#define SDMA_ONCE_STAT      0x04c
#define SDMA_ONCE_CMD       0x050
#define SDMA_EVT_MIRROR     0x054
#define SDMA_ILLINSTADDR    0x058
#define SDMA_CHN0ADDR       0x05c
#define SDMA_EVT_MIRROR2    0x060
#define SDMA_XTRIG_CONF1    0x070
#define SDMA_XTRIG_CONF2    0x074
#define SDMA_CHNPRI0        0x100
#define SDMA_CHNENBL0       0x200

#define RESET_RESET         BIT(0)
#define RESET_RESCHED       BIT(1)
#define CONFIG_RW_MASK      0x00001813u     /* DSPDMA, RTDOBS, ACR, CSM */
#define CHN0ADDR_SMSZ       BIT(14)         /* 32 word contexts */
#define CHN0ADDR_RW_MASK    0x00007fffu

/* SDMA memory map (data memory word addresses) */
#define SDMA_DM_RAM_BASE    0x0800
#define SDMA_CTX_BASE       0x0800
#define SDMA_PM_RAM_BASE    (SDMA_DM_RAM_BASE * 2)

/* Channel control block in host memory: current BD, base BD, 2 reserved */
#define CCB_SIZE            16

/* Buffer descriptor */
#define BD_DONE             0x01
#define BD_WRAP             0x02
#define BD_CONT             0x04
#define BD_INTR             0x08
#define BD_RROR             0x10
#define BD_LAST             0x20
#define BD_EXTD             0x80

/* Channel 0 commands */
#define C0_SETDM            0x01
#define C0_GETDM            0x02
#define C0_SETPM            0x04
#define C0_GETPM            0x08
#define C0_CTX_MASK         0x07
#define C0_SETCTX           0x07
#define C0_GETCTX           0x03

/*
 * UART registers seen by uart_2_mcu, relative to the URXD address the
 * driver passes as the peripheral address
 */
#define UART_USR1           0x94
#define UART_USR2           0x98
#define URXD_CHARRDY        BIT(15)
#define URXD_ERR            BIT(14)
#define USR1_AGTIM          BIT(8)
#define USR2_IDLE           BIT(12)

typedef enum {
    SCRIPT_NONE,
    SCRIPT_AP_2_AP,
    SCRIPT_PER_2_MCU,       /* app_2_mcu, shp_2_mcu */
    SCRIPT_MCU_2_PER,       /* mcu_2_app, mcu_2_shp */
    SCRIPT_UART_2_MCU,      /* uart_2_mcu, uartsh_2_mcu */
    SCRIPT_UNKNOWN,
} SDMAScriptKind;

typedef struct SDMAScript {
    uint32_t pc;
    const char *name;
    SDMAScriptKind kind;
} SDMAScript;

/* i.MX 7D / 8M SDMA ROM script entry points (program memory addresses) */
static const SDMAScript sdma_rom_scripts[] = {
    { 644,  "ap_2_ap",      SCRIPT_AP_2_AP },
    { 685,  "app_2_mcu",    SCRIPT_PER_2_MCU },
    { 749,  "mcu_2_app",    SCRIPT_MCU_2_PER },
    { 819,  "uart_2_mcu",   SCRIPT_UART_2_MCU },
    { 893,  "shp_2_mcu",    SCRIPT_PER_2_MCU },
    { 962,  "mcu_2_shp",    SCRIPT_MCU_2_PER },
    { 1034, "uartsh_2_mcu", SCRIPT_UART_2_MCU },
};

/*
 * RAM scripts of the external firmware (sdma-imx7d.bin), by their index in
 * the header's script address table (Linux struct sdma_script_start_addrs).
 * The firmware is not shipped; its header is only read to learn where the
 * guest's copy places each script, the code itself is not interpreted. Only
 * scripts whose semantics the model implements are listed.
 */
static const struct {
    uint32_t index;
    const char *name;
    SDMAScriptKind kind;
} sdma_fw_scripts[] = {
    { 0,  "ap_2_ap",            SCRIPT_AP_2_AP },
    { 11, "uart_2_mcu_ram",     SCRIPT_UART_2_MCU },
    { 13, "mcu_2_app",          SCRIPT_MCU_2_PER },
    { 16, "uartsh_2_mcu_ram",   SCRIPT_UART_2_MCU },
    { 18, "mcu_2_shp",          SCRIPT_MCU_2_PER },
    { 22, "app_2_mcu",          SCRIPT_PER_2_MCU },
    { 24, "shp_2_mcu",          SCRIPT_PER_2_MCU },
    { 41, "mcu_2_sai",          SCRIPT_MCU_2_PER },
    { 42, "sai_2_mcu",          SCRIPT_PER_2_MCU },
    { 43, "uart_2_mcu",         SCRIPT_UART_2_MCU },
    { 44, "uartsh_2_mcu",       SCRIPT_UART_2_MCU },
};

#define SDMA_FW_MAGIC       0x414d4453      /* "SDMA" */
#define SDMA_FW_HDR_WORDS   7

typedef struct SDMABD {
    uint32_t addr;
    uint16_t count;
    uint8_t status;
    uint8_t command;
    uint32_t buf;
    uint32_t ext;
} SDMABD;

static void sdma_update_irq(IMXSDMAState *s)
{
    qemu_set_irq(s->irq, s->intr != 0);
}

static uint32_t sdma_ctx_words(IMXSDMAState *s)
{
    return (s->chn0addr & CHN0ADDR_SMSZ) ? 32 : 24;
}

/* Bus master accesses */
static uint32_t sdma_ldl(IMXSDMAState *s, uint32_t addr)
{
    uint32_t v = 0;

    if (dma_memory_read(s->dma_as, addr, &v, 4, MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bus error reading 0x%08x\n",
                      TYPE_IMX_SDMA, addr);
    }
    return le32_to_cpu(v);
}

static void sdma_stl(IMXSDMAState *s, uint32_t addr, uint32_t val)
{
    val = cpu_to_le32(val);
    if (dma_memory_write(s->dma_as, addr, &val, 4, MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bus error writing 0x%08x\n",
                      TYPE_IMX_SDMA, addr);
    }
}

static void sdma_read_bd(IMXSDMAState *s, uint32_t addr, SDMABD *bd)
{
    uint32_t mode = sdma_ldl(s, addr);

    bd->addr = addr;
    bd->count = mode & 0xffff;
    bd->status = (mode >> 16) & 0xff;
    bd->command = mode >> 24;
    bd->buf = sdma_ldl(s, addr + 4);
    bd->ext = (bd->status & BD_EXTD) ? sdma_ldl(s, addr + 8) : 0;
}

static void sdma_write_bd_mode(IMXSDMAState *s, SDMABD *bd)
{
    sdma_stl(s, bd->addr, bd->count | (bd->status << 16) |
                          ((uint32_t)bd->command << 24));
}

static uint32_t sdma_ccb(IMXSDMAState *s, int ch)
{
    return s->mc0ptr + ch * CCB_SIZE;
}

/* Internal RAM, data memory view */
static bool sdma_dm_valid(IMXSDMAState *s, uint32_t dm)
{
    return dm >= SDMA_DM_RAM_BASE && dm - SDMA_DM_RAM_BASE < s->ram_words;
}

static uint32_t sdma_dm_read(IMXSDMAState *s, uint32_t dm)
{
    if (!sdma_dm_valid(s, dm)) {
        qemu_log_mask(dm < SDMA_DM_RAM_BASE ? LOG_UNIMP : LOG_GUEST_ERROR,
                      "%s: data memory 0x%04x is %s\n", TYPE_IMX_SDMA, dm,
                      dm < SDMA_DM_RAM_BASE ? "ROM (content not available)"
                                            : "outside RAM");
        return 0;
    }
    return s->ram[dm - SDMA_DM_RAM_BASE];
}

static void sdma_ctx_written(IMXSDMAState *s, uint32_t dm)
{
    uint32_t words = sdma_ctx_words(s);
    int ch;

    if (dm < SDMA_CTX_BASE || dm >= SDMA_CTX_BASE + words * IMX_SDMA_CHANNELS) {
        return;
    }
    ch = (dm - SDMA_CTX_BASE) / words;
    s->chan[ch].fresh = true;
    s->chan[ch].active = false;
    s->chan[ch].blocked = false;
}

static void sdma_dm_write(IMXSDMAState *s, uint32_t dm, uint32_t val)
{
    if (!sdma_dm_valid(s, dm)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to data memory 0x%04x "
                      "outside RAM\n", TYPE_IMX_SDMA, dm);
        return;
    }
    s->ram[dm - SDMA_DM_RAM_BASE] = val;
    sdma_ctx_written(s, dm);
}

/*
 * Program memory view: 16 bit words, two per data memory word. The core is
 * big endian, the even program address is the upper half of the data word.
 */
static uint16_t sdma_pm_read(IMXSDMAState *s, uint32_t pm)
{
    uint32_t w = sdma_dm_read(s, pm >> 1);

    return (pm & 1) ? w & 0xffff : w >> 16;
}

static void sdma_pm_write(IMXSDMAState *s, uint32_t pm, uint16_t val)
{
    uint32_t dm = pm >> 1;
    uint32_t w;

    if (!sdma_dm_valid(s, dm)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to program memory 0x%04x "
                      "outside RAM\n", TYPE_IMX_SDMA, pm);
        return;
    }
    w = s->ram[dm - SDMA_DM_RAM_BASE];
    w = (pm & 1) ? (w & 0xffff0000) | val : (w & 0xffff) | (val << 16);
    sdma_dm_write(s, dm, w);
}

/* Channel 0: execute one boot script command */
static void sdma_c0_command(IMXSDMAState *s, SDMABD *bd)
{
    uint32_t i;

    if ((bd->command & C0_CTX_MASK) == C0_SETCTX ||
        (bd->command & C0_CTX_MASK) == C0_GETCTX) {
        int ch = bd->command >> 3;
        uint32_t base = SDMA_CTX_BASE + ch * sdma_ctx_words(s);
        bool set = (bd->command & C0_CTX_MASK) == C0_SETCTX;

        for (i = 0; i < bd->count; i++) {
            if (set) {
                sdma_dm_write(s, base + i, sdma_ldl(s, bd->buf + i * 4));
            } else {
                sdma_stl(s, bd->buf + i * 4, sdma_dm_read(s, base + i));
            }
        }
        return;
    }

    switch (bd->command) {
    case C0_SETDM:
        for (i = 0; i < bd->count; i++) {
            sdma_dm_write(s, bd->ext + i, sdma_ldl(s, bd->buf + i * 4));
        }
        break;
    case C0_GETDM:
        for (i = 0; i < bd->count; i++) {
            sdma_stl(s, bd->buf + i * 4, sdma_dm_read(s, bd->ext + i));
        }
        break;
    case C0_SETPM:
        for (i = 0; i < bd->count; i++) {
            uint16_t v;

            dma_memory_read(s->dma_as, bd->buf + i * 2, &v, 2,
                            MEMTXATTRS_UNSPECIFIED);
            sdma_pm_write(s, bd->ext + i, le16_to_cpu(v));
        }
        break;
    case C0_GETPM:
        for (i = 0; i < bd->count; i++) {
            uint16_t v = cpu_to_le16(sdma_pm_read(s, bd->ext + i));

            dma_memory_write(s->dma_as, bd->buf + i * 2, &v, 2,
                             MEMTXATTRS_UNSPECIFIED);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: channel 0 command 0x%02x not "
                      "implemented\n", TYPE_IMX_SDMA, bd->command);
        bd->status |= BD_RROR;
        break;
    }
}

static void sdma_run_channel0(IMXSDMAState *s)
{
    uint32_t base = sdma_ldl(s, sdma_ccb(s, 0) + 4);
    uint32_t addr = sdma_ldl(s, sdma_ccb(s, 0));
    int guard;

    /* Process the chain of BDs owned by the SDMA */
    for (guard = 0; guard < 4096; guard++) {
        SDMABD bd;

        sdma_read_bd(s, addr, &bd);
        if (!(bd.status & BD_DONE)) {
            break;
        }
        sdma_c0_command(s, &bd);
        bd.status &= ~BD_DONE;
        sdma_write_bd_mode(s, &bd);
        if (bd.status & BD_INTR) {
            s->intr |= BIT(0);
        }
        if (!(bd.status & BD_CONT)) {
            break;
        }
        addr = (bd.status & BD_WRAP) ? base :
               addr + ((bd.status & BD_EXTD) ? 12 : 8);
    }
    s->psw = 0;
    s->he &= ~BIT(0);
    sdma_update_irq(s);
}

static uint32_t sdma_event_level_mask(IMXSDMAState *s, int ch)
{
    uint32_t mask = 0;
    int e;

    for (e = 0; e < IMX_SDMA_EVENTS; e++) {
        if ((s->chnenbl[e] & BIT(ch)) && (s->evt_level & BIT_ULL(e))) {
            mask = 1;
            break;
        }
    }
    return mask;
}

static bool sdma_runnable(IMXSDMAState *s, int ch)
{
    uint32_t b = BIT(ch);

    return s->mc0ptr && ch != 0 && s->chnpri[ch] &&
           ((s->he | s->hostovr) & b) &&
           ((s->evtpend | s->evtovr) & b) &&
           ((s->dspenbl | s->dspovr) & b) &&
           !s->chan[ch].blocked;
}

static uint32_t sdma_access_width(uint8_t command)
{
    /* BD command for peripheral scripts: 0 = 32 bit, else bytes */
    return command == 0 || command > 3 ? 4 : command;
}

/*
 * General register n of a channel context. The context starts with the
 * two-word channel state (PC/RPC/SPC/EPC and flags, Linux struct
 * sdma_state_registers), so gReg[0] is word 2.
 */
static uint32_t sdma_ctx_greg(IMXSDMAState *s, int ch, int n)
{
    return sdma_dm_read(s, SDMA_CTX_BASE + ch * sdma_ctx_words(s) + 2 + n);
}

static void sdma_start_script(IMXSDMAState *s, int ch)
{
    IMXSDMAChannel *c = &s->chan[ch];
    uint32_t state = sdma_dm_read(s, SDMA_CTX_BASE + ch * sdma_ctx_words(s));
    unsigned i;

    c->pc = state & 0x3fff;
    c->script = s->nscripts;
    for (i = 0; i < s->nscripts; i++) {
        if (s->scripts[i].pc == c->pc) {
            c->script = i;
            break;
        }
    }
    c->bd = sdma_ldl(s, sdma_ccb(s, ch));
    c->bd_bytes = 0;
    c->fresh = false;
    c->active = true;
    c->blocked = false;
}

static SDMAScriptKind sdma_kind(IMXSDMAState *s, IMXSDMAChannel *c)
{
    return c->script < s->nscripts ? s->scripts[c->script].kind
                                   : SCRIPT_UNKNOWN;
}

/* The channel ran out of work: clear HE, the script is done */
static void sdma_channel_done(IMXSDMAState *s, int ch)
{
    s->he &= ~BIT(ch);
    s->evtpend &= ~BIT(ch);
    s->chan[ch].active = false;
    s->chan[ch].fresh = true;
}

/* Close the current BD and advance. Returns false if the channel ended. */
static bool sdma_complete_bd(IMXSDMAState *s, int ch, SDMABD *bd)
{
    IMXSDMAChannel *c = &s->chan[ch];

    bd->status &= ~BD_DONE;
    sdma_write_bd_mode(s, bd);
    if (bd->status & BD_INTR) {
        s->intr |= BIT(ch);
    }
    c->bd_bytes = 0;
    if (!(bd->status & BD_CONT)) {
        sdma_channel_done(s, ch);
        return false;
    }
    c->bd = (bd->status & BD_WRAP) ? sdma_ldl(s, sdma_ccb(s, ch) + 4) :
            bd->addr + ((bd->status & BD_EXTD) ? 12 : 8);
    return true;
}

/*
 * Fetch the current BD. If the CPU still owns it the channel waits:
 * returns false and marks it blocked until the next poll or event.
 */
static bool sdma_fetch_bd(IMXSDMAState *s, int ch, SDMABD *bd)
{
    IMXSDMAChannel *c = &s->chan[ch];

    sdma_read_bd(s, c->bd, bd);
    if (bd->status & BD_DONE) {
        c->blocked = false;
        return true;
    }
    c->blocked = true;
    if (s->bd_poll_us) {
        timer_mod(s->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->bd_poll_us * SCALE_US);
    }
    return false;
}

static void sdma_script_error(IMXSDMAState *s, int ch)
{
    IMXSDMAChannel *c = &s->chan[ch];
    SDMABD bd;

    qemu_log_mask(LOG_UNIMP, "%s: channel %d: script at PC %u is not one of "
                  "the modelled ROM scripts, transfer aborted\n",
                  TYPE_IMX_SDMA, ch, c->pc);
    sdma_read_bd(s, c->bd, &bd);
    if (bd.status & BD_DONE) {
        bd.status = (bd.status & ~BD_DONE) | BD_RROR;
        sdma_write_bd_mode(s, &bd);
    }
    s->intr |= BIT(ch);
    sdma_channel_done(s, ch);
}

static void sdma_ap_2_ap(IMXSDMAState *s, int ch)
{
    uint8_t buf[256];
    int guard;

    for (guard = 0; guard < 65536; guard++) {
        SDMABD bd;
        uint32_t off;

        if (!sdma_fetch_bd(s, ch, &bd)) {
            return;
        }
        for (off = 0; off < bd.count; off += sizeof(buf)) {
            uint32_t n = MIN(sizeof(buf), bd.count - off);

            if (dma_memory_read(s->dma_as, bd.buf + off, buf, n,
                                MEMTXATTRS_UNSPECIFIED) ||
                dma_memory_write(s->dma_as, bd.ext + off, buf, n,
                                 MEMTXATTRS_UNSPECIFIED)) {
                bd.status |= BD_RROR;
                break;
            }
        }
        if (!sdma_complete_bd(s, ch, &bd)) {
            return;
        }
    }
}

/* One DMA request worth of a peripheral script */
static void sdma_per_burst(IMXSDMAState *s, int ch)
{
    IMXSDMAChannel *c = &s->chan[ch];
    SDMAScriptKind kind = sdma_kind(s, c);
    uint32_t per = sdma_ctx_greg(s, ch, 6);
    uint32_t wml = sdma_ctx_greg(s, ch, 7) & 0xffff;
    SDMABD bd;
    uint32_t width, burst, done = 0;

    if (!sdma_fetch_bd(s, ch, &bd)) {
        return;
    }
    width = sdma_access_width(bd.command);
    burst = MAX(wml, width);

    while (done < burst && c->bd_bytes < bd.count) {
        uint32_t mem = bd.buf + c->bd_bytes;
        uint32_t v = 0;

        if (kind == SCRIPT_MCU_2_PER) {
            dma_memory_read(s->dma_as, mem, &v, width, MEMTXATTRS_UNSPECIFIED);
            sdma_stl(s, per, le32_to_cpu(v));
        } else {
            v = cpu_to_le32(sdma_ldl(s, per));
            dma_memory_write(s->dma_as, mem, &v, width, MEMTXATTRS_UNSPECIFIED);
        }
        c->bd_bytes += width;
        done += width;
    }

    if (c->bd_bytes >= bd.count) {
        if (kind != SCRIPT_MCU_2_PER) {
            /* Peripheral to memory reports the received byte count */
            bd.count = MIN(c->bd_bytes, bd.count);
        }
        sdma_complete_bd(s, ch, &bd);
    }
}

/*
 * uart_2_mcu / uartsh_2_mcu, one DMA request. The UART raises its RX
 * request for RRDY (RX FIFO at RXTL), for the ageing timer (AGTIM) or for
 * idle detect (IDLE):
 *  - RRDY: read watermark characters from URXD, continuing in the next BD
 *    when one fills up;
 *  - AGTIM or IDLE: the line went quiet, drain the FIFO, close the current
 *    BD with the number of bytes received so far (the driver takes the
 *    count from the BD) and clear the flag in the UART so the request
 *    drops. The next characters go to the next BD.
 * A character with URXD.ERR (overrun, frame error, break) marks the BD
 * with BD_RROR. If the next BD still belongs to the CPU the script waits,
 * leaving the flags set so the request stays asserted.
 */
static void sdma_uart_2_mcu(IMXSDMAState *s, int ch)
{
    IMXSDMAChannel *c = &s->chan[ch];
    uint32_t per = sdma_ctx_greg(s, ch, 6);
    uint32_t wml = MAX(sdma_ctx_greg(s, ch, 7) & 0xffff, 1);
    uint32_t usr1 = sdma_ldl(s, per + UART_USR1);
    uint32_t usr2 = sdma_ldl(s, per + UART_USR2);
    bool flush = (usr1 & USR1_AGTIM) || (usr2 & USR2_IDLE);
    uint32_t done = 0;
    int guard;

    for (guard = 0; guard < 65536; guard++) {
        bool empty = false;
        SDMABD bd;

        if (!sdma_fetch_bd(s, ch, &bd)) {
            return;
        }
        while (c->bd_bytes < bd.count && (flush || done < wml)) {
            uint32_t rx = sdma_ldl(s, per);
            uint8_t ch8 = rx & 0xff;

            if (!(rx & URXD_CHARRDY)) {
                empty = true;
                break;
            }
            if (rx & URXD_ERR) {
                bd.status |= BD_RROR;
                sdma_write_bd_mode(s, &bd);
            }
            dma_memory_write(s->dma_as, bd.buf + c->bd_bytes, &ch8, 1,
                             MEMTXATTRS_UNSPECIFIED);
            c->bd_bytes++;
            done++;
        }
        if (c->bd_bytes < bd.count && !(flush && empty)) {
            /* Burst done, the BD stays open for the next request */
            return;
        }
        if (c->bd_bytes) {
            /* Full, or closed early by ageing/idle: report the count */
            bd.count = c->bd_bytes;
            if (!sdma_complete_bd(s, ch, &bd)) {
                break;
            }
        }
        if (empty || (!flush && done >= wml)) {
            break;
        }
    }
    if (flush) {
        sdma_stl(s, per + UART_USR1, usr1 & USR1_AGTIM);
        sdma_stl(s, per + UART_USR2, usr2 & USR2_IDLE);
    }
}

static void sdma_step(IMXSDMAState *s, int ch)
{
    IMXSDMAChannel *c = &s->chan[ch];

    if (c->fresh || !c->active) {
        sdma_start_script(s, ch);
    }
    s->psw = ch | ((s->chnpri[ch] & 7) << 5);

    switch (sdma_kind(s, c)) {
    case SCRIPT_AP_2_AP:
        sdma_ap_2_ap(s, ch);
        break;
    case SCRIPT_PER_2_MCU:
    case SCRIPT_MCU_2_PER:
        sdma_per_burst(s, ch);
        break;
    case SCRIPT_UART_2_MCU:
        sdma_uart_2_mcu(s, ch);
        break;
    default:
        sdma_script_error(s, ch);
        break;
    }
    /* The script yields: EP is consumed, re-latched while still requested */
    if (!(s->evtovr & BIT(ch)) && !sdma_event_level_mask(s, ch)) {
        s->evtpend &= ~BIT(ch);
    }
    if (c->blocked) {
        s->evtpend &= ~BIT(ch);
    }
}

/* Scheduler: run eligible channels, highest priority first */
static void sdma_run(IMXSDMAState *s)
{
    int iter;

    if (s->in_run) {
        return;
    }
    s->in_run = true;
    for (iter = 0; iter < 100000; iter++) {
        int ch, best = -1;

        for (ch = 1; ch < IMX_SDMA_CHANNELS; ch++) {
            if (sdma_runnable(s, ch) &&
                (best < 0 || s->chnpri[ch] > s->chnpri[best])) {
                best = ch;
            }
        }
        if (best < 0) {
            break;
        }
        sdma_step(s, best);
    }
    s->in_run = false;
    sdma_update_irq(s);
}

static void sdma_bh(void *opaque)
{
    sdma_run(opaque);
}

static void sdma_poll(void *opaque)
{
    IMXSDMAState *s = opaque;
    bool again = false;
    int ch;

    for (ch = 1; ch < IMX_SDMA_CHANNELS; ch++) {
        IMXSDMAChannel *c = &s->chan[ch];

        if (!c->blocked) {
            continue;
        }
        c->blocked = false;
        /* Recheck if the channel is still requested */
        if (sdma_event_level_mask(s, ch) || (s->evtovr & BIT(ch))) {
            s->evtpend |= BIT(ch);
            again = true;
        }
    }
    if (again) {
        sdma_run(s);
    }
}

static void sdma_event(void *opaque, int n, int level)
{
    IMXSDMAState *s = opaque;
    bool was = s->evt_level & BIT_ULL(n);
    uint32_t chans = s->chnenbl[n];

    if (level) {
        s->evt_level |= BIT_ULL(n);
    } else {
        s->evt_level &= ~BIT_ULL(n);
    }
    if (!level || was) {
        return;
    }
    /* Rising edge: latch EP, a pending EP means the event was missed */
    s->evterr |= s->evtpend & chans;
    s->evtpend |= chans;
    if (chans) {
        int ch;

        for (ch = 0; ch < IMX_SDMA_CHANNELS; ch++) {
            if (chans & BIT(ch)) {
                s->chan[ch].blocked = false;
            }
        }
        qemu_bh_schedule(s->bh);
    }
}

static void sdma_core_reset(IMXSDMAState *s)
{
    int ch;

    s->he = 0;
    s->evtpend = 0;
    s->psw = 0;
    for (ch = 0; ch < IMX_SDMA_CHANNELS; ch++) {
        s->chan[ch].active = false;
        s->chan[ch].fresh = true;
        s->chan[ch].blocked = false;
    }
    timer_del(s->poll_timer);
}

static uint64_t sdma_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXSDMAState *s = opaque;
    uint32_t val;

    if (offset >= SDMA_CHNPRI0 && offset < SDMA_CHNPRI0 + 4 * IMX_SDMA_CHANNELS) {
        return s->chnpri[(offset - SDMA_CHNPRI0) / 4];
    }
    if (offset >= SDMA_CHNENBL0 &&
        offset < SDMA_CHNENBL0 + 4 * IMX_SDMA_EVENTS) {
        return s->chnenbl[(offset - SDMA_CHNENBL0) / 4];
    }

    switch (offset) {
    case SDMA_MC0PTR:
        return s->mc0ptr;
    case SDMA_INTR:
        return s->intr;
    case SDMA_STOP_STAT:
    case SDMA_HSTART:
        return s->he;
    case SDMA_EVTOVR:
        return s->evtovr;
    case SDMA_DSPOVR:
        return s->dspovr;
    case SDMA_HOSTOVR:
        return s->hostovr;
    case SDMA_EVTPEND:
        return s->evtpend;
    case SDMA_DSPENBL:
        return s->dspenbl;
    case SDMA_RESET:
        return 0;
    case SDMA_EVTERR:
        /* Cleared by reading */
        val = s->evterr;
        s->evterr = 0;
        return val;
    case SDMA_INTRMASK:
        return s->intrmask;
    case SDMA_PSW:
        return s->psw;
    case SDMA_EVTERRDBG:
        return s->evterr;
    case SDMA_CONFIG:
        return s->config;
    case SDMA_LOCK:
        return s->lock;
    case SDMA_ONCE_ENB:
        return s->once_enb;
    case SDMA_ONCE_DATA:
        return s->once_data;
    case SDMA_ONCE_INSTR:
        return s->once_instr;
    case SDMA_ONCE_STAT:
        return s->once_stat;
    case SDMA_ONCE_CMD:
        return s->once_cmd;
    case SDMA_EVT_MIRROR:
        return (uint32_t)s->evt_level;
    case SDMA_ILLINSTADDR:
        return s->illinstaddr;
    case SDMA_CHN0ADDR:
        return s->chn0addr;
    case SDMA_EVT_MIRROR2:
        return (uint32_t)(s->evt_level >> 32) & 0xffff;
    case SDMA_XTRIG_CONF1:
    case SDMA_XTRIG_CONF2:
        return s->xtrig_conf[(offset - SDMA_XTRIG_CONF1) / 4];
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read of reserved offset 0x%03x\n",
                      TYPE_IMX_SDMA, (int)offset);
        return 0;
    }
}

static void sdma_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMXSDMAState *s = opaque;
    uint32_t val = value;
    bool kick = false;

    if (offset >= SDMA_CHNPRI0 && offset < SDMA_CHNPRI0 + 4 * IMX_SDMA_CHANNELS) {
        s->chnpri[(offset - SDMA_CHNPRI0) / 4] = val & 7;
        qemu_bh_schedule(s->bh);
        return;
    }
    if (offset >= SDMA_CHNENBL0 &&
        offset < SDMA_CHNENBL0 + 4 * IMX_SDMA_EVENTS) {
        s->chnenbl[(offset - SDMA_CHNENBL0) / 4] = val;
        return;
    }

    switch (offset) {
    case SDMA_MC0PTR:
        s->mc0ptr = val;
        kick = true;
        break;
    case SDMA_INTR:
        s->intr &= ~val;
        break;
    case SDMA_STOP_STAT:
        /* Writing 1 clears HE: the channel stops at its next yield */
        s->he &= ~val;
        break;
    case SDMA_HSTART:
        s->he |= val;
        if (val & BIT(0)) {
            if (s->mc0ptr) {
                /* Channel 0 completes before the write returns */
                sdma_run_channel0(s);
            }
        }
        kick = true;
        break;
    case SDMA_EVTOVR:
        s->evtovr = val;
        kick = true;
        break;
    case SDMA_DSPOVR:
        s->dspovr = val;
        kick = true;
        break;
    case SDMA_HOSTOVR:
        s->hostovr = val;
        kick = true;
        break;
    case SDMA_EVTPEND:
        /* Write 1 to clear a pending event */
        s->evtpend &= ~val;
        break;
    case SDMA_DSPENBL:
        s->dspenbl = val;
        kick = true;
        break;
    case SDMA_RESET:
        if (val & RESET_RESET) {
            sdma_core_reset(s);
        }
        if (val & RESET_RESCHED) {
            kick = true;
        }
        break;
    case SDMA_EVTERR:
    case SDMA_PSW:
    case SDMA_EVTERRDBG:
    case SDMA_EVT_MIRROR:
    case SDMA_EVT_MIRROR2:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only 0x%03x\n",
                      TYPE_IMX_SDMA, (int)offset);
        break;
    case SDMA_INTRMASK:
        s->intrmask = val;
        break;
    case SDMA_CONFIG:
        s->config = val & CONFIG_RW_MASK;
        break;
    case SDMA_LOCK:
        s->lock = val & 1;
        break;
    case SDMA_ONCE_ENB:
        s->once_enb = val & 1;
        break;
    case SDMA_ONCE_DATA:
        s->once_data = val;
        break;
    case SDMA_ONCE_INSTR:
        s->once_instr = val & 0xffff;
        break;
    case SDMA_ONCE_CMD:
        s->once_cmd = val & 0xf;
        qemu_log_mask(LOG_UNIMP, "%s: OnCE command %u not implemented\n",
                      TYPE_IMX_SDMA, val & 0xf);
        break;
    case SDMA_ILLINSTADDR:
        s->illinstaddr = val & 0x3fff;
        break;
    case SDMA_CHN0ADDR:
        s->chn0addr = val & CHN0ADDR_RW_MASK;
        break;
    case SDMA_XTRIG_CONF1:
    case SDMA_XTRIG_CONF2:
        s->xtrig_conf[(offset - SDMA_XTRIG_CONF1) / 4] = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to reserved offset 0x%03x\n",
                      TYPE_IMX_SDMA, (int)offset);
        break;
    }
    sdma_update_irq(s);
    if (kick) {
        qemu_bh_schedule(s->bh);
    }
}

static const MemoryRegionOps sdma_ops = {
    .read = sdma_read,
    .write = sdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sdma_reset(DeviceState *dev)
{
    IMXSDMAState *s = IMX_SDMA(dev);

    s->mc0ptr = 0;
    s->intr = 0;
    s->evtovr = 0;
    s->dspovr = 0xffffffff;     /* no DSP on this SoC */
    s->hostovr = 0;
    s->dspenbl = 0;
    s->evterr = 0;
    s->intrmask = 0;
    s->config = 0x3;            /* dynamic context switching */
    s->lock = 0;
    s->once_enb = 0;
    s->once_data = 0;
    s->once_instr = 0;
    s->once_stat = 0;
    s->once_cmd = 0;
    s->illinstaddr = 1;
    s->chn0addr = 0x0050;       /* ROM boot script */
    memset(s->xtrig_conf, 0, sizeof(s->xtrig_conf));
    memset(s->chnpri, 0, sizeof(s->chnpri));
    memset(s->chnenbl, 0, sizeof(s->chnenbl));
    memset(s->ram, 0, s->ram_words * 4);
    sdma_core_reset(s);
    sdma_update_irq(s);
}

/*
 * Script table: the ROM entry points, plus the RAM entry points listed in
 * the header of the firmware file the guest will load (struct
 * sdma_firmware_header: magic, version major/minor, script_addrs_start,
 * num_script_addrs, ram_code_start, ram_code_size; little endian).
 */
static bool sdma_build_scripts(IMXSDMAState *s, Error **errp)
{
    g_autofree char *data = NULL;
    gsize len = 0;
    uint32_t hdr[SDMA_FW_HDR_WORDS];
    unsigned i;

    s->nscripts = ARRAY_SIZE(sdma_rom_scripts);
    s->scripts = g_new(SDMAScript, s->nscripts + ARRAY_SIZE(sdma_fw_scripts));
    memcpy(s->scripts, sdma_rom_scripts, sizeof(sdma_rom_scripts));
    if (!s->firmware || !*s->firmware) {
        return true;
    }

    if (!g_file_get_contents(s->firmware, &data, &len, NULL) ||
        len < sizeof(hdr)) {
        error_setg(errp, "%s: cannot read firmware '%s'", TYPE_IMX_SDMA,
                   s->firmware);
        return false;
    }
    for (i = 0; i < SDMA_FW_HDR_WORDS; i++) {
        hdr[i] = ldl_le_p(data + i * 4);
    }
    if (hdr[0] != SDMA_FW_MAGIC || hdr[3] > len ||
        hdr[4] > (len - hdr[3]) / 4) {
        error_setg(errp, "%s: '%s' is not an SDMA firmware file",
                   TYPE_IMX_SDMA, s->firmware);
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(sdma_fw_scripts); i++) {
        int32_t pc;

        if (sdma_fw_scripts[i].index >= hdr[4]) {
            continue;
        }
        pc = (int32_t)ldl_le_p(data + hdr[3] + sdma_fw_scripts[i].index * 4);
        if (pc <= 0) {
            continue;
        }
        s->scripts[s->nscripts++] = (SDMAScript) {
            .pc = pc,
            .name = sdma_fw_scripts[i].name,
            .kind = sdma_fw_scripts[i].kind,
        };
    }
    return true;
}

static void sdma_realize(DeviceState *dev, Error **errp)
{
    IMXSDMAState *s = IMX_SDMA(dev);

    if (!s->ram_size || s->ram_size % 4 || s->ram_size > 0x8000) {
        error_setg(errp, "%s: ram-size must be a multiple of 4 up to 32 KiB",
                   TYPE_IMX_SDMA);
        return;
    }
    if (!s->dma_mr || s->dma_mr == get_system_memory()) {
        s->dma_as = &address_space_memory;
    } else {
        s->dma_as = g_new0(AddressSpace, 1);
        address_space_init(s->dma_as, s->dma_mr, TYPE_IMX_SDMA "-dma");
    }
    if (!sdma_build_scripts(s, errp)) {
        return;
    }
    s->ram_words = s->ram_size / 4;
    s->ram = g_new0(uint32_t, s->ram_words);
    s->bh = qemu_bh_new_guarded(sdma_bh, s, &dev->mem_reentrancy_guard);
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdma_poll, s);
}

static void sdma_init(Object *obj)
{
    IMXSDMAState *s = IMX_SDMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sdma_ops, s, TYPE_IMX_SDMA,
                          IMX_SDMA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), sdma_event, IMX_SDMA_GPIO_EVENTS,
                            IMX_SDMA_EVENTS);
}

static const VMStateDescription vmstate_sdma_chan = {
    .name = "imx-sdma-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(fresh, IMXSDMAChannel),
        VMSTATE_BOOL(active, IMXSDMAChannel),
        VMSTATE_BOOL(blocked, IMXSDMAChannel),
        VMSTATE_UINT32(pc, IMXSDMAChannel),
        VMSTATE_UINT32(script, IMXSDMAChannel),
        VMSTATE_UINT32(bd, IMXSDMAChannel),
        VMSTATE_UINT32(bd_bytes, IMXSDMAChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sdma = {
    .name = TYPE_IMX_SDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mc0ptr, IMXSDMAState),
        VMSTATE_UINT32(intr, IMXSDMAState),
        VMSTATE_UINT32(he, IMXSDMAState),
        VMSTATE_UINT32(evtovr, IMXSDMAState),
        VMSTATE_UINT32(dspovr, IMXSDMAState),
        VMSTATE_UINT32(hostovr, IMXSDMAState),
        VMSTATE_UINT32(evtpend, IMXSDMAState),
        VMSTATE_UINT32(dspenbl, IMXSDMAState),
        VMSTATE_UINT32(evterr, IMXSDMAState),
        VMSTATE_UINT32(intrmask, IMXSDMAState),
        VMSTATE_UINT32(psw, IMXSDMAState),
        VMSTATE_UINT32(config, IMXSDMAState),
        VMSTATE_UINT32(lock, IMXSDMAState),
        VMSTATE_UINT32(once_enb, IMXSDMAState),
        VMSTATE_UINT32(once_data, IMXSDMAState),
        VMSTATE_UINT32(once_instr, IMXSDMAState),
        VMSTATE_UINT32(once_stat, IMXSDMAState),
        VMSTATE_UINT32(once_cmd, IMXSDMAState),
        VMSTATE_UINT32(illinstaddr, IMXSDMAState),
        VMSTATE_UINT32(chn0addr, IMXSDMAState),
        VMSTATE_UINT32_ARRAY(xtrig_conf, IMXSDMAState, 2),
        VMSTATE_UINT32_ARRAY(chnpri, IMXSDMAState, IMX_SDMA_CHANNELS),
        VMSTATE_UINT32_ARRAY(chnenbl, IMXSDMAState, IMX_SDMA_EVENTS),
        VMSTATE_UINT64(evt_level, IMXSDMAState),
        VMSTATE_VARRAY_UINT32_ALLOC(ram, IMXSDMAState, ram_words, 0,
                                    vmstate_info_uint32, uint32_t),
        VMSTATE_STRUCT_ARRAY(chan, IMXSDMAState, IMX_SDMA_CHANNELS, 1,
                             vmstate_sdma_chan, IMXSDMAChannel),
        VMSTATE_TIMER_PTR(poll_timer, IMXSDMAState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sdma_properties[] = {
    DEFINE_PROP_LINK("dma-mr", IMXSDMAState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    /* Internal RAM size, starting at data memory 0x800 */
    DEFINE_PROP_UINT32("ram-size", IMXSDMAState, ram_size, 16 * KiB),
    /* Recheck interval for a channel waiting for the CPU to return a BD */
    DEFINE_PROP_UINT32("bd-poll-us", IMXSDMAState, bd_poll_us, 100),
    DEFINE_PROP_STRING("firmware", IMXSDMAState, firmware),
};

static void sdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sdma_realize;
    dc->desc = "i.MX SDMA v2";
    dc->vmsd = &vmstate_sdma;
    device_class_set_legacy_reset(dc, sdma_reset);
    device_class_set_props(dc, sdma_properties);
}

static const TypeInfo sdma_info = {
    .name          = TYPE_IMX_SDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXSDMAState),
    .instance_init = sdma_init,
    .class_init    = sdma_class_init,
};

static void sdma_register_types(void)
{
    type_register_static(&sdma_info);
}

type_init(sdma_register_types)
