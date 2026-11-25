/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "hv.h"
#include "adt.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "gxf.h"
#include "heapblock.h"
#include "hv_sprr.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "string.h"
#include "usb.h"
#include "utils.h"

#define HV_TICK_RATE      1000
#define HV_SLOW_TICK_RATE 1

/*
 * Where a core starts the guest at PSCI CPU_ON (Arm DEN 0022 F.b 6.4.1, 6.4.3.3): at EL1 with
 * its own stack pointer, and all four exception masks set
 */
#define HV_CPU_ON_SPSR (SPSR_D | SPSR_A | SPSR_I | SPSR_F | SPSR_M_EL1H)

/* How long a core with nothing left to do may take to get back to where it waits */
#define HV_IDLE_TIMEOUT_US 10000

DECLARE_SPINLOCK(bhl);

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry, u64 spsr);
void hv_exit_guest(void) __attribute__((noreturn));

extern char _hv_vectors_start[0];

u64 hv_tick_interval;
u64 hv_secondary_tick_interval;

int hv_pinned_cpu;
int hv_want_cpu;

enum hv_sptm_mode hv_sptm_mode = SPTM_NONE;
bool hv_genter_trap_enabled = false;

static bool hv_has_ecv;
static bool hv_leaving; // every core is to leave the guest, and the hypervisor ends
static bool hv_should_exit[MAX_CPUS];
bool hv_started_cpus[MAX_CPUS];
u64 hv_cpus_in_guest;
u64 hv_saved_sp[MAX_CPUS];

/* Why a core of the device tree did not come into EL2 at hv_init(), NULL if it did */
static const char *hv_cpu_missing[MAX_CPUS];

/* What a core is let into the guest with. It reads this once it is on its way there. */
static struct hv_cpu_entry {
    void *entry;
    u64 regs[4];
    u64 count; // how often it was let in since hv_init()
} hv_cpu_entry[MAX_CPUS];

/*
 * How deep each core goes into its EL2 stack, the one the hypervisor runs on for it: the boot
 * core's is m1n1's own, the others' are in secondary_stacks (src/smp.c). Each core marks what it
 * has not used of it the first time it enters the guest after hv_init(), and hv_stack_report()
 * finds the deepest word that is not the mark any more (SiliconNT docs/findings/0100, conclusion
 * 11).
 */
extern u8 _stack_top[], _stack_bot[];
extern u8 secondary_stacks[MAX_CPUS][SECONDARY_STACK_SIZE];

#define HV_STACK_MARK 0x4b4354534b435453UL // "STCKSTCK"
/* Left as it is below where hv_stack_mark() is: its own frame and what it may call */
#define HV_STACK_SLACK 1024

static bool hv_stack_marked[MAX_CPUS];

static void hv_stack_range(int cpu, u64 *low, u64 *high)
{
    if (cpu == boot_cpu_idx) {
        *low = (u64)_stack_top;
        *high = (u64)_stack_bot;
    } else {
        *low = (u64)secondary_stacks[cpu];
        *high = *low + SECONDARY_STACK_SIZE;
    }
}

/* On the core itself, once per hv_init(). Not if it does not run on that stack. */
static void hv_stack_mark(void)
{
    int cpu = smp_id();
    u64 here = (u64)__builtin_frame_address(0);
    u64 low, high;

    if (hv_stack_marked[cpu])
        return;
    hv_stack_range(cpu, &low, &high);
    if (here < low + HV_STACK_SLACK || here > high)
        return;

    for (volatile u64 *p = (u64 *)low; (u64)p < here - HV_STACK_SLACK; p++)
        *p = HV_STACK_MARK;
    hv_stack_marked[cpu] = true;
}

/*
 * For the log, with every core out of the guest. What a core used before it marked its stack,
 * down to HV_STACK_SLACK below where it was then, counts as used.
 */
static void hv_stack_report(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 low, high;
        const u64 *p;

        if (!hv_stack_marked[cpu])
            continue;
        hv_stack_range(cpu, &low, &high);
        for (p = (const u64 *)low; (u64)p < high && *p == HV_STACK_MARK; p++)
            ;
        printf("HV: CPU %d used at most %ld KiB of its %ld KiB EL2 stack\n", cpu,
               (high - (u64)p) >> 10, (high - low) >> 10);
    }
}

