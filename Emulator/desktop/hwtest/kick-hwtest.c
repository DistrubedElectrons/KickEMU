/*
 * kick-hwtest: hardware check and front panel demo for Linux on the KICK
 *
 * Runs on any Linux root file system on the KICK (or KickEmu): first a
 * report of what the kernel sees (SoC, memory, storage, SOM EEPROM, RTC,
 * temperature, backlight, touch, network, audio, USB), then a light test
 * over the control-surface MCU on UART1 (/dev/ttymxc0, 230400 8N1) in the
 * packets the KICK application uses (Kick/modules/contracts/dcl), then an
 * interactive mode: pads flash when hit, keys light while held, knobs and
 * the slider are reported, the pads keep a slow rainbow going.
 *
 * Nothing here produces sound: audio devices are listed, never opened.
 *
 * Usage: kick-hwtest [--once] [--no-backlight] [--mcu /dev/ttyX]
 *   --once          report and light test, then exit (exit code = failures)
 *   --no-backlight  do not dim the panel during the backlight check
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "pktmgr.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* KnobCmd, SliderCmd (dcl_contracts.h) */
#define KNOB_ROTARY         0
#define SLIDER_TOUCH_INPUT  0
/* BtnLedSequence ids of the jog ring and LINK LEDs in the emulator (see
 * Emulator/docs/ARCHITECTURE.md); the highest key LED id is EN_ERASE = 52 */
#define LED_KEY_LAST        52
#define LED_JOG_RING_FIRST  56
#define LED_JOG_RING_COUNT  6
#define LED_LINK_FIRST      62
#define LED_LINK_COUNT      4
#define PADS                16

/* PadColourIds in hue order, plus white */
static const uint8_t rainbow[] = { 12, 13, 14, 4, 3, 5, 6, 7, 8, 1, 10, 9 };
#define COLOUR_WHITE        16

static const char *const key_names[] = {
    "BANK A", "BANK B", "BANK C", "BANK D", "E", "F", "G", "H",
    "MAIN", "GRID", "BROWSE", "SAVE", "TRACK MIX", "PAD MIX", "TRACK MUTE",
    "PAD MUTE", "NEXT SEQ", "FX", "TAP TEMPO", "MASTER", "COPY", "PLUS",
    "MENU", "PREFS", "NOTE REPEAT", "LATCH", "JOG ENTER", "MINUS", "JOG CCW",
    "BACK", "JOG CW", "SHIFT", "STEP SEQ", "AUTO", "TC", "ON/OFF", "SAMPLER",
    "LOOP", "SAMPLE EDIT", "LINK EDIT", "PROG EDIT", "MIDI CTRL", "RECORD",
    "OVERDUB", "STOP", "PLAY", "PLAY START", "LINK", "FULL LEVEL",
    "HALF LEVEL", "16 LEVELS", "NOTES", "ERASE",
};

static int mcu_fd = -1;
static volatile sig_atomic_t quit;
static unsigned failures;
static int linked;
static unsigned frames_rx;
static int mcu_version = -1;     /* major << 8 | minor, -1 = not heard */

/* ---- output -------------------------------------------------------------- */

static int tty;

static void result(const char *status, const char *what, const char *fmt, ...)
{
    const char *colour = "";
    va_list ap;

    if (tty) {
        colour = !strcmp(status, "PASS") ? "\033[32m" :
                 !strcmp(status, "FAIL") ? "\033[31m" : "\033[33m";
    }
    if (!strcmp(status, "FAIL")) {
        failures++;
    }
    printf("  %s%-4s%s  %-11s ", colour, status, tty ? "\033[0m" : "", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void heading(const char *title)
{
    printf(tty ? "\n\033[1m%s\033[0m\n" : "\n%s\n", title);
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- sysfs helpers ------------------------------------------------------- */

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
        n--;
    }
    buf[n] = 0;
    return (int)n;
}

static long read_long(const char *path, long fallback)
{
    char buf[64];

    return read_text(path, buf, sizeof(buf)) > 0 ? strtol(buf, NULL, 10)
                                                  : fallback;
}

static int write_long(const char *path, long value)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (!f) {
        return -1;
    }
    ok = fprintf(f, "%ld\n", value) > 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* First match of a glob pattern, or NULL */
static char *first_match(const char *pattern)
{
    static char path[512];
    glob_t g;
    char *r = NULL;

    if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc) {
        snprintf(path, sizeof(path), "%s", g.gl_pathv[0]);
        r = path;
    }
    globfree(&g);
    return r;
}

