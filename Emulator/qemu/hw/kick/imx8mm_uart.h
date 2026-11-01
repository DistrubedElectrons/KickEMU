/*
 * i.MX 8M Mini UART: upstream imx_serial with SDMA request lines
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_IMX8MM_UART_H
#define KICK_IMX8MM_UART_H

#include "hw/char/imx_serial.h"
#include "qom/object.h"

#define TYPE_IMX8MM_UART "imx8mm-uart"
OBJECT_DECLARE_SIMPLE_TYPE(IMX8MMUARTState, IMX8MM_UART)

/* GPIO names: interrupt to the GIC and the two SDMA request lines */
#define IMX8MM_UART_GPIO_IRQ    "irq"
#define IMX8MM_UART_GPIO_DMA_RX "dma-rx"
#define IMX8MM_UART_GPIO_DMA_TX "dma-tx"

struct IMX8MMUARTState {
    IMXSerialState parent_obj;

    qemu_irq irq;
    qemu_irq dma_rx;
    qemu_irq dma_tx;
};

#endif /* KICK_IMX8MM_UART_H */