static u64 hv_cpu_state_regs[MAX_CPUS];

static bool hv_cpu_state_access(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    if (width != 2 && width != 3)
        return false;
    if (write)
        return hv_pa_rw(ctx, ipa, val, write, width);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 base = hv_cpu_state_regs[cpu];
        if (!base)
            continue;
        if (ipa != base)
            continue;

        /* Clear out the lower byte when the core hasn't been started by the guest yet */
        if (!hv_pa_rw(ctx, ipa, val, write, width))
            return false;
        if (!hv_started_cpus[cpu])
            *val &= ~0xffUL;
        return true;
    }

    return false;
}

/*
 * For macOS under SPTM only, which upstream added this for: it reads the register of each core
 * at cpu-impl-reg + 0x100 over and over. Any other guest reads it as the hardware has it, like
 * every other register of the cores (SiliconNT docs/DESIGN.md, "设计核心" 1).
 */
static bool hv_cpu_state_setup_hook(void)
{
    int node;

    memset(hv_cpu_state_regs, 0, sizeof(hv_cpu_state_regs));

    if (hv_sptm_mode == SPTM_NONE)
        return true;

    node = adt_path_offset(adt, "/cpus");
    if (node < 0)
        return false;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 cpu;
        u64 reg[2];

        if (ADT_GETPROP(adt, node, "cpu-id", &cpu) != sizeof(cpu))
            return false;
        if (ADT_GETPROP_ARRAY(adt, node, "cpu-impl-reg", reg) != sizeof(reg))
            return false;

        hv_cpu_state_regs[cpu] = reg[0] + 0x100;
        if (hv_map_hook(hv_cpu_state_regs[cpu], hv_cpu_state_access, 8) < 0)
            return false;
    }

    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    return true;
}

struct hv_secondary_info_t {
    uint64_t hcr;
    uint64_t hacr;
    uint64_t vtcr, vttbr;
    uint64_t mdcr;
    uint64_t mdscr;
    uint64_t amx_ctl;
    uint64_t apvmkeylo, apvmkeyhi, apsts;
    uint64_t actlr_el2;
    uint64_t actlr_el1;
    uint64_t cnthctl;
    uint64_t sprr_config;
    uint64_t gxf_config;
    uint64_t agt_cnt_rdir_el1;
    uint64_t agt_cnt_rdir_el12;
};

static struct hv_secondary_info_t hv_secondary_info;

u64 nvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));
u64 anvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));

static void hv_enable_sme_zt0(void)
{
    u64 sme = FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1));
    if (sme != ID_AA64PFR1_SME2)
        return;

    /* Don't trap SME instructions to EL2 */
    reg_set(SYS_SMCR_EL2, SMCR_EZT0);
}

/* must be called before hv_init */
void hv_sptm_set_mode(enum hv_sptm_mode mode)
{
    /*
     * hv_sprr.c write-protects the 16 KB pages of its guest one stage 2 page at a time, and
     * stage 2 has 4 KB pages in this tree (hv_vm.c): three quarters would stay writable.
     */
    if (mode == SPTM_EMULATION)
        panic("HV: SPTM emulation needs 16 KB pages in stage 2, this tree has 4 KB\n");

    printf("HV: SPTM mode %d\n", mode);
    hv_sptm_mode = mode;
}

/*
 * Whether a core that runs m1n1 waits for something to do. One that has just left the guest may
 * not be back where it waits yet (hv_enter_secondary()); one that m1n1 uses for something else,
 * as hv_wdt.c does, will not be.
 */
static bool hv_cpu_waits(int cpu)
{
    for (int us = 0; us < HV_IDLE_TIMEOUT_US; us++) {
        if (smp_is_idle(cpu))
            return true;
        udelay(1);
    }
    return false;
}

/*
 * Have every core of the device tree run m1n1, where it waits in EL2 for the guest's CPU_ON
 * (hv_psci.c). What did not work is kept for hv_start(), and said here as well: this is where
 * the screen is still m1n1's.
 */
