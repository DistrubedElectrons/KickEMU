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
 * Register view (property "register-map"):
 *  - "pcf85263": the PCF85263A register map described above. This is what
 *    the silicon on the KICK board implements.
 *  - "ds1337" (default): a Maxim DS1337 compatible register map (DS1337
 *    data sheet: 0x00..0x06 time with century bit, alarm 1 0x07..0x0A and
 *    alarm 2 0x0B..0x0D with mask bits, control 0x0E with nEOSC/INTCN/
 *    A2IE/A1IE, status 0x0F with OSF/A2F/A1F) on the same time base. The
 *    KICK DT overrides this node's compatible with "dallas,ds1337" and the
 *    kernel only has RTC_DRV_DS1307, so the shipped kernel speaks DS1337 to
 *    the chip. This view gives that driver a working clock; select
 *    "pcf85263" to reproduce the mismatch of the real board. The INTCN = 0
 *    square wave output is not modelled.
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

/* DS1337 register view */
#define DS_SECONDS          0x00
#define DS_MINUTES          0x01
#define DS_HOURS            0x02    /* bit 6: 12 hour mode, bit 5: PM */
#define DS_DAY              0x03    /* 1..7 */
#define DS_DATE             0x04
#define DS_MONTH            0x05    /* bit 7: century */
#define DS_YEAR             0x06
#define DS_A1_SECONDS       0x07    /* bit 7 of 0x07..0x0D: alarm mask */
#define DS_A1_MINUTES       0x08
#define DS_A1_HOURS         0x09
#define DS_A1_DAYDATE       0x0a    /* bit 6: DY/DT */
#define DS_A2_MINUTES       0x0b
#define DS_A2_HOURS         0x0c
#define DS_A2_DAYDATE       0x0d
#define DS_CONTROL          0x0e
#define DS_STATUS           0x0f
#define DS_NUM_REGS         0x10

#define DS_HOURS_12         BIT(6)
#define DS_HOURS_PM         BIT(5)
#define DS_MONTH_CENTURY    BIT(7)
#define DS_ALARM_MASK       BIT(7)
#define DS_ALARM_DY         BIT(6)
#define DS_CTRL_NEOSC       BIT(7)
#define DS_CTRL_RS_MASK     0x18
#define DS_CTRL_INTCN       BIT(2)
#define DS_CTRL_A2IE        BIT(1)
#define DS_CTRL_A1IE        BIT(0)
#define DS_CTRL_RW_MASK     0x9f
#define DS_CTRL_RESET       0x18    /* RS2 = RS1 = 1 */
#define DS_STAT_OSF         BIT(7)
#define DS_STAT_A2F         BIT(1)
#define DS_STAT_A1F         BIT(0)
#define DS_STAT_FLAGS       (DS_STAT_OSF | DS_STAT_A2F | DS_STAT_A1F)

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

    /* DS1337 register view */
    char *register_map;
    bool ds_view;
    uint8_t ds[DS_NUM_REGS];
    bool ds_time_dirty;
    int8_t ds_day_offset;
    int64_t ds_frozen_ns;
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

static void ds_tick(PCF85263State *s);

