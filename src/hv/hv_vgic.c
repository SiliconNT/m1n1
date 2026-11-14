/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A GICv3 for a guest, on a machine whose interrupt controller is AIC.
 *
 * Apple cores implement the GICv3 virtual CPU interface in hardware (list registers and
 * the ICV_* registers behind ICC_* at EL1). With HCR_EL2.IMO set, such a guest acknowledges
 * and ends interrupts without leaving EL1; the hypervisor only has to put the interrupt into a
 * list register.
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
 * The Distributor and the Redistributors are memory the guest reads and writes; there is
 * nothing at those addresses, so every access traps and is answered here (hv_map_hook). What
 * is emulated is what the Windows kernel uses: which interrupts are enabled, their priority,
 * their trigger, where an SPI goes and whether it is pending. An interrupt only goes into a
 * list register while the Distributor and the interrupt itself are enabled; until then it
 * stays pending here. Whoever changes that for another core calls it into EL2 with the same
 * IPI, and the core looks again at what it can deliver.
 *
 * Register layouts: Arm IHI 0069H.b, sections 12.2.22, 12.4, 12.9 and 12.11.
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

#define VGIC_NR_SGIS   16
#define VGIC_NR_LOCAL  32   // SGIs and PPIs: every core has its own
#define VGIC_NR_INTIDS 1020 // 1020-1023 are not interrupts
#define VGIC_NR_WORDS  32   // 32 interrupts per word of the enable and pending registers

/* Aff3, Aff2, Aff1 and Aff0 of MPIDR_EL1 */
#define VGIC_AFFINITY      0xff00ffffffUL
#define VGIC_AFFINITY_AFF0 0xffUL

/* Distributor, offsets from HV_VGIC_DIST_BASE */
#define GICD_CTLR             0x0000
#define GICD_CTLR_ENABLE_GRP0 BIT(0)
#define GICD_CTLR_ENABLE_GRP1 BIT(1)
#define GICD_CTLR_ARE         BIT(4)
#define GICD_CTLR_DS          BIT(6)
#define GICD_TYPER            0x0004
#define GICD_IGROUPR          0x0080
#define GICD_ISENABLER        0x0100
#define GICD_ICENABLER        0x0180
#define GICD_ISPENDR          0x0200
#define GICD_ICPENDR          0x0280
#define GICD_IPRIORITYR       0x0400
#define GICD_ICFGR            0x0c00
#define GICD_IROUTER          0x6000
#define GICD_IROUTER_IRM      BIT(31)
#define GICD_IROUTER_AFFINITY 0xffffffUL // Aff2.Aff1.Aff0; Aff3 is in the upper half
#define GICD_PIDR2            0xffe8

/*
 * 1020 INTIDs (ITLinesNumber 31), 16 bits of INTID (IDbits 15), one Security state, and none
 * of LPIs, extended SPIs, message-based SPIs, NMIs, Aff3, range selector for SGIs.
 */
#define GICD_TYPER_VALUE (FIELD_PREP(GENMASK(23, 19), 15) | FIELD_PREP(GENMASK(4, 0), 31))
/* ArchRev 3; the rest of the register is IMPLEMENTATION DEFINED */
#define GIC_PIDR2_VALUE 0x30

/* Redistributor of one core: RD_base, and SGI_base 64K behind it */
#define GICR_CTLR       0x0000
#define GICR_TYPER      0x0008
#define GICR_TYPER_LAST BIT(4)
#define GICR_WAKER      0x0014
#define GICR_PIDR2      0xffe8
#define GICR_SGI_BASE   0x10000
#define GICR_IGROUPR0   (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0 (GICR_SGI_BASE + 0x0180)
#define GICR_ISPENDR0   (GICR_SGI_BASE + 0x0200)
#define GICR_ICPENDR0   (GICR_SGI_BASE + 0x0280)
#define GICR_IPRIORITYR (GICR_SGI_BASE + 0x0400)
#define GICR_ICFGR0     (GICR_SGI_BASE + 0x0c00)
#define GICR_ICFGR1     (GICR_SGI_BASE + 0x0c04)

/* Int_config: bit 1 of each field is the trigger (1 = edge), bit 0 is RES0 */
#define GIC_ICFGR_EDGE 0xaaaaaaaaU

/*
 * The registers of the Distributor and of the Redistributors are written by the core that
 * traps, with the big hypervisor lock held (hv_exc_sync), and read by any core on its way
 * back to the guest, where that lock is not always held. So they are single words or bytes
 * that are loaded and stored whole. Sequentially consistent, like every other atomic access
 * below: one core stores A and then loads B while another stores B and then loads A (an
 * interrupt is enabled here while it is made pending there), and at least one of them has to
 * see what the other did.
 */
#define vgic_load(x)     __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define vgic_store(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

