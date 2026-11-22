// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_pmu.c. The file under test is the real one, compiled
 * against the stand-in headers of this directory; this file plays the guest (the registers of
 * an hvc). hv_pmu.c has no business with any register of the hardware: the stand-ins for mrs
 * and msr fail the test.
 *
 *   build/pmu-model                 the checks
 *   build/pmu-model <file> ...      also scan each file as it lies on disk, word by word, and
 *                                   print how many accesses there are in it: to compare with
 *                                   what tools/scan_pmu_words.py of the SiliconNT repository
 *                                   says about the same Windows binary
 *
 * Which registers are PMUv3 registers is written down here a second time, by name, from
 * Arm DDI 0487 M.d D24.5, so that a slip in the ranges of hv_pmu_is_access() shows.
 */
#include <stdlib.h>
#include <string.h>

#include "hv.h"
#include "utils.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            if (failures <= 40) {                                                                  \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                            \
                printf(__VA_ARGS__);                                                               \
                printf("\n");                                                                      \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/* ---- the hardware ---- */

u64 mock_mrs(int reg)
{
    printf("FAIL: hv_pmu.c read register 0x%x\n", reg);
    failures++;
    return 0;
}

void mock_msr(int reg, u64 val)
{
    printf("FAIL: hv_pmu.c wrote 0x%lx to register 0x%x\n", val, reg);
    failures++;
}

void mock_wfe(void)
{
}

/* ---- the instructions ---- */

#define R(op0, op1, crn, crm, op2)                                                                 \
    ((((op0)-2) << 19) | ((op1) << 16) | ((crn) << 12) | ((crm) << 8) | ((op2) << 5))
#define MRS(reg, rt) (0xd5300000u | (reg) | (rt))
#define MSR(reg, rt) (0xd5100000u | (reg) | (rt))


/* op0, op1, CRn, CRm, op2 */
#define PMCR_EL0        R(3, 3, 9, 12, 0)
#define PMCNTENSET_EL0  R(3, 3, 9, 12, 1)
#define PMCNTENCLR_EL0  R(3, 3, 9, 12, 2)
#define PMOVSCLR_EL0    R(3, 3, 9, 12, 3)
#define PMSWINC_EL0     R(3, 3, 9, 12, 4)
#define PMSELR_EL0      R(3, 3, 9, 12, 5)
#define PMCEID0_EL0     R(3, 3, 9, 12, 6)
#define PMCEID1_EL0     R(3, 3, 9, 12, 7)
#define PMCCNTR_EL0     R(3, 3, 9, 13, 0)
#define PMXEVTYPER_EL0  R(3, 3, 9, 13, 1)
#define PMXEVCNTR_EL0   R(3, 3, 9, 13, 2)
#define PMUSERENR_EL0   R(3, 3, 9, 14, 0)
#define PMOVSSET_EL0    R(3, 3, 9, 14, 3)
#define PMINTENSET_EL1  R(3, 0, 9, 14, 1)
#define PMINTENCLR_EL1  R(3, 0, 9, 14, 2)
#define PMMIR_EL1       R(3, 0, 9, 14, 6)
#define PMEVCNTR0_EL0   R(3, 3, 14, 8, 0)
#define PMEVCNTR30_EL0  R(3, 3, 14, 11, 6)
#define PMEVTYPER0_EL0  R(3, 3, 14, 12, 0)
#define PMEVTYPER30_EL0 R(3, 3, 14, 15, 6)
#define PMCCFILTR_EL0   R(3, 3, 14, 15, 7)

/* Neighbours that must be left alone */
#define X_CNTFRQ      R(3, 3, 14, 0, 0)
#define CNTVCT_EL0      R(3, 3, 14, 0, 2)
#define CNTV_CTL_EL0    R(3, 3, 14, 3, 1)
#define CNTP_CVAL_EL0   R(3, 3, 14, 2, 2)
#define PMSCR_EL1       R(3, 0, 9, 9, 0)
#define PMBLIMITR_EL1   R(3, 0, 9, 10, 0)
#define PMBSR_EL1       R(3, 0, 9, 10, 3)
#define TPIDR_EL0       R(3, 3, 13, 0, 2)
#define FPCR            R(3, 3, 4, 4, 0)
#define ICC_PMR_EL1     R(3, 0, 4, 6, 0)
#define MDSCR_EL1       R(2, 0, 0, 2, 2)
#define DBGBVR9_EL1     R(2, 0, 0, 9, 4)
#define ID_AA64DFR0_EL1 R(3, 0, 0, 5, 0)
#define CNTHCTL_EL2x    R(3, 4, 14, 1, 0)
#define PMCR_OP1_4      R(3, 4, 9, 12, 0) /* no such register */


