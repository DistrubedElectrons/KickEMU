/*
 * i.MX 8M Mini Synchronous Audio Interface (SAI)
 *
 * Model of the SAI v3.1 block of the i.MX 8M Mini ("fsl,imx8mm-sai",
 * Linux soc data fsl_sai_imx8mq: register offset 8, 128 word FIFOs, eight
 * data lines). Reference: i.MX 8M Mini Applications Processor Reference
 * Manual, chapter "Synchronous Audio Interface (SAI)", and the Linux
 * driver sound/soc/fsl/fsl_sai.c.
 *
 * Register map (offsets from the block base):
 *   0x000 VERID   0x004 PARAM
 *   0x008 TCSR    0x00C..0x01C TCR1..TCR5
 *   0x020..0x03C TDR0..7 (write)   0x040..0x05C TFR0..7   0x060 TMR
 *   0x088 RCSR    0x08C..0x09C RCR1..RCR5
 *   0x0A0..0x0BC RDR0..7 (read)    0x0C0..0x0DC RFR0..7   0x0E0 RMR
 *   0x100 MCTL    0x104 MDIV
 *
 * Data path: a frame engine shifts words out of the transmit FIFOs and into
 * the receive FIFOs at the frame rate given by the clock configuration
 * (xCR2 MSEL/DIV/BYP, xCR4 FRSZ, xCR5 W0W/WNW) of the side that generates
 * the clocks (xCR2.SYNC selects the other side). It runs on the virtual
 * clock in batches of tick-us. Data line 0 is connected to the QEMU
 * audiodev: the first two words of each frame are the left and right
 * samples (I2S), converted to signed 32 bit. The other enabled data lines
 * are shifted at the same rate, their data is not routed anywhere.
 *
 * Status and DMA: xCSR.FRF (FIFO request) and FWF (warning) are computed
 * from the FIFO levels of the enabled channels (xCR3.TCE/RCE) and the
 * watermark in xCR1; FEF is set on transmit underrun and receive overflow;
 * WSF on the start of the word selected by xCR3.WDFL. The DMA request lines
 * follow (FRDE && FRF) || (FWDE && FWF). The interrupt line is the OR of
 * the enabled flags of both directions.
 *
 * Not modelled (LOG_UNIMP): FIFO combine (xCR4.FCOMB), FIFO packing
 * (xCR4.FPACK), LSB first or offset bit order (xCR5.FBT != word width - 1),
 * timestamp/bit counter registers, sync error (SEF never set), more than
 * two slots towards the audiodev.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "imx8mm_sai.h"

/* Register offsets */
#define SAI_VERID           0x000
#define SAI_PARAM           0x004
#define SAI_TCSR            0x008
#define SAI_TCR1            0x00c
#define SAI_TCR5            0x01c
#define SAI_TDR0            0x020
#define SAI_TDR7            0x03c
#define SAI_TFR0            0x040
#define SAI_TFR7            0x05c
#define SAI_TMR             0x060
#define SAI_TTCTL           0x070
#define SAI_TTCAP           0x07c
#define SAI_RX_OFFSET       0x080   /* receive registers mirror transmit */
#define SAI_RCSR            0x088
#define SAI_RDR0            0x0a0
#define SAI_RFR0            0x0c0
#define SAI_RMR             0x0e0
#define SAI_MCTL            0x100
#define SAI_MDIV            0x104

/* xCSR */
#define CSR_TE              BIT(31)     /* TE / RE */
#define CSR_STOPE           BIT(30)
#define CSR_DBGE            BIT(29)
#define CSR_BCE             BIT(28)
#define CSR_FR              BIT(25)
#define CSR_SR              BIT(24)
#define CSR_WSF             BIT(20)
#define CSR_SEF             BIT(19)
#define CSR_FEF             BIT(18)
#define CSR_FWF             BIT(17)
#define CSR_FRF             BIT(16)
#define CSR_W1C_MASK        (CSR_WSF | CSR_SEF | CSR_FEF)
#define CSR_FLAGS_MASK      (0x1f << 16)
#define CSR_IE_SHIFT        8
#define CSR_IE_MASK         (0x1f << CSR_IE_SHIFT)
#define CSR_FWDE            BIT(1)
#define CSR_FRDE            BIT(0)
#define CSR_RW_MASK         (CSR_TE | CSR_STOPE | CSR_DBGE | CSR_BCE | \
                             CSR_SR | CSR_IE_MASK | CSR_FWDE | CSR_FRDE)

