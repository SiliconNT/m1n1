/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Interrupt delivery to a guest that expects a GICv3.
 *
 * Apple cores implement the GICv3 virtual CPU interface in hardware (list registers and
 * the ICV_* registers behind ICC_* at EL1), although the interrupt controller of the SoC
 * is AIC. With HCR_EL2.IMO set, such a guest acknowledges and ends interrupts without
 * leaving EL1; the hypervisor only has to put the interrupt into a list register.
 *
 * The approach (write the interrupt into a list register, learn about the end of interrupt
 * from the maintenance interrupt) follows src/hv_exc.c of NT-for-ASi/m1n1_windows at commit
 * 6dcca4760cd5. The code is new: the number of list registers is read from ICH_VTR_EL2, an
 * interrupt is never put into two list registers at once, and everything is Group 1.
 *
 * SGIs: the guest sends one by writing ICC_SGI1R_EL1, which always traps. The sending core
 * sets a bit in the pending mask of every target core and, unless the target is itself,
 * calls that core into EL2 with one of m1n1's own IPIs. Every core moves its pending SGIs
 * into list registers on its way back to the guest. The IPI carries no information, so it
 * does not matter that the hypervisor uses the same IPI to gather the cores (hv_rendezvous).
 *
 * Register layouts: Arm IHI 0069H.b, sections 12.2.22 and 12.4.
 */

#include "hv.h"
#include "aic.h"
#include "aic_regs.h"
#include "cpu_regs.h"
#include "smp.h"
#include "utils.h"

/* IRQs that arrive with neither an AIC event nor a maintenance interrupt, before giving up */
#define VGIC_SPURIOUS_LIMIT 16
/* AIC events without a GIC interrupt, before giving up: more than there are AIC lines */
#define VGIC_UNMAPPED_LIMIT (2 * AIC_MAX_HW_NUM)

#define VGIC_NR_SGIS 16

/* Aff3, Aff2, Aff1 and Aff0 of MPIDR_EL1 */
#define VGIC_AFFINITY      0xff00ffffffUL
#define VGIC_AFFINITY_AFF0 0xffUL

/*
 * Everything is only touched by the core it belongs to, except:
 *  - sgi_pending: other cores set bits. All accesses are atomic.
 *  - affinity: written once by its core in hv_vgic_init(), before the core is marked as
 *    started in hv_started_cpus[]; read by any core after that.
 */
struct hv_vgic_pcpu {
    u64 injected;
    u64 maintenance;
    u64 unmapped;
    u64 spurious;
    u64 sgi_writes;   // writes to ICC_SGI1R_EL1 by the guest on this core
    u64 sgi_deferred; // returns to the guest with SGIs left waiting for a list register
    u64 affinity;     // what the guest on this core reads from MPIDR_EL1, affinity fields only
    u32 sgi_pending;  // SGIs 0-15 for the guest on this core that are not in a list register yet
} ALIGNED(64);

static struct hv_vgic_pcpu vgic_pcpu[MAX_CPUS];

extern bool hv_started_cpus[MAX_CPUS];

#define VGIC_PERCPU(x) vgic_pcpu[mrs(TPIDR_EL2)].x

/* Log the 1st, 2nd, 4th, 8th... occurrence, so that a storm cannot flood the console */
static bool vgic_should_log(u64 count)
{
    return (count & (count - 1)) == 0;
}

static u64 vgic_read_lr(int n)
{
    switch (n) {
        case 0:
            return mrs(SYS_ICH_LR0_EL2);
        case 1:
            return mrs(SYS_ICH_LR1_EL2);
        case 2:
            return mrs(SYS_ICH_LR2_EL2);
        case 3:
            return mrs(SYS_ICH_LR3_EL2);
        case 4:
            return mrs(SYS_ICH_LR4_EL2);
        case 5:
            return mrs(SYS_ICH_LR5_EL2);
        case 6:
            return mrs(SYS_ICH_LR6_EL2);
        case 7:
            return mrs(SYS_ICH_LR7_EL2);
        case 8:
            return mrs(SYS_ICH_LR8_EL2);
        case 9:
            return mrs(SYS_ICH_LR9_EL2);
        case 10:
            return mrs(SYS_ICH_LR10_EL2);
        case 11:
            return mrs(SYS_ICH_LR11_EL2);
        case 12:
            return mrs(SYS_ICH_LR12_EL2);
        case 13:
            return mrs(SYS_ICH_LR13_EL2);
        case 14:
            return mrs(SYS_ICH_LR14_EL2);
        case 15:
            return mrs(SYS_ICH_LR15_EL2);
    }
    return 0;
}