/*
 * Everything is only touched by the core it belongs to, except:
 *  - sgi_pending: other cores set bits. All accesses are atomic.
 *  - kick: other cores set it. All accesses are atomic.
 *  - enable, cfg1, priority: see vgic_load() above.
 *  - ppi_level: written by its core, read by any.
 *  - present, affinity: written by the boot core in vgic_init_global(), before the guest runs
 *    anywhere; a core writes its affinity once more, with the same value, in hv_vgic_init().
 */
struct hv_vgic_pcpu {
    u64 injected;
    u64 maintenance;
    u64 unmapped;
    u64 spurious;
    u64 retracted;    // interrupts taken back out of a list register
    u64 sgi_writes;   // writes to ICC_SGI1R_EL1 by the guest on this core
    u64 sgi_deferred; // returns to the guest with SGIs left waiting for a list register
    u64 spi_deferred; // ...and with SPIs left waiting
    u64 affinity;     // what the guest on this core reads from MPIDR_EL1, affinity fields only
    u32 sgi_pending;  // SGIs 0-15 for the guest on this core that are not in a list register
    u32 ppi_level;    // PPIs whose source is high
    u32 kick;         // what this core may deliver has changed: look again
    u32 enable;       // GICR_ISENABLER0
    u32 cfg1;         // GICR_ICFGR1
    u8 priority[VGIC_NR_LOCAL];
    bool present;  // the core runs m1n1: its Redistributor exists
    bool spi_left; // SPIs for this core are waiting for a list register
} ALIGNED(64);

static struct hv_vgic_pcpu vgic_pcpu[MAX_CPUS];

/* SPIs. The first word of each array, and the first 32 bytes, belong to SGIs and PPIs. */
static struct {
    u32 ctlr;                   // GICD_CTLR: the group enables
    u32 enable[VGIC_NR_WORDS];  // GICD_ISENABLER<n>
    u32 pending[VGIC_NR_WORDS]; // pending and in no list register. Atomic: any core, no lock.
    u32 cfg[2 * VGIC_NR_WORDS]; // GICD_ICFGR<n>
    u32 router[32 * VGIC_NR_WORDS]; // GICD_IROUTER<n>, the lower half; the upper half is RES0
    u8 priority[32 * VGIC_NR_WORDS];
} vgic_dist;

/* Offsets the guest wrote that are not emulated, each reported once */
#define VGIC_UNHANDLED_MAX 16
static u32 vgic_unhandled[VGIC_UNHANDLED_MAX];
static int vgic_nr_unhandled;
static u64 vgic_unhandled_writes;
static u64 vgic_unhandled_reads;
static bool vgic_group0_reported;

static int vgic_nr_frames; // Redistributors: one per core index up to the last that runs m1n1
static int vgic_boot_cpu;

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

static bool vgic_dist_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
static bool vgic_redist_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

/* The state a guest finds, and the two ranges of guest memory. On the boot core, once. */
static void vgic_init_global(void)
{
    int self = smp_id();

    vgic_boot_cpu = self;
    vgic_nr_frames = 0;
    vgic_nr_unhandled = 0;
    vgic_unhandled_writes = 0;
    vgic_unhandled_reads = 0;
    vgic_group0_reported = false;

    vgic_store(vgic_dist.ctlr, 0);
    for (int i = 0; i < VGIC_NR_WORDS; i++) {
        vgic_store(vgic_dist.enable[i], 0);
        __atomic_store_n(&vgic_dist.pending[i], 0, __ATOMIC_SEQ_CST);
    }
    for (int i = 0; i < 2 * VGIC_NR_WORDS; i++)
        vgic_store(vgic_dist.cfg[i], 0);
    for (int i = 0; i < 32 * VGIC_NR_WORDS; i++) {
        vgic_store(vgic_dist.router[i], 0);
        vgic_store(vgic_dist.priority[i], 0);
    }

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct hv_vgic_pcpu *p = &vgic_pcpu[cpu];

        /*
         * The guest can only ever be entered on a core that runs m1n1 by now; those are the
         * PEs of its system. m1n1 knows the MPIDR_EL1 of such a core, without Aff3, which no
         * Apple core uses.
         */
        p->present = cpu == self || smp_is_alive(cpu);
        vgic_store(p->affinity, cpu == self ? mrs(MPIDR_EL1) & VGIC_AFFINITY
                                            : smp_get_mpidr(cpu) & VGIC_AFFINITY);
        if (p->present)
            vgic_nr_frames = cpu + 1;

        vgic_store(p->enable, 0);
        vgic_store(p->cfg1, 0);
        for (int i = 0; i < VGIC_NR_LOCAL; i++)
            vgic_store(p->priority[i], 0);
        vgic_store(p->ppi_level, 0);
        __atomic_store_n(&p->sgi_pending, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&p->kick, 0, __ATOMIC_SEQ_CST);
        p->spi_left = false;
        p->injected = p->maintenance = p->unmapped = p->spurious = p->retracted = 0;
        p->sgi_writes = p->sgi_deferred = p->spi_deferred = 0;
    }

    if (hv_map_hook(HV_VGIC_DIST_BASE, vgic_dist_hook, HV_VGIC_DIST_SIZE) < 0 ||
        hv_map_hook(HV_VGIC_REDIST_BASE, vgic_redist_hook, vgic_nr_frames * HV_VGIC_REDIST_SIZE) <
            0)
        printf("HV: vGIC: mapping the Distributor and the Redistributors failed\n");

    printf("HV: vGIC: Distributor at 0x%lx, %d Redistributors from 0x%lx\n", HV_VGIC_DIST_BASE,
           vgic_nr_frames, HV_VGIC_REDIST_BASE);
}

