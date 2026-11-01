/*
 * PSCI calls over SMC (the firmware interface the KICK DT declares)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef KICK_DEMO_PSCI_H
#define KICK_DEMO_PSCI_H

#include <stdint.h>

/* PSCI_VERSION: major << 16 | minor, or a negative error */
int32_t psci_version(void);
/* Number of CPUs PSCI knows (AFFINITY_INFO answers for MPIDR 0..max-1) */
int psci_count_cpus(int max);

#endif