static const u32 named[] = {
    MRS(PMCR_EL0, 0),       MRS(PMCNTENSET_EL0, 0), MRS(PMCNTENCLR_EL0, 0), MRS(PMOVSCLR_EL0, 0),
    MRS(PMSWINC_EL0, 0),    MRS(PMSELR_EL0, 0),     MRS(PMCEID0_EL0, 0),    MRS(PMCEID1_EL0, 0),
    MRS(PMCCNTR_EL0, 0),    MRS(PMXEVTYPER_EL0, 0), MRS(PMXEVCNTR_EL0, 0),  MRS(PMUSERENR_EL0, 0),
    MRS(PMOVSSET_EL0, 0),   MRS(PMINTENSET_EL1, 0), MRS(PMINTENCLR_EL1, 0), MRS(PMMIR_EL1, 0),
    MRS(PMEVCNTR0_EL0, 0),  MRS(PMEVCNTR30_EL0, 0), MRS(PMEVTYPER0_EL0, 0), MRS(PMEVTYPER30_EL0, 0),
    MRS(PMCCFILTR_EL0, 0),
};

static const u32 others[] = {
    MRS(X_CNTFRQ, 0),    MRS(CNTVCT_EL0, 8),   MSR(CNTV_CTL_EL0, 1),  MSR(CNTP_CVAL_EL0, 2),
    MRS(PMSCR_EL1, 0),     MRS(PMBLIMITR_EL1, 3), MSR(PMBSR_EL1, 4),    MRS(TPIDR_EL0, 18),
    MSR(FPCR, 0),          MSR(ICC_PMR_EL1, 9),  MRS(MDSCR_EL1, 0),     MSR(DBGBVR9_EL1, 0),
    MRS(ID_AA64DFR0_EL1, 8), MRS(CNTHCTL_EL2x, 0), MRS(PMCR_OP1_4, 0),
    0xd503201f, /* nop */
    0xd65f03c0, /* ret */
    0xd4000002, /* hvc #0: PSCI */
    0xd4000022, /* hvc #1: a hypercall to the Microsoft hypervisor */
    0xd4000082, /* hvc #4: how the Windows loader starts that hypervisor */
    0xd4000001, /* svc #0 */
    0xd4000003, /* smc #0 */
    0xd4200000, /* brk #0 */
    0xd5033fdf, /* isb */
    0xd50b7420, /* dc zva, x0 */
    0xd53b9d1f & ~0x00100000u, /* PMCCNTR_EL0 with bit 20 clear: not MRS/MSR (register) */
    0x00000000,
    0xffffffff,
};

/* A read of the cycle counter, into any register */
static bool is_ccntr_read(u32 w)
{
    return (w & ~31u) == MRS(PMCCNTR_EL0, 0);
}

/* The hvc that would stand for a read of the cycle counter, if reads of it became hvcs */
#define HVC_IMM_CCNTR_READ(rt) (0x8000 | (0x168 << 6) | 0x20 | (rt))

static bool in_named_group(u32 w)
{
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if ((w & 0xffdfffe0) == (named[i] & 0xffdfffe0))
            return true;
    /* PMEVCNTR<n> and PMEVTYPER<n>, n = 0..30: CRm 8..15, any op2 */
    u32 op1 = (w >> 16) & 7, crn = (w >> 12) & 15, crm = (w >> 8) & 15;
    return (w & 0xffd80000) == 0xd5180000 && op1 == 3 && crn == 14 && crm >= 8;
}

/* What hv_pmu_is_access() may take although it has no name: the rest of the same CRm rows */
static bool in_group_rows(u32 w)
{
    u32 op1 = (w >> 16) & 7, crn = (w >> 12) & 15, crm = (w >> 8) & 15;

    if ((w & 0xffd80000) != 0xd5180000)
        return false;
    return (op1 == 3 && crn == 9 && crm >= 12 && crm <= 14) || (op1 == 0 && crn == 9 && crm == 14) ||
           (op1 == 3 && crn == 14 && crm >= 8);
}

