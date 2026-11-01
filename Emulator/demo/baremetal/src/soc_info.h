/*
 * SoC identification (ANATOP DIGPROG, OCOTP fuses) and die temperature
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_SOC_INFO_H
#define KICK_DEMO_SOC_INFO_H

#include <stdint.h>

typedef struct soc_info {
    uint32_t digprog;
    const char *name;       /* "i.MX8MM", "i.MX8MN", ... */
    const char *variant;    /* "Quad", "QuadLite", ... */
    unsigned rev_major;
    unsigned rev_minor;
    uint64_t uid;
    uint32_t speed_grade;   /* OCOTP TESTER3 */
    uint8_t mac[6];         /* OCOTP MAC0 fuses */
} soc_info;

void soc_info_read(soc_info *info);
/* TMU immediate temperature in degrees C, -1 if the reading is not ready */
int soc_temperature(void);

#endif