/* xCR2 */
#define CR2_SYNC_SHIFT      30
#define CR2_SYNC_MASK       (3u << CR2_SYNC_SHIFT)
#define CR2_MSEL_SHIFT      26
#define CR2_MSEL_MASK       (3u << CR2_MSEL_SHIFT)
#define CR2_BCD             BIT(24)
#define CR2_BYP             BIT(23)
#define CR2_DIV_MASK        0xff
#define CR2_RW_MASK         0xff8000ffu

/* xCR3 */
#define CR3_CFR_SHIFT       24
#define CR3_CE_SHIFT        16
#define CR3_CE_MASK         (0xffu << CR3_CE_SHIFT)
#define CR3_WDFL_MASK       0x1f
#define CR3_RW_MASK         (CR3_CE_MASK | CR3_WDFL_MASK)

/* xCR4 */
#define CR4_FCOMB_MASK      (3u << 26)
#define CR4_FPACK_MASK      (3u << 24)
#define CR4_FRSZ_SHIFT      16
#define CR4_FRSZ_MASK       (0x1fu << CR4_FRSZ_SHIFT)
#define CR4_RW_MASK         0x1f1f1f3fu

/* xCR5 */
#define CR5_WNW_SHIFT       24
#define CR5_W0W_SHIFT       16
#define CR5_FBT_SHIFT       8
#define CR5_RW_MASK         0x1f1f1f00u

/* MCTL / MDIV */
#define MCTL_RW_MASK        0x438000ffu
#define MDIV_RW_MASK        0x000fffffu

#define VERID_FEATURE_TSTMP BIT(1)

static const char *const sai_dir_name[2] = { "tx", "rx" };

static uint32_t sai_fifo_count(IMX8MMSAIState *s, IMX8MMSAIDir *x, int ch)
{
    return (x->wfp[ch] - x->rfp[ch]) & (2 * s->fifo_words - 1);
}

static uint32_t sai_enabled_channels(IMX8MMSAIState *s, IMX8MMSAIDir *x)
{
    return ((x->cr[2] & CR3_CE_MASK) >> CR3_CE_SHIFT) &
           ((1u << s->datalines) - 1);
}

/* Live FIFO flags of a direction (RM xCSR FRF/FWF descriptions) */
static uint32_t sai_fifo_flags(IMX8MMSAIState *s, int d)
{
    IMX8MMSAIDir *x = &s->dir[d];
    uint32_t ce = sai_enabled_channels(s, x);
    uint32_t wm = x->cr[0] & (s->fifo_words - 1);
    uint32_t flags = 0;
    int ch;

    for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
        uint32_t cnt;

        if (!(ce & BIT(ch))) {
            continue;
        }
        cnt = sai_fifo_count(s, x, ch);
        if (d == IMX8MM_SAI_TX) {
            if (cnt <= wm) {
                flags |= CSR_FRF;
            }
            if (cnt == 0) {
                flags |= CSR_FWF;
            }
        } else {
            if (cnt > wm) {
                flags |= CSR_FRF;
            }
            if (cnt == s->fifo_words) {
                flags |= CSR_FWF;
            }
        }
    }
    return flags;
}

static uint32_t sai_csr(IMX8MMSAIState *s, int d)
{
    return s->dir[d].csr | sai_fifo_flags(s, d);
}

static void sai_update(IMX8MMSAIState *s)
{
    bool irq = false;
    int d;

    for (d = 0; d < 2; d++) {
        uint32_t csr = sai_csr(s, d);
        uint32_t ie = (csr & CSR_IE_MASK) << (16 - CSR_IE_SHIFT);
        bool req = !(csr & CSR_SR) &&
                   (((csr & CSR_FRDE) && (csr & CSR_FRF)) ||
                    ((csr & CSR_FWDE) && (csr & CSR_FWF)));

        irq |= (csr & CSR_FLAGS_MASK & ie) != 0;
        if (req != s->dma_level[d]) {
            s->dma_level[d] = req;
            qemu_set_irq(s->dma_req[d], req);
        }
    }
    qemu_set_irq(s->irq, irq);
}

static void sai_fifo_reset(IMX8MMSAIState *s, int d, uint32_t mask)
{
    IMX8MMSAIDir *x = &s->dir[d];
    int ch;

    for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
        if (mask & BIT(ch)) {
            x->wfp[ch] = 0;
            x->rfp[ch] = 0;
        }
    }
}

/* Index of the direction providing bit clock and frame sync for d */
static int sai_clock_side(IMX8MMSAIState *s, int d)
{
    uint32_t sync = (s->dir[d].cr[1] & CR2_SYNC_MASK) >> CR2_SYNC_SHIFT;

    return sync == 1 ? !d : d;
}