/* Bring up the virtual CPU interface of the core this runs on, with nothing pending */
void hv_vgic_init(void)
{
    u64 sre = mrs(SYS_ICC_SRE_EL2);

    /* hv_init() runs on the boot core, before the guest is entered anywhere */
    if (smp_id() == boot_cpu_idx)
        vgic_init_global();

    /*
     * The Redistributors were laid out when the hypervisor started. A core that only began to
     * run m1n1 after that has none, and the guest on it gets no SGI and no PPI.
     */
    if (!VGIC_PERCPU(present))
        printf("HV: vGIC: CPU %d was not running m1n1 at hv_init(): it has no Redistributor\n",
               smp_id());

    /* hv_start() and hv_init_secondary() set VMPIDR_EL2 to the MPIDR_EL1 of the core */
    vgic_store(VGIC_PERCPU(affinity), mrs(MPIDR_EL1) & VGIC_AFFINITY);
    __atomic_store_n(&VGIC_PERCPU(sgi_pending), 0, __ATOMIC_SEQ_CST);
    vgic_store(VGIC_PERCPU(ppi_level), 0);
    VGIC_PERCPU(spi_left) = false;
    /* There may be SPIs that have been waiting for this core */
    __atomic_store_n(&VGIC_PERCPU(kick), 1, __ATOMIC_SEQ_CST);

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
    if (vintid >= VGIC_NR_INTIDS)
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

/* Take an interrupt back if the guest has not acknowledged it yet. An active one stays. */
static void vgic_retract(u32 vintid)
{
    int nr_lr = vgic_nr_lr();

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);

        if (FIELD_GET(ICH_LR_STATE, lr) == ICH_LR_STATE_PENDING &&
            FIELD_GET(ICH_LR_VINTID, lr) == vintid) {
            vgic_write_lr(i, 0);
            VGIC_PERCPU(retracted)++;
        }
    }
}

/* The interrupts of one word of the pending registers that wait in a list register here */
static u32 vgic_lr_pending(u32 word)
{
    int nr_lr = vgic_nr_lr();
    u32 pending = 0;

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);
        u32 vintid = FIELD_GET(ICH_LR_VINTID, lr);

        if ((FIELD_GET(ICH_LR_STATE, lr) & ICH_LR_STATE_PENDING) && vintid / 32 == word)
            pending |= BIT(vintid % 32);
    }
    return pending;
}

/* Have a core look again at what it can deliver */
static void vgic_kick(int cpu)
{
    __atomic_store_n(&vgic_pcpu[cpu].kick, 1, __ATOMIC_SEQ_CST);

    /*
     * This core passes hv_vgic_deliver() before it returns to the guest. Another core has to
     * be told. The flag is set, and made visible to the other cores, before the IPI is sent,
     * and the target acknowledges the IPI before it looks at the flag, so nothing is missed.
     */
    if (cpu != smp_id() && hv_started_cpus[cpu]) {
        sysop("dsb ishst");
        smp_send_ipi(cpu);
    }
}

/* ...and every core the guest runs on, after a change in the Distributor */
static void vgic_kick_all(void)
{
    int self = smp_id();

    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (cpu == self || hv_started_cpus[cpu])
            vgic_kick(cpu);
}

static bool vgic_grp1_enabled(void)
{
    return vgic_load(vgic_dist.ctlr) & GICD_CTLR_ENABLE_GRP1;
}

/*
 * The core an SPI goes to, -1 if GICD_IROUTER names none. With Interrupt_Routing_Mode set
 * any core will do; the boot core is one the guest is always on.
 */
static int vgic_spi_target(u32 intid)
{
    u32 router = vgic_load(vgic_dist.router[intid]);

    if (router & GICD_IROUTER_IRM)
        return vgic_boot_cpu;

    for (int cpu = 0; cpu < vgic_nr_frames; cpu++)
        if (vgic_pcpu[cpu].present &&
            vgic_load(vgic_pcpu[cpu].affinity) == (router & GICD_IROUTER_AFFINITY))
            return cpu;

    return -1;
}

