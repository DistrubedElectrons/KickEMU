/*
 * NXP PCF85263A real-time clock (I2C slave)
 *
 * Register level model of the RTC mode of the PCF85263A (datasheet rev. 4,
 * section 7 "Functional description"): 8-bit register addressing with
 * auto-increment over 0x00..0x2F (wrapping to 0x00), BCD time registers
 * including 100th seconds, alarm 1 and 2, periodic interrupt, the control
 * registers and the reset register (CPR/CTS/SR).
 *
 * Time keeping follows the QEMU RTC conventions: the time is derived from
 * rtc_clock (-rtc base=/clock=) plus a guest-set offset. While STOP is set
 * the time registers are frozen and can be written as a block; clearing
 * STOP restarts the clock from the written value. Year 00..99 maps to
 * 2000..2099. Multi-byte accesses see a consistent snapshot, as the chip
 * buffers the counters during an I2C transfer.
 *
 * Not modelled (registers are stored and read back, LOG_UNIMP on use):
 * stopwatch mode (Function.RTCM = 1), timestamps (TS pin not connected on
 * the KICK board), watchdog, battery switch-over and frequency offset
 * calibration.
 *
 * Note for the KICK board: the vendor DT overrides this node's compatible
 * with "dallas,ds1337" and the kernel only enables RTC_DRV_DS1307, so the
 * guest talks to this register map with the DS1337 layout, as on the real
 * board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/rtc.h"
#include "system/system.h"
#include "pcf85263.h"

OBJECT_DECLARE_SIMPLE_TYPE(PCF85263State, PCF85263)

/* RTC mode register map */
#define REG_100THS          0x00
#define REG_SECONDS         0x01    /* bit 7: OS, oscillator stopped */
#define REG_MINUTES         0x02    /* bit 7: EMON */
#define REG_HOURS           0x03    /* 12h mode: bit 5 AMPM */
#define REG_DAYS            0x04
#define REG_WEEKDAYS        0x05
#define REG_MONTHS          0x06
#define REG_YEARS           0x07
#define REG_SECOND_ALM1     0x08
#define REG_MINUTE_ALM1     0x09
#define REG_HOUR_ALM1       0x0a
#define REG_DAY_ALM1        0x0b
#define REG_MONTH_ALM1      0x0c
#define REG_MINUTE_ALM2     0x0d
#define REG_HOUR_ALM2       0x0e
#define REG_WEEKDAY_ALM2    0x0f
#define REG_ALARM_ENABLES   0x10
#define REG_TSR1            0x11    /* timestamps 0x11..0x22 */
#define REG_TSR_MODE        0x23
#define REG_OFFSET          0x24
#define REG_OSCILLATOR      0x25
#define REG_BATTERY_SWITCH  0x26
#define REG_PIN_IO          0x27
#define REG_FUNCTION        0x28
#define REG_INTA_ENABLE     0x29
#define REG_INTB_ENABLE     0x2a
#define REG_FLAGS           0x2b
#define REG_RAM_BYTE        0x2c
#define REG_WATCHDOG        0x2d
#define REG_STOP_ENABLE     0x2e
#define REG_RESETS          0x2f
#define NUM_REGS            0x30

#define SECONDS_OS          BIT(7)
#define MINUTES_EMON        BIT(7)
#define HOURS_AMPM          BIT(5)

/* Alarm_enables */
#define ALM_SEC_A1E         BIT(0)
#define ALM_MIN_A1E         BIT(1)
#define ALM_HR_A1E          BIT(2)
#define ALM_DAY_A1E         BIT(3)
#define ALM_MON_A1E         BIT(4)
#define ALM_MIN_A2E         BIT(5)
#define ALM_HR_A2E          BIT(6)
#define ALM_DAY_A2E         BIT(7)

/* Oscillator */
#define OSC_12_24           BIT(5)  /* 1: 12 hour mode */

/* Pin_IO */
#define PINIO_INTAPM_MASK   0x03
#define PINIO_INTAPM_INTA   0x02
#define PINIO_TSPM_MASK     0x0c
#define PINIO_TSPM_INTB     0x08

/* Function */
#define FUNC_100TH          BIT(7)
#define FUNC_PI_SHIFT       5
#define FUNC_PI_MASK        (3 << FUNC_PI_SHIFT)
#define FUNC_RTCM           BIT(4)
#define FUNC_STOPM          BIT(3)