static uint32_t sai_words_per_frame(IMX8MMSAIDir *x)
{
    return ((x->cr[3] & CR4_FRSZ_MASK) >> CR4_FRSZ_SHIFT) + 1;
}

static uint32_t sai_word_width(IMX8MMSAIDir *x, uint32_t word)
{
    uint32_t shift = word ? CR5_WNW_SHIFT : CR5_W0W_SHIFT;

    return ((x->cr[4] >> shift) & 0x1f) + 1;
}

/* Frame rate in Hz from the clock side configuration, 0 if not running */
static uint32_t sai_frame_rate(IMX8MMSAIState *s, int d)
{
    IMX8MMSAIDir *c = &s->dir[sai_clock_side(s, d)];
    uint32_t msel = (c->cr[1] & CR2_MSEL_MASK) >> CR2_MSEL_SHIFT;
    uint64_t src, bclk, bits;
    uint32_t w;

    if (!(c->cr[1] & CR2_BCD)) {
        /* Bit clock input: the external master defines the rate */
        return s->ext_frame_rate;
    }
    src = msel ? s->mclk_hz[msel] : s->bus_clk_hz;
    if (c->cr[1] & CR2_BYP) {
        bclk = src;
    } else {
        bclk = src / (((c->cr[1] & CR2_DIV_MASK) + 1) * 2);
    }
    bits = sai_word_width(c, 0);
    for (w = 1; w < sai_words_per_frame(c); w++) {
        bits += sai_word_width(c, w);
    }
    return bits ? bclk / bits : 0;
}

static void sai_audio_out_cb(void *opaque, int avail);
static void sai_audio_in_cb(void *opaque, int avail);

/* (Re)open the host voice of a direction for the current frame format */
static void sai_open_voice(IMX8MMSAIState *s, int d)
{
    IMX8MMSAIDir *x = &s->dir[d];
    /* Always stereo towards the host; a one word frame is duplicated */
    uint32_t chans = 2;
    struct audsettings as = {
        .freq = x->rate,
        .nchannels = chans,
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = HOST_BIG_ENDIAN,
    };
    uint32_t frames = MAX(x->rate * s->buffer_ms / 1000, 64);

    if (x->voice_rate != x->rate || x->voice_channels != chans ||
        x->ring_frames != frames) {
        g_free(x->ring);
        x->ring = g_new0(int32_t, frames * 2);
        x->ring_frames = frames;
        x->voice_rate = x->rate;
        x->voice_channels = chans;
    }
    x->ring_rd = x->ring_wr = x->ring_fill = 0;

    if (!s->audio_be) {
        return;
    }
    if (d == IMX8MM_SAI_TX) {
        s->voice_out = audio_be_open_out(s->audio_be, s->voice_out, "sai.out",
                                         s, sai_audio_out_cb, &as);
        audio_be_set_active_out(s->audio_be, s->voice_out, true);
    } else {
        s->voice_in = audio_be_open_in(s->audio_be, s->voice_in, "sai.in",
                                       s, sai_audio_in_cb, &as);
        audio_be_set_active_in(s->audio_be, s->voice_in, true);
    }
}

static void sai_close_voice(IMX8MMSAIState *s, int d)
{
    if (!s->audio_be) {
        return;
    }
    if (d == IMX8MM_SAI_TX && s->voice_out) {
        audio_be_set_active_out(s->audio_be, s->voice_out, false);
    } else if (d == IMX8MM_SAI_RX && s->voice_in) {
        audio_be_set_active_in(s->audio_be, s->voice_in, false);
    }
}

static void sai_check_format(IMX8MMSAIState *s, IMX8MMSAIDir *x, int d)
{
    uint32_t w0 = sai_word_width(x, 0);
    uint32_t fbt = (x->cr[4] >> CR5_FBT_SHIFT) & 0x1f;

    if (x->fmt_logged) {
        return;
    }
    if (x->cr[3] & (CR4_FCOMB_MASK | CR4_FPACK_MASK)) {
        qemu_log_mask(LOG_UNIMP, "%s: %s FIFO combine/packing not modelled\n",
                      TYPE_IMX8MM_SAI, sai_dir_name[d]);
        x->fmt_logged = true;
    }
    if (fbt != w0 - 1) {
        qemu_log_mask(LOG_UNIMP, "%s: %s first bit %u (word width %u) not "
                      "modelled, MSB first assumed\n", TYPE_IMX8MM_SAI,
                      sai_dir_name[d], fbt, w0);
        x->fmt_logged = true;
    }
}