static void vgic_write_lr(int n, u64 val)
{
    switch (n) {
        case 0:
            msr(SYS_ICH_LR0_EL2, val);
            break;
        case 1:
            msr(SYS_ICH_LR1_EL2, val);
            break;
        case 2:
            msr(SYS_ICH_LR2_EL2, val);
            break;
        case 3:
            msr(SYS_ICH_LR3_EL2, val);
            break;
        case 4:
            msr(SYS_ICH_LR4_EL2, val);
            break;
        case 5:
            msr(SYS_ICH_LR5_EL2, val);
            break;
        case 6:
            msr(SYS_ICH_LR6_EL2, val);
            break;
        case 7:
            msr(SYS_ICH_LR7_EL2, val);
            break;
        case 8:
            msr(SYS_ICH_LR8_EL2, val);
            break;
        case 9:
            msr(SYS_ICH_LR9_EL2, val);
            break;
        case 10:
            msr(SYS_ICH_LR10_EL2, val);
            break;
        case 11:
            msr(SYS_ICH_LR11_EL2, val);
            break;
        case 12:
            msr(SYS_ICH_LR12_EL2, val);
            break;
        case 13:
            msr(SYS_ICH_LR13_EL2, val);
            break;
        case 14:
            msr(SYS_ICH_LR14_EL2, val);
            break;
        case 15:
            msr(SYS_ICH_LR15_EL2, val);
            break;
    }
}

/* The number of list registers is a property of the core this runs on */
static int vgic_nr_lr(void)
{
    return FIELD_GET(ICH_VTR_LISTREGS, mrs(SYS_ICH_VTR_EL2)) + 1;
}

/* Bring up the virtual CPU interface of the core this runs on, with nothing pending */
void hv_vgic_init(void)
{
    u64 sre = mrs(SYS_ICC_SRE_EL2);

    /* hv_start() and hv_init_secondary() set VMPIDR_EL2 to the MPIDR_EL1 of the core */
    VGIC_PERCPU(affinity) = mrs(MPIDR_EL1) & VGIC_AFFINITY;
    __atomic_store_n(&VGIC_PERCPU(sgi_pending), 0, __ATOMIC_RELEASE);

    /* ICH_* are only accessible, and ICC_SRE_EL1 only usable by the guest, with these set */
    if ((sre & (ICC_SRE_ENABLE | ICC_SRE_SRE)) != (ICC_SRE_ENABLE | ICC_SRE_SRE)) {
        msr(SYS_ICC_SRE_EL2, sre | ICC_SRE_ENABLE | ICC_SRE_SRE);
        sysop("isb");
    }

    u64 vtr = mrs(SYS_ICH_VTR_EL2);
    int nr_lr = FIELD_GET(ICH_VTR_LISTREGS, vtr) + 1;
    int pre_bits = FIELD_GET(ICH_VTR_PREBITS, vtr) + 1;

    msr(SYS_ICH_HCR_EL2, 0);
    msr(SYS_ICH_VMCR_EL2, 0);

    /* AP0R<n> must be written before AP1R<n> (IHI 0069H.b 6.3.2) */
    if (pre_bits >= 7) {
        msr(SYS_ICH_AP0R3_EL2, 0);
        msr(SYS_ICH_AP0R2_EL2, 0);
    }
    if (pre_bits >= 6)
        msr(SYS_ICH_AP0R1_EL2, 0);
    msr(SYS_ICH_AP0R0_EL2, 0);

    if (pre_bits >= 7) {
        msr(SYS_ICH_AP1R3_EL2, 0);
        msr(SYS_ICH_AP1R2_EL2, 0);
    }
    if (pre_bits >= 6)
        msr(SYS_ICH_AP1R1_EL2, 0);
    msr(SYS_ICH_AP1R0_EL2, 0);

    for (int i = 0; i < nr_lr; i++)
        vgic_write_lr(i, 0);

    /*
     * Only En. None of the maintenance interrupt enables are set: the only maintenance
     * interrupt in use is the per-list-register EOI one (ICH_LR<n>_EL2.EOI).
     */
    msr(SYS_ICH_HCR_EL2, ICH_HCR_EN);
    sysop("isb");

    printf("HV: vGIC: CPU %d: %d list registers, ICH_VTR_EL2=0x%lx, ICC_SRE_EL2 was 0x%lx\n",
           smp_id(), nr_lr, vtr, sre);
}

/*
 * Make a Group 1 interrupt pending for the guest on this core. Returns true if the interrupt
 * is in a list register afterwards, including when it already was pending or active there
 * (two valid list registers with the same INTID are UNPREDICTABLE, IHI 0069H.b 12.4.6).
 * Returns false if there is no free list register; the caller has to try again later.
 *
 * edge is for interrupts that are events rather than levels, such as SGIs: one that arrives
 * while the guest is still handling the previous one must be taken again after that has
 * ended, so an active list register becomes active and pending. A level-sensitive source is
 * simply looked at again by its owner once the interrupt has ended.
 */
