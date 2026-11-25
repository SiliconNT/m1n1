// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_dbg.c. The file under test is the real one, compiled
 * against the stand-in headers of this directory; this file plays the guest (the syndrome of a
 * trapped MRS or MSR, and the general-purpose registers) and the CPU (OSLSR_EL1, the one
 * register hv_dbg.c may read; any other read, and any write, fails the test).
 *
 * Which accesses are the breakpoint and watchpoint registers is written down here a second
 * time, from the encodings in Arm DDI 0487 M.d: D24.3.2 (DBGBCR<n>_EL1), D24.3.3
 * (DBGBVR<n>_EL1), D24.3.11 (DBGWCR<n>_EL1), D24.3.12 (DBGWVR<n>_EL1), D24.3.28 (OSLSR_EL1).
 * The syndrome is that of D24.2.45, "ISS encoding for an exception from MSR, MRS, or System
 * instruction execution in AArch64 state".
 */
#include <stdlib.h>
#include <string.h>

#include "cpu_regs.h"
#include "hv.h"
#include "smp.h"
#include "utils.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            if (failures <= 40) {                                                                  \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                             \
                printf(__VA_ARGS__);                                                               \
                printf("\n");                                                                      \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/* ---- the CPU ---- */

static int cur_cpu;
static u64 hw_oslsr;
static int hw_oslsr_reads, hw_other;

int smp_id(void)
{
    return cur_cpu;
}

u64 mock_mrs(int reg)
{
    if (reg == SYS_OSLSR_EL1) {
        hw_oslsr_reads++;
        return hw_oslsr;
    }
    hw_other++;
    return 0x6666666666666666UL;
}

void mock_msr(int reg, u64 val)
{
    UNUSED(reg);
    UNUSED(val);
    hw_other++;
}

void mock_wfe(void)
{
}

/* ---- the guest ---- */

/* Op0 [21:20], Op2 [19:17], Op1 [16:14], CRn [13:10], Rt [9:5], CRm [4:1], bit 0: 1 = MRS */
#define ISS(op0, op1, crn, crm, op2, rt, read)                                                     \
    (((u64)(op0) << 20) | ((u64)(op2) << 17) | ((u64)(op1) << 14) | ((u64)(crn) << 10) |           \
     ((u64)(rt) << 5) | ((u64)(crm) << 1) | ((read) ? 1 : 0))

/* op2 of the four kinds of register, each with n = CRm = 0 to 15 */
enum kind { BVR = 4, BCR = 5, WVR = 6, WCR = 7 };
static const enum kind kinds[] = {BVR, BCR, WVR, WCR};
static const char *const kind_names[] = {
    [BVR] = "DBGBVR", [BCR] = "DBGBCR", [WVR] = "DBGWVR", [WCR] = "DBGWCR"};

#define DBG(kind, n, rt, read) ISS(2, 0, 0, n, kind, rt, read)
#define OSLSR(rt, read)        ISS(2, 0, 1, 1, 4, rt, read)

static struct exc_info before, ctx;

/* The registers as the guest left them: no two alike, none a value any test writes */
static void fill(void)
{
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < 31; i++)
        ctx.regs[i] = 0x5a5a000000000000UL | ((u64)i << 32) | (0x1000 + i);
    /* What is in the slot of the stack pointer is nobody's value: Rt = 31 is the zero register */
    ctx.regs[31] = 0xdeadbeefdeadbeefUL;
    ctx.elr = 0xfffff80012345678UL;
    ctx.spsr = 0x3c5;
    ctx.cpu_id = cur_cpu;
    before = ctx;
}

/* Everything but x0 to x30 is as it was: the caller moves the PC on, not hv_dbg.c */
static bool rest_untouched(void)
{
    struct exc_info a = ctx, b = before;
    memset(a.regs, 0, sizeof(a.regs));
    memset(b.regs, 0, sizeof(b.regs));
    return !memcmp(&a, &b, sizeof(a));
}