static bool sai_dir_should_run(IMX8MMSAIState *s, int d)
{
    IMX8MMSAIDir *x = &s->dir[d];
    int c = sai_clock_side(s, d);

    if (!(x->csr & CSR_TE) || (x->csr & CSR_SR)) {
        return false;
    }
    /* In synchronous mode the other side must be enabled to run the clocks */
    return c == d || (s->dir[c].csr & CSR_TE);
}

static void sai_schedule_tick(IMX8MMSAIState *s, int64_t when)
{
    if (s->dir[0].running || s->dir[1].running) {
        timer_mod(s->tick, when);
    } else {
        timer_del(s->tick);
    }
}

static void sai_start_stop(IMX8MMSAIState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int d;

    for (d = 0; d < 2; d++) {
        IMX8MMSAIDir *x = &s->dir[d];
        bool run = sai_dir_should_run(s, d);

        if (run && !x->running) {
            x->rate = sai_frame_rate(s, d);
            if (!x->rate) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: %s enabled without a "
                              "usable bit clock\n", TYPE_IMX8MM_SAI,
                              sai_dir_name[d]);
                continue;
            }
            sai_check_format(s, x, d);
            x->running = true;
            x->t0_ns = now;
            x->frames_done = 0;
            x->word = 0;
            sai_open_voice(s, d);
        } else if (!run && x->running) {
            x->running = false;
            sai_close_voice(s, d);
        }
    }
    if (!timer_pending(s->tick)) {
        sai_schedule_tick(s, now + (int64_t)s->tick_us * SCALE_US);
    }
}

/* Convert between FIFO words (right aligned, MSB first) and S32 samples */
static int32_t sai_word_to_s32(uint32_t word, uint32_t width)
{
    return width >= 32 ? (int32_t)word : (int32_t)(word << (32 - width));
}

static uint32_t sai_s32_to_word(int32_t sample, uint32_t width)
{
    return width >= 32 ? (uint32_t)sample : (uint32_t)sample >> (32 - width);
}

/*
 * Shift out one transmit frame. Returns false without consuming anything
 * when an unmasked word finds the data line 0 FIFO empty while the DMA is
 * asked for data, so the caller can let the DMA catch up first.
 */
static bool sai_tx_frame(IMX8MMSAIState *s, bool allow_underrun)
{
    IMX8MMSAIDir *x = &s->dir[IMX8MM_SAI_TX];
    uint32_t words = sai_words_per_frame(x);
    uint32_t ce = sai_enabled_channels(s, x);
    uint32_t wdfl = x->cr[2] & CR3_WDFL_MASK;
    int32_t out[2] = { 0, 0 };
    uint32_t w;
    int ch;

    if (!allow_underrun) {
        uint32_t need = 0;

        for (w = 0; w < words; w++) {
            if (!(x->mr & BIT(w))) {
                need++;
            }
        }
        for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
            if ((ce & BIT(ch)) && sai_fifo_count(s, x, ch) < need) {
                return false;
            }
        }
    }

    for (w = 0; w < words; w++) {
        if (w == wdfl) {
            x->csr |= CSR_WSF;
        }
        if (x->mr & BIT(w)) {
            /* Masked word: data line tristated, FIFO not read */
            continue;
        }
        for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
            uint32_t data = 0;

            if (!(ce & BIT(ch))) {
                continue;
            }
            if (sai_fifo_count(s, x, ch)) {
                data = x->fifo[ch][x->rfp[ch] & (s->fifo_words - 1)];
                x->rfp[ch]++;
            } else {
                x->csr |= CSR_FEF;      /* underrun */
            }
            if (ch == 0 && w < 2) {
                out[w] = sai_word_to_s32(data, sai_word_width(x, w));
            }
        }
    }
    if (words == 1) {
        out[1] = out[0];
    }

    /* Data line 0 to the host ring; drop the oldest frame when full */
    if (x->ring) {
        if (x->ring_fill == x->ring_frames) {
            x->ring_rd = (x->ring_rd + 1) % x->ring_frames;
            x->ring_fill--;
        }
        x->ring[x->ring_wr * 2] = out[0];
        x->ring[x->ring_wr * 2 + 1] = out[1];
        x->ring_wr = (x->ring_wr + 1) % x->ring_frames;
        x->ring_fill++;
    }
    return true;
}

