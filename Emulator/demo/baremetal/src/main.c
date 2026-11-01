/*
 * KICK bare-metal demo: brings up the i.MX 8M Mini peripherals of the KICK
 * from scratch, prints a report on the debug console (UART4), runs a light
 * show on the front panel through the control-surface MCU (UART1) and draws
 * an intro and live status on the display (LCDIF).
 *
 * It only touches SoC registers, exactly as on the hardware; nothing in it
 * is specific to the emulator.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "clock.h"
#include "console.h"
#include "fmt.h"
#include "gfx.h"
#include "gic.h"
#include "gpio.h"
#include "gpt.h"
#include "i2c.h"
#include "imx8mm.h"
#include "lcdif.h"
#include "mmu.h"
#include "panel.h"
#include "psci.h"
#include "screen.h"
#include "soc_info.h"
#include "uart.h"

/* KICK: GPIO4_IO25 is the V4220M codec mute line ("codecmute" LED) */
#define DEMO_GPIO_BANK      4
#define DEMO_GPIO_PIN       25
/* SN65DSI84 DSI-to-LVDS bridge enable (imx8mm-var-som.dtsi enable-gpios) */
#define BRIDGE_EN_BANK      2
#define BRIDGE_EN_PIN       11

/* Panel timing: 800x480, porches as in the VAR-SOM LVDS display timings */
static const lcdif_mode panel_mode = {
    .hactive = KICK_FB_WIDTH, .hfp = 40, .hsync = 48, .hbp = 40,
    .vactive = KICK_FB_HEIGHT, .vfp = 13, .vsync = 3, .vbp = 29,
};

/* UART1 receive ring: filled by the IRQ, drained by the main loop */
#define RX_RING 256
static volatile uint8_t rx_ring[RX_RING];
static volatile unsigned rx_head, rx_tail;

static void mcu_rx(uint8_t b)
{
    rx_ring[rx_head % RX_RING] = b;
    rx_head++;
}

static void uart1_irq(void)
{
    uart_rx_irq(UART1_BASE);
}

void fatal_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
    kprintf(C_RED "\nexception: ESR %016llx ELR %016llx FAR %016llx\n"
            C_RESET, (unsigned long long)esr, (unsigned long long)elr,
            (unsigned long long)far);
}

static unsigned current_el(void)
{
    uint64_t el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (unsigned)(el >> 2) & 3;
}

static uint32_t midr(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    return (uint32_t)v;
}

static void report_i2c(char *summary, int len)
{
    int pos = 0;

    summary[0] = 0;
    for (int bus = 1; bus <= 4; bus++) {
        kprintf("  I2C%d:", bus);
        i2c_init(bus);
        pos += fmt_snprintf(summary + pos, len - pos, "%s%d:", bus > 1 ?
                            " " : "", bus);
        for (int addr = 0x08; addr < 0x78; addr++) {
            if (i2c_probe(bus, addr)) {
                kprintf(" 0x%02x", addr);
                pos += fmt_snprintf(summary + pos, len - pos, "%02x,", addr);
            }
        }
        if (summary[pos - 1] == ',') {
            summary[--pos] = 0;
        }
        kprintf("\n");
    }
}