/* x0 to x30 are as they were, except this one (-1: none) */
static bool regs_untouched_but(int rt)
{
    /* The slot after x30 too: with Rt = 31 nothing is written anywhere */
    for (int i = 0; i < 32; i++)
        if (i != rt && ctx.regs[i] != before.regs[i])
            return false;
    return true;
}

/* msr <register>, x<rt> with this value in x<rt> */
static bool guest_write(u64 iss, int rt, u64 val)
{
    fill();
    if (rt != 31)
        ctx.regs[rt] = before.regs[rt] = val;
    return hv_dbg_handle_msr(&ctx, iss);
}

/* mrs x<rt>, <register> */
static bool guest_read(u64 iss)
{
    fill();
    return hv_dbg_handle_msr(&ctx, iss);
}

/* A value for register n of a kind on a core: every bit in use, no two registers alike */
static u64 pattern(enum kind kind, int n, int cpu)
{
    return 0x8000000000000001UL | ((u64)kind << 56) | ((u64)n << 48) | ((u64)cpu << 40) |
           (0x0123456789UL ^ ((u64)(kind * 16 + n) * 0x10001));
}

/* Before the guest has written anything, every register reads as 0 */
static void fresh_tests(void)
{
    hv_dbg_init();
    for (cur_cpu = 0; cur_cpu < MAX_CPUS; cur_cpu += 3)
        for (unsigned k = 0; k < 4; k++)
            for (int n = 0; n < 16; n++) {
                int rt = (n + k) % 31;
                bool handled = guest_read(DBG(kinds[k], n, rt, 1));
                CHECK(handled, "read of %s%d on core %d", kind_names[kinds[k]], n, cur_cpu);
                CHECK(ctx.regs[rt] == 0, "%s%d on core %d reads 0x%lx", kind_names[kinds[k]], n,
                      cur_cpu, ctx.regs[rt]);
                CHECK(regs_untouched_but(rt) && rest_untouched(), "%s%d", kind_names[kinds[k]], n);
            }
}

/* What is written to a register is what is read from it, and from no other */
static void keep_tests(void)
{
    hv_dbg_init();
    cur_cpu = 6;
    for (unsigned k = 0; k < 4; k++)
        for (int n = 0; n < 16; n++) {
            int rt = (3 * n + k) % 31;
            bool handled = guest_write(DBG(kinds[k], n, rt, 0), rt, pattern(kinds[k], n, 6));
            CHECK(handled, "write of %s%d", kind_names[kinds[k]], n);
            /* A write changes nothing the guest can see */
            CHECK(regs_untouched_but(-1) && rest_untouched(), "%s%d", kind_names[kinds[k]], n);
        }
    for (unsigned k = 0; k < 4; k++)
        for (int n = 0; n < 16; n++) {
            int rt = (5 * n + k + 1) % 31;
            bool handled = guest_read(DBG(kinds[k], n, rt, 1));
            CHECK(handled, "read of %s%d", kind_names[kinds[k]], n);
            CHECK(ctx.regs[rt] == pattern(kinds[k], n, 6), "%s%d reads 0x%lx, written 0x%lx",
                  kind_names[kinds[k]], n, ctx.regs[rt], pattern(kinds[k], n, 6));
            CHECK(regs_untouched_but(rt) && rest_untouched(), "%s%d", kind_names[kinds[k]], n);
        }

    /* The last write counts */
    guest_write(DBG(BCR, 3, 9, 0), 9, 0x1e5);
    guest_write(DBG(BCR, 3, 10, 0), 10, 0);
    guest_read(DBG(BCR, 3, 11, 1));
    CHECK(ctx.regs[11] == 0, "DBGBCR3 reads 0x%lx after 0 was written", ctx.regs[11]);
}