static void hv_start_cpus(void)
{
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 line;

        hv_cpu_missing[cpu] = NULL;
        hv_cpu_entry[cpu].count = 0;
        if (!smp_has_cpu(cpu))
            continue;

        hv_cpu_missing[cpu] = smp_try_start(cpu);
        if (!hv_cpu_missing[cpu] && cpu != boot_cpu_idx && !hv_cpu_waits(cpu))
            hv_cpu_missing[cpu] = "it is busy with something else of m1n1";
        if (hv_cpu_missing[cpu])
            continue;

        if (cpu == boot_cpu_idx) {
            line = hv_xtrack_line();
        } else {
            smp_call0(cpu, hv_xtrack_line);
            line = smp_wait(cpu);
        }
        if (line < HV_XTRACK_LINE) {
            printf("HV: CPU %d: the smallest data cache line is %ld bytes, and cache maintenance "
                   "here goes by %ld\n",
                   cpu, line, HV_XTRACK_LINE);
            hv_cpu_missing[cpu] = "its data cache lines are too small";
        }
    }

    hv_cpus_ready();
}

/* Whether hv_init() has every core of the device tree in EL2. Says which are not, and why. */
bool hv_cpus_ready(void)
{
    bool ready = true;

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (!hv_cpu_missing[cpu])
            continue;
        printf("HV: CPU %d is not in EL2: %s\n", cpu, hv_cpu_missing[cpu]);
        ready = false;
    }

    if (smp_cpus_beyond()) {
        printf("HV: %d CPUs are not in EL2: m1n1 has room for CPUs 0 to %d only\n",
               smp_cpus_beyond(), MAX_CPUS - 1);
        ready = false;
    }

    return ready;
}

void hv_init(void)
{
    pcie_shutdown();
    // Make sure we wake up DCP if we put it to sleep, just quiesce it to match ADT
    if (display_is_external && display_start_dcp() >= 0)
        display_shutdown(DCP_QUIESCED);
    // reenable hpm interrupts for the guest for unused iodevs
    usb_hpm_restore_irqs(0);
    // Before any core waits: M4 loses CPU state in WFI (src/kboot.c)
    smp_set_wfe_mode(true);
    // Before hv_vgic_init(), which gives a Redistributor to every core that runs m1n1
    hv_start_cpus();
    hv_wdt_init();

    hv_pt_init();
    hv_sprr_init();
    hv_aic_init();

    // Configure hypervisor defaults
    hv_write_hcr(HCR_API | // Allow PAuth instructions
                 HCR_APK | // Allow PAuth key registers
                 HCR_TEA | // Trap external aborts
                 HCR_E2H | // VHE mode (forced)
                 HCR_RW |  // AArch64 guest
                 HCR_AMO | // Trap SError exceptions
                 HCR_IMO | // Take IRQs to EL2, the guest gets them as virtual interrupts
                 HCR_TWI | // Trap WFI: the core waits in EL2, see hv_wfi.c
                 HCR_VM);  // Enable stage 2 translation
    sysop("isb");

    hv_vgic_init();

    if (hv_sptm_mode == SPTM_EMULATION) {
        hv_write_hcr(mrs(HCR_EL2) | HCR_EnSCXT);
        if (FIELD_GET(ID_AA64PFR1_MTE, mrs(SYS_ID_AA64PFR1_EL1)) >= ID_AA64PFR1_MTE2) {
            hv_write_hcr(mrs(HCR_EL2) | HCR_ATA);
            sysop("isb");
        }
    }

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        printf("Enabling NV\n");
        hv_write_hcr(mrs(SYS_HCR_EL2) | HCR_NV2 | HCR_NV);
        msr(VNCR_EL2, &nvMem[0]);
        msr(SYS_IMP_APL_AVNCR_EL2, &anvMem[0]);
    }

    // No guest vectors initially
    msr(VBAR_EL12, 0);

    // Compute tick interval
    hv_tick_interval = mrs(CNTFRQ_EL0) / HV_TICK_RATE;

    hv_has_ecv = mrs(ID_AA64MMFR0_EL1) & (0xfULL << 60);

    if (hv_has_ecv) {
        printf("HV: ECV enabled\n");
        reg_set(CNTHCTL_EL2,
                CNTHCTL_EL1NVVCT | CNTHCTL_EL1NVPCT | CNTHCTL_EL1TVT | CNTHCTL_EL1PCTEN);
        hv_secondary_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
    } else {
        printf("HV: No ECV supported\n");
        // Enable physical timer for EL1
        msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);

        hv_secondary_tick_interval = hv_tick_interval;
    }

    hv_exc_init();
    memset(hv_stack_marked, 0, sizeof(hv_stack_marked));
    // After CNTHCTL_EL2 has been set up above: this adds the event stream
    hv_wfi_init();
    hv_psci_init();
    hv_pmu_init();
    hv_dbg_init();
    hv_xtrack_init();

    // Set deep WFI back to defaults
    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    sysop("dsb ishst");
    sysop("tlbi alle1is");
    sysop("dsb ish");
    sysop("isb");
}