/* INTA_enable / INTB_enable */
#define INT_WDIE            BIT(0)
#define INT_BSIE            BIT(1)
#define INT_TSRIE           BIT(2)
#define INT_A2IE            BIT(3)
#define INT_A1IE            BIT(4)
#define INT_OIE             BIT(5)
#define INT_PIE             BIT(6)
#define INT_ILP             BIT(7)  /* 1: level, 0: pulse */

/* Flags */
#define FLAG_TSR1F          BIT(0)
#define FLAG_TSR2F          BIT(1)
#define FLAG_TSR3F          BIT(2)
#define FLAG_BSF            BIT(3)
#define FLAG_WDF            BIT(4)
#define FLAG_A1F            BIT(5)
#define FLAG_A2F            BIT(6)
#define FLAG_PIF            BIT(7)

/* Stop_enable */
#define STOP_EN_STOP        BIT(0)

/* Resets: fixed command patterns */
#define RESET_CPR           0xa4    /* clear prescaler */
#define RESET_CTS           0x25    /* clear timestamps */
#define RESET_SR            0x2c    /* software reset */

#define NS_PER_SEC          1000000000LL

struct PCF85263State {
    I2CSlave parent_obj;

    uint32_t int_pulse_us;

    qemu_irq inta;
    qemu_irq intb;
    QEMUTimer *tick_timer;      /* fires at each second boundary */
    QEMUTimer *pulse_timer;     /* ends interrupt pulses */

    uint8_t regs[NUM_REGS];
    uint8_t ptr;
    bool addr_received;

    /* Running clock: time = reference + offset_ns. Stopped: regs hold it. */
    int64_t offset_ns;
    int64_t stopped_subsec_ns;  /* sub-second part while STOP is set */
    int8_t wday_offset;
    bool pulse_a;
    bool pulse_b;
};

/*
 * Reference time in ns on the rtc_clock time line. qemu_get_timedate()
 * provides the seconds (honouring -rtc base=), the sub-second part comes
 * from the same clock.
 */
static int64_t pcf_ref_ns(void)
{
    struct tm tm;
    int64_t ns1, ns2;

    do {
        ns1 = qemu_clock_get_ns(rtc_clock);
        qemu_get_timedate(&tm, 0);
        ns2 = qemu_clock_get_ns(rtc_clock);
    } while (ns1 / NS_PER_SEC != ns2 / NS_PER_SEC);
    return (int64_t)mktimegm(&tm) * NS_PER_SEC + ns2 % NS_PER_SEC;
}

static bool pcf_stopped(PCF85263State *s)
{
    return s->regs[REG_STOP_ENABLE] & STOP_EN_STOP;
}

static uint8_t pcf_hours_to_reg(PCF85263State *s, int hour)
{
    if (s->regs[REG_OSCILLATOR] & OSC_12_24) {
        bool pm = hour >= 12;
        int h12 = hour % 12;

        if (h12 == 0) {
            h12 = 12;
        }
        return to_bcd(h12) | (pm ? HOURS_AMPM : 0);
    }
    return to_bcd(hour);
}

static int pcf_reg_to_hours(PCF85263State *s, uint8_t reg)
{
    if (s->regs[REG_OSCILLATOR] & OSC_12_24) {
        int h = from_bcd(reg & 0x1f) % 12;

        return (reg & HOURS_AMPM) ? h + 12 : h;
    }
    return from_bcd(reg & 0x3f);
}

/* Fill the time registers from a time in ns (UTC broken down). */
static void pcf_time_to_regs(PCF85263State *s, int64_t t_ns)
{
    time_t secs = t_ns / NS_PER_SEC;
    int64_t subsec = t_ns % NS_PER_SEC;
    struct tm tm;

    if (subsec < 0) {
        subsec += NS_PER_SEC;
        secs--;
    }
    gmtime_r(&secs, &tm);
    s->regs[REG_100THS] = (s->regs[REG_FUNCTION] & FUNC_100TH) ?
                          to_bcd(subsec / 10000000) : 0;
    s->regs[REG_SECONDS] = (s->regs[REG_SECONDS] & SECONDS_OS) |
                           to_bcd(tm.tm_sec);
    s->regs[REG_MINUTES] = (s->regs[REG_MINUTES] & MINUTES_EMON) |
                           to_bcd(tm.tm_min);
    s->regs[REG_HOURS] = pcf_hours_to_reg(s, tm.tm_hour);
    s->regs[REG_DAYS] = to_bcd(tm.tm_mday);
    s->regs[REG_WEEKDAYS] = (tm.tm_wday + s->wday_offset + 7) % 7;
    s->regs[REG_MONTHS] = to_bcd(tm.tm_mon + 1);
    s->regs[REG_YEARS] = to_bcd(tm.tm_year % 100);
}