int main(void)
{
    static screen_info info;
    static gfx fb;
    soc_info soc;
    uint64_t arm_hz;
    int cpus;
    uint64_t last_tick = 0, last_second = 0;

    mmu_enable();

    /* Console: UART4 clock root from the 24 MHz oscillator */
    clock_root(ROOT_UART4, 0, 1, 1);
    clock_gate(CCGR_UART4, 1);
    console_init(OSC_24M_HZ);

    soc_info_read(&soc);
    arm_hz = clock_pll_hz(ANA_ARM_PLL, 0);
    cpus = psci_count_cpus(8);

    kprintf("\n" C_BOLD C_YELLOW
            "  _  _____ ___ _  __\n"
            " | |/ /_ _/ __| |/ /   KICK bare-metal demo %s\n"
            " | ' < | | (__| ' <    %s @ %s\n"
            " |_|\\_\\___\\___|_|\\_\\   Behringer KICK (BE 0700-AAM)\n"
            C_RESET "\n", KICK_DEMO_VERSION, KICK_GIT_BRANCH,
            KICK_GIT_COMMIT);

    kprintf(C_CYAN "SoC" C_RESET "      %s%s rev %u.%u (DIGPROG 0x%06x)\n",
            soc.name, soc.variant[0] ? "" : "", soc.rev_major,
            soc.rev_minor, soc.digprog);
    kprintf(C_CYAN "Variant" C_RESET "  %s, speed grade fuse 0x%08x\n",
            soc.variant, soc.speed_grade);
    kprintf(C_CYAN "UID" C_RESET "      %016llx\n",
            (unsigned long long)soc.uid);
    kprintf(C_CYAN "MAC" C_RESET "      %02x:%02x:%02x:%02x:%02x:%02x"
            " (OCOTP)\n", soc.mac[0], soc.mac[1], soc.mac[2], soc.mac[3],
            soc.mac[4], soc.mac[5]);
    kprintf(C_CYAN "CPU" C_RESET "      MIDR 0x%08x, running at EL%u, "
            "%d CPUs (PSCI %d.%d)\n", midr(), current_el(), cpus,
            psci_version() >> 16, psci_version() & 0xffff);
    kprintf(C_CYAN "Clocks" C_RESET "   ARM PLL %llu MHz, SYS PLL1 %llu MHz, "
            "VIDEO PLL1 %llu MHz\n", (unsigned long long)(arm_hz / 1000000),
            (unsigned long long)(clock_pll_hz(ANA_SYS_PLL1, 0) / 1000000),
            (unsigned long long)(clock_pll_hz(ANA_VIDEO_PLL1, 1) / 1000000));
    kprintf(C_CYAN "Temp" C_RESET "     %d C (TMU)\n", soc_temperature());

    /* GIC, then the GPT1 tick */
    gic_init();
    clock_root(ROOT_GPT1, 0, 1, 1);
    clock_gate(CCGR_GPT1, 1);
    gpt_start(OSC_24M_HZ, KICK_TICK_HZ);
    irq_enable();
    kprintf(C_CYAN "Timer" C_RESET "    GPT1 at %u Hz on GIC SPI %d\n",
            KICK_TICK_HZ, IRQ_GPT1 - 32);

    /* I2C bus scan */
    for (int bus = 0; bus < 4; bus++) {
        clock_gate(CCGR_I2C1 + bus, 1);
    }
    /* The SN65DSI84 bridge (I2C1 0x2c) only answers while EN is high */
    gpio_set_output(BRIDGE_EN_BANK, BRIDGE_EN_PIN, 1);
    kprintf(C_CYAN "I2C scan" C_RESET "\n");
    report_i2c(info.i2c, sizeof(info.i2c));

    /* GPIO */
    gpio_set_output(DEMO_GPIO_BANK, DEMO_GPIO_PIN, 1);
    kprintf(C_CYAN "GPIO" C_RESET "     GPIO%d_IO%02d toggles every second "
            "(codec mute), reads back %d\n", DEMO_GPIO_BANK, DEMO_GPIO_PIN,
            gpio_get(DEMO_GPIO_BANK, DEMO_GPIO_PIN));

    /* MCU link on UART1 at 230400 8N1 */
    clock_root(ROOT_UART1, 0, 1, 1);
    clock_gate(CCGR_UART1, 1);
    uart_init(UART1_BASE, OSC_24M_HZ, KICK_MCU_BAUD);
    uart_enable_rx_irq(UART1_BASE, mcu_rx);
    gic_enable_spi(IRQ_UART1, 0x90, uart1_irq);
    panel_init();
    kprintf(C_CYAN "MCU" C_RESET "      UART1 %u baud, PktMgr COMM_INIT sent\n",
            KICK_MCU_BAUD);

    /* Display */
    fmt_snprintf(info.soc, sizeof(info.soc), "%s %s rev %u.%u", soc.name,
                 soc.variant, soc.rev_major, soc.rev_minor);
    fmt_snprintf(info.uid, sizeof(info.uid), "%016llx",
                 (unsigned long long)soc.uid);
    fmt_snprintf(info.cpus, sizeof(info.cpus), "%dx Cortex-A53, EL%u", cpus,
                 current_el());
    fmt_snprintf(info.clocks, sizeof(info.clocks), "ARM PLL %llu MHz",
                 (unsigned long long)(arm_hz / 1000000));
    gfx_init(&fb, KICK_FB_ADDR, KICK_FB_WIDTH, KICK_FB_HEIGHT);
    screen_init(&fb, &info);
    lcdif_start(&panel_mode, KICK_FB_ADDR);
    kprintf(C_CYAN "Display" C_RESET "  LCDIF %ux%u XRGB8888 at 0x%08x\n\n",
            KICK_FB_WIDTH, KICK_FB_HEIGHT, KICK_FB_ADDR);

    for (;;) {
        uint64_t tick = gpt_ticks();

        while (rx_tail != rx_head) {
            panel_rx(rx_ring[rx_tail % RX_RING]);
            rx_tail++;
        }
        if (tick != last_tick) {
            last_tick = tick;
            panel_animate(tick);
            screen_update(tick);
        }
        if (tick / KICK_TICK_HZ != last_second) {
            const panel_state *p = panel_get();

            last_second = tick / KICK_TICK_HZ;
            gpio_set(DEMO_GPIO_BANK, DEMO_GPIO_PIN, (int)(last_second & 1));
            kprintf("\r" C_DIM "[%5llus]" C_RESET " ticks %llu  irqs %lu  "
                    "frames %u  mcu %s tx %u rx %u  gpio %d  last pad %d   ",
                    (unsigned long long)last_second,
                    (unsigned long long)tick, gic_irq_count(), lcdif_frames(),
                    p->linked ? "linked" : "waiting", p->frames_sent,
                    p->frames_received,
                    gpio_get(DEMO_GPIO_BANK, DEMO_GPIO_PIN), p->last_pad);
        }
        __asm__ volatile("wfi");
    }
    return 0;
}
