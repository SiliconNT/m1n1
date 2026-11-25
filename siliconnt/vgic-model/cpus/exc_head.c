/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the ways of a core into EL2 and out of it stand on, and the host's: hv_exc_entry(),
 * hv_exc_exit(), hv_exc_fiq(), hv_exc_proxy() and what they call of src/hv/hv_exc.c. The Makefile
 * puts those, as they are there, after this, and what cpus_test.c calls after them.
 */

#include "cpu_regs.h"
#include "exception.h"
#include "hv.h"
#include "hv_sprr.h"
#include "smp.h"
#include "uartproxy.h"
#include "utils.h"

extern spinlock_t bhl;

void hv_wdt_breadcrumb(char c)
{
    UNUSED(c);
}

void hv_wdt_suspend(void)
{
}

void hv_wdt_resume(void)
{
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

u64 hv_translate(u64 addr, bool s1only, bool w, u64 *par_out)
{
    UNUSED(addr);
    UNUSED(s1only);
    UNUSED(w);
    UNUSED(par_out);
    return 0;
}

/* Without a host, and the host: in cpus_test.c */
void model_stop(int reason, u32 code);
int model_host(int reason, u32 code);

void hv_stop_if_no_host(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type)
{
    UNUSED(ctx);
    model_stop(reason, type);
}

int uartproxy_run(struct uartproxy_msg_start *start)
{
    return model_host(start->reason, start->code);
}

void print_regs(u64 *regs, int el12)
{
    UNUSED(regs);
    UNUSED(el12);
}

void model_steal_time(u64 ticks);
u64 model_stolen_time(void);
void model_exc_entry(void);
void model_exc_exit(void);
void model_exc_fiq(void);
void model_exc_proxy(int reason, u32 type);
void model_cpu_off(void);
void model_records_fill(int cpu);
bool model_records_clear(int cpu);
