/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The Arm PMUv3 registers for a guest on a core that does not have them.
 *
 * Windows reads the PMUv3 cycle counter PMCCNTR_EL0 as a time stamp, in its boot manager, its
 * loader, its kernel and its serial debugger module, and sets the counter up through PMCR_EL0
 * and a handful of other registers. On the M4 (ID_AA64DFR0_EL1.PMUVer = 0xF) these registers
 * do not exist: an access is an Undefined Instruction exception that the guest takes itself, at
 * EL1, and none of the controls that hand such an access to EL2 take effect under m1n1.
 *
 * So the accesses are taken out of the guest's code. The words of a page that are an MRS or an
 * MSR of one of these registers are each replaced by an HVC whose immediate says which
 * register, which direction and which general-purpose register:
 *
 *   bit 15       1, which no PSCI call has (those are hvc #0)
 *   bits 14:6    the register: op1 == 3, CRn == 14, CRm, op2
 *   bit 5        1 for MRS (a read)
 *   bits 4:0     Rt
 *
 * The original instruction can be had back from the immediate. The guest executes the HVC, EL2
 * answers here, and the guest goes on behind it with its own ELR_EL1, SPSR_EL1 and ESR_EL1
 * untouched, as after a trapped access. HVC is undefined at EL0 (Arm DDI 0487 M.d, rule
 * RZVTPR), so this serves code that runs at EL1 only.
 *
 * When a page is scanned is decided in hv_xtrack.c. This file is the part that does not depend
 * on page tables: which words, what they become and what the answers are.
 *
 * The answers: PMCCNTR_EL0 is the guest's view of the generic timer count and always runs,
 * whether or not the guest has enabled it. The control registers keep what is written to them,
 * as far as a PMU with a cycle counter and no event counters (PMCR_EL0.N = 0) has the bits.
 * Event counters read as zero and ignore writes. Nothing overflows, nothing interrupts.
 *
 * Everything here runs with the hypervisor lock held.
 */

#include "hv.h"
#include "smp.h"
#include "utils.h"

/* MRS/MSR (register): 1101 0101 00 L 1 o0 op1 CRn CRm op2 Rt, op0 = 2 + o0 (C5.1.3) */
#define SYSINSN_MASK 0xffd80000
#define SYSINSN_OP0_3 0xd5180000
#define SYSINSN_L    BIT(21)

#define HVC_OPCODE 0xd4000002 // hvc #0
#define HVC_MASK   0xffe0001f

#define REG_OP1_3  BIT(8)
#define REG_CRN_14 BIT(7)

/* op1 == 3, CRn == 14, CRm, op2 packed as in the immediate */
#define REG(op1, crn, crm, op2)                                                                    \
    (((op1) == 3 ? REG_OP1_3 : 0) | ((crn) == 14 ? REG_CRN_14 : 0) | ((crm) << 3) | (op2))

#define R_PMCR       REG(3, 9, 12, 0)
#define R_PMCNTENSET REG(3, 9, 12, 1)
#define R_PMCNTENCLR REG(3, 9, 12, 2)
#define R_PMOVSCLR   REG(3, 9, 12, 3)
#define R_PMSELR     REG(3, 9, 12, 5)
#define R_PMCCNTR    REG(3, 9, 13, 0)
#define R_PMXEVTYPER REG(3, 9, 13, 1)
#define R_PMUSERENR  REG(3, 9, 14, 0)
#define R_PMOVSSET   REG(3, 9, 14, 3)
#define R_PMINTENSET REG(0, 9, 14, 1)
#define R_PMINTENCLR REG(0, 9, 14, 2)
#define R_PMCCFILTR  REG(3, 14, 15, 7)

/* The cycle counter is counter 31 in every per-counter register; there are no others */
#define PMU_C BIT(31)

#define PMCR_E   BIT(0)
#define PMCR_C   BIT(2) // write-only: reset the cycle counter
#define PMCR_LC  BIT(6)
/* E, D, X, DP, LC, LP. P and C read as zero; N, IDCODE and IMP are zero */
#define PMCR_KEPT (BIT(0) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7))

#define PMUSERENR_KEPT  GENMASK(3, 0)
#define PMSELR_KEPT     GENMASK(4, 0)
#define PMCCFILTR_KEPT  GENMASK(31, 24)

static struct pmu_cpu {
    u64 base; // PMCCNTR_EL0 = guest count - base
    u64 pmcr;
    u64 cnten;
    u64 inten;
    u64 ovs;
    u64 selr;
    u64 userenr;
    u64 ccfiltr;
} pmu_cpu[MAX_CPUS];

static struct {
    u64 pages;        // pages scanned
    u64 patched;      // words replaced
    u64 ccntr_reads;  // answers given, by kind
    u64 other_reads;
    u64 writes;
    u64 unknown;      // accesses to registers of the group that have no case below
    u16 last_unknown;
} pmu_stats;

void hv_pmu_init(void)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        pmu_cpu[i].base = 0;
        pmu_cpu[i].pmcr = 0;
        pmu_cpu[i].cnten = 0;
        pmu_cpu[i].inten = 0;
        pmu_cpu[i].ovs = 0;
        pmu_cpu[i].selr = 0;
        pmu_cpu[i].userenr = 0;
        pmu_cpu[i].ccfiltr = 0;
    }
    pmu_stats.pages = 0;
    pmu_stats.patched = 0;
    pmu_stats.ccntr_reads = 0;
    pmu_stats.other_reads = 0;
    pmu_stats.writes = 0;
    pmu_stats.unknown = 0;
    pmu_stats.last_unknown = 0;
}