/* Convert the time registers back to ns. Also updates wday_offset. */
static int64_t pcf_regs_to_time(PCF85263State *s)
{
    struct tm tm = {
        .tm_sec = from_bcd(s->regs[REG_SECONDS] & 0x7f),
        .tm_min = from_bcd(s->regs[REG_MINUTES] & 0x7f),
        .tm_hour = pcf_reg_to_hours(s, s->regs[REG_HOURS]),
        .tm_mday = from_bcd(s->regs[REG_DAYS] & 0x3f),
        .tm_mon = from_bcd(s->regs[REG_MONTHS] & 0x1f) - 1,
        .tm_year = 100 + from_bcd(s->regs[REG_YEARS]),
    };
    time_t secs = mktimegm(&tm);
    struct tm check;

    gmtime_r(&secs, &check);
    s->wday_offset = ((s->regs[REG_WEEKDAYS] & 0x07) - check.tm_wday + 7) % 7;
    return (int64_t)secs * NS_PER_SEC +
           (int64_t)from_bcd(s->regs[REG_100THS]) * 10000000;
}

static int64_t pcf_now_ns(PCF85263State *s)
{
    return pcf_ref_ns() + s->offset_ns;
}

/* Bring the time registers up to date when the clock is running. */
static void pcf_capture(PCF85263State *s)
{
    if (!pcf_stopped(s)) {
        pcf_time_to_regs(s, pcf_now_ns(s));
    }
}

static void pcf_update_irq(PCF85263State *s)
{
    uint8_t flags = s->regs[REG_FLAGS];
    uint8_t ena = s->regs[REG_INTA_ENABLE];
    uint8_t enb = s->regs[REG_INTB_ENABLE];
    /* Flag bits PIF/A2F/A1F/WDF/BSF/TSR* map onto the enable bits */
    uint8_t srcs = ((flags & FLAG_PIF) ? INT_PIE : 0) |
                   ((flags & FLAG_A1F) ? INT_A1IE : 0) |
                   ((flags & FLAG_A2F) ? INT_A2IE : 0) |
                   ((flags & FLAG_WDF) ? INT_WDIE : 0) |
                   ((flags & FLAG_BSF) ? INT_BSIE : 0) |
                   ((flags & (FLAG_TSR1F | FLAG_TSR2F | FLAG_TSR3F)) ?
                    INT_TSRIE : 0);
    bool a, b;

    a = (ena & INT_ILP) ? (srcs & ena) != 0 : s->pulse_a;
    b = (enb & INT_ILP) ? (srcs & enb) != 0 : s->pulse_b;
    a = a && (s->regs[REG_PIN_IO] & PINIO_INTAPM_MASK) == PINIO_INTAPM_INTA;
    b = b && (s->regs[REG_PIN_IO] & PINIO_TSPM_MASK) == PINIO_TSPM_INTB;
    /* Both outputs are active low */
    qemu_set_irq(s->inta, !a);
    qemu_set_irq(s->intb, !b);
}

static void pcf_pulse_end(void *opaque)
{
    PCF85263State *s = opaque;

    s->pulse_a = false;
    s->pulse_b = false;
    pcf_update_irq(s);
}

/* Raise flag bits and generate the interrupt pulses of enabled sources. */
static void pcf_raise_flags(PCF85263State *s, uint8_t flags, uint8_t int_bits)
{
    s->regs[REG_FLAGS] |= flags;
    if (s->regs[REG_INTA_ENABLE] & int_bits) {
        s->pulse_a = true;
    }
    if (s->regs[REG_INTB_ENABLE] & int_bits) {
        s->pulse_b = true;
    }
    if (s->pulse_a || s->pulse_b) {
        timer_mod(s->pulse_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->int_pulse_us * SCALE_US);
    }
    pcf_update_irq(s);
}

static bool pcf_tick_needed(PCF85263State *s)
{
    return !pcf_stopped(s) &&
           (s->regs[REG_ALARM_ENABLES] ||
            (s->regs[REG_FUNCTION] & FUNC_PI_MASK));
}