static void pcf_tick(void *opaque)
{
    PCF85263State *s = opaque;
    uint8_t en = s->regs[REG_ALARM_ENABLES];
    uint8_t *r = s->regs;
    uint8_t flags = 0, ints = 0;
    uint8_t pi;

    if (s->ds_view) {
        ds_tick(s);
        return;
    }
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

/*
 * DS1337 register view
 */

static bool ds_stopped(PCF85263State *s)
{
    return s->ds[DS_CONTROL] & DS_CTRL_NEOSC;
}

static int64_t ds_now_ns(PCF85263State *s)
{
    return ds_stopped(s) ? s->ds_frozen_ns : pcf_now_ns(s);
}

static void ds_time_to_regs(PCF85263State *s, int64_t t_ns)
{
    time_t secs = t_ns / NS_PER_SEC;
    uint8_t *r = s->ds;
    struct tm tm;

    if (t_ns % NS_PER_SEC < 0) {
        secs--;
    }
    gmtime_r(&secs, &tm);
    r[DS_SECONDS] = to_bcd(tm.tm_sec);
    r[DS_MINUTES] = to_bcd(tm.tm_min);
    if (r[DS_HOURS] & DS_HOURS_12) {
        int h = tm.tm_hour % 12;

        r[DS_HOURS] = DS_HOURS_12 | (tm.tm_hour >= 12 ? DS_HOURS_PM : 0) |
                      to_bcd(h ? h : 12);
    } else {
        r[DS_HOURS] = to_bcd(tm.tm_hour);
    }
    r[DS_DAY] = (tm.tm_wday + s->ds_day_offset) % 7 + 1;
    r[DS_DATE] = to_bcd(tm.tm_mday);
    r[DS_MONTH] = to_bcd(tm.tm_mon + 1) |
                  (tm.tm_year >= 200 ? DS_MONTH_CENTURY : 0);
    r[DS_YEAR] = to_bcd(tm.tm_year % 100);
}

/* Time written by the host; a write restarts the countdown chain at .0 */
static int64_t ds_regs_to_time(PCF85263State *s)
{
    uint8_t *r = s->ds;
    struct tm tm, check;
    time_t secs;
    int hour;

    if (r[DS_HOURS] & DS_HOURS_12) {
        hour = from_bcd(r[DS_HOURS] & 0x1f) % 12 +
               ((r[DS_HOURS] & DS_HOURS_PM) ? 12 : 0);
    } else {
        hour = from_bcd(r[DS_HOURS] & 0x3f);
    }
    tm = (struct tm) {
        .tm_sec = from_bcd(r[DS_SECONDS] & 0x7f),
        .tm_min = from_bcd(r[DS_MINUTES] & 0x7f),
        .tm_hour = hour,
        .tm_mday = from_bcd(r[DS_DATE] & 0x3f),
        .tm_mon = from_bcd(r[DS_MONTH] & 0x1f) - 1,
        .tm_year = 100 + from_bcd(r[DS_YEAR]) +
                   ((r[DS_MONTH] & DS_MONTH_CENTURY) ? 100 : 0),
    };
    secs = mktimegm(&tm);
    gmtime_r(&secs, &check);
    /* The day register is a free 1..7 counter: keep the written phase */
    s->ds_day_offset = ((r[DS_DAY] & 7) - 1 - check.tm_wday + 14) % 7;
    return (int64_t)secs * NS_PER_SEC;
}

static void ds_update_irq(PCF85263State *s)
{
    uint8_t c = s->ds[DS_CONTROL];
    uint8_t st = s->ds[DS_STATUS];
    bool a1 = (st & DS_STAT_A1F) && (c & DS_CTRL_A1IE);
    bool a2 = (st & DS_STAT_A2F) && (c & DS_CTRL_A2IE);
    bool inta, intb;

    /* INTCN = 1: alarm 1 on INTA, alarm 2 on SQW/INTB; else both on INTA */
    if (c & DS_CTRL_INTCN) {
        inta = a1;
        intb = a2;
    } else {
        inta = a1 || a2;
        intb = false;
    }
    /* Open drain, active low */
    qemu_set_irq(s->inta, !inta);
    qemu_set_irq(s->intb, !intb);
}

static void ds_schedule_tick(PCF85263State *s)
{
    int64_t sub;

    if (ds_stopped(s)) {
        timer_del(s->tick_timer);
        return;
    }
    sub = pcf_now_ns(s) % NS_PER_SEC;
    if (sub < 0) {
        sub += NS_PER_SEC;
    }
    timer_mod(s->tick_timer, qemu_clock_get_ns(rtc_clock) + NS_PER_SEC - sub);
}

static bool ds_alarm_field(uint8_t alarm, uint8_t now)
{
    return (alarm & DS_ALARM_MASK) || (alarm & 0x7f) == (now & 0x7f);
}

static bool ds_alarm_daydate(PCF85263State *s, uint8_t alarm)
{
    if (alarm & DS_ALARM_MASK) {
        return true;
    }
    if (alarm & DS_ALARM_DY) {
        return (alarm & 0x0f) == s->ds[DS_DAY];
    }
    return (alarm & 0x3f) == s->ds[DS_DATE];
}

/* Once per second: alarm comparison, flags are set regardless of A1IE/A2IE */
static void ds_tick(PCF85263State *s)
{
    uint8_t *r = s->ds;

    if (ds_stopped(s)) {
        return;
    }
    if (!s->ds_time_dirty) {
        /* Round to the second that just started */
        ds_time_to_regs(s, pcf_now_ns(s) + NS_PER_SEC / 2);
    }
    if (ds_alarm_field(r[DS_A1_SECONDS], r[DS_SECONDS]) &&
        ds_alarm_field(r[DS_A1_MINUTES], r[DS_MINUTES]) &&
        ds_alarm_field(r[DS_A1_HOURS], r[DS_HOURS]) &&
        ds_alarm_daydate(s, r[DS_A1_DAYDATE])) {
        r[DS_STATUS] |= DS_STAT_A1F;
    }
    /* Alarm 2 has minute resolution */
    if (r[DS_SECONDS] == 0 &&
        ds_alarm_field(r[DS_A2_MINUTES], r[DS_MINUTES]) &&
        ds_alarm_field(r[DS_A2_HOURS], r[DS_HOURS]) &&
        ds_alarm_daydate(s, r[DS_A2_DAYDATE])) {
        r[DS_STATUS] |= DS_STAT_A2F;
    }
    ds_update_irq(s);
    ds_schedule_tick(s);
}

static void ds_write_control(PCF85263State *s, uint8_t val)
{
    bool was = ds_stopped(s);
    bool stop = val & DS_CTRL_NEOSC;

    if (!was && stop) {
        /* Oscillator stopped: the time freezes and OSF is raised */
        s->ds_frozen_ns = pcf_now_ns(s);
        s->ds[DS_STATUS] |= DS_STAT_OSF;
    } else if (was && !stop) {
        s->offset_ns = s->ds_frozen_ns - pcf_ref_ns();
    }
    s->ds[DS_CONTROL] = val & DS_CTRL_RW_MASK;
    if (!(val & DS_CTRL_INTCN)) {
        qemu_log_mask(LOG_UNIMP, "%s: DS1337 square wave output (INTCN = 0) "
                      "not modelled\n", TYPE_PCF85263);
    }
    ds_schedule_tick(s);
    ds_update_irq(s);
}

static void ds_write(PCF85263State *s, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case DS_SECONDS ... DS_YEAR:
        /* Applied as a whole at the end of the transfer */
        s->ds[reg] = val;
        s->ds_time_dirty = true;
        break;
    case DS_A1_SECONDS ... DS_A2_DAYDATE:
        s->ds[reg] = val;
        break;
    case DS_CONTROL:
        ds_write_control(s, val);
        break;
    case DS_STATUS:
        /* OSF, A2F and A1F can only be cleared */
        s->ds[reg] &= val | ~DS_STAT_FLAGS;
        ds_update_irq(s);
        break;
    default:
        g_assert_not_reached();
    }
}

