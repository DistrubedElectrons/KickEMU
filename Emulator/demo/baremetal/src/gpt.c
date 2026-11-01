/*
 * General Purpose Timer (IMX8MMRM chapter 12.1 "GPT").
 *
 * GPT1 runs from its CCM clock root (CLKSRC = 1, ipg_clk), divided by the
 * prescaler to 1 MHz. Output compare 1 in restart mode (FRR = 0) resets the
 * counter on every match and raises OF1, which is enabled in IR and reaches
 * the GIC as SPI 55.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gic.h"
#include "gpt.h"
#include "imx8mm.h"

#define GPT_CR          0x00
#define GPT_PR          0x04
#define GPT_SR          0x08
#define GPT_IR          0x0c
#define GPT_OCR1        0x10

#define CR_EN           (1u << 0)
#define CR_ENMOD        (1u << 1)
#define CR_CLKSRC_IPG   (1u << 6)
#define CR_SWR          (1u << 15)
#define SR_OF1          (1u << 0)
#define IR_OF1IE        (1u << 0)

#define GPT_COUNT_HZ    1000000u

static volatile uint64_t ticks;

static void gpt_irq(void)
{
    wr32(GPT1_BASE + GPT_SR, SR_OF1);   /* write 1 to clear */
    ticks++;
}

void gpt_start(uint32_t clk_hz, uint32_t hz)
{
    wr32(GPT1_BASE + GPT_CR, CR_SWR);
    while (rd32(GPT1_BASE + GPT_CR) & CR_SWR) {
    }
    wr32(GPT1_BASE + GPT_PR, clk_hz / GPT_COUNT_HZ - 1);
    wr32(GPT1_BASE + GPT_OCR1, GPT_COUNT_HZ / hz - 1);
    wr32(GPT1_BASE + GPT_SR, 0x3f);
    wr32(GPT1_BASE + GPT_IR, IR_OF1IE);
    gic_enable_spi(IRQ_GPT1, 0x80, gpt_irq);
    wr32(GPT1_BASE + GPT_CR, CR_EN | CR_ENMOD | CR_CLKSRC_IPG);
}

uint64_t gpt_ticks(void)
{
    return ticks;
}