static void hv_set_gxf_vbar(void)
{
    msr(SYS_IMP_APL_VBAR_GL1, _hv_vectors_start);
}

/*
 * The core that does what one of them has to do for all: the tick (hv_exc_fiq()), and the SPIs
 * that go to any core (hv_vgic.c). The boot core; while the guest has that turned off, the one
 * with the lowest index that the guest runs on. -1 if it runs on none.
 */
int hv_lead_cpu(void)
{
    if (boot_cpu_idx >= 0 && __atomic_load_n(&hv_started_cpus[boot_cpu_idx], __ATOMIC_SEQ_CST))
        return boot_cpu_idx;
    for (int i = 0; i < MAX_CPUS; i++)
        if (__atomic_load_n(&hv_started_cpus[i], __ATOMIC_SEQ_CST))
            return i;
    return -1;
}

/* Whether the guest is being left. Nothing but that is to happen with it then. */
bool hv_is_leaving(void)
{
    return __atomic_load_n(&hv_leaving, __ATOMIC_SEQ_CST);
}

/*
 * The guest is left: every core is to leave it, and then the hypervisor ends on the boot core, in
 * hv_start(). Called with the hypervisor lock held, on whichever core it is that has been told.
 */
static void hv_leave(void)
{
    __atomic_store_n(&hv_leaving, true, __ATOMIC_SEQ_CST);

    /* Those in the guest, and those that wait in EL2 for a WFI of the guest to end (hv_wfi.c) */
    sysop("dsb ishst");
    for (int i = 0; i < MAX_CPUS; i++)
        if (i != smp_id() && hv_started_cpus[i])
            smp_send_ipi(i);
    /* The boot core, should the guest have turned it off */
    sysop("sev");
}

/*
 * This core leaves the guest and goes back to where it was let in from, hv_start() or
 * hv_enter_secondary(): it has been told to, or the guest has turned it off. Called with the
 * hypervisor lock held, which is let go of. From then on it is not one of the cores the guest
 * runs on, and can become one again.
 */
void hv_leave_guest(void)
{
    int cpu = smp_id();
    u64 count = hv_cpu_entry[cpu].count;

    __atomic_store_n(&hv_started_cpus[cpu], false, __ATOMIC_SEQ_CST);

    /* A core the host wanted to see is not going to get there: nobody is to wait for it */
    if (hv_want_cpu == cpu)
        hv_want_cpu = -1;
    if (hv_pinned_cpu == cpu)
        hv_pinned_cpu = -1;

    /* Neither the tick nor the timers of the guest are for a core that waits */
    msr(CNTP_CTL_EL0, 0);
    msr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, 0);
    sysop("isb");

    /* The last one: there is nobody left to turn another one on */
    if (hv_lead_cpu() < 0)
        hv_leave();
    if (!hv_leaving)
        hv_vgic_cpu_off();

    /* As for the way in, hv_start_secondary(): the guest may do this over and over */
    if (hv_leaving || !(count & (count - 1)))
        printf("HV: Exiting from CPU %d\n", cpu);

    spin_unlock(&bhl);
    hv_exit_guest();
}