/* May this interrupt be in a list register of this core? */
static bool vgic_deliverable_here(u32 intid)
{
    if (!vgic_grp1_enabled())
        return false;
    if (intid < VGIC_NR_LOCAL)
        return vgic_load(VGIC_PERCPU(enable)) & BIT(intid);
    return (vgic_load(vgic_dist.enable[intid / 32]) & BIT(intid % 32)) &&
           vgic_spi_target(intid) == smp_id();
}

/*
 * Make an SPI pending. It reaches a list register of its target core in vgic_deliver_spis().
 * AIC lines that are forwarded to the guest will come in here as well.
 */
static void vgic_pend_spi(u32 intid, bool tell_self)
{
    int cpu;

    __atomic_or_fetch(&vgic_dist.pending[intid / 32], BIT(intid % 32), __ATOMIC_SEQ_CST);

    cpu = vgic_spi_target(intid);
    if (cpu < 0)
        return;
    if (cpu == smp_id() ? tell_self : hv_started_cpus[cpu])
        vgic_kick(cpu);
}

/*
 * Something changed that decides what this core may deliver. Whatever waits in a list
 * register and may not be there any more goes back to being pending in software: "If an
 * interrupt is pending within a CPU interface when the corresponding GICD_CTLR.EnableGrpX
 * bit is written from 1 to 0 the interrupt must be retrieved from the CPU interface"
 * (IHI 0069H.b 12.9.4), and likewise for the enable bit of one interrupt. An interrupt the
 * guest has acknowledged stays active until the guest ends it.
 */
static void vgic_resync(void)
{
    int nr_lr = vgic_nr_lr();

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);
        u64 state = FIELD_GET(ICH_LR_STATE, lr);
        u32 intid = FIELD_GET(ICH_LR_VINTID, lr);

        if (!(state & ICH_LR_STATE_PENDING) || vgic_deliverable_here(intid))
            continue;

        if (state == ICH_LR_STATE_PENDING)
            vgic_write_lr(i, 0);
        else
            vgic_write_lr(i, lr & ~FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_PENDING));
        VGIC_PERCPU(retracted)++;

        if (intid < VGIC_NR_SGIS)
            __atomic_or_fetch(&VGIC_PERCPU(sgi_pending), BIT(intid), __ATOMIC_SEQ_CST);
        else if (intid >= VGIC_NR_LOCAL)
            vgic_pend_spi(intid, false);
        /* A PPI is a level: its owner says so again in hv_vgic_ppi_level() */
    }
}

/*
 * The source of a PPI of this core is high or low. Called on every return to the guest for
 * as long as the source is high: a level-sensitive interrupt that has ended and is still
 * high is injected again, one that went low before the guest acknowledged it is taken back.
 */
void hv_vgic_ppi_level(u32 intid, bool high)
{
    u32 level = vgic_load(VGIC_PERCPU(ppi_level));

    if (high) {
        if (!(level & BIT(intid)))
            vgic_store(VGIC_PERCPU(ppi_level), level | BIT(intid));
        if (vgic_deliverable_here(intid)) {
            vgic_inject(intid, vgic_load(VGIC_PERCPU(priority[intid])), false);
            return;
        }
    } else if (level & BIT(intid)) {
        vgic_store(VGIC_PERCPU(ppi_level), level & ~BIT(intid));
    }

    vgic_retract(intid);
}

/*
 * The guest on this core wrote val to ICC_SGI1R_EL1 (IHI 0069H.b 12.2.22). Runs without the
 * hypervisor lock: the only state shared with other cores is their sgi_pending.
 *
 * The SGI becomes pending on its targets whether or not it is enabled there; the enable bit
 * of the target decides when it is delivered (vgic_deliver_sgis()).
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
            u64 affinity = vgic_load(vgic_pcpu[cpu].affinity);
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
        __atomic_or_fetch(&vgic_pcpu[cpu].sgi_pending, BIT(intid), __ATOMIC_SEQ_CST);

        /*
         * This core picks its own up in hv_vgic_deliver() before it returns to the guest.
         * Another core has to be told. The bit is set, and made visible to the other cores,
         * before the IPI is sent, and the target acknowledges the IPI before it looks at the
         * bits, so none is missed.
         */
        if (cpu != self) {
            sysop("dsb ishst");
            smp_send_ipi(cpu);
        }
    }
}

/*
 * Move the SGIs that are pending and enabled for the guest on this core into list registers.
 *
 * Those that do not fit stay pending. There is a next time: no list register being free means
 * that each of them either holds an interrupt, whose end raises a maintenance interrupt
 * (ICH_LR<n>_EL2.EOI is always set), or already waits for hv_vgic_handle_irq().
 */
