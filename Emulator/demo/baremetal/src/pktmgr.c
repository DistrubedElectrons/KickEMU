/*
 * KICK control-surface protocol, a C port of ComProtocol/PktMgr.
 *
 * The transmit side mirrors CPktMgr::BuildAndTransmitPacket() byte for
 * byte. The receive side follows CPktMgr::PushByteToRxFifo() (sync on
 * 'M' 'T', then type, dir, id, size, size - 1 data bytes, CRC) with one
 * difference: bytes before the 'M' sync are not included in the CRC, so a
 * stray trailer byte between frames cannot invalidate the next one.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pktmgr.h"

uint8_t pkt_crc(const uint8_t *buf, int len)
{
    uint8_t crc = 0xff;

    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : crc << 1;
        }
    }
    return crc;
}

int pkt_build(const pkt *p, uint8_t *out)
{
    int n = 0;

    out[n++] = 'M';
    out[n++] = 'T';
    out[n++] = p->type;
    out[n++] = p->dir;
    out[n++] = p->cmd;
    out[n++] = p->len + 1;      /* data + CRC byte */
    for (int i = 0; i < p->len; i++) {
        out[n++] = p->data[i];
    }
    out[n] = pkt_crc(out, n);
    return n + 1;
}

void pkt_send(pkt_tx_fn tx, uint8_t type, uint8_t cmd, const uint8_t *data,
              int len)
{
    pkt p = { .type = type, .dir = PKT_REQUEST, .cmd = cmd, .len = len };
    uint8_t frame[PKT_MAX_DATA + 8];

    for (int i = 0; i < len && i < PKT_MAX_DATA; i++) {
        p.data[i] = data[i];
    }
    tx(frame, pkt_build(&p, frame));
}

enum {
    RX_SYNC_M, RX_SYNC_T, RX_TYPE, RX_DIR, RX_ID, RX_SIZE, RX_DATA, RX_CRC,
};

static struct {
    int state;
    uint8_t buf[PKT_MAX_DATA + 8];
    int n;
    int need;
    pkt_rx_fn cb;
    unsigned errors;
} rx;

void pkt_rx_init(pkt_rx_fn cb)
{
    rx.state = RX_SYNC_M;
    rx.cb = cb;
}

unsigned pkt_rx_errors(void)
{
    return rx.errors;
}

void pkt_rx_byte(uint8_t b)
{
    switch (rx.state) {
    case RX_SYNC_M:
        if (b == 'M') {
            rx.n = 0;
            rx.buf[rx.n++] = b;
            rx.state = RX_SYNC_T;
        }
        return;
    case RX_SYNC_T:
        rx.buf[rx.n++] = b;
        rx.state = b == 'T' ? RX_TYPE : (b == 'M' ? RX_SYNC_T : RX_SYNC_M);
        if (b == 'M') {
            rx.n = 1;
        }
        return;
    case RX_TYPE:
    case RX_DIR:
    case RX_ID:
        rx.buf[rx.n++] = b;
        rx.state++;
        return;
    case RX_SIZE:
        rx.buf[rx.n++] = b;
        if (b == 0 || b > PKT_MAX_DATA + 1) {
            rx.errors++;
            rx.state = RX_SYNC_M;
            return;
        }
        rx.need = b - 1;
        rx.state = rx.need ? RX_DATA : RX_CRC;
        return;
    case RX_DATA:
        rx.buf[rx.n++] = b;
        if (--rx.need == 0) {
            rx.state = RX_CRC;
        }
        return;
    case RX_CRC:
        rx.state = RX_SYNC_M;
        if (pkt_crc(rx.buf, rx.n) != b) {
            rx.errors++;
            return;
        }
        if (rx.cb) {
            pkt p = {
                .type = rx.buf[2], .dir = rx.buf[3], .cmd = rx.buf[4],
                .len = rx.buf[5] - 1,
            };

            for (int i = 0; i < p.len; i++) {
                p.data[i] = rx.buf[6 + i];
            }
            rx.cb(&p);
        }
        return;
    }
}