static bool sai_rx_frame(IMX8MMSAIState *s, bool allow_overflow)
{
    IMX8MMSAIDir *x = &s->dir[IMX8MM_SAI_RX];
    uint32_t words = sai_words_per_frame(x);
    uint32_t ce = sai_enabled_channels(s, x);
    uint32_t wdfl = x->cr[2] & CR3_WDFL_MASK;
    int32_t in[2] = { 0, 0 };
    uint32_t w;
    int ch;

    if (!allow_overflow) {
        uint32_t need = 0;

        for (w = 0; w < words; w++) {
            if (!(x->mr & BIT(w))) {
                need++;
            }
        }
        for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
            if ((ce & BIT(ch)) &&
                s->fifo_words - sai_fifo_count(s, x, ch) < need) {
                return false;
            }
        }
    }

    if (x->ring && x->ring_fill) {
        in[0] = x->ring[x->ring_rd * 2];
        in[1] = x->ring[x->ring_rd * 2 + 1];
        x->ring_rd = (x->ring_rd + 1) % x->ring_frames;
        x->ring_fill--;
    }

    for (w = 0; w < words; w++) {
        if (w == wdfl) {
            x->csr |= CSR_WSF;
        }
        if (x->mr & BIT(w)) {
            continue;
        }
        for (ch = 0; ch < IMX8MM_SAI_LINES; ch++) {
            uint32_t data;

            if (!(ce & BIT(ch))) {
                continue;
            }
            data = (ch == 0 && w < 2) ?
                   sai_s32_to_word(in[w], sai_word_width(x, w)) : 0;
            if (sai_fifo_count(s, x, ch) < s->fifo_words) {
                x->fifo[ch][x->wfp[ch] & (s->fifo_words - 1)] = data;
                x->wfp[ch]++;
            } else {
                x->csr |= CSR_FEF;      /* overflow, word lost */
            }
        }
    }
    return true;
}

/*
 * Frame engine. Frames that are due according to the virtual clock are
 * processed. When a FIFO runs dry (TX) or full (RX) while its DMA request is
 * asserted, the rest of the batch is deferred to an immediate re-run so the
 * DMA engine, which runs from a bottom half, can service the request; if it
 * still has not by then, the underrun/overflow happens as on hardware.
 */
static void sai_tick(void *opaque)
{
    IMX8MMSAIState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool defer = false;
    int d;

    for (d = 0; d < 2; d++) {
        IMX8MMSAIDir *x = &s->dir[d];
        uint64_t due;
        bool progress = false;

        if (!x->running) {
            continue;
        }
        due = muldiv64(now - x->t0_ns, x->rate, NANOSECONDS_PER_SECOND);
        while (x->frames_done < due) {
            bool waiting_dma = s->dma_level[d];
            bool ok = d == IMX8MM_SAI_TX ? sai_tx_frame(s, !waiting_dma ||
                                                         !progress)
                                         : sai_rx_frame(s, !waiting_dma ||
                                                         !progress);

            if (!ok) {
                defer = true;
                break;
            }
            x->frames_done++;
            progress = true;
            sai_update(s);
        }
    }
    sai_update(s);
    sai_schedule_tick(s, defer ? now : now + (int64_t)s->tick_us * SCALE_US);
}

static void sai_audio_out_cb(void *opaque, int avail)
{
    IMX8MMSAIState *s = opaque;
    IMX8MMSAIDir *x = &s->dir[IMX8MM_SAI_TX];
    const uint32_t fbytes = 2 * sizeof(int32_t);

    /* Hand over contiguous runs of the ring, whole frames only */
    while (avail >= (int)fbytes && x->ring_fill) {
        uint32_t run = MIN(x->ring_fill, x->ring_frames - x->ring_rd);
        size_t n;

        run = MIN(run, avail / fbytes);
        n = audio_be_write(s->audio_be, s->voice_out,
                           &x->ring[x->ring_rd * 2], run * fbytes) / fbytes;
        if (!n) {
            break;
        }
        x->ring_rd = (x->ring_rd + n) % x->ring_frames;
        x->ring_fill -= n;
        avail -= n * fbytes;
    }
}

