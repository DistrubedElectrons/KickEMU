/*
 * Front panel light show, spoken to the control-surface MCU in the same
 * packets the KICK application sends (Kick/modules/contracts/dcl):
 *
 *   handshake   SYSTEM/COMM_INIT [CONNECTED], sent on start and whenever
 *               the MCU reports CHECK_COMM_STATUS [1]; keepalive
 *               CHECK_COMM_STATUS [2] is echoed back
 *   pads        PAD/SET_ALL_MULTI_COLOUR [state, colour x16]: a rainbow
 *               wave over the 4x4 grid; PAD/SET_SINGLE_PAD [pad, state,
 *               colour] flashes a pad the user hits
 *   buttons     LED/SET_SINGLE_LED [id, state, colour]: a chase over the
 *               key LEDs
 *   jog ring    LED ids 56..61 (emulator ids for the ring, clockwise from
 *               the top, see docs/ARCHITECTURE.md), one lit at a time
 *   LINK LEDs   LED ids 62..65, a four-step beat counter
 *   slider      SLIDER/LEVEL_LED [percent], a triangle sweep
 *
 * Pad presses come back as PAD/PAD_STATE [pad, state, velocity, after].
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gfx.h"
#include "imx8mm.h"
#include "panel.h"
#include "pktmgr.h"
#include "uart.h"

/* PadColourIds in hue order, for the rainbow wave */
static const uint8_t rainbow[] = { 12, 13, 14, 4, 3, 5, 6, 7, 8, 1, 10, 9 };
/* Colour burst for a pad hit: white, pink, magenta, violet, purple */
static const uint8_t burst[] = { 16, 16, 11, 11, 9, 10, 1, 1 };

/*
 * Key LEDs (BtnLedSequence ids) in panel order, left to right, top to
 * bottom, as laid out on the KICK top panel.
 */
static const uint8_t chase[] = {
    8, 10, 12, 14, 16, 18, 24, 22, 31, 32, 34, 36, 38, 40, 48, 50, 52,
    20, 21, 27, 29, 47, 42, 43, 44, 45, 46, 0, 1, 2, 3,
};

#define LED_JOG_RING_FIRST  56
#define LED_JOG_RING_COUNT  6
#define LED_LINK_FIRST      62
#define LED_LINK_COUNT      4

/* Animation steps per second (from the 100 Hz GPT tick) */
#define ANIM_DIV            10
#define BURST_STEPS         ((int)ARRAY_SIZE(burst))

/* The pad palette the host shows (Emulator/config pad_palette) */
static const uint32_t palette[18] = {
    0x94172e, 0x7c00f8, 0xab7b00, 0x1df800, 0xe4f800, 0x22ab00,
    0x0df0ff, 0x3daeff, 0x0d34ff, 0xff00e9, 0xcc00ff, 0xff89fa,
    0xff250d, 0xff730d, 0xffb608, 0x000000, 0xffffff, 0xc2c2c2,
};

static panel_state st;
static int burst_left[PANEL_PADS];
static int chase_pos = -1;
static int ring_pos = -1;
static int link_pos = -1;

static void mcu_tx(const uint8_t *buf, int len)
{
    uart_write(UART1_BASE, buf, len);
    st.frames_sent++;
}

static void led(int id, int on, int colour)
{
    uint8_t d[3] = { id, on ? STATE_ON : STATE_OFF, colour };

    pkt_send(mcu_tx, PKT_LED, LED_SET_SINGLE_LED, d, 3);
}

static void comm_init(void)
{
    uint8_t d = SYS_COMM_CONNECTED;

    pkt_send(mcu_tx, PKT_SYSTEM, SYS_COMM_INIT, &d, 1);
}

