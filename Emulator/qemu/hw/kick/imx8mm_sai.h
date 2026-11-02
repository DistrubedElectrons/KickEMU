/*
 * i.MX 8M Mini Synchronous Audio Interface (SAI)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_KICK_IMX8MM_SAI_H
#define HW_KICK_IMX8MM_SAI_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qemu/notify.h"
#include "qom/object.h"

#define TYPE_IMX8MM_SAI "imx8mm-sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMSAIState, IMX8MM_SAI)

#define IMX8MM_SAI_MMIO_SIZE    0x10000
#define IMX8MM_SAI_LINES        8       /* data lines / FIFOs per direction */
#define IMX8MM_SAI_FIFO_MAX     128

/*
 * Outputs (qdev named GPIOs): DMA request lines for the SDMA event inputs.
 * The interrupt is sysbus IRQ 0.
 */
#define IMX8MM_SAI_GPIO_DMA_TX  "dma-tx"
#define IMX8MM_SAI_GPIO_DMA_RX  "dma-rx"

enum {
    IMX8MM_SAI_TX = 0,
    IMX8MM_SAI_RX = 1,
};

typedef struct IMX8MMSAIDir {
    uint32_t csr;
    uint32_t cr[5];             /* xCR1..xCR5 */
    uint32_t mr;
    uint32_t fifo[IMX8MM_SAI_LINES][IMX8MM_SAI_FIFO_MAX];
    uint32_t wfp[IMX8MM_SAI_LINES];     /* free running, mod 2 * depth */
    uint32_t rfp[IMX8MM_SAI_LINES];

    /* Frame engine */
    bool running;
    uint32_t rate;
    int64_t t0_ns;
    uint64_t frames_done;
    uint32_t word;              /* next word index within the frame */
    int64_t stall_ns;           /* FIFO waiting for the DMA since, or 0 */

    /* Host audio ring (interleaved int32 stereo or mono) */
    int32_t *ring;
    uint32_t ring_frames;
    uint32_t ring_rd;
    uint32_t ring_wr;
    uint32_t ring_fill;
    uint32_t voice_rate;
    uint32_t voice_channels;
    bool fmt_logged;
} IMX8MMSAIDir;

/*
 * WAV file attached to data line 0: capture input (replaces the audiodev
 * input) or playback recording. See imx8mm_sai.c "WAV files".
 */
typedef struct IMX8MMSAIWav {
    char *path;                 /* NULL when off */
    FILE *f;
    /* input */
    uint64_t data_off;          /* first sample byte */
    uint64_t frames;            /* frames in the data chunk */
    uint64_t pos;               /* next frame to read */
    uint16_t channels;
    uint16_t bits;
    uint16_t block_align;
    bool is_float;
    /* output */
    uint32_t rate;              /* header sample rate */
    bool full;                  /* reached the 4 GiB RIFF limit */
} IMX8MMSAIWav;

struct IMX8MMSAIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req[2];
    QEMUTimer *tick;
    AudioBackend *audio_be;
    SWVoiceOut *voice_out;
    SWVoiceIn *voice_in;

    /* Properties */
    uint32_t verid;
    uint32_t datalines;
    uint32_t fifo_words;
    uint32_t max_slots;
    uint32_t bus_clk_hz;
    uint32_t mclk_hz[4];        /* MCLK option 1..3 (index 0 unused) */
    uint32_t ext_frame_rate;    /* frame rate when clocks come from outside */
    uint32_t tick_us;
    uint32_t buffer_ms;

    IMX8MMSAIDir dir[2];
    uint32_t mctl;
    uint32_t mdiv;
    bool dma_level[2];

    /* WAV files on data line 0 and frame counters since start-up */
    IMX8MMSAIWav wav_in;
    IMX8MMSAIWav wav_out;
    bool wav_in_loop;
    uint64_t out_frames;        /* frames shifted out by the transmitter */
    uint64_t in_frames;         /* frames shifted in by the receiver */
    uint64_t wav_out_frames;    /* frames in the current recording */
    Notifier exit_notifier;
};

#endif