/*
 * Arm the tick timer for the next second boundary of the RTC time line.
 * The timer runs on rtc_clock so it follows the same clock as the counters.
 */
static void pcf_schedule_tick(PCF85263State *s)
{
    int64_t now, sub;

    if (!pcf_tick_needed(s)) {
        timer_del(s->tick_timer);
        return;
    }
    now = pcf_now_ns(s);
    sub = now % NS_PER_SEC;
    if (sub < 0) {
        sub += NS_PER_SEC;
    }
    timer_mod(s->tick_timer, qemu_clock_get_ns(rtc_clock) + NS_PER_SEC - sub);
}

static bool pcf_alarm_field(uint8_t alarm, uint8_t now, uint8_t mask)
{
    return (alarm & mask) == (now & mask);
}

static void pcf_tick(void *opaque)
{
    PCF85263State *s = opaque;
    uint8_t en = s->regs[REG_ALARM_ENABLES];
    uint8_t *r = s->regs;
    uint8_t flags = 0, ints = 0;
    uint8_t pi;

    if (pcf_stopped(s)) {
        return;
    }
    /* Round to the second that just started */
    pcf_time_to_regs(s, pcf_now_ns(s) + NS_PER_SEC / 2);

    if (en & (ALM_SEC_A1E | ALM_MIN_A1E | ALM_HR_A1E | ALM_DAY_A1E |
              ALM_MON_A1E)) {
        bool match = true;

        match &= !(en & ALM_SEC_A1E) ||
                 pcf_alarm_field(r[REG_SECOND_ALM1], r[REG_SECONDS], 0x7f);
        match &= !(en & ALM_MIN_A1E) ||
                 pcf_alarm_field(r[REG_MINUTE_ALM1], r[REG_MINUTES], 0x7f);
        match &= !(en & ALM_HR_A1E) ||
                 pcf_alarm_field(r[REG_HOUR_ALM1], r[REG_HOURS], 0x3f);
        match &= !(en & ALM_DAY_A1E) ||
                 pcf_alarm_field(r[REG_DAY_ALM1], r[REG_DAYS], 0x3f);
        match &= !(en & ALM_MON_A1E) ||
                 pcf_alarm_field(r[REG_MONTH_ALM1], r[REG_MONTHS], 0x1f);
        if (match) {
            flags |= FLAG_A1F;
            ints |= INT_A1IE;
        }
    }
    /* Alarm 2 has minute resolution: evaluate at second 00 only */
    if ((en & (ALM_MIN_A2E | ALM_HR_A2E | ALM_DAY_A2E)) &&
        r[REG_SECONDS] == 0) {
        bool match = true;

        match &= !(en & ALM_MIN_A2E) ||
                 pcf_alarm_field(r[REG_MINUTE_ALM2], r[REG_MINUTES], 0x7f);
        match &= !(en & ALM_HR_A2E) ||
                 pcf_alarm_field(r[REG_HOUR_ALM2], r[REG_HOURS], 0x3f);
        match &= !(en & ALM_DAY_A2E) ||
                 pcf_alarm_field(r[REG_WEEKDAY_ALM2], r[REG_WEEKDAYS], 0x07);
        if (match) {
            flags |= FLAG_A2F;
            ints |= INT_A2IE;
        }
    }
    /* Periodic interrupt: 01 every second, 10 every minute, 11 every hour */
    pi = (r[REG_FUNCTION] & FUNC_PI_MASK) >> FUNC_PI_SHIFT;
    if ((pi == 1) ||
        (pi == 2 && (r[REG_SECONDS] & 0x7f) == 0) ||
        (pi == 3 && (r[REG_SECONDS] & 0x7f) == 0 && r[REG_MINUTES] == 0)) {
        flags |= FLAG_PIF;
        ints |= INT_PIE;
    }
    if (flags) {
        pcf_raise_flags(s, flags, ints);
    }
    pcf_schedule_tick(s);
}

/* Power-on values of the control registers (datasheet table 6) */
static void pcf_reset_control(PCF85263State *s)
{
    memset(&s->regs[REG_SECOND_ALM1], 0, REG_STOP_ENABLE - REG_SECOND_ALM1);
    s->regs[REG_RESETS] = 0;
    s->pulse_a = false;
    s->pulse_b = false;
    timer_del(s->pulse_timer);
}