static void test_which_words(void)
{
    u32 accepted = 0;

    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        for (u32 rt = 0; rt < 32; rt++) {
            CHECK(hv_pmu_is_access(named[i] | rt), "read 0x%08x", named[i] | rt);
            CHECK(hv_pmu_is_access((named[i] | rt) & ~0x00200000u), "write 0x%08x", named[i] | rt);
        }

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        CHECK(!hv_pmu_is_access(others[i]), "0x%08x", others[i]);

    /* Every MRS and MSR (register) there is: 2^21 words */
    for (u32 v = 0; v < (1u << 21); v++) {
        u32 w = 0xd5100000 | ((v >> 20) << 21) | (v & 0xfffff);
        bool is = hv_pmu_is_access(w);

        if (is)
            accepted++;
        if (in_named_group(w))
            CHECK(is, "named 0x%08x not taken", w);
        if (!in_group_rows(w))
            CHECK(!is, "0x%08x taken", w);

        if (!is)
            continue;

        u32 h = hv_pmu_patch_word(w);
        CHECK(!hv_pmu_is_access(h), "0x%08x", h);

        /* A read of the cycle counter becomes a read of the virtual count, same register */
        if (is_ccntr_read(w)) {
            CHECK(h == MRS(CNTVCT_EL0, w & 31), "0x%08x -> 0x%08x is not mrs CNTVCT_EL0", w, h);
            CHECK(hv_pmu_unpatch_word(h) == 0, "0x%08x -> 0x%08x has a way back", w, h);
            continue;
        }

        CHECK((h & 0xffe0001f) == 0xd4000002, "0x%08x -> 0x%08x is not an hvc", w, h);
        CHECK(h & (0x8000u << 5), "0x%08x -> 0x%08x: bit 15 of the immediate", w, h);
        CHECK(hv_pmu_unpatch_word(h) == w, "0x%08x -> 0x%08x -> 0x%08x", w, h,
              hv_pmu_unpatch_word(h));
    }

    /* 24 + 8 + 64 register encodings, read and write, 32 general-purpose registers */
    CHECK(accepted == (24 + 8 + 64) * 2 * 32, "%u words taken", accepted);

    /* A write of the cycle counter is an hvc like the rest */
    CHECK((hv_pmu_patch_word(MSR(PMCCNTR_EL0, 5)) & 0xffe0001f) == 0xd4000002, "msr PMCCNTR_EL0");

    /*
     * Every hvc there is: the ones that stand for an access are as many as the accesses that
     * become hvcs, which is all but the 32 reads of the cycle counter
     */
    u32 back = 0;
    for (u32 imm = 0; imm < 0x10000; imm++) {
        u32 h = 0xd4000002 | (imm << 5);
        u32 w = hv_pmu_unpatch_word(h);

        if (!w)
            continue;
        back++;
        CHECK(imm & 0x8000, "hvc #0x%x", imm);
        CHECK(hv_pmu_is_access(w) && hv_pmu_patch_word(w) == h, "hvc #0x%x -> 0x%08x", imm, w);
    }
    CHECK(back == accepted - 32, "%u hvcs stand for an access, %u accesses", back, accepted);
    for (u32 rt = 0; rt < 32; rt++)
        CHECK(hv_pmu_unpatch_word(0xd4000002 | (HVC_IMM_CCNTR_READ(rt) << 5)) == 0,
              "hvc #0x%x stands for a read of the cycle counter", HVC_IMM_CCNTR_READ(rt));

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        CHECK(hv_pmu_unpatch_word(others[i]) == 0, "0x%08x", others[i]);

    /* Words that are not system instructions at all */
    u64 x = 0x9e3779b97f4a7c15UL;
    for (int i = 0; i < 4000000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        u32 w = x >> 16;
        if ((w & 0xffd00000) == 0xd5100000)
            continue;
        CHECK(!hv_pmu_is_access(w), "0x%08x", w);
    }
}

#define PAGE_WORDS 4096

