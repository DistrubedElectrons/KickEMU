/*
 * i.MX 8M Mini PWM (the i.MX27-style PWMv2 block)
 *
 * Register level, without output timing: the KICK uses PWM1 only for the panel
 * backlight (pwm-backlight in the device tree), so what matters is the duty
 * cycle the guest programs. The model keeps the registers Linux' pwm-imx27
 * driver uses and reports the resulting duty through the read-only
 * "duty-permille" property, which the host reads to dim the emulated panel.
 *
 * Register map (i.MX 8M Mini RM, PWM chapter; same as drivers/pwm/pwm-imx27.c):
 *   0x00 PWMCR   EN[0] REPEAT[2:1] SWR[3] PRESCALER[15:4] CLKSRC[17:16]
 *                POUTC[19:18] HCTR[20] BCTR[21] DBGEN..STOPEN[25:22] FWM[27:26]
 *   0x04 PWMSR   FIFOAV[2:0] FE[3] ROV[4] CMP[5] FWE[6]   (ROV/CMP/FWE w1c)
 *   0x08 PWMIR   FIE[0] RIE[1] CIE[2]
 *   0x0c PWMSAR  sample, written into a 4-deep FIFO
 *   0x10 PWMPR   period: the counter runs PWMPR + 2 cycles
 *   0x14 PWMCNR  counter
 *
 * The sample FIFO is drained the moment it is written, so FIFOAV reads 0 and
 * FE reads 1: pwm-imx27 waits for a free FIFO slot before each update and
 * must never see the FIFO full. SWR completes immediately.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "imx8mm_pwm.h"

#define PWMCR       0x00
#define PWMSR       0x04
#define PWMIR       0x08
#define PWMSAR      0x0c
#define PWMPR       0x10
#define PWMCNR      0x14

#define CR_EN           (1u << 0)
#define CR_SWR          (1u << 3)
#define CR_POUTC_SHIFT  18
#define CR_POUTC_MASK   (3u << CR_POUTC_SHIFT)
#define CR_WRITABLE     0x0ffffff7u   /* everything but SWR, which self-clears */

#define SR_FE           (1u << 3)
#define SR_ROV          (1u << 4)
#define SR_CMP          (1u << 5)
#define SR_FWE          (1u << 6)
#define SR_W1C          (SR_ROV | SR_CMP | SR_FWE)

#define IR_MASK         0x7u
#define SAR_MASK        0xffffu
#define PR_MASK         0xffffu

uint32_t imx8mm_pwm_duty_permille(IMX8MMPWMState *s)
{
    uint64_t period = (uint64_t)s->pr + 2;
    uint64_t duty;

    if (!(s->cr & CR_EN)) {
        return 0;
    }
    duty = MIN((uint64_t)s->sar, period) * 1000 / period;
    /* POUTC 1: output cleared at rollover, set at compare (inverted) */
    if (((s->cr & CR_POUTC_MASK) >> CR_POUTC_SHIFT) == 1) {
        duty = 1000 - duty;
    } else if (((s->cr & CR_POUTC_MASK) >> CR_POUTC_SHIFT) >= 2) {
        duty = 0;   /* output disconnected */
    }
    return duty;
}

static void imx8mm_pwm_update_irq(IMX8MMPWMState *s)
{
    /* FIE asks for "FIFO empty", which is always true here */
    bool fifo = (s->ir & 1u) != 0;
    bool rov = (s->ir & 2u) && (s->sr & SR_ROV);
    bool cmp = (s->ir & 4u) && (s->sr & SR_CMP);

    qemu_set_irq(s->irq, fifo || rov || cmp);
}

static void imx8mm_pwm_reset(DeviceState *dev)
{
    IMX8MMPWMState *s = IMX8MM_PWM(dev);

    s->cr = 0;
    s->sr = 0;
    s->ir = 0;
    s->sar = 0;
    s->pr = 0xfffe;     /* reset value of PWMPR */
    imx8mm_pwm_update_irq(s);
}

static uint64_t imx8mm_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX8MMPWMState *s = opaque;

    switch (offset) {
    case PWMCR:
        return s->cr;
    case PWMSR:
        return s->sr | SR_FE;
    case PWMIR:
        return s->ir;
    case PWMSAR:
        return s->sar;
    case PWMPR:
        return s->pr;
    case PWMCNR:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imx8mm.pwm: read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void imx8mm_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX8MMPWMState *s = opaque;

    switch (offset) {
    case PWMCR:
        if (value & CR_SWR) {
            /* Software reset: every register back to its reset value at once */
            imx8mm_pwm_reset(DEVICE(s));
            return;
        }
        s->cr = value & CR_WRITABLE;
        break;
    case PWMSR:
        s->sr &= ~(value & SR_W1C);
        break;
    case PWMIR:
        s->ir = value & IR_MASK;
        break;
    case PWMSAR:
        s->sar = value & SAR_MASK;
        break;
    case PWMPR:
        s->pr = value & PR_MASK;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imx8mm.pwm: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", value, offset);
        return;
    }
    imx8mm_pwm_update_irq(s);
}

static const MemoryRegionOps imx8mm_pwm_ops = {
    .read = imx8mm_pwm_read,
    .write = imx8mm_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx8mm_pwm_get_duty(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t duty = imx8mm_pwm_duty_permille(IMX8MM_PWM(obj));

    visit_type_uint32(v, name, &duty, errp);
}

static void imx8mm_pwm_init(Object *obj)
{
    IMX8MMPWMState *s = IMX8MM_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &imx8mm_pwm_ops, s,
                          TYPE_IMX8MM_PWM, IMX8MM_PWM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    object_property_add(obj, "duty-permille", "uint32", imx8mm_pwm_get_duty,
                        NULL, NULL, NULL);
}

static const VMStateDescription vmstate_imx8mm_pwm = {
    .name = TYPE_IMX8MM_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMX8MMPWMState),
        VMSTATE_UINT32(sr, IMX8MMPWMState),
        VMSTATE_UINT32(ir, IMX8MMPWMState),
        VMSTATE_UINT32(sar, IMX8MMPWMState),
        VMSTATE_UINT32(pr, IMX8MMPWMState),
        VMSTATE_END_OF_LIST()
    }
};

static void imx8mm_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, imx8mm_pwm_reset);
    dc->vmsd = &vmstate_imx8mm_pwm;
    dc->desc = "i.MX 8M Mini PWM";
}

static const TypeInfo imx8mm_pwm_info = {
    .name = TYPE_IMX8MM_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX8MMPWMState),
    .instance_init = imx8mm_pwm_init,
    .class_init = imx8mm_pwm_class_init,
};

static void imx8mm_pwm_register_types(void)
{
    type_register_static(&imx8mm_pwm_info);
}

type_init(imx8mm_pwm_register_types)