static void pcf_clear_prescaler(PCF85263State *s)
{
    if (pcf_stopped(s)) {
        s->stopped_subsec_ns = 0;
        s->regs[REG_100THS] = 0;
    } else {
        int64_t now = pcf_now_ns(s);
        int64_t sub = now % NS_PER_SEC;

        if (sub < 0) {
            sub += NS_PER_SEC;
        }
        /* The seconds counter restarts its current second right now */
        s->offset_ns -= sub;
    }
}

static void pcf_write_stop(PCF85263State *s, uint8_t val)
{
    bool was = pcf_stopped(s);
    bool stop = val & STOP_EN_STOP;

    if (!was && stop) {
        int64_t now = pcf_now_ns(s);

        pcf_time_to_regs(s, now);
        s->stopped_subsec_ns = now % 10000000;
    }
    s->regs[REG_STOP_ENABLE] = val & STOP_EN_STOP;
    if (was && !stop) {
        s->offset_ns = pcf_regs_to_time(s) + s->stopped_subsec_ns -
                       pcf_ref_ns();
    }
    pcf_schedule_tick(s);
}

static void pcf_write(PCF85263State *s, uint8_t reg, uint8_t val)
{
    if (reg <= REG_YEARS) {
        if (pcf_stopped(s)) {
            s->regs[reg] = val;
        } else {
            /*
             * Writing a running counter: take the current time, replace the
             * field and restart the time line from there.
             */
            int64_t now = pcf_now_ns(s);

            pcf_time_to_regs(s, now);
            s->regs[reg] = val;
            s->offset_ns = pcf_regs_to_time(s) + now % 10000000 - pcf_ref_ns();
            pcf_schedule_tick(s);
        }
        return;
    }
    switch (reg) {
    case REG_SECOND_ALM1 ... REG_ALARM_ENABLES:
        s->regs[reg] = val;
        pcf_schedule_tick(s);
        break;
    case REG_TSR1 ... REG_TSR_MODE:
        /* Timestamp registers are read only except the mode register */
        if (reg == REG_TSR_MODE) {
            s->regs[reg] = val;
            if (val) {
                qemu_log_mask(LOG_UNIMP, "%s: timestamp mode 0x%02x: TS pin "
                              "events are not modelled\n", TYPE_PCF85263, val);
            }
        }
        break;
    case REG_OFFSET:
    case REG_BATTERY_SWITCH:
    case REG_RAM_BYTE:
        s->regs[reg] = val;
        break;
    case REG_OSCILLATOR:
        /* Switching 12/24 h mode reformats the hours register */
        pcf_capture(s);
        {
            int hour = pcf_reg_to_hours(s, s->regs[REG_HOURS]);

            s->regs[reg] = val;
            s->regs[REG_HOURS] = pcf_hours_to_reg(s, hour);
        }
        break;
    case REG_PIN_IO:
    case REG_INTA_ENABLE:
    case REG_INTB_ENABLE:
        s->regs[reg] = val;
        pcf_update_irq(s);
        break;
    case REG_FUNCTION:
        if (val & FUNC_RTCM) {
            qemu_log_mask(LOG_UNIMP, "%s: stopwatch mode not modelled\n",
                          TYPE_PCF85263);
        }
        s->regs[reg] = val;
        pcf_schedule_tick(s);
        break;
    case REG_FLAGS:
        /* Flags are cleared by writing 0, writing 1 has no effect */
        s->regs[reg] &= val;
        pcf_update_irq(s);
        break;
    case REG_WATCHDOG:
        s->regs[reg] = val;
        if (val & 0x7c) {
            qemu_log_mask(LOG_UNIMP, "%s: watchdog not modelled\n",
                          TYPE_PCF85263);
        }
        break;
    case REG_STOP_ENABLE:
        pcf_write_stop(s, val);
        break;
    case REG_RESETS:
        switch (val) {
        case RESET_CPR:
            pcf_clear_prescaler(s);
            break;
        case RESET_CTS:
            memset(&s->regs[REG_TSR1], 0, REG_TSR_MODE - REG_TSR1);
            break;
        case RESET_SR:
            /* Software reset: control registers to defaults, CPR and CTS */
            pcf_capture(s);
            pcf_reset_control(s);
            memset(&s->regs[REG_TSR1], 0, REG_TSR_MODE - REG_TSR1);
            pcf_clear_prescaler(s);
            pcf_schedule_tick(s);
            pcf_update_irq(s);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid reset command 0x%02x\n",
                          TYPE_PCF85263, val);
            break;
        }
        break;
    default:
        g_assert_not_reached();
    }
}