static void sai_audio_in_cb(void *opaque, int avail)
{
    IMX8MMSAIState *s = opaque;
    IMX8MMSAIDir *x = &s->dir[IMX8MM_SAI_RX];
    const uint32_t fbytes = 2 * sizeof(int32_t);

    while (avail >= (int)fbytes && x->ring) {
        int32_t frame[2];
        size_t n = audio_be_read(s->audio_be, s->voice_in, frame, fbytes);

        if (n < fbytes) {
            break;
        }
        if (x->ring_fill == x->ring_frames) {
            /* Guest not consuming: drop the oldest captured frame */
            x->ring_rd = (x->ring_rd + 1) % x->ring_frames;
            x->ring_fill--;
        }
        x->ring[x->ring_wr * 2] = frame[0];
        x->ring[x->ring_wr * 2 + 1] = frame[1];
        x->ring_wr = (x->ring_wr + 1) % x->ring_frames;
        x->ring_fill++;
        avail -= fbytes;
    }
}

static uint32_t sai_param(IMX8MMSAIState *s)
{
    /* SPF and WPF are log2 encoded, DLN is the number of data lines */
    return (ctz32(s->max_slots) << 16) | (ctz32(s->fifo_words) << 8) |
           s->datalines;
}

static uint64_t sai_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMSAIState *s = opaque;
    int d = offset >= SAI_RX_OFFSET && offset < SAI_MCTL ? IMX8MM_SAI_RX
                                                         : IMX8MM_SAI_TX;
    hwaddr reg = d == IMX8MM_SAI_RX ? offset - SAI_RX_OFFSET : offset;
    IMX8MMSAIDir *x = &s->dir[d];
    uint32_t val = 0;
    int ch;

    switch (offset) {
    case SAI_VERID:
        return s->verid;
    case SAI_PARAM:
        return sai_param(s);
    case SAI_MCTL:
        return s->mctl;
    case SAI_MDIV:
        return s->mdiv;
    default:
        break;
    }

    switch (reg) {
    case SAI_TCSR:
        val = sai_csr(s, d);
        break;
    case SAI_TCR1 ... SAI_TCR5:
        val = x->cr[(reg - SAI_TCR1) / 4];
        break;
    case SAI_TDR0 ... SAI_TDR7:
        if (d == IMX8MM_SAI_TX) {
            /* Transmit data registers are write only */
            qemu_log_mask(LOG_GUEST_ERROR, "%s: read of TDR%d\n",
                          TYPE_IMX8MM_SAI, (int)(reg - SAI_TDR0) / 4);
            break;
        }
        ch = (reg - SAI_TDR0) / 4;
        if (sai_fifo_count(s, x, ch)) {
            val = x->fifo[ch][x->rfp[ch] & (s->fifo_words - 1)];
            x->rfp[ch]++;
            sai_update(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: RDR%d read with empty FIFO\n",
                          TYPE_IMX8MM_SAI, ch);
        }
        break;
    case SAI_TFR0 ... SAI_TFR7:
        ch = (reg - SAI_TFR0) / 4;
        /* WFP in [23:16], RFP in [7:0], both with the wrap bit */
        val = ((x->wfp[ch] & (2 * s->fifo_words - 1)) << 16) |
              (x->rfp[ch] & (2 * s->fifo_words - 1));
        break;
    case SAI_TMR:
        val = x->mr;
        break;
    case SAI_TTCTL ... SAI_TTCAP:
        qemu_log_mask(LOG_UNIMP, "%s: timestamp register 0x%03x not "
                      "implemented\n", TYPE_IMX8MM_SAI, (int)offset);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read of reserved offset 0x%03x\n",
                      TYPE_IMX8MM_SAI, (int)offset);
        break;
    }
    return val;
}

static void sai_write_csr(IMX8MMSAIState *s, int d, uint32_t val)
{
    IMX8MMSAIDir *x = &s->dir[d];
    uint32_t flags = (x->csr & CSR_W1C_MASK) & ~(val & CSR_W1C_MASK);

    x->csr = (val & CSR_RW_MASK) | flags;
    if (val & CSR_FR) {
        sai_fifo_reset(s, d, 0xff);
    }
    if (val & CSR_SR) {
        /* Software reset: transmitter logic, FIFO pointers and flags */
        sai_fifo_reset(s, d, 0xff);
        x->csr &= ~CSR_W1C_MASK;
    }
    sai_start_stop(s);
    sai_update(s);
}