/*
 * Is this word an MRS or MSR of a PMUv3 register? The group, with op0 == 3 (D24.5):
 *   op1 == 3, CRn == 9, CRm == 12..14   PMCR, PMCNTENSET/CLR, PMOVSCLR/SET, PMSWINC, PMSELR,
 *                                       PMCEID0/1, PMCCNTR, PMXEVTYPER, PMXEVCNTR, PMUSERENR
 *   op1 == 0, CRn == 9, CRm == 14       PMINTENSET/CLR_EL1, PMMIR_EL1
 *   op1 == 3, CRn == 14, CRm == 8..15   PMEVCNTR<n>, PMEVTYPER<n>, PMCCFILTR
 * CRn == 14 with CRm below 8 is the generic timer, and CRn == 9 with CRm below 12 the
 * statistical profiling and trace buffer extensions, which the guest finds absent on its own.
 */
bool hv_pmu_is_access(u32 insn)
{
    if ((insn & SYSINSN_MASK) != SYSINSN_OP0_3)
        return false;

    u32 op1 = (insn >> 16) & 7;
    u32 crn = (insn >> 12) & 15;
    u32 crm = (insn >> 8) & 15;

    if (crn == 9 && op1 == 3 && crm >= 12 && crm <= 14)
        return true;
    if (crn == 9 && op1 == 0 && crm == 14)
        return true;
    if (crn == 14 && op1 == 3 && crm >= 8)
        return true;
    return false;
}

/* The HVC that stands for an access; insn must be one */
u32 hv_pmu_patch_word(u32 insn)
{
    u32 op1 = (insn >> 16) & 7;
    u32 crn = (insn >> 12) & 15;
    u32 crm = (insn >> 8) & 15;
    u32 op2 = (insn >> 5) & 7;
    u32 imm = BIT(15) | (REG(op1, crn, crm, op2) << 6) | (insn & SYSINSN_L ? BIT(5) : 0) |
              (insn & 31);

    return HVC_OPCODE | (imm << 5);
}

/* The access an HVC stands for, or 0 if the word is not one of ours */
u32 hv_pmu_unpatch_word(u32 insn)
{
    if ((insn & HVC_MASK) != HVC_OPCODE)
        return 0;

    u32 imm = (insn >> 5) & 0xffff;
    if (!(imm & BIT(15)))
        return 0;

    u32 reg = (imm >> 6) & 0x1ff;
    u32 op1 = reg & REG_OP1_3 ? 3 : 0;
    u32 crn = reg & REG_CRN_14 ? 14 : 9;
    u32 orig = SYSINSN_OP0_3 | (imm & BIT(5) ? SYSINSN_L : 0) | (op1 << 16) | (crn << 12) |
               ((reg & 0x7f) << 5) | (imm & 31);

    /* An immediate that no access produces: bit 15 set by something else */
    if (!hv_pmu_is_access(orig))
        return 0;
    return orig;
}

/*
 * Replace the accesses in count words. Returns how many there were. Words that are already
 * HVCs, ours or the guest's own, are left alone, so scanning twice does no harm.
 */
u32 hv_pmu_patch_range(u32 *words, u32 count)
{
    u32 n = 0;

    for (u32 i = 0; i < count; i++) {
        /* All of the group has 0xd51 or 0xd53 on top: one compare rejects nearly every word */
        if ((words[i] & SYSINSN_MASK) != SYSINSN_OP0_3)
            continue;
        if (!hv_pmu_is_access(words[i]))
            continue;
        words[i] = hv_pmu_patch_word(words[i]);
        n++;
    }

    pmu_stats.pages++;
    pmu_stats.patched += n;
    return n;
}

