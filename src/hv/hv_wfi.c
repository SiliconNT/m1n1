/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * WFI of the guest.
 *
 * A core of the M4 that executes WFI loses its state (src/kboot.c), and the register that
 * would make it keep the state is locked. So the guest's WFI is trapped (HCR_EL2.TWI) and the
 * core waits here, in EL2, for something the guest would have woken up for.
 *
 * It waits with WFE, which keeps the state of the core ("when a WFE or WFET instruction is
 * executed, memory coherency and architectural state are not lost", Arm DDI 0487 M.d D1.7.1),
 * and with IRQs, FIQs and SErrors masked. Nothing is taken or acknowledged here: the wait ends as soon as ISR_EL1 shows a physical interrupt, and the
 * core takes that interrupt the usual way once it is back in the guest.
 *
 * Every source is a level that stays up until its handler has dealt with it, so looking at
 * ISR_EL1 before each WFE cannot miss one. What the architecture does not promise is that an
 * interrupt which is masked ends a WFE (D1.7.1.4), so the event stream of the physical counter
 * is turned on: it ends a WFE at least every HV_WFI_EVENT_PERIOD_US, which bounds how late
 * the core notices.
 */

#include "hv.h"
#include "cpu_regs.h"
#include "smp.h"
#include "utils.h"

/* The longest a waiting core may sleep through something it should have woken up for */
#define HV_WFI_EVENT_PERIOD_US 100

extern spinlock_t bhl;

/* Only touched by the core it belongs to */
struct hv_wfi_pcpu {
    u64 traps;     // WFI and WFIT of the guest that trapped
    u64 wfit;      // ...of which WFIT: never waited for
    u64 immediate; // ...of which did not wait: an interrupt for the guest was there already
    u64 waits;     // ...of which waited
    u64 wfes;      // WFE instructions executed while waiting
    u64 ticks;     // time spent waiting, in ticks of the system counter
    u64 longest;   // the longest wait
    u64 ipi_waits; // waits during which the vGIC sent this core an IPI
    u64 ipi_ticks; // ...from the IPI to the end of the wait, in all
    u64 ipi_late;  // ...the longest
} ALIGNED(64);

static struct hv_wfi_pcpu wfi_pcpu[MAX_CPUS];

/*
 * When the vGIC last sent each core an IPI, 0 if not since that core began to wait. The only
 * thing here that other cores write: atomic.
 */
static u64 wfi_ipi_sent[MAX_CPUS];

/*
 * The bit of the counter whose 0 to 1 transitions make the event stream: the highest of bits
 * 0-15 that gives a period of no more than HV_WFI_EVENT_PERIOD_US. One event per 2^(bit + 1)
 * ticks.
 */
static u64 hv_wfi_event_bit(u64 freq)
{
    u64 ticks = freq / (1000000 / HV_WFI_EVENT_PERIOD_US);
    u64 bit = 0;

    while (bit < 15 && (4UL << bit) <= ticks)
        bit++;
    return bit;
}

/* On the boot core, from hv_init(); the other cores get CNTHCTL_EL2 from hv_secondary_info */
void hv_wfi_init(void)
{
    u64 freq = mrs(CNTFRQ_EL0);
    u64 bit = hv_wfi_event_bit(freq);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        wfi_pcpu[cpu] = (struct hv_wfi_pcpu){0};
        __atomic_store_n(&wfi_ipi_sent[cpu], 0, __ATOMIC_SEQ_CST);
    }

    /* EVNTIS, where it exists, would move the choice to bits 8-23 (FEAT_ECV) */
    msr(CNTHCTL_EL2, (mrs(CNTHCTL_EL2) & ~(CNTHCTL_EVNTIS | CNTHCTL_EVNTI | CNTHCTL_EVNTDIR)) |
                         FIELD_PREP(CNTHCTL_EVNTI, bit) | CNTHCTL_EVNTEN);
    sysop("isb");

    printf("HV: WFI: trapped, the event stream of bit %ld ends a WFE every %ld us\n", bit,
           (2000000UL << bit) / freq);
}

/*
 * The vGIC is about to send an IPI to this core, which may be waiting here. The first since the
 * wait began counts: how late the wait ends after it is what a masked IPI costs, the event
 * stream being what ends the WFE at the latest (SiliconNT docs/findings/0098, conclusion 28).
 */