/* spin_lock(&bhl), unless another core has it */
static bool hv_try_lock(void)
{
    s64 nobody = -1;

    if (!__atomic_compare_exchange_n(&bhl.lock, &nobody, (s64)smp_id(), false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return false;
    bhl.count++;
    return true;
}

/*
 * What has made the boot core leave the guest may be PSCI CPU_OFF (hv_cpu_off()): until the guest
 * is left, it waits for CPU_ON then, as the other cores do in smp_secondary_entry(), and this is
 * called every time before it sleeps.
 */
static bool hv_boot_cpu_waits(void)
{
    if (hv_is_leaving())
        return false;

    /*
     * AIC tells the boot core of its lines, in the guest or not (R50: SiliconNT
     * docs/findings/0098), so it goes on taking them for the guest. An IRQ is masked here; the
     * event stream (hv_wfi.c) is what ends the WFE. There is no maintenance interrupt among them:
     * the list registers are empty (hv_vgic_cpu_off()).
     *
     * It does not wait for the lock: who has it may be waiting for this core, to let it into the
     * guest. There may be another line as well. So it does not sleep before it has looked again.
     */
    if (mrs(ISR_EL1) & ISR_I) {
        if (hv_try_lock()) {
            /* Who had the lock may have masked the line since */
            if ((mrs(ISR_EL1) & ISR_I) && !hv_vgic_handle_irq())
                hv_panic("HV: CPU %d: an IRQ that is nobody's, the guest has it turned off\n",
                         smp_id());
            spin_unlock(&bhl);
        }
        sysop("sevl");
    }

    return true;
}

void hv_start(void *entry, u64 regs[4])
{
    if (boot_cpu_idx == -1) {
        printf("Boot CPU has not been found, can't start hypervisor\n");
        return;
    }

    if (!hv_cpus_ready()) {
        printf("HV: the guest would not have all its CPUs; can't start the hypervisor\n");
        return;
    }

    if (!hv_vgic_ranges_free()) {
        printf("HV: the guest would not find its GIC; can't start the hypervisor\n");
        return;
    }

    /* hv_init() has said why: no AIC, or more lines than SPIs */
    if (!hv_vgic_has_aicmap()) {
        printf("HV: the AIC lines have no SPIs for the guest; can't start the hypervisor\n");
        return;
    }

    if (!hv_cpu_power_init()) {
        printf("HV: CPU power protection hooks failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_amcc_init()) {
        printf("HV: AMCC interrupt hook failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_cpu_state_setup_hook()) {
        printf("CPU state hook setup failed, can't start the hypervisor\n");
        return;
    }

    /* Both loaders have made every mapping by now: one check for the two */
    if (hv_pt_check_ctrr() < 0) {
        printf("HV: the guest has memory the other cores cannot write; can't start the "
               "hypervisor\n");
        return;
    }

    /*
     * What the tables, the shadow pages and the rest of m1n1 have left of what was kept for
     * them (selfboot_m1n1_heap()). dlmalloc may have more, of what it has taken already.
     */
    size_t heap_left;
    if (heapblock_left(&heap_left))
        printf("HV: %zu KiB of m1n1's heap left below its limit\n", heap_left >> 10);
    else
        printf("HV: m1n1's heap has no limit\n");

    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));

    hv_leaving = false;
    hv_started_cpus[boot_cpu_idx] = true;

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_secondary_info.hcr = mrs(HCR_EL2);
    hv_secondary_info.hacr = mrs(HACR_EL2);
    hv_secondary_info.vtcr = mrs(VTCR_EL2);
    hv_secondary_info.vttbr = mrs(VTTBR_EL2);
    hv_secondary_info.mdcr = mrs(MDCR_EL2);
    hv_secondary_info.mdscr = mrs(MDSCR_EL1);
    hv_secondary_info.amx_ctl = mrs(SYS_IMP_APL_AMX_CTL_EL2);
    hv_secondary_info.apvmkeylo = mrs(SYS_IMP_APL_APVMKEYLO_EL2);
    hv_secondary_info.apvmkeyhi = mrs(SYS_IMP_APL_APVMKEYHI_EL2);
    hv_secondary_info.apsts = mrs(SYS_IMP_APL_APSTS_EL12);
    hv_secondary_info.actlr_el2 = mrs(ACTLR_EL2);
    if (cpu_features->actlr_el2)
        hv_secondary_info.actlr_el1 = mrs(SYS_ACTLR_EL12);
    else
        hv_secondary_info.actlr_el1 = mrs(SYS_IMP_APL_ACTLR_EL12);
    hv_secondary_info.cnthctl = mrs(CNTHCTL_EL2);
    hv_secondary_info.sprr_config = mrs(SYS_IMP_APL_SPRR_CONFIG_EL1);
    hv_secondary_info.gxf_config = mrs(SYS_IMP_APL_GXF_CONFIG_EL1);
    if (cpu_features->counter_redirect) {
        hv_secondary_info.agt_cnt_rdir_el1 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL1);
        hv_secondary_info.agt_cnt_rdir_el12 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL12);
    }

    hv_arm_tick(false);
    hv_pinned_cpu = -1;
    hv_want_cpu = -1;
    hv_cpus_in_guest = BIT(smp_id());

    hv_write_cntvoff();
    hv_vgic_start();
    hv_stack_mark();
    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry, mrs(DAIF) | SPSR_M_EL1H);

    /*
     * The guest has turned this core off, or it is all over. In the first case this goes on when
     * it is all over: the guest may have had the core back in the meantime.
     */
    smp_boot_cpu_wait(hv_boot_cpu_waits);

    spin_lock(&bhl);

    hv_wdt_stop();

    printf("HV: Exiting hypervisor (main CPU)\n");

    spin_unlock(&bhl);
    // Wait a bit for the guest CPUs to exit on their own if they are in the process.
    udelay(200000);
    spin_lock(&bhl);

    for (int i = 0; i < MAX_CPUS; i++) {
        /* One that is not in the guest any more may not be back where it waits yet */
        if (i == boot_cpu_idx || !hv_cpu_entry[i].count)
            continue;
        if (hv_started_cpus[i])
            printf("HV: Waiting for CPU %d to exit\n", i);
        spin_unlock(&bhl);
        smp_wait(i);
        spin_lock(&bhl);
    }

    printf("HV: All CPUs exited\n");
    hv_vgic_report();
    hv_vgic_stop();
    hv_wfi_report();
    hv_exc_report();
    hv_stack_report();
    hv_psci_report();
    hv_pmu_report();
    hv_dbg_report();
    hv_xtrack_report();
    spin_unlock(&bhl);
}