/*
 * What the guest sees as the count of the generic timer: the physical count less the offset
 * the hypervisor keeps in CNTVOFF_EL2, which is where it takes out the time the guest was
 * stopped for the host (hv_exc.c).
 */
static u64 hv_pmu_now(void)
{
    return mrs(CNTPCT_EL0) - mrs(CNTVOFF_EL2);
}

static u64 hv_pmu_read(struct pmu_cpu *p, u32 reg)
{
    switch (reg) {
        case R_PMCCNTR:
            pmu_stats.ccntr_reads++;
            return hv_pmu_now() - p->base;
        case R_PMCR:
            return p->pmcr;
        case R_PMCNTENSET:
        case R_PMCNTENCLR:
            return p->cnten;
        case R_PMINTENSET:
        case R_PMINTENCLR:
            return p->inten;
        case R_PMOVSCLR:
        case R_PMOVSSET:
            return p->ovs;
        case R_PMSELR:
            return p->selr;
        case R_PMUSERENR:
            return p->userenr;
        case R_PMCCFILTR:
            return p->ccfiltr;
        case R_PMXEVTYPER:
            /* With PMSELR_EL0.SEL == 31 this is PMCCFILTR_EL0 (D24.5) */
            return p->selr == 31 ? p->ccfiltr : 0;
    }

    /* PMCEID0/1, PMMIR, PMSWINC, PMXEVCNTR and the event counters: nothing there */
    pmu_stats.unknown++;
    pmu_stats.last_unknown = reg;
    return 0;
}

static void hv_pmu_write(struct pmu_cpu *p, u32 reg, u64 val)
{
    switch (reg) {
        case R_PMCCNTR:
            p->base = hv_pmu_now() - val;
            return;
        case R_PMCR:
            p->pmcr = val & PMCR_KEPT;
            if (val & PMCR_C)
                p->base = hv_pmu_now();
            return;
        case R_PMCNTENSET:
            p->cnten |= val & PMU_C;
            return;
        case R_PMCNTENCLR:
            p->cnten &= ~val;
            return;
        case R_PMINTENSET:
            p->inten |= val & PMU_C;
            return;
        case R_PMINTENCLR:
            p->inten &= ~val;
            return;
        case R_PMOVSSET:
            p->ovs |= val & PMU_C;
            return;
        case R_PMOVSCLR:
            p->ovs &= ~val;
            return;
        case R_PMSELR:
            p->selr = val & PMSELR_KEPT;
            return;
        case R_PMUSERENR:
            p->userenr = val & PMUSERENR_KEPT;
            return;
        case R_PMCCFILTR:
            p->ccfiltr = val & PMCCFILTR_KEPT;
            return;
        case R_PMXEVTYPER:
            if (p->selr == 31)
                p->ccfiltr = val & PMCCFILTR_KEPT;
            return;
    }

    pmu_stats.unknown++;
    pmu_stats.last_unknown = reg;
}

/*
 * The guest executed an hvc with bit 15 of the immediate set; ctx->elr is behind it already.
 * Returns false if the immediate is not one that hv_pmu_patch_word() makes.
 */
bool hv_pmu_hvc(struct exc_info *ctx, u16 imm)
{
    if (!hv_pmu_unpatch_word(HVC_OPCODE | ((u32)imm << 5)))
        return false;

    struct pmu_cpu *p = &pmu_cpu[ctx->cpu_id];
    u32 reg = (imm >> 6) & 0x1ff;
    u32 rt = imm & 31;

    if (imm & BIT(5)) {
        u64 val = hv_pmu_read(p, reg);
        if (reg != R_PMCCNTR)
            pmu_stats.other_reads++;
        /* Rt == 31 is the zero register */
        if (rt != 31)
            ctx->regs[rt] = val;
    } else {
        pmu_stats.writes++;
        hv_pmu_write(p, reg, rt == 31 ? 0 : ctx->regs[rt]);
    }

    return true;
}

void hv_pmu_report(void)
{
    printf("HV: PMU: %ld pages scanned, %ld instructions replaced; %ld reads of PMCCNTR_EL0, "
           "%ld other reads, %ld writes",
           pmu_stats.pages, pmu_stats.patched, pmu_stats.ccntr_reads, pmu_stats.other_reads,
           pmu_stats.writes);
    if (pmu_stats.unknown)
        printf(", %ld of them to registers that are not there (the last one 0x%x)",
               pmu_stats.unknown, pmu_stats.last_unknown);
    printf("\n");
}