static void sai_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMX8MMSAIState *s = opaque;
    int d = offset >= SAI_RX_OFFSET && offset < SAI_MCTL ? IMX8MM_SAI_RX
                                                         : IMX8MM_SAI_TX;
    hwaddr reg = d == IMX8MM_SAI_RX ? offset - SAI_RX_OFFSET : offset;
    IMX8MMSAIDir *x = &s->dir[d];
    uint32_t val = value;
    int ch;

    switch (offset) {
    case SAI_VERID:
    case SAI_PARAM:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only 0x%03x\n",
                      TYPE_IMX8MM_SAI, (int)offset);
        return;
    case SAI_MCTL:
        s->mctl = val & MCTL_RW_MASK;
        return;
    case SAI_MDIV:
        s->mdiv = val & MDIV_RW_MASK;
        return;
    default:
        break;
    }

    switch (reg) {
    case SAI_TCSR:
        sai_write_csr(s, d, val);
        break;
    case SAI_TCR1:
        x->cr[0] = val & (s->fifo_words - 1);
        sai_update(s);
        break;
    case SAI_TCR1 + 4:
        x->cr[1] = val & CR2_RW_MASK;
        break;
    case SAI_TCR1 + 8:
        /* CFR: per channel FIFO reset, self clearing */
        sai_fifo_reset(s, d, val >> CR3_CFR_SHIFT);
        x->cr[2] = val & CR3_RW_MASK;
        sai_update(s);
        break;
    case SAI_TCR1 + 12:
        x->cr[3] = val & CR4_RW_MASK;
        break;
    case SAI_TCR5:
        x->cr[4] = val & CR5_RW_MASK;
        break;
    case SAI_TDR0 ... SAI_TDR7:
        if (d == IMX8MM_SAI_RX) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write to RDR%d\n",
                          TYPE_IMX8MM_SAI, (int)(reg - SAI_TDR0) / 4);
            break;
        }
        ch = (reg - SAI_TDR0) / 4;
        /* Writes to a full FIFO are ignored */
        if (sai_fifo_count(s, x, ch) < s->fifo_words) {
            x->fifo[ch][x->wfp[ch] & (s->fifo_words - 1)] = val;
            x->wfp[ch]++;
            sai_update(s);
        }
        break;
    case SAI_TFR0 ... SAI_TFR7:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only FIFO register\n",
                      TYPE_IMX8MM_SAI);
        break;
    case SAI_TMR:
        x->mr = val;
        break;
    case SAI_TTCTL ... SAI_TTCAP:
        qemu_log_mask(LOG_UNIMP, "%s: timestamp register 0x%03x not "
                      "implemented\n", TYPE_IMX8MM_SAI, (int)offset);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to reserved offset 0x%03x\n",
                      TYPE_IMX8MM_SAI, (int)offset);
        break;
    }
}

static const MemoryRegionOps sai_ops = {
    .read = sai_read,
    .write = sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sai_reset(DeviceState *dev)
{
    IMX8MMSAIState *s = IMX8MM_SAI(dev);
    int d;

    for (d = 0; d < 2; d++) {
        IMX8MMSAIDir *x = &s->dir[d];

        if (x->running) {
            sai_close_voice(s, d);
        }
        x->csr = 0;
        memset(x->cr, 0, sizeof(x->cr));
        x->mr = 0;
        memset(x->wfp, 0, sizeof(x->wfp));
        memset(x->rfp, 0, sizeof(x->rfp));
        x->running = false;
        x->fmt_logged = false;
    }
    s->mctl = 0;
    s->mdiv = 0;
    timer_del(s->tick);
    s->dma_level[0] = s->dma_level[1] = false;
    qemu_set_irq(s->dma_req[0], 0);
    qemu_set_irq(s->dma_req[1], 0);
    qemu_set_irq(s->irq, 0);
}

static void sai_realize(DeviceState *dev, Error **errp)
{
    IMX8MMSAIState *s = IMX8MM_SAI(dev);

    if (!is_power_of_2(s->fifo_words) || s->fifo_words > IMX8MM_SAI_FIFO_MAX ||
        s->fifo_words < 2) {
        error_setg(errp, "%s: fifo-words must be a power of 2 up to %d",
                   TYPE_IMX8MM_SAI, IMX8MM_SAI_FIFO_MAX);
        return;
    }
    if (!s->datalines || s->datalines > IMX8MM_SAI_LINES) {
        error_setg(errp, "%s: datalines must be 1..%d", TYPE_IMX8MM_SAI,
                   IMX8MM_SAI_LINES);
        return;
    }
    if (!is_power_of_2(s->max_slots) || s->max_slots > 32) {
        error_setg(errp, "%s: max-slots must be a power of 2 up to 32",
                   TYPE_IMX8MM_SAI);
        return;
    }
    if (!s->tick_us) {
        error_setg(errp, "%s: tick-us must not be 0", TYPE_IMX8MM_SAI);
        return;
    }
    if (s->verid & VERID_FEATURE_TSTMP) {
        warn_report("%s: VERID announces timestamps, which are not modelled",
                    TYPE_IMX8MM_SAI);
    }
    /* The audiodev is optional: without it the data path runs unconnected */
    if (s->audio_be && !audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, sai_tick, s);
}

static void sai_init(Object *obj)
{
    IMX8MMSAIState *s = IMX8MM_SAI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sai_ops, s, TYPE_IMX8MM_SAI,
                          IMX8MM_SAI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dma_req[IMX8MM_SAI_TX],
                             IMX8MM_SAI_GPIO_DMA_TX, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dma_req[IMX8MM_SAI_RX],
                             IMX8MM_SAI_GPIO_DMA_RX, 1);
}