/*
 * On a core that is about to be let into the guest, every time: the EL2 settings of the boot
 * core, and for EL1 what a core that PSCI has just turned on has, whatever is left there from
 * m1n1 or from the last time the guest ran on it.
 */
static void hv_init_secondary(struct hv_secondary_info_t *info)
{
    int cpu = smp_id();

    if (cpu_features->apple_sysregs_unlocked)
        gxf_init();

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    msr(HCR_EL2, info->hcr);
    msr(HACR_EL2, info->hacr);
    msr(VTCR_EL2, info->vtcr);
    msr(VTTBR_EL2, info->vttbr);
    msr(MDCR_EL2, info->mdcr);
    msr(MDSCR_EL1, info->mdscr);

    hv_vgic_init_cpu();

    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_AMX_CTL_EL2, info->amx_ctl);
        msr(SYS_IMP_APL_APVMKEYLO_EL2, info->apvmkeylo);
        msr(SYS_IMP_APL_APVMKEYHI_EL2, info->apvmkeyhi);
        msr(SYS_IMP_APL_APSTS_EL12, info->apsts);
    }
    msr(ACTLR_EL2, info->actlr_el2);
    if (cpu_features->actlr_el2)
        msr(SYS_ACTLR_EL12, info->actlr_el1);
    else
        msr(SYS_IMP_APL_ACTLR_EL12, info->actlr_el1);
    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_SPRR_CONFIG_EL1, info->sprr_config);
        /* GXF_CONFIG is UNDEF unless SPRR is enabled */
        if (info->sprr_config & SPRR_CONFIG_EN)
            msr(SYS_IMP_APL_GXF_CONFIG_EL1, info->gxf_config);
    }
    if (cpu_features->counter_redirect) {
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, info->agt_cnt_rdir_el1);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, info->agt_cnt_rdir_el12);
    }

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        msr(VNCR_EL2, &nvMem[smp_id()]);
        msr(SYS_IMP_APL_AVNCR_EL2, &anvMem[smp_id()]);
    }

    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    /* Little endian, stage 1 MMU and caches off (Arm DEN 0022 F.b 6.4.3.2, 6.4.3.4) */
    msr(SYS_SCTLR_EL12, mrs(SYS_SCTLR_EL12) & ~(SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_EE));
    /* As on the boot core: no guest vectors. What was there may be m1n1's (src/exception.c). */
    msr(VBAR_EL12, 0);

    hv_exc_init_cpu(cpu);
    hv_xtrack_init_cpu(cpu);
    hv_dbg_init_cpu(cpu);
    hv_pmu_init_cpu(cpu);
    hv_write_cntvoff();

    // For M3 and up, CNTHCTL_EL2 must be written after the counter redirection
    sysop("isb");
    msr(CNTHCTL_EL2, info->cnthctl);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);
}

