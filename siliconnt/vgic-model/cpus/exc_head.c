/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What hv_exc_entry(), hv_exc_exit() and hv_write_cntvoff() of src/hv/hv_exc.c stand on. The
 * Makefile puts the three, as they are there, after this, and the two that cpus_test.c calls
 * after them.
 */

#include "cpu_regs.h"
#include "hv.h"
#include "hv_sprr.h"
#include "smp.h"
#include "utils.h"

#define PERCPU(x) pcpu[mrs(TPIDR_EL2)].x

struct hv_pcpu_data {
    u64 exc_entry_pmcr0_cnt;
};
static struct hv_pcpu_data pcpu[MAX_CPUS];

static u64 stolen_time;
static __maybe_unused u64 exc_entry_time;

extern spinlock_t bhl;
extern u64 hv_cpus_in_guest;

void hv_wdt_breadcrumb(char c)
{
    UNUSED(c);
}

/* Where the lock is let go of, the others come in: what they are to see has to be there by then */
void model_window(void);
#define spin_unlock(lock)                                                                          \
    do {                                                                                           \
        (spin_unlock)(lock);                                                                       \
        model_window();                                                                            \
    } while (0)

/* Timers, SGIs and SPIs: not here */
void hv_update_fiq(void)
{
}

/* The host has handled an exception: hv_exc_proxy() */
void model_steal_time(u64 ticks);

void model_steal_time(u64 ticks)
{
    stolen_time += ticks;
}