/* Each core has registers of its own */
static void per_core_tests(void)
{
    hv_dbg_init();
    for (cur_cpu = 0; cur_cpu < MAX_CPUS; cur_cpu++) {
        /* What the cores before this one wrote is not here */
        CHECK(guest_read(DBG(BVR, 2, 4, 1)) && ctx.regs[4] == 0,
              "DBGBVR2 on core %d reads 0x%lx before the core wrote it", cur_cpu, ctx.regs[4]);
        for (unsigned k = 0; k < 4; k++)
            for (int n = 0; n < 16; n++)
                guest_write(DBG(kinds[k], n, 1, 0), 1, pattern(kinds[k], n, cur_cpu));
    }
    for (cur_cpu = 0; cur_cpu < MAX_CPUS; cur_cpu++)
        for (unsigned k = 0; k < 4; k++)
            for (int n = 0; n < 16; n++) {
                guest_read(DBG(kinds[k], n, 2, 1));
                CHECK(ctx.regs[2] == pattern(kinds[k], n, cur_cpu), "%s%d on core %d reads 0x%lx",
                      kind_names[kinds[k]], n, cur_cpu, ctx.regs[2]);
            }
}

/* A core that starts the guest again has them as after a reset; the others keep theirs */
static void init_cpu_tests(void)
{
    static const int cores[] = {0, 5, 6, MAX_CPUS - 1};
    const unsigned nr = sizeof(cores) / sizeof(cores[0]);

    for (unsigned again = 0; again < nr; again++) {
        hv_dbg_init();
        for (unsigned c = 0; c < nr; c++) {
            cur_cpu = cores[c];
            for (unsigned k = 0; k < 4; k++)
                for (int n = 0; n < 16; n++)
                    guest_write(DBG(kinds[k], n, 1, 0), 1, pattern(kinds[k], n, cores[c]));
        }
        hv_dbg_init_cpu(cores[again]);
        for (unsigned c = 0; c < nr; c++) {
            cur_cpu = cores[c];
            for (unsigned k = 0; k < 4; k++)
                for (int n = 0; n < 16; n++) {
                    u64 want = c == again ? 0 : pattern(kinds[k], n, cores[c]);

                    guest_read(DBG(kinds[k], n, 2, 1));
                    CHECK(ctx.regs[2] == want, "%s%d on core %d reads 0x%lx, not 0x%lx",
                          kind_names[kinds[k]], n, cur_cpu, ctx.regs[2], want);
                }
        }
    }
}

/* Rt = 31 is the zero register (D24.2.45, Rt): it reads as 0 and a read into it is dropped */
static void zero_register_tests(void)
{
    hv_dbg_init();
    cur_cpu = 6;
    guest_write(DBG(WVR, 1, 7, 0), 7, 0xfffff80011112220UL);
    CHECK(guest_write(DBG(WVR, 1, 31, 0), 31, 0), "msr DBGWVR1_EL1, xzr");
    CHECK(regs_untouched_but(-1) && rest_untouched(), "msr DBGWVR1_EL1, xzr");
    guest_read(DBG(WVR, 1, 8, 1));
    CHECK(ctx.regs[8] == 0, "DBGWVR1 reads 0x%lx after xzr was written", ctx.regs[8]);

    guest_write(DBG(BVR, 0, 7, 0), 7, 0xfffff80033334440UL);
    CHECK(guest_read(DBG(BVR, 0, 31, 1)), "mrs xzr, DBGBVR0_EL1");
    CHECK(regs_untouched_but(-1) && rest_untouched(), "mrs xzr, DBGBVR0_EL1");
    guest_read(DBG(BVR, 0, 8, 1));
    CHECK(ctx.regs[8] == 0xfffff80033334440UL, "DBGBVR0 reads 0x%lx", ctx.regs[8]);
}