static int pcf_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF85263State *s = PCF85263(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->addr_received = false;
        /* fall through */
    case I2C_START_RECV:
        /* Counters are buffered for the duration of the transfer */
        pcf_capture(s);
        break;
    default:
        break;
    }
    return 0;
}

static int pcf_send(I2CSlave *i2c, uint8_t data)
{
    PCF85263State *s = PCF85263(i2c);

    if (!s->addr_received) {
        s->ptr = data;
        s->addr_received = true;
        if (s->ptr >= NUM_REGS) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02x out of range\n",
                          TYPE_PCF85263, data);
        }
        return 0;
    }
    if (s->ptr < NUM_REGS) {
        pcf_write(s, s->ptr, data);
    }
    s->ptr = (s->ptr + 1) % NUM_REGS;
    return 0;
}

static uint8_t pcf_recv(I2CSlave *i2c)
{
    PCF85263State *s = PCF85263(i2c);
    uint8_t val = s->ptr < NUM_REGS ? s->regs[s->ptr] : 0;

    s->ptr = (s->ptr + 1) % NUM_REGS;
    return val;
}

static void pcf_reset(DeviceState *dev)
{
    PCF85263State *s = PCF85263(dev);

    pcf_reset_control(s);
    memset(&s->regs[REG_TSR1], 0, REG_TSR_MODE - REG_TSR1 + 1);
    s->regs[REG_STOP_ENABLE] = 0;
    s->ptr = 0;
    s->addr_received = false;
    /*
     * Battery backed: the clock keeps running across emulator resets with
     * the guest offset, and OS stays clear.
     */
    s->regs[REG_SECONDS] &= ~SECONDS_OS;
    pcf_capture(s);
    timer_del(s->tick_timer);
    pcf_update_irq(s);
}

static void pcf_realize(DeviceState *dev, Error **errp)
{
    PCF85263State *s = PCF85263(dev);

    s->tick_timer = timer_new_ns(rtc_clock, pcf_tick, s);
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pcf_pulse_end, s);
}

static void pcf_unrealize(DeviceState *dev)
{
    PCF85263State *s = PCF85263(dev);

    timer_free(s->tick_timer);
    timer_free(s->pulse_timer);
}

static void pcf_init(Object *obj)
{
    PCF85263State *s = PCF85263(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->inta, PCF85263_GPIO_INTA, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->intb, PCF85263_GPIO_INTB, 1);
}

static int pcf_post_load(void *opaque, int version_id)
{
    pcf_schedule_tick(opaque);
    return 0;
}

static const VMStateDescription vmstate_pcf85263 = {
    .name = TYPE_PCF85263,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcf_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF85263State),
        VMSTATE_UINT8_ARRAY(regs, PCF85263State, NUM_REGS),
        VMSTATE_UINT8(ptr, PCF85263State),
        VMSTATE_BOOL(addr_received, PCF85263State),
        VMSTATE_INT64(offset_ns, PCF85263State),
        VMSTATE_INT64(stopped_subsec_ns, PCF85263State),
        VMSTATE_INT8(wday_offset, PCF85263State),
        VMSTATE_BOOL(pulse_a, PCF85263State),
        VMSTATE_BOOL(pulse_b, PCF85263State),
        VMSTATE_TIMER_PTR(pulse_timer, PCF85263State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pcf_properties[] = {
    /* Length of an interrupt pulse when INTx_enable.ILP = 0 */
    DEFINE_PROP_UINT32("int-pulse-us", PCF85263State, int_pulse_us, 15625),
};

static void pcf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf_event;
    k->recv = pcf_recv;
    k->send = pcf_send;
    dc->realize = pcf_realize;
    dc->unrealize = pcf_unrealize;
    dc->desc = "NXP PCF85263A real-time clock";
    dc->vmsd = &vmstate_pcf85263;
    device_class_set_legacy_reset(dc, pcf_reset);
    device_class_set_props(dc, pcf_properties);
}

static const TypeInfo pcf_info = {
    .name          = TYPE_PCF85263,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCF85263State),
    .instance_init = pcf_init,
    .class_init    = pcf_class_init,
};

static void pcf_register_types(void)
{
    type_register_static(&pcf_info);
}

type_init(pcf_register_types)