static size_t count_matches(const char *pattern)
{
    glob_t g;
    size_t n = 0;

    if (glob(pattern, 0, NULL, &g) == 0) {
        n = g.gl_pathc;
    }
    globfree(&g);
    return n;
}

/* ---- system checks ------------------------------------------------------- */

static void check_soc(void)
{
    char id[64], rev[32];
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (read_text("/sys/devices/soc0/soc_id", id, sizeof(id)) > 0) {
        if (read_text("/sys/devices/soc0/revision", rev, sizeof(rev)) <= 0) {
            strcpy(rev, "?");
        }
        result(strstr(id, "i.MX8MM") ? "PASS" : "WARN", "SoC", "%s rev %s",
               id, rev);
    } else {
        result("FAIL", "SoC", "no /sys/devices/soc0");
    }
    result(cpus == 4 ? "PASS" : "WARN", "CPUs", "%ld online (KICK: 4x A53)",
           cpus);
}

static void check_memory(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    long kb = 0;
    char line[128];

    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) {
            break;
        }
    }
    if (f) {
        fclose(f);
    }
    result(kb > 1000000 ? "PASS" : "FAIL", "Memory", "%ld MiB", kb / 1024);
}

static void check_block(const char *what, const char *dev, int required)
{
    char path[128];
    long sectors;

    snprintf(path, sizeof(path), "/sys/block/%s/size", dev);
    sectors = read_long(path, -1);
    if (sectors > 0) {
        double mib = sectors / 2048.0;

        result("PASS", what, mib >= 1024 ? "/dev/%s, %.1f GiB"
                                         : "/dev/%s, %.0f MiB", dev,
               mib >= 1024 ? mib / 1024 : mib);
    } else {
        result(required ? "FAIL" : "INFO", what,
               required ? "/dev/%s missing" : "no card in the slot", dev);
    }
}

/*
 * The Variscite SOM EEPROM (AT24, I2C1 = i2c-0, 0x52). The vendor kernel has
 * no at24 driver, so read it through i2c-dev: write offset 0, read the header.
 */
static int eeprom_read(unsigned char *buf, size_t len, const char **how)
{
    char *path = first_match("/sys/bus/i2c/devices/0-0052/eeprom");
    unsigned char offset = 0;
    struct i2c_msg msgs[2] = {
        { .addr = 0x52, .flags = 0, .len = 1, .buf = &offset },
        { .addr = 0x52, .flags = I2C_M_RD, .len = (unsigned short)len,
          .buf = buf },
    };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };
    int fd, ok;

    *how = path ? "at24" : "i2c-0";
    fd = open(path ? path : "/dev/i2c-0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ok = path ? read(fd, buf, len) == (ssize_t)len
              : ioctl(fd, I2C_RDWR, &xfer) == 2;
    close(fd);
    return ok ? 0 : -1;
}

static void check_eeprom(void)
{
    unsigned char rom[0x22];
    const char *how;

    if (eeprom_read(rom, sizeof(rom), &how) < 0) {
        result(errno == EACCES ? "SKIP" : "FAIL", "SOM EEPROM",
               "I2C1 0x52 via %s: %s", how, strerror(errno));
        return;
    }
    /* Variscite DART-MX8M-MINI header: magic "8M", MAC at 0x18 */
    if (rom[0] != '8' || rom[1] != 'M') {
        result("FAIL", "SOM EEPROM", "bad magic %02x %02x", rom[0], rom[1]);
        return;
    }
    result("PASS", "SOM EEPROM",
           "%.3s %.10s, MAC %02x:%02x:%02x:%02x:%02x:%02x, %u GiB DRAM",
           &rom[0x02], &rom[0x05], rom[0x18], rom[0x19], rom[0x1a], rom[0x1b],
           rom[0x1c], rom[0x1d], rom[0x21]);
}

