/*
 * Minimal MIPI DSI peripheral interface for the KICK display models
 *
 * A DSI host model (the i.MX8MM DSIM) hands every command-mode packet it
 * transmits to the attached peripheral. Video-mode pixel streams are not
 * passed through: pixels are scanned out by the LCDIF model directly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_DSI_H
#define KICK_DSI_H

#include "qom/object.h"

#define TYPE_KICK_DSI_PERIPHERAL "kick-dsi-peripheral"

typedef struct KickDsiPeripheralClass KickDsiPeripheralClass;
DECLARE_CLASS_CHECKERS(KickDsiPeripheralClass, KICK_DSI_PERIPHERAL,
                       TYPE_KICK_DSI_PERIPHERAL)
#define KICK_DSI_PERIPHERAL(obj) \
    INTERFACE_CHECK(KickDsiPeripheral, (obj), TYPE_KICK_DSI_PERIPHERAL)

typedef struct KickDsiPeripheral KickDsiPeripheral;

/* Largest response a peripheral may return (long read response payload). */
#define KICK_DSI_MAX_RESPONSE   64

/*
 * Response to a packet. For a read request (generic or DCS read) the
 * peripheral fills in a DSI response packet: data type, and for short
 * responses the one or two data bytes in data[], for long responses the
 * payload in data[] with len set. ack_only reports an acknowledge without
 * data (BTA after a write with ACK requested).
 */
typedef struct KickDsiResponse {
    uint8_t data_type;
    uint16_t len;
    uint8_t data[KICK_DSI_MAX_RESPONSE];
} KickDsiResponse;

struct KickDsiPeripheralClass {
    InterfaceClass parent_class;

    /*
     * Deliver one packet. @vc is the virtual channel, @dt the data type,
     * @payload/@len the long packet payload or the two short packet data
     * bytes. Returns true if the peripheral answered with @resp (only
     * meaningful for read requests).
     */
    bool (*transfer)(KickDsiPeripheral *p, uint8_t vc, uint8_t dt,
                     const uint8_t *payload, size_t len, KickDsiResponse *resp);
};

#endif
