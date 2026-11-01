/*
 * i.MX 8M Mini UART: upstream imx_serial with SDMA request lines
 *
 * QEMU's imx_serial models the register interface and interrupts but has no
 * DMA request outputs. On the KICK the Linux imx UART driver runs UART1
 * (MCU link) and UART3 (MIDI) in DMA mode through SDMA1 because their DT
 * nodes have "dmas". This subclass adds the request lines the UART drives
 * towards the SDMA event inputs (IMX8MMRM 14.2 "UART", DMA requests):
 *
 *   RX request: UCR1.RXDMAEN and the RX FIFO at or above RXTL (USR1.RRDY),
 *               or UCR1.ATDMAEN and the ageing timer expired (USR1.AGTIM)
 *   TX request: UCR1.TXDMAEN and the TX FIFO below TXTL (USR1.TRDY)
 *
 * The parent recomputes its state after every register access, received
 * character and ageing timer event and then drives its interrupt output.
 * That output is routed back into this object, which forwards it to the
 * GIC and re-evaluates both request levels from the same state, so the
 * requests follow the FIFO exactly as the interrupt does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"

#include "imx8mm_uart.h"

#define UCR1_ATDMAEN    BIT(2)
#define UCR1_TXDMAEN    BIT(3)
#define UCR1_RXDMAEN    BIT(8)

static void imx8mm_uart_update(void *opaque, int n, int level)
{
    IMX8MMUARTState *s = IMX8MM_UART(opaque);
    IMXSerialState *p = IMX_SERIAL(opaque);
    bool rx, tx;

    qemu_set_irq(s->irq, level);

    rx = ((p->ucr1 & UCR1_RXDMAEN) && (p->usr1 & USR1_RRDY)) ||
         ((p->ucr1 & UCR1_ATDMAEN) && (p->usr1 & USR1_AGTIM));
    tx = (p->ucr1 & UCR1_TXDMAEN) && (p->usr1 & USR1_TRDY);
    qemu_set_irq(s->dma_rx, rx);
    qemu_set_irq(s->dma_tx, tx);
}

static void imx8mm_uart_init(Object *obj)
{
    IMX8MMUARTState *s = IMX8MM_UART(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_out_named(dev, &s->irq, IMX8MM_UART_GPIO_IRQ, 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx, IMX8MM_UART_GPIO_DMA_RX, 1);
    qdev_init_gpio_out_named(dev, &s->dma_tx, IMX8MM_UART_GPIO_DMA_TX, 1);
    qdev_init_gpio_in_named(dev, imx8mm_uart_update, "parent-irq", 1);
}

static DeviceRealize imx8mm_uart_parent_realize;

static void imx8mm_uart_realize(DeviceState *dev, Error **errp)
{
    Error *local_err = NULL;

    imx8mm_uart_parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    /* The parent's interrupt output feeds the request logic above */
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(dev, "parent-irq", 0));
}

static void imx8mm_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    imx8mm_uart_parent_realize = dc->realize;
    dc->realize = imx8mm_uart_realize;
    dc->desc = "i.MX8MM UART with SDMA requests";
}

static const TypeInfo imx8mm_uart_types[] = {
    {
        .name          = TYPE_IMX8MM_UART,
        .parent        = TYPE_IMX_SERIAL,
        .instance_size = sizeof(IMX8MMUARTState),
        .instance_init = imx8mm_uart_init,
        .class_init    = imx8mm_uart_class_init,
    },
};

DEFINE_TYPES(imx8mm_uart_types)
