/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Restarting and powering off the machine when the guest calls PSCI SYSTEM_RESET or
 * SYSTEM_OFF and there is no host (hv_stop.c): SiliconNT docs/DESIGN.md, "EL2 层做什么" 3,
 * and docs/findings/0067, which has where each step and each value comes from.
 *
 * Three things are touched for it, and only here: the watchdog, the key MBSE of the SMC
 * (Apple's system management coprocessor), and two bytes of the scratchpad of the primary
 * PMU (power management unit). The guest has their registers the rest of the time. By now
 * every core is in EL2 (hv_rendezvous() in _hv_exc_proxy()), the hypervisor's lock is held
 * and interrupts are masked: whatever is waited for here is polled.
 *
 *  - Restart: byte 1 of the scratchpad becomes 0, "shut down or restarted on purpose", and
 *    the shutdown flag in byte 15 is cleared, so that one left from before does not power the
 *    machine off. Then the second timer of the watchdog resets the machine (wdt_reboot()). A
 *    PMU that cannot be written does not keep the machine from restarting.
 *  - Power off: the SMC is woken and told MBSE = offw; byte 1 becomes 0, and byte 15 gets
 *    the shutdown flag as macOS sets it; then MBSE = off1. Without the flag off1 would
 *    restart the machine, so off1 is not sent if the flag cannot be set.
 *
 * Each returns only if the machine is still running: 0.5 s after the watchdog was set, 1 s
 * after off1, or at once when a step before off1 fails. Nothing becomes a restart instead:
 * hv_stop.c shows what happened on the screen.
 *
 * Tested on the build machine by siliconnt/nohost-model (power_test.c), against an SMC, an
 * SPMI bus with the PMU on it and a watchdog that the test plays.
 */

#include "hv.h"
#include "adt.h"
#include "smc.h"
#include "spmi.h"
#include "string.h"
#include "utils.h"
#include "wdt.h"

/* SMC_KEY() of Linux, include/linux/mfd/macsmc.h: the four characters, the first one highest */
#define SMC_KEY_MBSE          0x4d425345 // "MBSE"
#define MBSE_SHUTDOWN_WARNING 0x6f666677 // "offw"
#define MBSE_SHUTDOWN         0x6f666631 // "off1"

/* Bytes of the scratchpad of the primary PMU, from its info-leg_scrpad on */
#define PMU_BOOT_STAGE    1      // 0: shut down or restarted on purpose
#define PMU_BOOT_STATE    15     // what macOS calls kAppleDialogBootStateOffset
#define PMU_SHUTDOWN_FLAG BIT(3) // in PMU_BOOT_STATE: off1 powers off instead of restarting
#define PMU_OFF_KEEP      (BIT(0) | BIT(1) | BIT(2) | BIT(5)) // what macOS keeps of the rest

#define RESET_WAIT_USEC 500000
#define OFF_WAIT_USEC   1000000

#define SPMI_BUSES "nub-spmi"

struct pmu {
    spmi_dev_t *spmi;
    u8 addr;     // on the SPMI bus
    u16 scratch; // where the scratchpad starts
};

/* The first 32 bits of a property; with exact, of one that has no more than that */
static int get_u32(int node, const char *name, bool exact, u32 *val)
{
    u32 len;
    const void *p = adt_getprop(adt, node, name, &len);

    if (!p || len < sizeof(*val) || (exact && len != sizeof(*val)))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

/*
 * The PMU that is-primary, on one of the SPMI buses /arm-io/nub-spmi*. Its reg starts with its
 * address on the bus.
 */
static int pmu_open(struct pmu *pmu)
{
    char path[64];
    int bus = adt_path_offset(adt, "/arm-io");

    if (bus < 0) {
        printf("HV: power: no /arm-io in the device tree\n");
        return -1;
    }
    ADT_FOREACH_CHILD(adt, bus)
    {
        const char *bus_name = adt_get_name(adt, bus);
        int dev = bus;

        if (strncmp(bus_name, SPMI_BUSES, strlen(SPMI_BUSES)))
            continue;
        ADT_FOREACH_CHILD(adt, dev)
        {
            u32 primary, addr, scratch;

            if (get_u32(dev, "is-primary", true, &primary) < 0 || primary != 1)
                continue;
            if (get_u32(dev, "reg", false, &addr) < 0 || addr > 0xf ||
                get_u32(dev, "info-leg_scrpad", true, &scratch) < 0 ||
                scratch > 0xffff - PMU_BOOT_STATE) {
                printf("HV: power: the primary PMU on /arm-io/%s has no address or no "
                       "scratchpad that can be used\n",
                       bus_name);
                return -1;
            }
            snprintf(path, sizeof(path), "/arm-io/%s", bus_name);
            pmu->spmi = spmi_init(path);
            if (!pmu->spmi)
                return -1;
            pmu->addr = addr;
            pmu->scratch = scratch;
            return 0;
        }
    }
    printf("HV: power: no primary PMU on /arm-io/" SPMI_BUSES "*\n");
    return -1;
}

static int pmu_read(struct pmu *pmu, u16 offset, u8 *val)
{
    int ret = spmi_ext_read_long(pmu->spmi, pmu->addr, pmu->scratch + offset, val, 1);

    if (ret < 0)
        printf("HV: power: PMU byte 0x%x was not read (%d)\n", pmu->scratch + offset, ret);
    return ret;
}

static int pmu_write(struct pmu *pmu, u16 offset, u8 val)
{
    int ret = spmi_ext_write_long(pmu->spmi, pmu->addr, pmu->scratch + offset, &val, 1);

    if (ret < 0)
        printf("HV: power: PMU byte 0x%x was not written (%d)\n", pmu->scratch + offset, ret);
    return ret;
}

/*
 * Byte 1 becomes 0. In byte 15 the shutdown flag is set and of the rest only what macOS keeps
 * is kept, to power off; or the flag is cleared and the rest stays, to restart.
 */
static int pmu_mark(struct pmu *pmu, bool off)
{
    u8 state;

    if (pmu_write(pmu, PMU_BOOT_STAGE, 0) < 0 || pmu_read(pmu, PMU_BOOT_STATE, &state) < 0)
        return -1;
    if (off)
        state = (state & PMU_OFF_KEEP) | PMU_SHUTDOWN_FLAG;
    else
        state &= ~PMU_SHUTDOWN_FLAG;
    return pmu_write(pmu, PMU_BOOT_STATE, state);
}

void hv_power_reset(void)
{
    struct pmu pmu;

    printf("HV: restarting the machine\n");
    if (pmu_open(&pmu) < 0) {
        printf("HV: power: restarting without telling the PMU that it is on purpose\n");
    } else {
        if (pmu_mark(&pmu, false) < 0)
            printf("HV: power: restarting all the same\n");
        spmi_shutdown(pmu.spmi);
    }

    /* Returns at once if m1n1 found no watchdog when it started (wdt_disable()) */
    wdt_reboot();
    udelay(RESET_WAIT_USEC);
    printf("HV: the watchdog has not restarted the machine within %d ms\n", RESET_WAIT_USEC / 1000);
}

/*
 * The SMC is not put to rest afterwards (smc_shutdown()): its node in the device tree says
 * no-shutdown, and the machine is to be off by then anyway
 */
void hv_power_off(void)
{
    struct pmu pmu;
    smc_dev_t *smc;
    int ret;

    printf("HV: powering the machine off\n");
    smc = smc_init();
    if (!smc) {
        printf("HV: power: the SMC did not wake up\n");
        return;
    }
    if (smc_write_u32(smc, SMC_KEY_MBSE, MBSE_SHUTDOWN_WARNING)) {
        printf("HV: power: the SMC did not take MBSE = offw\n");
        return;
    }

    if (pmu_open(&pmu) < 0) {
        printf("HV: power: off1 without the shutdown flag would restart: not sending it\n");
        return;
    }
    ret = pmu_mark(&pmu, true);
    spmi_shutdown(pmu.spmi);
    if (ret < 0) {
        printf("HV: power: off1 without the shutdown flag would restart: not sending it\n");
        return;
    }

    if (smc_write_u32(smc, SMC_KEY_MBSE, MBSE_SHUTDOWN)) {
        printf("HV: power: the SMC did not take MBSE = off1\n");
        return;
    }
    udelay(OFF_WAIT_USEC);
    printf("HV: the machine is still on %d ms after MBSE = off1\n", OFF_WAIT_USEC / 1000);
}