static void hv_enter_secondary(void)
{
    const struct hv_cpu_entry *e = &hv_cpu_entry[smp_id()];

    hv_arm_tick(smp_id() != hv_lead_cpu());
    hv_stack_mark();
    hv_enter_guest(e->regs[0], e->regs[1], e->regs[2], e->regs[3], e->entry, HV_CPU_ON_SPSR);
}

/*
 * Let a core that waits in EL2 into the guest, for PSCI CPU_ON and for the host. Called with
 * the hypervisor lock held; the core has everything it is to have when this returns, and is on
 * its way into the guest. Returns false if that core cannot be let in.
 */
bool hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    struct hv_cpu_entry *e;

    /* The guest is being left. This core leaves too, on its way back. */
    if (hv_leaving)
        return true;

    if (hv_started_cpus[cpu]) {
        printf("HV: CPU %d is in the guest already\n", cpu);
        return false;
    }

    /* The boot core gets to where it waits once the guest has turned it off (hv_start()) */
    if (cpu != boot_cpu_idx && !smp_is_alive(cpu)) {
        printf("HV: CPU %d is not waiting in m1n1, cannot enter the guest on it\n", cpu);
        return false;
    }

    if (!hv_cpu_waits(cpu)) {
        printf("HV: CPU %d is busy with something else of m1n1\n", cpu);
        return false;
    }

    e = &hv_cpu_entry[cpu];
    e->entry = entry;
    memcpy(e->regs, regs, sizeof(e->regs));
    e->count++;
    /* The first few times: printing takes long, and the caller has the hypervisor lock */
    if (!(e->count & (e->count - 1)))
        printf("HV: CPU %d (MPIDR 0x%lx) enters the guest at %p, x0 = 0x%lx (#%ld)\n", cpu,
               smp_get_mpidr(cpu), entry, regs[0], e->count);

    if (hv_sptm_mode == SPTM_EMULATION)
        hv_sprr_init_secondary(cpu);

    mmu_init_secondary(cpu);
    smp_call4(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0);
    smp_wait(cpu);

    hv_should_exit[cpu] = false;
    __atomic_store_n(&hv_started_cpus[cpu], true, __ATOMIC_SEQ_CST);
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);

    smp_call0(cpu, hv_enter_secondary);
    return true;
}

/*
 * The guest turns this core off: it leaves the guest, with the lock held, and waits in EL2 until
 * hv_start_secondary() lets it in again. Returns only on the last core the guest runs on, which
 * stays in it.
 */
void hv_cpu_off(struct exc_info *ctx)
{
    /* The host is switching to this core: it gets to see it, in this call, before it is gone */
    hv_maybe_switch_cpu(ctx, START_HV, HV_CPU_SWITCH, NULL);

    if (!hv_leaving && !(hv_get_started_cpus() & ~BIT(smp_id())))
        return;

    hv_leave_guest();
}

/* The cores the guest runs on, a bit for each: the host asks, it is not told (P_HV_STARTED_CPUS) */
u64 hv_get_started_cpus(void)
{
    u64 mask = 0;

    for (int i = 0; i < MAX_CPUS; i++)
        if (hv_started_cpus[i])
            mask |= BIT(i);
    return mask;
}

void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = smp_id();

    printf("HV: Requesting exit of CPU#%d from the guest\n", cpu);
    /* The hypervisor ends with the boot core, in the guest or not */
    if (cpu == boot_cpu_idx)
        hv_leave();
    else
        hv_should_exit[cpu] = true;
}

/* The host leaves the guest. Called with the hypervisor lock held, on the core it has stopped. */
void hv_exit_all(void)
{
    hv_leave();
    hv_leave_guest();
}