static bool vgic_inject(u32 vintid, u8 priority, bool edge)
{
    int nr_lr = vgic_nr_lr();
    int free = -1;

    /* 1020-1023 are special, and this virtual CPU interface has no extended INTID range */
    if (vintid >= 1020)
        return false;

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);
        u64 state = FIELD_GET(ICH_LR_STATE, lr);

        if (state) {
            if (FIELD_GET(ICH_LR_VINTID, lr) == vintid) {
                if (edge && state == ICH_LR_STATE_ACTIVE)
                    vgic_write_lr(i, lr | FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_PENDING));
                return true;
            }
        } else if (!(lr & ICH_LR_EOI) && free < 0) {
            /* An invalid entry that still has EOI set is waiting for hv_vgic_handle_irq() */
            free = i;
        }
    }

    if (free < 0)
        return false;

    vgic_write_lr(free, FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_PENDING) | ICH_LR_GROUP1 |
                            FIELD_PREP(ICH_LR_PRIORITY, priority) | ICH_LR_EOI |
                            FIELD_PREP(ICH_LR_VINTID, vintid));
    VGIC_PERCPU(injected)++;

    return true;
}

/* A level-sensitive interrupt: call again for as long as the source is high */
bool hv_vgic_inject(u32 vintid, u8 priority)
{
    return vgic_inject(vintid, priority, false);
}

/*
 * The guest on this core wrote val to ICC_SGI1R_EL1 (IHI 0069H.b 12.2.22). Runs without the
 * hypervisor lock: the only state shared with other cores is their sgi_pending.
 *
 * Whether the SGI is enabled on a target, its group there (which decides if a write to this
 * register signals it at all, table 12-14) and its priority belong to the Redistributor of
 * the target, which is not emulated yet. Until then every SGI is delivered, as Group 1 and
 * with the default priority.
 */
void hv_vgic_send_sgi(u64 val)
{
    u32 intid = FIELD_GET(ICC_SGI1R_INTID, val);
    u32 list = FIELD_GET(ICC_SGI1R_TARGETLIST, val);
    u32 rs = FIELD_GET(ICC_SGI1R_RS, val);
    /* Aff3.Aff2.Aff1 of the targets, at their positions in MPIDR_EL1 */
    u64 cluster = (FIELD_GET(ICC_SGI1R_AFF3, val) << 32) | (FIELD_GET(ICC_SGI1R_AFF2, val) << 16) |
                  (FIELD_GET(ICC_SGI1R_AFF1, val) << 8);
    int self = smp_id();

    VGIC_PERCPU(sgi_writes)++;

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        /* The PEs of the guest's system are the cores it has been entered on */
        if (!hv_started_cpus[cpu])
            continue;

        if (val & ICC_SGI1R_IRM) {
            /* All PEs except the sender; the affinity and target list fields are RES0 */
            if (cpu == self)
                continue;
        } else {
            u64 affinity = vgic_pcpu[cpu].affinity;
            u32 aff0 = affinity & VGIC_AFFINITY_AFF0;

            if ((affinity & ~VGIC_AFFINITY_AFF0) != cluster)
                continue;
            /*
             * TargetList[n] stands for Aff0 == RS * 16 + n. The virtual CPU interface of the
             * M4 tells the guest that there is no range selector (ICC_CTLR_EL1.RSS reads 0),
             * so the guest has to write RS as 0. Should it not, no core matches: a target
             * list bit that does not correspond to a PE is ignored.
             */
            if (aff0 / 16 != rs || !(list & BIT(aff0 % 16)))
                continue;
        }

        /* A set bit stays set if the same SGI is sent again: there is one pending state */
        __atomic_or_fetch(&vgic_pcpu[cpu].sgi_pending, BIT(intid), __ATOMIC_RELEASE);

        /*
         * This core picks its own up in hv_vgic_deliver_sgis() before it returns to the
         * guest. Another core has to be told. The bit is set, and made visible to the other
         * cores, before the IPI is sent, and the target acknowledges the IPI before it looks
         * at the bits, so none is missed.
         */
        if (cpu != self) {
            sysop("dsb ishst");
            smp_send_ipi(cpu);
        }
    }
}

/*
 * Move the SGIs that are pending for the guest on this core into list registers. Called on
 * every return to the guest.
 *
 * Those that do not fit stay pending. There is a next time: no list register being free means
 * that each of them either holds an interrupt, whose end raises a maintenance interrupt
 * (ICH_LR<n>_EL2.EOI is always set), or already waits for hv_vgic_handle_irq().
 */
