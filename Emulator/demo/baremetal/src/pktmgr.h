/*
 * KICK control-surface protocol (ComProtocol/PktMgr) in C
 *
 * Frame: 'M' 'T' type dir cmd size data[size - 1] crc
 * size counts the data bytes plus the CRC byte, the CRC-8 (poly 0x31,
 * init 0xff, MSB first) covers every byte from 'M' up to the last data
 * byte. Frames from the MCU carry one extra trailer byte ('\n') that the
 * Linux reader skips; frames to the MCU end with the CRC, exactly as
 * CPktMgr::BuildAndTransmitPacket() builds them.
 *
 * Types, commands and data layouts are those of
 * Kick/modules/contracts/dcl/dcl_contracts.h and dcl_contracts_helper.cpp.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_PKTMGR_H
#define KICK_DEMO_PKTMGR_H

#include <stdint.h>

/* CPktMgr::enumPacketType */
enum {
    PKT_SYSTEM = 0,
    PKT_BUTTON,
    PKT_LED,
    PKT_PAD,
    PKT_KNOBS,
    PKT_MIDI,
    PKT_CV,
    PKT_SLIDER,
};

/* CPktMgr::enumPacketDir */
enum {
    PKT_REQUEST = 0,
    PKT_RESPONSE,
};

/* DeviceControlContracts::SysCmd */
enum {
    SYS_RESET_GD = 0,
    SYS_GET_SW_VER,
    SYS_COMM_INIT,
    SYS_SW_UPDATE,
    SYS_CHECK_COMM_STATUS,
    SYS_BPM,
    SYS_RECONNECT,
    SYS_SHUTDOWN,
};
#define SYS_COMM_CONNECTED      1   /* SysCommCmd::EN_SYS_COMM_CONNECTED */

/* LedCmd, PadCmd, SliderCmd, BtnCmd (subset used here) */
#define LED_SET_SINGLE_LED      0
#define PAD_STATE               0
#define PAD_SET_SINGLE_PAD      1
#define PAD_SET_ALL_MULTI_COLOUR 3
#define SLIDER_LEVEL_LED        2
#define BTN_STATUS              0

/* PadButtonState */
#define STATE_OFF               0
#define STATE_ON                1

#define PKT_MAX_DATA            40

typedef struct pkt {
    uint8_t type;
    uint8_t dir;
    uint8_t cmd;
    uint8_t len;
    uint8_t data[PKT_MAX_DATA];
} pkt;

typedef void (*pkt_tx_fn)(const uint8_t *buf, int len);
typedef void (*pkt_rx_fn)(const pkt *p);

uint8_t pkt_crc(const uint8_t *buf, int len);
/* Build a frame into @out (at least len + 7 bytes); returns its length */
int pkt_build(const pkt *p, uint8_t *out);
void pkt_send(pkt_tx_fn tx, uint8_t type, uint8_t cmd, const uint8_t *data,
              int len);

/* Receiver: feed bytes, @rx is called for every frame with a valid CRC */
void pkt_rx_init(pkt_rx_fn rx);
void pkt_rx_byte(uint8_t b);
unsigned pkt_rx_errors(void);

#endif