static void check_rtc(void)
{
    char date[32], tm[32], name[64];

    if (read_text("/sys/class/rtc/rtc0/date", date, sizeof(date)) > 0 &&
        read_text("/sys/class/rtc/rtc0/time", tm, sizeof(tm)) > 0) {
        if (read_text("/sys/class/rtc/rtc0/name", name, sizeof(name)) <= 0) {
            strcpy(name, "rtc0");
        }
        result("PASS", "RTC", "%s %s UTC (%s)", date, tm, name);
    } else {
        result("FAIL", "RTC", "no /sys/class/rtc/rtc0");
    }
}

static void check_thermal(void)
{
    long mc = read_long("/sys/class/thermal/thermal_zone0/temp", -1000000);

    if (mc > -1000000) {
        result(mc > 0 && mc < 95000 ? "PASS" : "WARN", "Temperature",
               "%.1f C (TMU)", mc / 1000.0);
    } else {
        result("FAIL", "Temperature", "no thermal zone");
    }
}

static void check_backlight(int dim)
{
    char *dir = first_match("/sys/class/backlight/*");
    char path[600], actual[600];
    long max, cur;
    int ok = 1;

    if (!dir) {
        result("FAIL", "Backlight", "no backlight device");
        return;
    }
    snprintf(path, sizeof(path), "%s/max_brightness", dir);
    max = read_long(path, -1);
    snprintf(path, sizeof(path), "%s/brightness", dir);
    snprintf(actual, sizeof(actual), "%s/actual_brightness", dir);
    cur = read_long(path, -1);
    if (!dim) {
        result("PASS", "Backlight", "%s at %ld of %ld", strrchr(dir, '/') + 1,
               cur, max);
        return;
    }
    /* Dim down and back up; the PWM duty follows (visible in KickEmu) */
    for (long level = max; level >= 0 && ok; level -= (max + 3) / 4) {
        ok = write_long(path, level) == 0 &&
             read_long(actual, -1) == level;
        usleep(250000);
    }
    for (long level = 0; level <= cur && ok; level += (max + 3) / 4) {
        ok = write_long(path, level) == 0;
        usleep(150000);
    }
    write_long(path, cur);
    if (ok) {
        result("PASS", "Backlight", "%s swept 0..%ld, back at %ld",
               strrchr(dir, '/') + 1, max, cur);
    } else {
        result(errno == EACCES ? "SKIP" : "FAIL", "Backlight", "%s: %s",
               path, errno == EACCES ? "no write access (run as root)"
                                     : "level did not stick");
    }
}

static void check_input(void)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    char line[256];
    int touch = 0, pwrkey = 0;

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "N: Name=", 8)) {
            continue;
        }
        touch |= strstr(line, "Goodix") != NULL;
        pwrkey |= strstr(line, "pwrkey") != NULL;
    }
    if (f) {
        fclose(f);
    }
    result(touch ? "PASS" : "FAIL", "Touch", touch ? "Goodix GT911"
                                                    : "no Goodix input device");
    result(pwrkey ? "PASS" : "WARN", "Power key",
           pwrkey ? "SNVS on/off key" : "no snvs-pwrkey input device");
}

static void check_network(void)
{
    char state[32], mac[32];

    if (read_text("/sys/class/net/eth0/operstate", state, sizeof(state)) <= 0) {
        result("FAIL", "Ethernet", "no eth0");
        return;
    }
    read_text("/sys/class/net/eth0/address", mac, sizeof(mac));
    result(!strcmp(state, "up") ? "PASS" : "WARN", "Ethernet", "eth0 %s, %s",
           state, mac);
}