static void on_packet(const pkt *p)
{
    st.frames_received++;
    if (p->type == PKT_SYSTEM && p->cmd == SYS_CHECK_COMM_STATUS &&
        p->len >= 1) {
        if (p->data[0] == 1) {
            comm_init();            /* MCU waits for the application */
        } else {
            /* keepalive: echo as the application does */
            pkt_send(mcu_tx, PKT_SYSTEM, SYS_CHECK_COMM_STATUS, p->data, 1);
        }
        st.linked = 1;
    } else if (p->type == PKT_PAD && p->cmd == PAD_STATE && p->len >= 3) {
        int pad = p->data[0];

        st.linked = 1;
        if (pad < PANEL_PADS && p->data[1]) {
            uint8_t d[3] = { pad, STATE_ON, burst[0] };

            burst_left[pad] = BURST_STEPS;
            st.last_pad = pad + 1;
            st.last_velocity = p->data[2];
            st.pad_presses++;
            st.pad_colour[pad] = burst[0];
            /* immediate feedback, the wave picks it up from here */
            pkt_send(mcu_tx, PKT_PAD, PAD_SET_SINGLE_PAD, d, 3);
        }
    } else if (p->type == PKT_BUTTON && p->cmd == BTN_STATUS && p->len >= 2) {
        st.linked = 1;
        if (p->data[1]) {
            st.last_button = p->data[0];
        }
    }
}

void panel_init(void)
{
    st.last_button = -1;
    pkt_rx_init(on_packet);
    comm_init();
}

void panel_rx(uint8_t byte)
{
    pkt_rx_byte(byte);
}

const panel_state *panel_get(void)
{
    return &st;
}

uint32_t panel_colour_rgb(int id)
{
    return id >= 0 && id < (int)ARRAY_SIZE(palette) ? palette[id] : 0;
}

static void animate_pads(int step)
{
    uint8_t d[1 + PANEL_PADS];

    d[0] = STATE_ON;
    for (int pad = 0; pad < PANEL_PADS; pad++) {
        int x = pad % 4, y = pad / 4;
        uint8_t c = rainbow[(x + y + step) % ARRAY_SIZE(rainbow)];

        if (burst_left[pad]) {
            c = burst[BURST_STEPS - burst_left[pad]];
            burst_left[pad]--;
        }
        st.pad_colour[pad] = c;
        d[1 + pad] = c;
    }
    pkt_send(mcu_tx, PKT_PAD, PAD_SET_ALL_MULTI_COLOUR, d, sizeof(d));
}

static void animate_leds(int step)
{
    uint8_t pct;
    int tri;

    /* key LED chase: one key at a time */
    if (chase_pos >= 0) {
        led(chase[chase_pos], 0, 0);
    }
    chase_pos = (chase_pos + 1) % (int)ARRAY_SIZE(chase);
    led(chase[chase_pos], 1, 1);

    /* jog ring spins clockwise */
    if (ring_pos >= 0) {
        led(LED_JOG_RING_FIRST + ring_pos, 0, 0);
    }
    ring_pos = (ring_pos + 1) % LED_JOG_RING_COUNT;
    led(LED_JOG_RING_FIRST + ring_pos, 1, 0);

    /* LINK LEDs count beats, two animation steps per beat */
    if (step % 2 == 0) {
        if (link_pos >= 0) {
            led(LED_LINK_FIRST + link_pos, 0, 0);
        }
        link_pos = (link_pos + 1) % LED_LINK_COUNT;
        led(LED_LINK_FIRST + link_pos, 1, 0);
    }

    /* touch slider level: triangle 0..100 % over 20 steps */
    tri = step % 20;
    pct = (uint8_t)((tri < 10 ? tri : 20 - tri) * 10);
    pkt_send(mcu_tx, PKT_SLIDER, SLIDER_LEVEL_LED, &pct, 1);
}

void panel_animate(uint64_t tick)
{
    static uint64_t last = (uint64_t)-1;
    int step;

    if (tick / ANIM_DIV == last) {
        return;
    }
    last = tick / ANIM_DIV;
    step = (int)last;
    animate_pads(step);
    animate_leds(step);
}
