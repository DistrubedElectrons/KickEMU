/*
 * i.MX 8M Mini UART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_UART_H
#define KICK_IMX8MM_UART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qemu/fifo32.h"
#include "qemu/timer.h"
#include "qom/object.h"

#include "imx8mm_ccm.h"

#define TYPE_IMX8MM_UART "imx8mm-uart"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMUARTState, IMX8MM_UART)

/* GPIO names: interrupt to the GIC and the two SDMA request lines */
#define IMX8MM_UART_GPIO_IRQ    "irq"
#define IMX8MM_UART_GPIO_DMA_RX "dma-rx"
#define IMX8MM_UART_GPIO_DMA_TX "dma-tx"

#define IMX8MM_UART_MMIO_SIZE   0x10000
#define IMX8MM_UART_FIFO_SIZE   32

struct IMX8MMUARTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    qemu_irq dma_rx;
    qemu_irq dma_tx;

    /* Registers (status bits that follow the FIFOs are recomputed) */
    uint32_t ucr1;
    uint32_t ucr2;
    uint32_t ucr3;
    uint32_t ucr4;
    uint32_t ufcr;
    uint32_t usr1;
    uint32_t usr2;
    uint32_t uesc;
    uint32_t utim;
    uint32_t ubir;
    uint32_t ubmr;
    uint32_t onems;
    uint32_t uts;
    uint32_t umcr;
    uint32_t urxd_last;     /* returned (without CHARRDY) on an empty read */

    /* RX FIFO entries are URXD values: data plus the per-character flags */
    Fifo32 rx_fifo;
    Fifo32 tx_fifo;

    /* Transmit shift register */
    bool tx_busy;           /* a character is on the line */
    bool tx_stalled;        /* the backend did not take tx_char yet */
    uint32_t tx_char;
    guint tx_watch;
    QEMUTimer tx_timer;

    /* Receive shift register */
    bool rx_busy;
    uint32_t rx_char;
    QEMUTimer rx_timer;

    QEMUTimer ageing_timer;
    QEMUTimer idle_timer;

    /* Reference clock: CCM root if linked, else ref_clock_hz */
    IMX8MMCCMState *ccm;
    uint32_t ccm_root;
    uint32_t ref_clock_hz;
    bool line_timing;
};

#endif /* KICK_IMX8MM_UART_H */