static void check_audio(void)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256];
    int cards = 0;

    while (f && fgets(line, sizeof(line), f)) {
        char *name = strstr(line, " - ");

        if (line[0] == ' ' && line[1] >= '0' && line[1] <= '9' && name) {
            name[strcspn(name, "\n")] = 0;
            result("PASS", cards ? "" : "Audio", "card %c:%s", line[1],
                   name + 2);
            cards++;
        }
    }
    if (f) {
        fclose(f);
    }
    if (!cards) {
        result("FAIL", "Audio", "no ALSA card");
    }
}

static void check_usb(void)
{
    size_t hubs = count_matches("/sys/bus/usb/devices/usb*");
    size_t all = count_matches("/sys/bus/usb/devices/[0-9]*-[0-9]*");

    result(hubs ? "PASS" : "FAIL", "USB", "%zu host controller(s), %zu "
           "device(s) attached", hubs, all - count_matches(
               "/sys/bus/usb/devices/[0-9]*-[0-9]*:*"));
}

static void check_serial(void)
{
    size_t n = count_matches("/sys/class/tty/ttymxc*");

    result(n >= 3 ? "PASS" : "FAIL", "UARTs", "%zu ttymxc ports (MCU, MIDI, "
           "console)", n);
}

/* ---- MCU link ------------------------------------------------------------ */

static void mcu_tx(const uint8_t *buf, int len)
{
    while (len > 0) {
        ssize_t n = write(mcu_fd, buf, len);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;
        }
        buf += n;
        len -= n;
    }
}

static void send(uint8_t type, uint8_t cmd, const uint8_t *data, int len)
{
    pkt_send(mcu_tx, type, cmd, data, len);
}

static void led(int id, int on, int colour)
{
    uint8_t d[3] = { id, on ? STATE_ON : STATE_OFF, colour };

    send(PKT_LED, LED_SET_SINGLE_LED, d, 3);
}

static void pads_all(const uint8_t colour[PADS], int on)
{
    uint8_t d[1 + PADS];

    d[0] = on ? STATE_ON : STATE_OFF;
    memcpy(&d[1], colour, PADS);
    send(PKT_PAD, PAD_SET_ALL_MULTI_COLOUR, d, sizeof(d));
}

static void pad(int index, int on, int colour)
{
    uint8_t d[3] = { index, on ? STATE_ON : STATE_OFF, colour };

    send(PKT_PAD, PAD_SET_SINGLE_PAD, d, 3);
}

static void slider_level(int percent)
{
    uint8_t d = percent;

    send(PKT_SLIDER, SLIDER_LEVEL_LED, &d, 1);
}

static void comm_init(void)
{
    uint8_t d = SYS_COMM_CONNECTED;

    send(PKT_SYSTEM, SYS_COMM_INIT, &d, 1);
}

static void all_off(void)
{
    uint8_t black[PADS] = { 0 };

    pads_all(black, 0);
    for (int id = 0; id <= LED_LINK_FIRST + LED_LINK_COUNT - 1; id++) {
        led(id, 0, 0);
    }
    slider_level(0);
}

/* Interactive state */
static int burst[PADS];
static int interactive;

