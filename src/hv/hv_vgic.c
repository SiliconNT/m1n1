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
 * Register layouts: Arm IHI 0069H.b, section 12.4.
 */

#include "hv.h"
#include "aic.h"
#include "aic_regs.h"
#include "cpu_regs.h"
#include "smp.h"
#include "utils.h"

/* IRQs that arrive with neither an AIC event nor a maintenance interrupt, before giving up */
#define VGIC_SPURIOUS_LIMIT 16

struct hv_vgic_pcpu {
    u64 injected;
    u64 maintenance;
    u64 unmapped;
    u64 spurious;
} ALIGNED(64);

static struct hv_vgic_pcpu vgic_pcpu[MAX_CPUS];

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

    printf("HV: vGIC: CPU %d: %d list registers, ICH_VTR_EL2=0x%lx\n", smp_id(), nr_lr, vtr);
}

/*
 * Make a Group 1 interrupt pending for the guest on this core. Returns true if the interrupt
 * is in a list register afterwards, including when it already was pending or active there
 * (two valid list registers with the same INTID are UNPREDICTABLE, IHI 0069H.b 12.4.6).
 * Returns false if there is no free list register; the caller has to try again later.
 */
bool hv_vgic_inject(u32 vintid, u8 priority)
{
    int nr_lr = vgic_nr_lr();
    int free = -1;

    /* 1020-1023 are special, and this virtual CPU interface has no extended INTID range */
    if (vintid >= 1020)
        return false;

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);

        if (FIELD_GET(ICH_LR_STATE, lr)) {
            if (FIELD_GET(ICH_LR_VINTID, lr) == vintid)
                return true;
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
             * has fired.
             */
            handled = true;
            if (vgic_should_log(++VGIC_PERCPU(unmapped)))
                printf("HV: vGIC: CPU %d: AIC event 0x%x has no GIC interrupt, left masked (#%ld)\n",
                       smp_id(), event, VGIC_PERCPU(unmapped));
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
