/*
 * qtest for the SAI2 WAV input/recording of the kick machine
 * (docs/host-interface.md, "Audio (SAI2)"): machine properties
 * audio-wav-in, audio-wav-in-loop, audio-wav-out, audio-tap and audio-status, driven
 * through the SAI2 registers as fsl_sai programs them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include <sys/un.h>

#define SAI             0x30020000
#define TCSR            0x008
#define TCR1            0x00c
#define TDR0            0x020
#define TFR0            0x040
#define RCSR            0x088
#define RDR0            0x0a0
#define RFR0            0x0c0
#define CSR_TE          (1u << 31)

#define MS              (1000 * 1000)
#define FRAMES_PER_MS   48

static void wr(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, SAI + off, val);
}

static uint32_t rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, SAI + off);
}

/* 2 x 32-bit I2S at 48 kHz, clock master from MCLK1 (as kick-sai-test) */
static void sai_setup_i2s(QTestState *qts, uint32_t base)
{
    wr(qts, base + 0x08, (1u << 26) | (1u << 24) | 1);
    wr(qts, base + 0x0c, 1u << 16);
    wr(qts, base + 0x10, (1u << 16) | (31u << 8) | (1 << 5) | (1 << 4) |
       (1 << 3) | (1 << 1) | 1);
    wr(qts, base + 0x14, (31u << 24) | (31u << 16) | (31u << 8));
    wr(qts, base + 0x58, 0xfffffffc);
}

static char *tmp_path(const char *tmpl)
{
    char *path = NULL;
    int fd = g_file_open_tmp(tmpl, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return path;
}

static void put16(GByteArray *b, uint16_t v)
{
    uint8_t x[2] = { v, v >> 8 };

    g_byte_array_append(b, x, 2);
}

static void put32(GByteArray *b, uint32_t v)
{
    put16(b, v);
    put16(b, v >> 16);
}

/* Canonical WAV file with the given format and raw sample bytes */
static char *make_wav(uint16_t tag, uint16_t ch, uint32_t rate,
                      uint16_t bits, const void *data, uint32_t len)
{
    char *path = tmp_path("kick-in-XXXXXX.wav");
    GByteArray *b = g_byte_array_new();

    g_byte_array_append(b, (const uint8_t *)"RIFF", 4);
    put32(b, 36 + len);
    g_byte_array_append(b, (const uint8_t *)"WAVEfmt ", 8);
    put32(b, 16);
    put16(b, tag);
    put16(b, ch);
    put32(b, rate);
    put32(b, rate * ch * bits / 8);
    put16(b, ch * bits / 8);
    put16(b, bits);
    g_byte_array_append(b, (const uint8_t *)"data", 4);
    put32(b, len);
    g_byte_array_append(b, data, len);
    g_assert_true(g_file_set_contents(path, (const char *)b->data, b->len,
                                      NULL));
    g_byte_array_unref(b);
    return path;
}

static QDict *qom_set_str(QTestState *qts, const char *prop, const char *v)
{
    return qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                     "{'path': '/machine', 'property': %s, 'value': %s}}",
                     prop, v);
}