static void on_packet(const pkt *p)
{
    frames_rx++;
    if (p->type == PKT_SYSTEM && p->cmd == SYS_CHECK_COMM_STATUS && p->len) {
        if (p->data[0] == 1) {
            comm_init();
        } else {
            send(PKT_SYSTEM, SYS_CHECK_COMM_STATUS, p->data, 1);
        }
        linked = 1;
        return;
    }
    linked = 1;
    if (p->type == PKT_SYSTEM && p->cmd == SYS_GET_SW_VER && p->len >= 2) {
        mcu_version = p->data[0] << 8 | p->data[1];
        return;
    }
    if (!interactive) {
        return;
    }
    if (p->type == PKT_PAD && p->cmd == PAD_STATE && p->len >= 3 &&
        p->data[0] < PADS) {
        if (p->data[1]) {
            burst[p->data[0]] = 6;
            pad(p->data[0], 1, COLOUR_WHITE);
            printf("  pad %2d     hit, velocity %3d\n", p->data[0] + 1,
                   p->data[2]);
        }
    } else if (p->type == PKT_BUTTON && p->cmd == BTN_STATUS && p->len >= 2) {
        int id = p->data[0];

        led(id, p->data[1] != 0, 1);
        printf("  key        %-12s %s\n",
               id < (int)ARRAY_SIZE(key_names) ? key_names[id] : "?",
               p->data[1] ? "down" : "up");
    } else if (p->type == PKT_KNOBS && p->cmd == KNOB_ROTARY && p->len >= 3) {
        int step = p->data[1] ? p->data[2] : -p->data[2];

        if (p->data[0] == 0) {
            printf("  jog wheel  %+d\n", step);
        } else {
            printf("  knob P%d    %+d\n", p->data[0], step);
        }
    } else if (p->type == PKT_SLIDER && p->cmd == SLIDER_TOUCH_INPUT &&
               p->len >= 1) {
        slider_level(p->data[0]);
        printf("  slider     %3d %%\n", p->data[0]);
    } else {
        printf("  mcu        type %d cmd %d, %d byte(s)\n", p->type, p->cmd,
               p->len);
    }
    fflush(stdout);
}

static void pump(double seconds)
{
    double end = now() + seconds;
    struct pollfd pfd = { .fd = mcu_fd, .events = POLLIN };

    for (;;) {
        double left = end - now();
        uint8_t buf[256];
        ssize_t n;

        if (left <= 0 || quit) {
            return;
        }
        if (poll(&pfd, 1, (int)(left * 1000) + 1) <= 0) {
            continue;
        }
        n = read(mcu_fd, buf, sizeof(buf));
        for (ssize_t i = 0; i < n; i++) {
            pkt_rx_byte(buf[i]);
        }
    }
}

static int mcu_open(const char *dev)
{
    struct termios t;

    mcu_fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (mcu_fd < 0) {
        result("FAIL", "MCU link", "%s: %s", dev, strerror(errno));
        return -1;
    }
    tcgetattr(mcu_fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B230400);
    cfsetospeed(&t, B230400);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(mcu_fd, TCSANOW, &t);
    tcflush(mcu_fd, TCIOFLUSH);
    pkt_rx_init(on_packet);
    return 0;
}