void hv_vgic_deliver_sgis(void)
{
    u32 *pending = &VGIC_PERCPU(sgi_pending);

    if (!__atomic_load_n(pending, __ATOMIC_ACQUIRE))
        return;

    u32 sgis = __atomic_exchange_n(pending, 0, __ATOMIC_ACQ_REL);
    u32 left = 0;

    for (u32 intid = 0; intid < VGIC_NR_SGIS; intid++)
        if ((sgis & BIT(intid)) && !vgic_inject(intid, HV_VGIC_PRIO_DEFAULT, true))
            left |= BIT(intid);

    if (left) {
        __atomic_or_fetch(pending, left, __ATOMIC_RELEASE);
        VGIC_PERCPU(sgi_deferred)++;
    }
}

/* What each core the guest ran on did, for the log. Called with the hypervisor lock held. */
void hv_vgic_report(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct hv_vgic_pcpu *p = &vgic_pcpu[cpu];

        if (!p->injected && !p->sgi_writes && !p->maintenance)
            continue;

        printf("HV: vGIC: CPU %d: %ld ICC_SGI1R_EL1 writes, %ld injected, %ld maintenance IRQs, "
               "%ld returns with SGIs waiting, SGIs still pending 0x%x\n",
               cpu, p->sgi_writes, p->injected, p->maintenance, p->sgi_deferred,
               __atomic_load_n(&p->sgi_pending, __ATOMIC_ACQUIRE));
    }
}

/*
 * The source of a level-sensitive interrupt went low: take the interrupt back if the guest
 * has not acknowledged it yet. An active interrupt stays until the guest ends it.
 */
void hv_vgic_retract(u32 vintid)
{
    int nr_lr = vgic_nr_lr();

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);

        if (FIELD_GET(ICH_LR_STATE, lr) == ICH_LR_STATE_PENDING &&
            FIELD_GET(ICH_LR_VINTID, lr) == vintid)
            vgic_write_lr(i, 0);
    }
}

/*
 * A physical IRQ arrived while the guest was running. Returns false if it could not be
 * accounted for, in which case the caller should stop and tell the host.
 *
 * On Apple SoCs the vGIC maintenance interrupt is an IRQ that does not go through AIC and
 * cannot be masked there (Linux drivers/irqchip/irq-apple-aic.c, aic_handle_irq()), so it
 * shows up as an IRQ without an AIC event.
 */
bool hv_vgic_handle_irq(void)
{
    bool handled = false;

    if (aic) {
        /* Reading the event register acknowledges the line and masks it in AIC */
        u32 event = aic_ack();

        if (FIELD_GET(AIC_EVENT_TYPE, event)) {
            /*
             * No AIC line is forwarded to the guest yet, so every line stays masked once it
             * has fired. If they keep coming anyway, the masking did not work: stop.
             */
            handled = true;
            if (vgic_should_log(++VGIC_PERCPU(unmapped)))
                printf("HV: vGIC: CPU %d: AIC event 0x%x has no GIC interrupt, left masked (#%ld)\n",
                       smp_id(), event, VGIC_PERCPU(unmapped));
            if (VGIC_PERCPU(unmapped) >= VGIC_UNMAPPED_LIMIT)
                return false;
        }
    }

    u64 misr = mrs(SYS_ICH_MISR_EL2);
    if (misr) {
        int nr_lr = vgic_nr_lr();
        u64 eisr = mrs(SYS_ICH_EISR_EL2);

        handled = true;

        /* The guest ended these interrupts. Emptying the list register drops the request. */
        for (int i = 0; i < nr_lr; i++)
            if (eisr & BIT(i))
                vgic_write_lr(i, 0);
        sysop("isb");

        if (vgic_should_log(++VGIC_PERCPU(maintenance)))
            printf("HV: vGIC: CPU %d: maintenance IRQ #%ld, MISR=0x%lx EISR=0x%lx, %ld injected\n",
                   smp_id(), VGIC_PERCPU(maintenance), misr, eisr, VGIC_PERCPU(injected));

        u64 left = mrs(SYS_ICH_MISR_EL2);
        if (left) {
            /* Not something this code enabled. Turn the interface off rather than loop. */
            msr(SYS_ICH_HCR_EL2, 0);
            sysop("isb");
            printf("HV: vGIC: CPU %d: MISR still 0x%lx, virtual CPU interface disabled\n", smp_id(),
                   left);
            return false;
        }
    }

    if (!handled) {
        u64 count = ++VGIC_PERCPU(spurious);

        printf("HV: vGIC: CPU %d: IRQ without AIC event or maintenance interrupt (#%ld)\n",
               smp_id(), count);
        if (count >= VGIC_SPURIOUS_LIMIT)
            return false;
    }

    return true;
}