/* OSLSR_EL1 is the CPU's own: a read gets what the register says, a write is not answered */
static void oslsr_tests(void)
{
    static const u64 values[] = {0x8, 0xa, 0};

    hv_dbg_init();
    cur_cpu = 6;
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        hw_oslsr = values[i];
        hw_oslsr_reads = 0;
        CHECK(guest_read(OSLSR(12, 1)), "mrs x12, OSLSR_EL1");
        CHECK(ctx.regs[12] == values[i], "OSLSR_EL1 reads 0x%lx, the CPU says 0x%lx", ctx.regs[12],
              values[i]);
        CHECK(hw_oslsr_reads == 1, "%d reads of the register", hw_oslsr_reads);
        CHECK(regs_untouched_but(12) && rest_untouched(), "mrs x12, OSLSR_EL1");
    }
    hw_oslsr_reads = 0;
    CHECK(guest_read(OSLSR(31, 1)) && regs_untouched_but(-1), "mrs xzr, OSLSR_EL1");

    /* The register is read-only: a write is for whoever handles what this file does not */
    CHECK(!guest_write(OSLSR(3, 0), 3, 0), "msr OSLSR_EL1, x3 was answered");
    CHECK(!memcmp(&ctx, &before, sizeof(ctx)), "msr OSLSR_EL1, x3 changed the context");
}

/*
 * Every other encoding there is, in both directions, is somebody else's: not answered, and
 * nothing changed. Then the registers still hold what was written before.
 */
static void not_ours_tests(void)
{
    int answered = 0, expected = 0;

    hv_dbg_init();
    cur_cpu = 6;
    for (unsigned k = 0; k < 4; k++)
        for (int n = 0; n < 16; n++)
            guest_write(DBG(kinds[k], n, 1, 0), 1, pattern(kinds[k], n, 6));

    for (int op0 = 0; op0 < 4; op0++)
        for (int op1 = 0; op1 < 8; op1++)
            for (int crn = 0; crn < 16; crn++)
                for (int crm = 0; crm < 16; crm++)
                    for (int op2 = 0; op2 < 8; op2++)
                        for (int read = 0; read < 2; read++) {
                            bool ours = op0 == 2 && op1 == 0 && crn == 0 && op2 >= 4;
                            bool oslsr =
                                op0 == 2 && op1 == 0 && crn == 1 && crm == 1 && op2 == 4 && read;
                            bool handled;

                            fill();
                            handled =
                                hv_dbg_handle_msr(&ctx, ISS(op0, op1, crn, crm, op2, 5, read));
                            answered += handled;
                            expected += ours || oslsr;
                            if (ours || oslsr)
                                continue;
                            CHECK(!handled, "s%d_%d_c%d_c%d_%d (%s) was answered", op0, op1, crn,
                                  crm, op2, read ? "mrs" : "msr");
                            CHECK(!memcmp(&ctx, &before, sizeof(ctx)),
                                  "s%d_%d_c%d_c%d_%d (%s) changed the context", op0, op1, crn, crm,
                                  op2, read ? "mrs" : "msr");
                        }
    /* 4 kinds, 16 of each, two directions; and the read of OSLSR_EL1 */
    CHECK(expected == 4 * 16 * 2 + 1, "%d", expected);
    CHECK(answered == expected, "%d encodings answered, not %d", answered, expected);

    /* Bits of the syndrome above Op0 are not part of the register (RES0, bits [24:22]) */
    CHECK(guest_read(DBG(BVR, 5, 3, 1) | (7UL << 22)), "bits [24:22] of the syndrome set");

    /* The MSRs in the sweep above wrote x5 as it was after fill() */
    fill();
    for (unsigned k = 0; k < 4; k++)
        for (int n = 0; n < 16; n++) {
            guest_read(DBG(kinds[k], n, 2, 1));
            CHECK(ctx.regs[2] == before.regs[5], "%s%d reads 0x%lx", kind_names[kinds[k]], n,
                  ctx.regs[2]);
        }
}

int main(void)
{
    fresh_tests();
    keep_tests();
    per_core_tests();
    init_cpu_tests();
    zero_register_tests();
    oslsr_tests();
    not_ours_tests();

    /* Nothing of the guest's goes into the CPU, and nothing but OSLSR_EL1 is read from it */
    CHECK(hw_other == 0, "%d accesses to registers of the CPU other than reads of OSLSR_EL1",
          hw_other);

    hv_dbg_report();

    printf("dbg_test: %d checks, %d failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