static void vgic_deliver_sgis(void)
{
    u32 *pending = &VGIC_PERCPU(sgi_pending);
    u32 sgis = __atomic_load_n(pending, __ATOMIC_SEQ_CST);
    u32 left = 0;

    if (!sgis || !vgic_grp1_enabled())
        return;
    sgis &= vgic_load(VGIC_PERCPU(enable));
    if (!sgis)
        return;

    /* Take these; what another core adds in the meantime stays for the next time */
    sgis &= __atomic_fetch_and(pending, ~sgis, __ATOMIC_SEQ_CST);

    for (u32 intid = 0; intid < VGIC_NR_SGIS; intid++)
        if ((sgis & BIT(intid)) &&
            !vgic_inject(intid, vgic_load(VGIC_PERCPU(priority[intid])), true))
            left |= BIT(intid);

    if (left) {
        __atomic_or_fetch(pending, left, __ATOMIC_SEQ_CST);
        VGIC_PERCPU(sgi_deferred)++;
    }
}

/* The same for the SPIs that are routed to this core */
static void vgic_deliver_spis(void)
{
    int self = smp_id();
    bool left = false;

    if (vgic_grp1_enabled()) {
        for (u32 word = 1; word < VGIC_NR_WORDS; word++) {
            u32 ready = __atomic_load_n(&vgic_dist.pending[word], __ATOMIC_SEQ_CST) &
                        vgic_load(vgic_dist.enable[word]);

            while (ready) {
                u32 bit = __builtin_ctz(ready);
                u32 intid = 32 * word + bit;

                ready &= ready - 1;
                if (vgic_spi_target(intid) != self)
                    continue;
                /* Only one core gets it, should the route change under our feet */
                if (!(__atomic_fetch_and(&vgic_dist.pending[word], ~BIT(bit), __ATOMIC_SEQ_CST) &
                      BIT(bit)))
                    continue;
                if (!vgic_inject(intid, vgic_load(vgic_dist.priority[intid]), true)) {
                    /*
                     * Back to pending. It was in nobody's sight for a moment: if it has been
                     * rerouted since the check above, the new core may have looked for it
                     * already and has to be told. If not, this core tries again.
                     */
                    vgic_pend_spi(intid, false);
                    left = true;
                }
            }
        }
    }

    VGIC_PERCPU(spi_left) = left;
    if (left)
        VGIC_PERCPU(spi_deferred)++;
}

/*
 * Bring the list registers of this core up to date with what is pending and enabled for the
 * guest on it. Called on every return to the guest (hv_update_fiq()).
 */
void hv_vgic_deliver(void)
{
    u32 *kick = &VGIC_PERCPU(kick);
    bool scan = VGIC_PERCPU(spi_left);

    if (__atomic_load_n(kick, __ATOMIC_SEQ_CST) && __atomic_exchange_n(kick, 0, __ATOMIC_SEQ_CST)) {
        vgic_resync();
        scan = true;
    }

    vgic_deliver_sgis();
    if (scan)
        vgic_deliver_spis();
}

/*
 * Is there an interrupt for the guest on this core that it would wake up from WFI for? Called
 * after hv_vgic_deliver(), when the guest has executed WFI.
 *
 * Yes if a list register holds one that is pending, or if one that is pending and enabled did
 * not get a list register. That is more than a GIC would wake the guest up for (it would not
 * for a priority below ICC_PMR_EL1, nor with ICC_IGRPEN1_EL1 clear); the guest then simply
 * executes its WFI again. An interrupt that is pending and not enabled is no reason, as with a
 * GIC: whoever enables it calls this core into EL2 (vgic_kick()).
 */
bool hv_vgic_irq_waiting(void)
{
    int nr_lr = vgic_nr_lr();
    /* PPIs whose source is high: each has to be in a list register, or it is waiting for one */
    u32 local = vgic_load(VGIC_PERCPU(ppi_level));

    for (int i = 0; i < nr_lr; i++) {
        u64 lr = vgic_read_lr(i);
        u64 state = FIELD_GET(ICH_LR_STATE, lr);
        u32 intid = FIELD_GET(ICH_LR_VINTID, lr);

        if (state & ICH_LR_STATE_PENDING)
            return true;
        if (state && intid < VGIC_NR_LOCAL)
            local &= ~BIT(intid);
    }

    /* No list register was free, and only the guest can free one */
    if (VGIC_PERCPU(spi_left))
        return true;
    local |= __atomic_load_n(&VGIC_PERCPU(sgi_pending), __ATOMIC_SEQ_CST);
    if (vgic_grp1_enabled() && (local & vgic_load(VGIC_PERCPU(enable))))
        return true;

    /* Something changed since hv_vgic_deliver() looked */
    return __atomic_load_n(&VGIC_PERCPU(kick), __ATOMIC_SEQ_CST);
}

/* ---- The Distributor and the Redistributors as the guest sees them ---- */

/* The interrupts that exist in one word of the Distributor's bitmaps */
static u32 vgic_spi_mask(u32 word)
{
    if (word == 0)
        return 0; /* SGIs and PPIs are in the Redistributors when affinity routing is on */
    if (word == VGIC_NR_WORDS - 1)
        return MASK(VGIC_NR_INTIDS % 32);
    return 0xffffffff;
}