static void test_patch_range(void)
{
    static u32 page[PAGE_WORDS], orig[PAGE_WORDS], copy[PAGE_WORDS], again[PAGE_WORDS];
    u64 x = 12345;

    for (int i = 0; i < PAGE_WORDS; i++) {
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        page[i] = x >> 32;
        if (hv_pmu_is_access(page[i]))
            page[i] = 0xd503201f;
    }
    /* The first and the last word, two in a row, and the neighbours that are not to change */
    page[0] = MRS(PMCCNTR_EL0, 9);
    page[1] = 0xd4000002;
    page[100] = MRS(PMCR_EL0, 8);
    page[101] = MSR(PMCR_EL0, 8);
    page[102] = MRS(CNTVCT_EL0, 8);
    page[2000] = MSR(PMCCFILTR_EL0, 31);
    page[2001] = MRS(PMBLIMITR_EL1, 0);
    page[PAGE_WORDS - 1] = MSR(PMINTENCLR_EL1, 3);
    memcpy(orig, page, sizeof(page));

    /* Counting changes nothing; the copy has every access replaced, and the page stays as it was */
    CHECK(hv_pmu_count_accesses(page, PAGE_WORDS) == 5 && !memcmp(orig, page, sizeof(page)),
          "counting the accesses");
    memset(copy, 0x5a, sizeof(copy));
    CHECK(hv_pmu_patch_range(page, copy, PAGE_WORDS) == 5, "first scan");
    CHECK(!memcmp(orig, page, sizeof(page)), "the page that was copied from has changed");
    for (int i = 0; i < PAGE_WORDS; i++) {
        bool was = i == 0 || i == 100 || i == 101 || i == 2000 || i == PAGE_WORDS - 1;

        /* The guest has a read of CNTVCT_EL0 of its own at 102, which is copied as it is */
        if (i == 0)
            CHECK(copy[i] == MRS(CNTVCT_EL0, 9), "word 0 is 0x%08x", copy[i]);
        else if (was)
            CHECK(copy[i] != orig[i] && hv_pmu_unpatch_word(copy[i]) == orig[i], "word %d", i);
        else
            CHECK(copy[i] == orig[i], "word %d is not copied as it is", i);
    }
    CHECK(hv_pmu_count_accesses(copy, PAGE_WORDS) == 0, "accesses left in the copy");
    /* What an access becomes is not replaced again */
    memset(again, 0x5a, sizeof(again));
    CHECK(hv_pmu_patch_range(copy, again, PAGE_WORDS) == 0 && !memcmp(copy, again, sizeof(copy)),
          "second scan");
    CHECK(hv_pmu_patch_range(page, again, 0) == 0 && !memcmp(copy, again, sizeof(copy)),
          "no words");

    /* Only the words it is given */
    memset(copy, 0x5a, sizeof(copy));
    CHECK(hv_pmu_patch_range(page + 1, copy + 1, 99) == 0, "words 1..99");
    CHECK(copy[0] == 0x5a5a5a5a && copy[100] == 0x5a5a5a5a && copy[1] == orig[1] &&
              copy[99] == orig[99],
          "the words outside, or the words inside");
}

/* ---- the guest ---- */

static struct exc_info before, ctx;

static void fill(int cpu)
{
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < 32; i++)
        ctx.regs[i] = 0x5a5a000000000000UL | ((u64)i << 32) | (0x1000 + i);
    ctx.cpu_id = cpu;
    ctx.elr = 0x140204354;
    before = ctx;
}

/* Execute what the instruction was replaced by; the other registers must not change */
static bool exec(int cpu, u32 insn, u64 *x)
{
    u32 h = hv_pmu_patch_word(insn);
    u32 rt = insn & 31;

    fill(cpu);
    if (!(insn & 0x00200000))
        ctx.regs[rt] = before.regs[rt] = *x;
    if (!hv_pmu_hvc(&ctx, (h >> 5) & 0xffff))
        return false;
    for (int i = 0; i < 32; i++)
        if (i != (int)rt || !(insn & 0x00200000) || rt == 31)
            CHECK(ctx.regs[i] == before.regs[i], "x%d changed by 0x%08x", i, insn);
    CHECK(ctx.elr == before.elr && ctx.spsr == before.spsr && ctx.cpu_id == before.cpu_id,
          "context changed by 0x%08x", insn);
    if (insn & 0x00200000)
        *x = ctx.regs[rt];
    return true;
}

#define RD(cpu, reg) ({ u64 _v = 0; CHECK(exec(cpu, MRS(reg, 11), &_v), "read"); _v; })
#define WR(cpu, reg, val) ({ u64 _v = (val); CHECK(exec(cpu, MSR(reg, 12), &_v), "write"); })

/*
 * The cycle counter is not answered here: a read of it is the guest reading CNTVCT_EL0 itself.
 * What is left for EL2 is that writing it, and resetting it, go through and change nothing.
 */