static int sai_post_load(void *opaque, int version_id)
{
    IMX8MMSAIState *s = opaque;
    int d;

    for (d = 0; d < 2; d++) {
        if (s->dir[d].running) {
            sai_open_voice(s, d);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sai_dir = {
    .name = "imx8mm-sai-dir",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, IMX8MMSAIDir),
        VMSTATE_UINT32_ARRAY(cr, IMX8MMSAIDir, 5),
        VMSTATE_UINT32(mr, IMX8MMSAIDir),
        VMSTATE_UINT32_2DARRAY(fifo, IMX8MMSAIDir, IMX8MM_SAI_LINES,
                               IMX8MM_SAI_FIFO_MAX),
        VMSTATE_UINT32_ARRAY(wfp, IMX8MMSAIDir, IMX8MM_SAI_LINES),
        VMSTATE_UINT32_ARRAY(rfp, IMX8MMSAIDir, IMX8MM_SAI_LINES),
        VMSTATE_BOOL(running, IMX8MMSAIDir),
        VMSTATE_UINT32(rate, IMX8MMSAIDir),
        VMSTATE_INT64(t0_ns, IMX8MMSAIDir),
        VMSTATE_UINT64(frames_done, IMX8MMSAIDir),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sai = {
    .name = TYPE_IMX8MM_SAI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sai_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(dir, IMX8MMSAIState, 2, 1, vmstate_sai_dir,
                             IMX8MMSAIDir),
        VMSTATE_UINT32(mctl, IMX8MMSAIState),
        VMSTATE_UINT32(mdiv, IMX8MMSAIState),
        VMSTATE_BOOL_ARRAY(dma_level, IMX8MMSAIState, 2),
        VMSTATE_TIMER_PTR(tick, IMX8MMSAIState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sai_properties[] = {
    DEFINE_AUDIO_PROPERTIES(IMX8MMSAIState, audio_be),
    /* VERID: major 3, minor 1, standard feature set */
    DEFINE_PROP_UINT32("verid", IMX8MMSAIState, verid, 0x03010000),
    /* PARAM fields */
    DEFINE_PROP_UINT32("datalines", IMX8MMSAIState, datalines, 8),
    DEFINE_PROP_UINT32("fifo-words", IMX8MMSAIState, fifo_words, 128),
    DEFINE_PROP_UINT32("max-slots", IMX8MMSAIState, max_slots, 32),
    /* Clock inputs, set by the SoC from its clock tree */
    DEFINE_PROP_UINT32("bus-clk-hz", IMX8MMSAIState, bus_clk_hz, 0),
    DEFINE_PROP_UINT32("mclk1-hz", IMX8MMSAIState, mclk_hz[1], 0),
    DEFINE_PROP_UINT32("mclk2-hz", IMX8MMSAIState, mclk_hz[2], 0),
    DEFINE_PROP_UINT32("mclk3-hz", IMX8MMSAIState, mclk_hz[3], 0),
    /* Frame rate used when BCD = 0 (bit clock from an external master) */
    DEFINE_PROP_UINT32("ext-frame-rate", IMX8MMSAIState, ext_frame_rate,
                       48000),
    /* Frame engine batch period and host audio buffer length */
    DEFINE_PROP_UINT32("tick-us", IMX8MMSAIState, tick_us, 500),
    DEFINE_PROP_UINT32("buffer-ms", IMX8MMSAIState, buffer_ms, 100),
};

static void sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sai_realize;
    dc->desc = "i.MX 8M Mini SAI";
    dc->vmsd = &vmstate_sai;
    device_class_set_legacy_reset(dc, sai_reset);
    device_class_set_props(dc, sai_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo sai_info = {
    .name          = TYPE_IMX8MM_SAI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX8MMSAIState),
    .instance_init = sai_init,
    .class_init    = sai_class_init,
};

static void sai_register_types(void)
{
    type_register_static(&sai_info);
}

type_init(sai_register_types)