/* frame: -1 for the Distributor, else the core of the Redistributor */
static void vgic_report_unhandled(int frame, u32 off, bool write, int width)
{
    u32 key = ((frame < 0 ? 0xff : frame) << 24) | off;

    if (!write) {
        vgic_unhandled_reads++;
        return;
    }

    vgic_unhandled_writes++;
    for (int i = 0; i < vgic_nr_unhandled; i++)
        if (vgic_unhandled[i] == key)
            return;
    if (vgic_nr_unhandled == VGIC_UNHANDLED_MAX)
        return;
    vgic_unhandled[vgic_nr_unhandled++] = key;

    if (frame < 0)
        printf("HV: vGIC: CPU %d: %d byte write to GICD + 0x%x is ignored\n", smp_id(), 1 << width,
               off);
    else
        printf("HV: vGIC: CPU %d: %d byte write to GICR of CPU %d + 0x%x is ignored\n", smp_id(),
               1 << width, frame, off);
}

/* The guest asked for an interrupt in Group 0. There is only Group 1 here. */
static void vgic_report_group0(int frame, u32 off, u32 val)
{
    if (vgic_group0_reported)
        return;
    vgic_group0_reported = true;
    printf("HV: vGIC: CPU %d: IGROUPR at 0x%x of frame %d written with 0x%x; every interrupt "
           "stays in Group 1\n",
           smp_id(), off, frame, val);
}

static u32 vgic_dist_read(u32 off)
{
    u32 word = (off & 0x7f) / 4;
    u32 val = 0;

    switch (off) {
        case GICD_CTLR:
            /* One Security state, and no GICv2 compatibility: DS and ARE are RAO/WI */
            return vgic_load(vgic_dist.ctlr) | GICD_CTLR_ARE | GICD_CTLR_DS;
        case GICD_TYPER:
            return GICD_TYPER_VALUE;
        case GICD_PIDR2:
            return GIC_PIDR2_VALUE;
    }

    if (off >= GICD_IGROUPR && off < GICD_IGROUPR + 0x80)
        return vgic_spi_mask(word);

    /* The set and the clear register read the same */
    if (off >= GICD_ISENABLER && off < GICD_ICENABLER + 0x80)
        return vgic_load(vgic_dist.enable[word]) & vgic_spi_mask(word);

    if (off >= GICD_ISPENDR && off < GICD_ICPENDR + 0x80) {
        if (!word)
            return 0;
        /*
         * Pending here, or in a list register of this core. One that waits in a list register
         * of another core is not seen: only that core can read its list registers.
         */
        return __atomic_load_n(&vgic_dist.pending[word], __ATOMIC_SEQ_CST) | vgic_lr_pending(word);
    }

    if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + 32 * VGIC_NR_WORDS) {
        u32 intid = off - GICD_IPRIORITYR;

        for (int i = 0; i < 4; i++)
            if (intid + i >= VGIC_NR_LOCAL && intid + i < VGIC_NR_INTIDS)
                val |= (u32)vgic_load(vgic_dist.priority[intid + i]) << (8 * i);
        return val;
    }

    if (off >= GICD_ICFGR && off < GICD_ICFGR + 8 * VGIC_NR_WORDS) {
        word = (off - GICD_ICFGR) / 4;
        return word < 2 ? 0 : vgic_load(vgic_dist.cfg[word]);
    }

    if (off >= GICD_IROUTER && off < GICD_IROUTER + 8 * 32 * VGIC_NR_WORDS) {
        u32 intid = (off - GICD_IROUTER) / 8;

        if (intid < VGIC_NR_LOCAL || intid >= VGIC_NR_INTIDS || (off & 4))
            return 0;
        return vgic_load(vgic_dist.router[intid]);
    }

    vgic_report_unhandled(-1, off, false, 2);
    return 0;
}

static void vgic_dist_write_priority(u32 intid, u8 val)
{
    /* Used the next time the interrupt goes into a list register */
    if (intid >= VGIC_NR_LOCAL && intid < VGIC_NR_INTIDS)
        vgic_store(vgic_dist.priority[intid], val);
}