static void test_counter(void)
{
    hv_pmu_init();

    /* HalpTimerInitSystem writes 0 */
    WR(0, PMCCNTR_EL0, 0);
    WR(0, PMCCNTR_EL0, 0xfffffffffffffff0UL);
    u64 v = 0x1234;
    CHECK(exec(0, MSR(PMCCNTR_EL0, 31), &v), "write from xzr");

    /* PMCR_EL0.C does not stay set */
    WR(0, PMCR_EL0, 0x45);
    CHECK(RD(0, PMCR_EL0) == 0x41, "PMCR_EL0 = 0x%lx", RD(0, PMCR_EL0));

    /* Nothing replaces a read by an hvc, so the hvc that would stand for one is not ours */
    for (u32 rt = 0; rt < 32; rt++) {
        fill(0);
        CHECK(!hv_pmu_hvc(&ctx, HVC_IMM_CCNTR_READ(rt)), "hvc #0x%x answered",
              HVC_IMM_CCNTR_READ(rt));
        CHECK(!memcmp(&ctx, &before, sizeof(ctx)), "hvc #0x%x changed the context",
              HVC_IMM_CCNTR_READ(rt));
    }
}

static void test_windows(void)
{
    u64 v;

    /* winload.efi BlpArchInitialize+0x150 and the same code in bootmgfw.efi, counter off */
    hv_pmu_init();
    v = RD(0, PMCR_EL0);
    CHECK(v == 0, "PMCR_EL0 = 0x%lx at the start", v);
    WR(0, PMCNTENCLR_EL0, 0xffffffff);
    WR(0, PMOVSCLR_EL0, 0xffffffff);
    WR(0, PMINTENCLR_EL1, 0xffffffff);
    WR(0, PMCR_EL0, (v & ~0x28UL) | 0x51);
    WR(0, PMCCFILTR_EL0, 0x8000000);
    WR(0, PMCNTENSET_EL0, 0x80000000);
    WR(0, PMUSERENR_EL0, 0);
    CHECK(RD(0, PMCR_EL0) == 0x51, "PMCR_EL0 = 0x%lx", RD(0, PMCR_EL0));
    CHECK(RD(0, PMCNTENSET_EL0) == 0x80000000 && RD(0, PMCNTENCLR_EL0) == 0x80000000, "enable");
    /* A clear register clears the bits written as 1 and no others */
    WR(0, PMCNTENCLR_EL0, 0x7fffffff);
    WR(0, PMINTENSET_EL1, 0x80000000);
    WR(0, PMINTENCLR_EL1, 0x7fffffff);
    WR(0, PMOVSSET_EL0, 0x80000000);
    WR(0, PMOVSCLR_EL0, 0x7fffffff);
    CHECK(RD(0, PMCNTENSET_EL0) == 0x80000000 && RD(0, PMINTENSET_EL1) == 0x80000000 &&
              RD(0, PMOVSSET_EL0) == 0x80000000,
          "cleared by zeroes");
    WR(0, PMINTENCLR_EL1, 0x80000000);
    WR(0, PMOVSCLR_EL0, 0x80000000);
    CHECK(RD(0, PMCCFILTR_EL0) == 0x8000000, "PMCCFILTR_EL0");
    CHECK(RD(0, PMUSERENR_EL0) == 0, "PMUSERENR_EL0");
    CHECK(RD(0, PMOVSCLR_EL0) == 0 && RD(0, PMINTENSET_EL1) == 0, "overflow, interrupt");

    /* ntoskrnl.exe HalpCycleCounterInitialize, on a core where the loader did not run */
    v = RD(3, PMCR_EL0);
    CHECK(v == 0, "core 3 starts clean");
    WR(3, PMCR_EL0, (v & ~0x28UL) | 0x51);
    WR(3, PMINTENCLR_EL1, 0x80000000);
    WR(3, PMCCFILTR_EL0, 0x8000000);
    WR(3, PMCNTENSET_EL0, 0x80000000);
    v = RD(3, PMUSERENR_EL0);
    WR(3, PMUSERENR_EL0, v | 4);
    CHECK(RD(3, PMCR_EL0) == 0x51 && RD(3, PMUSERENR_EL0) == 4, "core 3");
    CHECK(RD(0, PMUSERENR_EL0) == 0, "core 0 unchanged");

    /* Set and clear pairs, and the bits a PMU without event counters does not have */
    WR(0, PMCNTENSET_EL0, 0xffffffff);
    CHECK(RD(0, PMCNTENSET_EL0) == 0x80000000, "only the cycle counter can be enabled");
    WR(0, PMCNTENCLR_EL0, 0x80000000);
    CHECK(RD(0, PMCNTENSET_EL0) == 0, "disabled");
    WR(0, PMINTENSET_EL1, 0xffffffff);
    CHECK(RD(0, PMINTENCLR_EL1) == 0x80000000, "interrupt enable");
    WR(0, PMINTENCLR_EL1, 0x80000000);
    CHECK(RD(0, PMINTENSET_EL1) == 0, "interrupt disable");
    WR(0, PMOVSSET_EL0, 0x80000001);
    CHECK(RD(0, PMOVSCLR_EL0) == 0x80000000, "overflow set");
    WR(0, PMOVSCLR_EL0, 0x80000000);
    CHECK(RD(0, PMOVSSET_EL0) == 0, "overflow clear");
    WR(0, PMCR_EL0, 0xffffffffffffffffUL);
    CHECK(RD(0, PMCR_EL0) == 0xf9, "PMCR_EL0 all ones reads 0x%lx: N, IDCODE, IMP are 0",
          RD(0, PMCR_EL0));

    /* PMXEVTYPER_EL0 is PMCCFILTR_EL0 while PMSELR_EL0.SEL is 31 */
    WR(0, PMSELR_EL0, 31);
    WR(0, PMXEVTYPER_EL0, 0xc8000000);
    CHECK(RD(0, PMCCFILTR_EL0) == 0xc8000000 && RD(0, PMXEVTYPER_EL0) == 0xc8000000, "SEL 31");
    WR(0, PMSELR_EL0, 2);
    WR(0, PMXEVTYPER_EL0, 0x11);
    CHECK(RD(0, PMXEVTYPER_EL0) == 0 && RD(0, PMCCFILTR_EL0) == 0xc8000000, "SEL 2");
    CHECK(RD(0, PMSELR_EL0) == 2, "PMSELR_EL0");

    /* What is not there reads as zero and ignores writes */
    WR(0, PMEVCNTR0_EL0, 0x1234);
    WR(0, PMEVTYPER30_EL0, 0x1234);
    WR(0, PMSWINC_EL0, 1);
    CHECK(RD(0, PMEVCNTR0_EL0) == 0 && RD(0, PMEVTYPER30_EL0) == 0 && RD(0, PMEVCNTR30_EL0) == 0,
          "event counters");
    CHECK(RD(0, PMCEID0_EL0) == 0 && RD(0, PMCEID1_EL0) == 0 && RD(0, PMMIR_EL1) == 0 &&
              RD(0, PMXEVCNTR_EL0) == 0,
          "identification");

    /* hv_pmu_init() starts over */
    hv_pmu_init();
    CHECK(RD(0, PMCR_EL0) == 0 && RD(3, PMUSERENR_EL0) == 0 && RD(0, PMCCFILTR_EL0) == 0, "reset");
}