void hv_rendezvous(void)
{
    int timeout = 1000000;

    if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
        return;

    /* IPI all CPUs. This might result in spurious IPIs to the guest... */
    for (int i = 0; i < MAX_CPUS; i++) {
        if (i != smp_id() && hv_started_cpus[i]) {
            smp_send_ipi(i);
        }
    }

    while (timeout--) {
        if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
            return;
    }

    hv_panic("HV: Failed to rendezvous, missing CPUs: 0x%lx (current: %d)\n",
             __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE), smp_id());
}

bool hv_switch_cpu(int cpu)
{
    if (cpu > MAX_CPUS || cpu < 0 || !hv_started_cpus[cpu]) {
        printf("HV: CPU #%d is inactive or invalid\n", cpu);
        return false;
    }
    printf("HV: switching to CPU #%d\n", cpu);
    hv_want_cpu = cpu;
    /*
     * hv_rendezvous() only sends IPIs while some core is in the guest. A core that waits out a
     * WFI of its guest in EL2 (hv_wfi.c) is not, and would only look at hv_want_cpu at its
     * next tick.
     */
    if (cpu != smp_id()) {
        sysop("dsb ishst");
        smp_send_ipi(cpu);
    }
    hv_rendezvous();
    return true;
}

void hv_pin_cpu(int cpu)
{
    hv_pinned_cpu = cpu;
}

void hv_write_hcr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hcr, val, 0, 0, 0);
    else
        msr(HCR_EL2, val);
}

void hv_write_hacr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hacr, val, 0, 0, 0);
    else
        msr(HACR_EL2, val);
}

u64 hv_get_spsr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_SPSR_GL1);
    else
        return mrs(SPSR_EL2);
}

void hv_set_spsr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_SPSR_GL1, val);
    else
        return msr(SPSR_EL2, val);
}

u64 hv_get_esr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ESR_GL1);
    else
        return mrs(ESR_EL2);
}

u64 hv_get_far(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_FAR_GL1);
    else
        return mrs(FAR_EL2);
}

u64 hv_get_afsr1(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_AFSR1_GL1);
    else
        return mrs(AFSR1_EL2);
}

u64 hv_get_elr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ELR_GL1);
    else
        return mrs(ELR_EL2);
}

void hv_set_elr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_ELR_GL1, val);
    else
        return msr(ELR_EL2, val);
}

void hv_arm_tick(bool secondary)
{
    if (secondary)
        msr(CNTP_TVAL_EL0, hv_secondary_tick_interval);
    else
        msr(CNTP_TVAL_EL0, hv_tick_interval);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    hv_sprr_timer_arm();
}

/* The core that has become the one to tick: the next tick may still be far away */
void hv_take_over_tick(void)
{
    s32 left = mrs(CNTP_TVAL_EL0);

    if (left > (s64)hv_tick_interval)
        hv_arm_tick(false);
}

void hv_maybe_exit(void)
{
    if (!hv_is_leaving() && !hv_should_exit[smp_id()])
        return;

    /* Before the lock: who has it may wait for this in hv_rendezvous() */
    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);
    hv_leave_guest();
}

void hv_tick(struct exc_info *ctx)
{
    hv_wdt_pet();
    iodev_handle_events(uartproxy_iodev);
    /*
     * Only a port the proxy listens on has a host behind it (hv_stop.c). Without one, as when
     * m1n1 has started the guest by itself (selfboot_run.c), uartproxy_iodev is the UART, and
     * a byte there is not a host that wants the guest stopped.
     */
    if (hv_has_host() && iodev_can_read(uartproxy_iodev)) {
        printf("HV: User interrupt\n");
        iodev_console_flush();
        if (hv_pinned_cpu == -1 || hv_pinned_cpu == smp_id())
            hv_exc_proxy(ctx, START_HV, HV_USER_INTERRUPT, NULL);
    }
    hv_vuart_poll();
}

bool hv_is_nested(void)
{
    u64 hcr = mrs(HCR_EL2);
    return (hcr | HCR_NV2 | HCR_NV) == hcr;
}

void hv_enable_genter_trap(void)
{
    hv_write_hacr(mrs(HACR_EL2) | HACR_TRAP_GENTER);
}

void hv_disable_genter_trap(void)
{
    hv_write_hacr(mrs(HACR_EL2) & ~HACR_TRAP_GENTER);
}

void hv_set_genter_trap(bool enabled)
{
    hv_genter_trap_enabled = enabled;
}