void hv_wfi_ipi_sent(int cpu)
{
    u64 none = 0;

    __atomic_compare_exchange_n(&wfi_ipi_sent[cpu], &none, mrs(CNTPCT_EL0), false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/*
 * The guest on this core executed WFI or WFIT and that trapped (ESR_ELx.EC 0x01, iss is its
 * ISS). Called with the hypervisor lock held, like the other handlers of hv_exc_sync();
 * returns with it held. Returns true if the guest is to go on behind the instruction.
 *
 * To the guest this is a WFI that completed, which a WFI may do at any time and for any reason
 * (D1.7.2). It can only be wrong by waiting when something the guest has to wake up for is
 * there, so everything that is, or that may be, ends the wait.
 */
bool hv_handle_wfx(u64 iss)
{
    int cpu = mrs(TPIDR_EL2);
    struct hv_wfi_pcpu *p = &wfi_pcpu[cpu];
    u64 ti = FIELD_GET(ESR_ISS_WFx_TI, iss);
    u64 start, end, ticks, sent;

    /* HCR_EL2.TWE is 0: WFE and WFET of the guest do not trap */
    if (ti == ESR_ISS_WFx_TI_WFE || ti == ESR_ISS_WFx_TI_WFET)
        return false;

    p->traps++;

    /*
     * WFIT also ends when its timeout does, and nothing here keeps track of that. Not waiting
     * at all is allowed: "The PE is permitted to treat WFI as a NOP" (D1.7.2).
     */
    if (ti == ESR_ISS_WFx_TI_WFIT) {
        p->wfit++;
        return true;
    }

    /*
     * Bring what the guest can see up to date, as on every return to it: its timers and the
     * SGIs and SPIs for this core go into list registers, the virtual FIQ line is set. A
     * virtual interrupt is a WFI wakeup event whether or not the guest has it masked.
     */
    hv_update_fiq();
    sysop("isb");

    if (hv_vgic_irq_waiting() || (mrs(HCR_EL2) & HCR_VF)) {
        p->immediate++;
        return true;
    }

    /*
     * From here on, whatever else the guest gets starts as a physical interrupt of this core:
     * its timers and the hypervisor's (FIQ), the IPI another core sends after it has made an
     * SGI or an SPI pending for this one or changed what is enabled (FIQ), AIC and the
     * maintenance interrupt (IRQ). A pending SError has to be taken as well.
     *
     * The core is not in the guest (hv_exc_entry()) and must not keep the others out of the
     * hypervisor while it waits.
     *
     * SErrors are masked for the wait, like IRQs and FIQs. One that the hypervisor takes in
     * EL2 ends in m1n1's own handler, which reboots the machine; one that is pending when the
     * core is back in the guest goes to hv_exc_serr() and the host, as it did when the guest
     * spent its idle time in WFI itself.
     */
    spin_unlock(&bhl);
    sysop("msr daifset, 4");

    p->waits++;
    __atomic_store_n(&wfi_ipi_sent[cpu], 0, __ATOMIC_SEQ_CST);
    start = mrs(CNTPCT_EL0);
    while (!(mrs(ISR_EL1) & (ISR_A | ISR_I | ISR_F))) {
        sysop("wfe");
        p->wfes++;
    }
    end = mrs(CNTPCT_EL0);
    ticks = end - start;
    p->ticks += ticks;
    if (ticks > p->longest)
        p->longest = ticks;

    sent = __atomic_exchange_n(&wfi_ipi_sent[cpu], 0, __ATOMIC_SEQ_CST);
    if (sent) {
        ticks = end > sent ? end - sent : 0;
        p->ipi_waits++;
        p->ipi_ticks += ticks;
        if (ticks > p->ipi_late)
            p->ipi_late = ticks;
    }

    /*
     * As in hv_exc_entry(): SErrors back on in the hypervisor, unless one is pending, once the
     * lock is held
     */
    hv_lock();
    if (!(mrs(ISR_EL1) & ISR_A))
        sysop("msr daifclr, 4");
    return true;
}

/* What each core did, for the log. Called with the hypervisor lock held. */
void hv_wfi_report(void)
{
    u64 freq = mrs(CNTFRQ_EL0);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct hv_wfi_pcpu *p = &wfi_pcpu[cpu];

        if (!p->traps)
            continue;

        printf("HV: WFI: CPU %d: %ld trapped (%ld WFIT), %ld returned at once, %ld waited: %ld "
               "WFEs, %ld ms in all, longest %ld us; %ld waits got an IPI of the vGIC and ended "
               "%ld us after it on average, %ld us at most\n",
               cpu, p->traps, p->wfit, p->immediate, p->waits, p->wfes, p->ticks * 1000 / freq,
               p->longest * 1000000 / freq, p->ipi_waits,
               p->ipi_waits ? p->ipi_ticks * 1000000 / freq / p->ipi_waits : 0,
               p->ipi_late * 1000000 / freq);
    }
}