static void test_not_ours(void)
{
    /* Immediates with bit 15 set that no access produces: op1 0 with CRn 14, CRm below 8, ... */
    static const u16 imms[] = {0x8000, 0x8001, 0x9fff, 0xa000 | (5 << 6), 0x8000 | (0x60 << 6)};

    for (unsigned i = 0; i < sizeof(imms) / sizeof(imms[0]); i++) {
        if (hv_pmu_unpatch_word(0xd4000002 | ((u32)imms[i] << 5)))
            continue; /* one of ours after all */
        fill(0);
        CHECK(!hv_pmu_hvc(&ctx, imms[i]), "hvc #0x%x answered", imms[i]);
        CHECK(!memcmp(&ctx, &before, sizeof(ctx)), "hvc #0x%x changed the context", imms[i]);
    }
    fill(0);
    CHECK(!hv_pmu_hvc(&ctx, 0x8000), "hvc #0x8000");
}

static int scan_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u32 *buf = malloc(size + 4);
    if (fread(buf, 1, size, f) != (size_t)size) {
        perror(path);
        return 1;
    }
    fclose(f);
    printf("%s: %u accesses in %ld words\n", path, hv_pmu_count_accesses(buf, size / 4), size / 4);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    test_which_words();
    test_patch_range();
    test_counter();
    test_windows();
    test_not_ours();

    printf("pmu model: %d checks, %d failed\n", checks, failures);
    for (int i = 1; i < argc; i++)
        if (scan_file(argv[i]))
            return 1;
    return failures ? 1 : 0;
}