static void light_test(void)
{
    uint8_t colours[PADS];
    double t0 = now();

    /*
     * COMM_INIT is answered with the firmware version only once per MCU
     * boot, after that only by the keepalive every ~2 s. GET_SW_VER is
     * answered every time, so ask for it as well.
     */
    comm_init();
    send(PKT_SYSTEM, SYS_GET_SW_VER, NULL, 0);
    while (mcu_version < 0 && !quit && now() - t0 < 3.0) {
        pump(0.01);
    }
    if (!linked) {
        result("FAIL", "MCU link", "no answer on UART1 after 3 s");
        return;
    }
    if (mcu_version < 0) {
        result("WARN", "MCU link", "linked, but no firmware version");
    } else {
        result("PASS", "MCU link", "firmware %d.%d, answered in %.0f ms",
               mcu_version >> 8, mcu_version & 0xff, (now() - t0) * 1000);
    }

    /* Every pad in each colour of the wheel */
    for (size_t c = 0; c < ARRAY_SIZE(rainbow) && !quit; c++) {
        memset(colours, rainbow[c], PADS);
        pads_all(colours, 1);
        pump(0.12);
    }
    /* Pads one by one */
    memset(colours, 0, PADS);
    pads_all(colours, 0);
    for (int i = 0; i < PADS && !quit; i++) {
        pad(i, 1, rainbow[i % ARRAY_SIZE(rainbow)]);
        pump(0.05);
    }
    result("PASS", "Pads", "16 pads, %zu colours", ARRAY_SIZE(rainbow));

    /* Key LEDs one after the other, then all on */
    for (int id = 0; id <= LED_KEY_LAST && !quit; id++) {
        led(id, 1, 1);
        pump(0.03);
        led(id, 0, 0);
    }
    for (int id = 0; id <= LED_KEY_LAST; id++) {
        led(id, 1, 1);
    }
    pump(0.5);
    for (int id = 0; id <= LED_KEY_LAST; id++) {
        led(id, 0, 0);
    }
    result("PASS", "Key LEDs", "%d keys", LED_KEY_LAST + 1);

    /* Jog ring twice round, LINK LEDs, slider sweep */
    for (int i = 0; i < 2 * LED_JOG_RING_COUNT && !quit; i++) {
        led(LED_JOG_RING_FIRST + i % LED_JOG_RING_COUNT, 1, 0);
        pump(0.06);
        led(LED_JOG_RING_FIRST + i % LED_JOG_RING_COUNT, 0, 0);
    }
    for (int i = 0; i < LED_LINK_COUNT && !quit; i++) {
        led(LED_LINK_FIRST + i, 1, 0);
        pump(0.1);
    }
    for (int pct = 0; pct <= 100 && !quit; pct += 10) {
        slider_level(pct);
        pump(0.04);
    }
    for (int pct = 100; pct >= 0 && !quit; pct -= 10) {
        slider_level(pct);
        pump(0.04);
    }
    for (int i = 0; i < LED_LINK_COUNT; i++) {
        led(LED_LINK_FIRST + i, 0, 0);
    }
    result("PASS", "Ring/LINK", "jog ring, LINK LEDs, slider meter");
}

static void run_interactive(void)
{
    uint8_t colours[PADS];
    int step = 0;

    interactive = 1;
    heading("Interactive: hit pads, press keys, turn knobs, touch the slider "
            "(Ctrl-C to quit)");
    while (!quit) {
        for (int i = 0; i < PADS; i++) {
            colours[i] = burst[i] ? COLOUR_WHITE
                                  : rainbow[(i % 4 + i / 4 + step) %
                                            ARRAY_SIZE(rainbow)];
            if (burst[i]) {
                burst[i]--;
            }
        }
        pads_all(colours, 1);
        led(LED_JOG_RING_FIRST + (step + LED_JOG_RING_COUNT - 1) %
            LED_JOG_RING_COUNT, 0, 0);
        led(LED_JOG_RING_FIRST + step % LED_JOG_RING_COUNT, 1, 0);
        step++;
        pump(0.15);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttymxc0";
    int once = 0, dim = 1;
    struct sigaction sa = { .sa_handler = on_signal };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) {
            once = 1;
        } else if (!strcmp(argv[i], "--no-backlight")) {
            dim = 0;
        } else if (!strcmp(argv[i], "--mcu") && i + 1 < argc) {
            dev = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--once] [--no-backlight] "
                    "[--mcu /dev/ttyX]\n", argv[0]);
            return 2;
        }
    }
    tty = isatty(STDOUT_FILENO);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    printf(tty ? "\033[1mKICK hardware test\033[0m\n" : "KICK hardware test\n");
    heading("System");
    check_soc();
    check_memory();
    check_thermal();
    check_rtc();
    check_eeprom();
    heading("Storage");
    check_block("eMMC", "mmcblk2", 1);
    check_block("SD card", "mmcblk1", 0);
    heading("Peripherals");
    check_backlight(dim);
    check_input();
    check_serial();
    check_network();
    check_audio();
    check_usb();
    heading("Front panel (MCU on UART1)");
    if (mcu_open(dev) == 0) {
        light_test();
    }
    printf("\n%u failure(s)\n", failures);
    fflush(stdout);
    if (mcu_fd >= 0 && linked && !once && !quit) {
        run_interactive();
    }
    if (mcu_fd >= 0) {
        all_off();
        tcdrain(mcu_fd);
        close(mcu_fd);
    }
    return failures > 125 ? 125 : (int)failures;
}
