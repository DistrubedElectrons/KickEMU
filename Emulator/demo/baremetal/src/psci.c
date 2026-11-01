/*
 * PSCI over SMC (Arm DEN 0022). On the KICK, ATF BL31 implements it; with
 * boot-mode=kernel QEMU answers the same calls.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "psci.h"

#define PSCI_VERSION            0x84000000u
#define PSCI_AFFINITY_INFO_64   0xc4000004u
#define PSCI_INVALID_PARAMS     (-2)

static int64_t smc(uint64_t fn, uint64_t a1, uint64_t a2)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;

    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2)
                     :: "x3", "memory");
    return (int64_t)x0;
}

int32_t psci_version(void)
{
    return (int32_t)smc(PSCI_VERSION, 0, 0);
}

int psci_count_cpus(int max)
{
    int n = 0;

    /* AFFINITY_INFO(mpidr, level 0): 0 ON, 1 OFF, 2 ON_PENDING */
    for (int cpu = 0; cpu < max; cpu++) {
        if (smc(PSCI_AFFINITY_INFO_64, (uint64_t)cpu, 0) !=
            PSCI_INVALID_PARAMS) {
            n++;
        }
    }
    return n;
}