static void vgic_dist_write(u32 off, u32 val)
{
    u32 word = (off & 0x7f) / 4;
    u32 old;

    if (off == GICD_CTLR) {
        old = vgic_load(vgic_dist.ctlr);
        val &= GICD_CTLR_ENABLE_GRP0 | GICD_CTLR_ENABLE_GRP1;
        vgic_store(vgic_dist.ctlr, val);
        if (old != val)
            vgic_kick_all();
        return;
    }

    if (off == GICD_TYPER || off == GICD_PIDR2)
        return;

    if (off >= GICD_IGROUPR && off < GICD_IGROUPR + 0x80) {
        if (~val & vgic_spi_mask(word))
            vgic_report_group0(-1, off, val);
        return;
    }

    if (off >= GICD_ISENABLER && off < GICD_ISENABLER + 0x80) {
        old = vgic_load(vgic_dist.enable[word]);
        val &= vgic_spi_mask(word) & ~old;
        if (!val)
            return;
        vgic_store(vgic_dist.enable[word], old | val);
        /* Somebody may have to deliver one that has been waiting for this */
        vgic_kick_all();
        return;
    }

    if (off >= GICD_ICENABLER && off < GICD_ICENABLER + 0x80) {
        old = vgic_load(vgic_dist.enable[word]);
        val &= old;
        if (!val)
            return;
        vgic_store(vgic_dist.enable[word], old & ~val);
        /* Any core may have one of them in a list register */
        vgic_kick_all();
        return;
    }

    if (off >= GICD_ISPENDR && off < GICD_ISPENDR + 0x80) {
        val &= vgic_spi_mask(word);
        while (val) {
            u32 bit = __builtin_ctz(val);

            val &= val - 1;
            vgic_pend_spi(32 * word + bit, true);
        }
        return;
    }

    if (off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + 32 * VGIC_NR_WORDS) {
        for (int i = 0; i < 4; i++)
            vgic_dist_write_priority(off - GICD_IPRIORITYR + i, val >> (8 * i));
        return;
    }

    if (off >= GICD_ICFGR && off < GICD_ICFGR + 8 * VGIC_NR_WORDS) {
        word = (off - GICD_ICFGR) / 4;
        if (word < 2)
            return;
        if (word == 2 * VGIC_NR_WORDS - 1)
            val &= MASK(2 * (VGIC_NR_INTIDS % 16));
        vgic_store(vgic_dist.cfg[word], val & GIC_ICFGR_EDGE);
        return;
    }

    if (off >= GICD_IROUTER && off < GICD_IROUTER + 8 * 32 * VGIC_NR_WORDS) {
        u32 intid = (off - GICD_IROUTER) / 8;

        /* The upper half only has Aff3, and no core has one (GICD_TYPER.A3V is 0) */
        if (intid < VGIC_NR_LOCAL || intid >= VGIC_NR_INTIDS || (off & 4))
            return;
        old = vgic_load(vgic_dist.router[intid]);
        val &= GICD_IROUTER_IRM | GICD_IROUTER_AFFINITY;
        if (old == val)
            return;
        vgic_store(vgic_dist.router[intid], val);
        /* The old target may have it in a list register, the new one may have to take it */
        vgic_kick_all();
        return;
    }

    vgic_report_unhandled(-1, off, true, 2);
}

static u32 vgic_redist_read(int cpu, u32 off)
{
    struct hv_vgic_pcpu *p = &vgic_pcpu[cpu];
    u64 affinity = vgic_load(p->affinity);
    u32 val = 0;

    switch (off) {
        case GICR_TYPER:
            /* Processor_Number, and Last on the frame behind which there is none */
            return (cpu << 8) | (cpu == vgic_nr_frames - 1 ? GICR_TYPER_LAST : 0);
        case GICR_TYPER + 4:
            /* Affinity_Value: Aff3.Aff2.Aff1.Aff0 */
            return p->present ? ((affinity >> 32) << 24) | (affinity & 0xffffff) : 0;
        case GICR_PIDR2:
            return GIC_PIDR2_VALUE;
        case GICR_CTLR:
            /* No LPIs, and nothing is ever in progress */
            return 0;
        case GICR_WAKER:
            /* ProcessorSleep and ChildrenAsleep are 0: there is nothing to wake up */
            return 0;
    }

    /* A core that does not run m1n1 cannot run the guest; the frame keeps the others in place */
    if (!p->present)
        return 0;

    switch (off) {
        case GICR_IGROUPR0:
            return 0xffffffff;
        case GICR_ISENABLER0:
        case GICR_ICENABLER0:
            return vgic_load(p->enable);
        case GICR_ISPENDR0:
        case GICR_ICPENDR0:
            /* As for SPIs: the list registers of another core are not seen from here */
            val = __atomic_load_n(&p->sgi_pending, __ATOMIC_SEQ_CST) | vgic_load(p->ppi_level);
            if (cpu == smp_id())
                val |= vgic_lr_pending(0);
            return val;
        case GICR_ICFGR0:
            /* SGIs are edge-triggered */
            return GIC_ICFGR_EDGE;
        case GICR_ICFGR1:
            return vgic_load(p->cfg1);
    }

    if (off >= GICR_IPRIORITYR && off < GICR_IPRIORITYR + VGIC_NR_LOCAL) {
        for (int i = 0; i < 4; i++)
            val |= (u32)vgic_load(p->priority[off - GICR_IPRIORITYR + i]) << (8 * i);
        return val;
    }

    vgic_report_unhandled(cpu, off, false, 2);
    return 0;
}