static void ds_commit_time(PCF85263State *s)
{
    int64_t t;

    if (!s->ds_time_dirty) {
        return;
    }
    s->ds_time_dirty = false;
    t = ds_regs_to_time(s);
    if (ds_stopped(s)) {
        s->ds_frozen_ns = t;
    } else {
        s->offset_ns = t - pcf_ref_ns();
    }
    ds_schedule_tick(s);
}

static int ds_event(PCF85263State *s, enum i2c_event event)
{
    switch (event) {
    case I2C_START_SEND:
        s->addr_received = false;
        /* fall through */
    case I2C_START_RECV:
        /* Time registers are latched for the duration of the transfer */
        if (!s->ds_time_dirty) {
            ds_time_to_regs(s, ds_now_ns(s));
        }
        break;
    case I2C_FINISH:
        ds_commit_time(s);
        break;
    default:
        break;
    }
    return 0;
}

static int ds_send(PCF85263State *s, uint8_t data)
{
    if (!s->addr_received) {
        s->ptr = data & (DS_NUM_REGS - 1);
        s->addr_received = true;
        if (data >= DS_NUM_REGS) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: DS1337 register 0x%02x out "
                          "of range\n", TYPE_PCF85263, data);
        }
        return 0;
    }
    ds_write(s, s->ptr, data);
    s->ptr = (s->ptr + 1) % DS_NUM_REGS;
    return 0;
}

static uint8_t ds_recv(PCF85263State *s)
{
    uint8_t val = s->ds[s->ptr];

    s->ptr = (s->ptr + 1) % DS_NUM_REGS;
    return val;
}

static void ds_reset(PCF85263State *s)
{
    uint8_t hours12 = s->ds[DS_HOURS] & DS_HOURS_12;

    memset(&s->ds[DS_A1_SECONDS], 0, DS_A2_DAYDATE - DS_A1_SECONDS + 1);
    s->ds[DS_CONTROL] = DS_CTRL_RESET;
    /* Battery backed: OSF stays clear and the clock keeps running */
    s->ds[DS_STATUS] = 0;
    s->ds[DS_HOURS] = hours12;
    s->ds_time_dirty = false;
    s->ptr = 0;
    s->addr_received = false;
    ds_time_to_regs(s, pcf_now_ns(s));
    ds_schedule_tick(s);
    ds_update_irq(s);
}

static int pcf_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF85263State *s = PCF85263(i2c);

    if (s->ds_view) {
        return ds_event(s, event);
    }
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

    if (s->ds_view) {
        return ds_send(s, data);
    }
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
    uint8_t val;

    if (s->ds_view) {
        return ds_recv(s);
    }
    val = s->ptr < NUM_REGS ? s->regs[s->ptr] : 0;
    s->ptr = (s->ptr + 1) % NUM_REGS;
    return val;
}

static void pcf_reset(DeviceState *dev)
{
    PCF85263State *s = PCF85263(dev);

    if (s->ds_view) {
        ds_reset(s);
        return;
    }
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

    if (!s->register_map || !strcmp(s->register_map, "ds1337")) {
        s->ds_view = true;
    } else if (!strcmp(s->register_map, "pcf85263")) {
        s->ds_view = false;
    } else {
        error_setg(errp, "%s: register-map must be 'ds1337' or 'pcf85263'",
                   TYPE_PCF85263);
        return;
    }
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
    PCF85263State *s = opaque;

    if (s->ds_view) {
        ds_schedule_tick(s);
    } else {
        pcf_schedule_tick(s);
    }
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
        VMSTATE_UINT8_ARRAY(ds, PCF85263State, DS_NUM_REGS),
        VMSTATE_BOOL(ds_time_dirty, PCF85263State),
        VMSTATE_INT8(ds_day_offset, PCF85263State),
        VMSTATE_INT64(ds_frozen_ns, PCF85263State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pcf_properties[] = {
    /*
     * "ds1337" (default when unset): DS1337 view used by the KICK kernel
     * (rtc-ds1307); "pcf85263": native register map of the board's part.
     */
    DEFINE_PROP_STRING("register-map", PCF85263State, register_map),
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