static void set_str(QTestState *qts, const char *prop, const char *v)
{
    QDict *rsp = qom_set_str(qts, prop, v);

    g_assert_false(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
}

static QDict *status(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': '/machine', 'property': 'audio-status'}}");
    QDict *st;

    g_assert_true(qdict_haskey(rsp, "return"));
    st = qobject_to(QDict, qobject_from_json(qdict_get_str(rsp, "return"),
                                             &error_abort));
    g_assert_nonnull(st);
    qobject_unref(rsp);
    return st;
}

static int64_t status_int(QTestState *qts, const char *key)
{
    QDict *st = status(qts);
    int64_t v = qdict_get_int(st, key);

    qobject_unref(st);
    return v;
}

static char *status_str(QTestState *qts, const char *key)
{
    QDict *st = status(qts);
    char *v = g_strdup(qdict_get_str(st, key));

    qobject_unref(st);
    return v;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* Left = (n + 1) * 0x01010101, right = its negation, as 32-bit slot words */
static uint32_t tx_word(int frame, int ch)
{
    uint32_t v = (frame + 1) * 0x01010101u;

    return ch ? -v : v;
}

/* Play @frames frames of tx_word() (at most 64, one FIFO) */
static void play(QTestState *qts, int frames)
{
    sai_setup_i2s(qts, 0x08);
    wr(qts, TCSR, 1u << 25);                /* FR: empty FIFO */
    for (int i = 0; i < 64; i++) {
        wr(qts, TDR0, tx_word(i, 0));
        wr(qts, TDR0, tx_word(i, 1));
    }
    wr(qts, TCSR, CSR_TE);
    qtest_clock_step(qts, frames / FRAMES_PER_MS * MS);
    wr(qts, TCSR, 0);
    g_assert_cmpuint(rd(qts, TFR0) & 0xff, ==, 2 * frames);
}

/* Check a recording of play(): header sizes, format and every sample */
static void check_recording(const char *path, int frames)
{
    g_autofree uint8_t *wav = NULL;
    gsize len;

    g_assert_true(g_file_get_contents(path, (char **)&wav, &len, NULL));
    g_assert_cmpuint(len, ==, 44 + frames * 8);
    g_assert_cmpmem(wav, 4, "RIFF", 4);
    g_assert_cmpuint(le32(wav + 4), ==, len - 8);
    g_assert_cmpmem(wav + 8, 8, "WAVEfmt ", 8);
    g_assert_cmpuint(le16(wav + 20), ==, 1);            /* PCM */
    g_assert_cmpuint(le16(wav + 22), ==, 2);
    g_assert_cmpuint(le32(wav + 24), ==, 48000);
    g_assert_cmpuint(le32(wav + 28), ==, 48000 * 8);
    g_assert_cmpuint(le16(wav + 32), ==, 8);
    g_assert_cmpuint(le16(wav + 34), ==, 32);
    g_assert_cmpmem(wav + 36, 4, "data", 4);
    g_assert_cmpuint(le32(wav + 40), ==, frames * 8);
    for (int i = 0; i < frames; i++) {
        g_assert_cmphex(le32(wav + 44 + i * 8), ==, tx_word(i, 0));
        g_assert_cmphex(le32(wav + 48 + i * 8), ==, tx_word(i, 1));
    }
}

/* Recording through qom-set; "" closes and finalises the file */
static void test_record(void)
{
    g_autofree char *out = tmp_path("kick-out-XXXXXX.wav");
    QTestState *qts = qtest_init("-M kick");
    g_autofree char *name = NULL;

    set_str(qts, "audio-wav-out", out);
    name = status_str(qts, "wav_out");
    g_assert_cmpstr(name, ==, out);
    play(qts, FRAMES_PER_MS);
    g_assert_cmpint(status_int(qts, "out_frames"), ==, FRAMES_PER_MS);
    g_assert_cmpint(status_int(qts, "wav_out_frames"), ==, FRAMES_PER_MS);
    set_str(qts, "audio-wav-out", "");
    g_free(name);
    name = status_str(qts, "wav_out");
    g_assert_cmpstr(name, ==, "");
    check_recording(out, FRAMES_PER_MS);

    /* Nothing reaches a closed recording */
    play(qts, FRAMES_PER_MS);
    g_assert_cmpint(status_int(qts, "out_frames"), ==, 2 * FRAMES_PER_MS);
    check_recording(out, FRAMES_PER_MS);
    qtest_quit(qts);
    unlink(out);
}

/* A recording given at start-up is finalised when QEMU exits */
static void test_record_exit(void)
{
    g_autofree char *out = tmp_path("kick-out-XXXXXX.wav");
    QTestState *qts = qtest_initf("-M kick,audio-wav-out=%s", out);

    play(qts, FRAMES_PER_MS);
    qtest_quit(qts);
    check_recording(out, FRAMES_PER_MS);
    unlink(out);
}

/* Read @frames capture frames from RDR0 into @l/@r (drains the FIFO) */
static void capture(QTestState *qts, int frames, int32_t *l, int32_t *r)
{
    uint32_t rfr;

    qtest_clock_step(qts, frames / FRAMES_PER_MS * MS);
    rfr = rd(qts, RFR0);
    g_assert_cmpuint(((rfr >> 16) - rfr) & 0xff, ==, 2 * frames);
    for (int i = 0; i < frames; i++) {
        l[i] = rd(qts, RDR0);
        r[i] = rd(qts, RDR0);
    }
}

static void start_capture(QTestState *qts)
{
    sai_setup_i2s(qts, 0x88);
    wr(qts, RCSR, CSR_TE);
}

#define IN_FRAMES   100

static char *make_ramp16(void)
{
    int16_t pcm[IN_FRAMES * 2];

    for (int i = 0; i < IN_FRAMES; i++) {
        pcm[2 * i] = GINT16_TO_LE((i + 1) * 300);
        pcm[2 * i + 1] = GINT16_TO_LE(-(i + 1) * 300);
    }
    return make_wav(1, 2, 48000, 16, pcm, sizeof(pcm));
}

/* 16-bit stereo input, paced by the receiver, silence after the end */
static void test_input(void)
{
    g_autofree char *in = make_ramp16();
    QTestState *qts = qtest_init("-M kick");
    int32_t l[FRAMES_PER_MS], r[FRAMES_PER_MS];
    g_autofree char *name = NULL;
    int n = 0;

    set_str(qts, "audio-wav-in", in);
    name = status_str(qts, "wav_in");
    g_assert_cmpstr(name, ==, in);
    /* Nothing is consumed while the receiver is off */
    qtest_clock_step(qts, 5 * MS);
    g_assert_cmpint(status_int(qts, "wav_in_pos"), ==, 0);

    start_capture(qts);
    for (int ms = 0; ms < 3; ms++) {
        capture(qts, FRAMES_PER_MS, l, r);
        for (int i = 0; i < FRAMES_PER_MS; i++, n++) {
            int32_t want = n < IN_FRAMES ? (n + 1) * 300 * 65536 : 0;

            g_assert_cmpint(l[i], ==, want);
            g_assert_cmpint(r[i], ==, -want);
        }
        g_assert_cmpint(status_int(qts, "wav_in_pos"), ==,
                        MIN(n, IN_FRAMES));
        g_assert_cmpint(status_int(qts, "in_frames"), ==, n);
    }

    /* "" stops the input */
    set_str(qts, "audio-wav-in", "");
    g_free(name);
    name = status_str(qts, "wav_in");
    g_assert_cmpstr(name, ==, "");
    qtest_quit(qts);
    unlink(in);
}

static void test_input_loop(void)
{
    g_autofree char *in = make_ramp16();
    QTestState *qts = qtest_initf("-M kick,audio-wav-in=%s,"
                                  "audio-wav-in-loop=on", in);
    int32_t l[FRAMES_PER_MS], r[FRAMES_PER_MS];
    int n = 0;

    start_capture(qts);
    for (int ms = 0; ms < 5; ms++) {
        capture(qts, FRAMES_PER_MS, l, r);
        for (int i = 0; i < FRAMES_PER_MS; i++, n++) {
            g_assert_cmpint(l[i], ==, (n % IN_FRAMES + 1) * 300 * 65536);
            g_assert_cmpint(r[i], ==, -l[i]);
        }
    }
    g_assert_cmpint(status_int(qts, "wav_in_pos"), ==, n % IN_FRAMES);
    qtest_quit(qts);
    unlink(in);
}

/* 24-bit mono (duplicated) and float 32 (scaled and saturated) */
static void test_input_formats(void)
{
    static const uint8_t pcm24[] = {
        0x00, 0x00, 0x40,   /* 0x400000 -> 0x40000000 */
        0x01, 0x00, 0x80,   /* -0x7fffff */
        0xff, 0xff, 0xff,   /* -1 -> -256 */
    };
    static const float fl[][2] = {
        { 0.5f, -0.5f }, { 1.0f, -1.0f }, { 2.0f, -2.0f },
    };
    g_autofree char *w24 = make_wav(1, 1, 48000, 24, pcm24, sizeof(pcm24));
    g_autofree char *wfl = make_wav(3, 2, 48000, 32, fl, sizeof(fl));
    QTestState *qts = qtest_init("-M kick");
    int32_t l[FRAMES_PER_MS], r[FRAMES_PER_MS];

    g_assert_true(HOST_BIG_ENDIAN == 0);    /* the float array is raw LE */
    set_str(qts, "audio-wav-in", w24);
    start_capture(qts);
    capture(qts, FRAMES_PER_MS, l, r);
    g_assert_cmphex((uint32_t)l[0], ==, 0x40000000);
    g_assert_cmphex((uint32_t)l[1], ==, 0x80000100u);
    g_assert_cmphex((uint32_t)l[2], ==, 0xffffff00u);
    for (int i = 0; i < 3; i++) {
        g_assert_cmphex((uint32_t)r[i], ==, (uint32_t)l[i]);
    }

    set_str(qts, "audio-wav-in", wfl);
    capture(qts, FRAMES_PER_MS, l, r);
    g_assert_cmphex((uint32_t)l[0], ==, 0x40000000);
    g_assert_cmphex((uint32_t)r[0], ==, 0xc0000000);
    g_assert_cmphex((uint32_t)l[1], ==, 0x7fffffff);
    g_assert_cmphex((uint32_t)r[1], ==, 0x80000000);
    g_assert_cmphex((uint32_t)l[2], ==, 0x7fffffff);
    g_assert_cmphex((uint32_t)r[2], ==, 0x80000000);
    qtest_quit(qts);
    unlink(w24);
    unlink(wfl);
}

/* Wrong rate or format: qom-set fails, the current input stays */
static void test_input_rejected(void)
{
    static const int16_t pcm[4];
    static const uint8_t pcm8[4];
    g_autofree char *good = make_ramp16();
    g_autofree char *rate = make_wav(1, 2, 44100, 16, pcm, sizeof(pcm));
    g_autofree char *u8 = make_wav(1, 2, 48000, 8, pcm8, sizeof(pcm8));
    g_autofree char *ch3 = make_wav(1, 3, 48000, 16, pcm, 6);
    QTestState *qts = qtest_init("-M kick");
    const char *bad[] = { rate, u8, ch3, "/nonexistent/kick.wav" };

    set_str(qts, "audio-wav-in", good);
    for (int i = 0; i < ARRAY_SIZE(bad); i++) {
        QDict *rsp = qom_set_str(qts, "audio-wav-in", bad[i]);
        g_autofree char *name = NULL;

        g_assert_true(qdict_haskey(rsp, "error"));
        qobject_unref(rsp);
        name = status_str(qts, "wav_in");
        g_assert_cmpstr(name, ==, good);
    }
    qtest_quit(qts);
    unlink(good);
    unlink(rate);
    unlink(u8);
    unlink(ch3);
}

static bool status_bool(QTestState *qts, const char *key)
{
    QDict *st = status(qts);
    bool v = qdict_get_bool(st, key);

    qobject_unref(st);
    return v;
}

/* Playback tap: the played frames arrive as raw S32LE stereo on the chardev */
static void test_tap(void)
{
    g_autofree char *dir = g_dir_make_tmp("ktap-XXXXXX", NULL);
    g_autofree char *path = g_strdup_printf("%s/t.sock", dir);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    uint8_t buf[FRAMES_PER_MS * 8];
    size_t got = 0;
    QTestState *qts;
    int fd, tries;

    g_assert_cmpuint(strlen(path), <, sizeof(sa.sun_path));
    qts = qtest_initf("-chardev socket,id=tap,path=%s,server=on,wait=off "
                      "-M kick,audio-tap=tap", path);
    /* Without a reader nothing is queued */
    play(qts, FRAMES_PER_MS);
    g_assert_false(status_bool(qts, "tap_connected"));
    g_assert_cmpint(status_int(qts, "tap_frames"), ==, 0);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(fd, >=, 0);
    strcpy(sa.sun_path, path);
    g_assert_cmpint(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), ==, 0);
    for (tries = 0; tries < 1000 && !status_bool(qts, "tap_connected");
         tries++) {
        g_usleep(1000);
    }
    g_assert_true(status_bool(qts, "tap_connected"));

    play(qts, FRAMES_PER_MS);
    while (got < sizeof(buf)) {
        GPollFD pfd = { .fd = fd, .events = G_IO_IN };
        ssize_t n;

        g_assert_cmpint(g_poll(&pfd, 1, 5000), ==, 1);
        n = read(fd, buf + got, sizeof(buf) - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
    for (int i = 0; i < FRAMES_PER_MS; i++) {
        g_assert_cmphex(le32(buf + i * 8), ==, tx_word(i, 0));
        g_assert_cmphex(le32(buf + i * 8 + 4), ==, tx_word(i, 1));
    }
    g_assert_cmpint(status_int(qts, "tap_frames"), ==, FRAMES_PER_MS);
    g_assert_cmpint(status_int(qts, "tap_dropped"), ==, 0);
    close(fd);
    qtest_quit(qts);
    unlink(path);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/audio/record", test_record);
    qtest_add_func("/kick/audio/record-exit", test_record_exit);
    qtest_add_func("/kick/audio/input", test_input);
    qtest_add_func("/kick/audio/input-loop", test_input_loop);
    qtest_add_func("/kick/audio/input-formats", test_input_formats);
    qtest_add_func("/kick/audio/input-rejected", test_input_rejected);
    qtest_add_func("/kick/audio/tap", test_tap);
    return g_test_run();
}