static void vgic_redist_write(int cpu, u32 off, u32 val)
{
    struct hv_vgic_pcpu *p = &vgic_pcpu[cpu];
    u32 old;

    if (off == GICR_TYPER || off == GICR_TYPER + 4 || off == GICR_PIDR2)
        return;

    if (!p->present)
        return;

    switch (off) {
        case GICR_IGROUPR0:
            if (~val)
                vgic_report_group0(cpu, off, val);
            return;
        case GICR_ISENABLER0:
        case GICR_ICENABLER0:
            old = vgic_load(p->enable);
            val = off == GICR_ISENABLER0 ? old | val : old & ~val;
            if (old == val)
                return;
            vgic_store(p->enable, val);
            vgic_kick(cpu);
            return;
        case GICR_ICFGR0:
            return;
        case GICR_ICFGR1:
            vgic_store(p->cfg1, val & GIC_ICFGR_EDGE);
            return;
    }

    if (off >= GICR_IPRIORITYR && off < GICR_IPRIORITYR + VGIC_NR_LOCAL) {
        for (int i = 0; i < 4; i++)
            vgic_store(p->priority[off - GICR_IPRIORITYR + i], (u8)(val >> (8 * i)));
        return;
    }

    /* Among them GICR_WAKER and GICR_CTLR, and the set-pending register, which Windows only reads */
    vgic_report_unhandled(cpu, off, true, 2);
}

/*
 * One access of the guest. Registers are 32 bits wide, or made of two halves that can be
 * accessed on their own (GICD_IROUTER<n>, GICR_TYPER); only the priority registers can be
 * written by the byte (IHI 0069H.b 12.1.3). Never fails: an access that is not emulated reads
 * as zero or is ignored, like a reserved register.
 */
static bool vgic_mmio(int frame, u32 off, u64 *val, bool write, int width)
{
    u32 bytes = 1 << width;

    if (width > 3 || (off & (bytes - 1))) {
        if (!write)
            for (u32 i = 0; i < (bytes + 7) / 8; i++)
                val[i] = 0;
        vgic_report_unhandled(frame, off, write, width);
        return true;
    }

    if (!write) {
        u32 base = off & ~3U;
        u64 v = frame < 0 ? vgic_dist_read(base) : vgic_redist_read(frame, base);

        if (width == 3)
            v |= (u64)(frame < 0 ? vgic_dist_read(off + 4) : vgic_redist_read(frame, off + 4)) << 32;
        else
            v = (v >> (8 * (off & 3))) & MASK(8 * bytes);
        *val = v;
        return true;
    }

    if (width >= 2) {
        for (u32 i = 0; i < bytes; i += 4) {
            if (frame < 0)
                vgic_dist_write(off + i, *val >> (8 * i));
            else
                vgic_redist_write(frame, off + i, *val >> (8 * i));
        }
        return true;
    }

    if (frame < 0 && off >= GICD_IPRIORITYR && off < GICD_IPRIORITYR + 32 * VGIC_NR_WORDS) {
        for (u32 i = 0; i < bytes; i++)
            vgic_dist_write_priority(off - GICD_IPRIORITYR + i, *val >> (8 * i));
    } else if (frame >= 0 && vgic_pcpu[frame].present && off >= GICR_IPRIORITYR &&
               off < GICR_IPRIORITYR + VGIC_NR_LOCAL) {
        for (u32 i = 0; i < bytes; i++)
            vgic_store(vgic_pcpu[frame].priority[off - GICR_IPRIORITYR + i], (u8)(*val >> (8 * i)));
    } else {
        vgic_report_unhandled(frame, off, true, width);
    }
    return true;
}

static bool vgic_dist_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    return vgic_mmio(-1, addr - HV_VGIC_DIST_BASE, val, write, width);
}

static bool vgic_redist_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 off = addr - HV_VGIC_REDIST_BASE;

    UNUSED(ctx);
    return vgic_mmio(off / HV_VGIC_REDIST_SIZE, off % HV_VGIC_REDIST_SIZE, val, write, width);
}

/* What each core the guest ran on did, for the log. Called with the hypervisor lock held. */
void hv_vgic_report(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct hv_vgic_pcpu *p = &vgic_pcpu[cpu];

        if (!p->injected && !p->sgi_writes && !p->maintenance)
            continue;

        printf("HV: vGIC: CPU %d: %ld ICC_SGI1R_EL1 writes, %ld injected, %ld maintenance IRQs, "
               "%ld taken back, %ld returns with SGIs waiting, %ld with SPIs waiting, SGIs still "
               "pending 0x%x\n",
               cpu, p->sgi_writes, p->injected, p->maintenance, p->retracted, p->sgi_deferred,
               p->spi_deferred, __atomic_load_n(&p->sgi_pending, __ATOMIC_SEQ_CST));
    }

    if (vgic_unhandled_writes || vgic_unhandled_reads)
        printf("HV: vGIC: accesses to registers that are not emulated: %ld writes, %ld reads\n",
               vgic_unhandled_writes, vgic_unhandled_reads);
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
