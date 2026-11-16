/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/hv/hv_xtrack.c: which page is scanned, and which page changes
 * state, when the guest executes or writes somewhere. And for src/hv/hv_xoprobe.c: what the
 * calls of the execute-only probe answer, and what becomes of a data abort while it is on.
 * And for the count of reads in hv_xtrack.c: which pages become execute-only, what a read of
 * one gives the guest, and when a page that was lent is taken back.
 *
 * The guest (Windows) has 4 KB pages. A page that runs is scanned and the accesses to PMUv3
 * registers in it are replaced (the real src/hv/hv_pmu.c, linked in). The pages next to it
 * are somebody else's, code or data, and are left alone: that is what is checked here, with
 * eight pages of memory that are all full of such accesses.
 *
 * The Makefile drops the #include lines of hv_xtrack.c and hv_xoprobe.c; this file stands in
 * for them and for the page tables: one state for each 4 KB page.
 */

#include <stdarg.h>
#include <string.h>

#include "hv.h"
#include "arm_cpu_regs.h"
#include "hv_xpage.h" /* HV_XPAGE_* and HV_XTRACK_* of the real hv.h, see the Makefile */
#include "smp.h"

/* The registers the two files read: the count and rate of the timer, and HPFAR_EL2 */
static u64 test_CNTPCT_EL0, test_CNTFRQ_EL0, test_HPFAR_EL2;
#define mrs(reg)  (test_##reg)
#define sysop(op) ((void)0)

#define ARENA_IPA 0x10100000000UL
#define NR_PAGES  8
#define WORDS     1024 /* in a page */

#define MRS_PMCCNTR 0xd53b9d00 /* mrs x0, PMCCNTR_EL0 */
#define MRS_CNTVCT  0xd53be040 /* mrs x0, CNTVCT_EL0 */

static u32 arena[NR_PAGES * WORDS] __attribute__((aligned(0x4000)));
static int state[NR_PAGES];
static u64 test_esr, test_far;
static bool test_xtrack_on = true;
static u64 test_spsr = 5; /* EL1h */

static void dc_cvau_range(void *addr, size_t size)
{
    UNUSED(addr);
    UNUSED(size);
}

static void ic_ialluis(void)
{
}

static u64 hv_get_esr(void)
{
    return test_esr;
}

static u64 hv_get_far(void)
{
    return test_far;
}

static u64 hv_get_spsr(void)
{
    return test_spsr;
}

static u64 hv_translate(u64 addr, bool s1, bool w, u64 *par_out)
{
    UNUSED(s1);
    UNUSED(w);
    UNUSED(par_out);
    return addr;
}

static bool hv_pt_xtrack_on(void)
{
    return test_xtrack_on;
}

static int page_of(u64 ipa)
{
    if (ipa < ARENA_IPA || ipa >= ARENA_IPA + NR_PAGES * 0x1000UL)
        return -1;
    return (ipa - ARENA_IPA) >> 12;
}

static int hv_pt_scanned(u64 ipa, u64 *pa)
{
    int n = page_of(ipa);

    if (n < 0)
        return -1;
    *pa = (u64)&arena[n * WORDS];
    return state[n];
}

static int hv_pt_set_scanned(u64 ipa, int st)
{
    int n = page_of(ipa);

    if (n < 0)
        return -1;
    state[n] = st;
    return 0;
}

void hv_xtrack_close(void);
bool hv_xtrack_counts_reads(void);

#include "hv_xoprobe.inc"
#include "hv_xtrack.inc"

static unsigned long checks, failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    failures++;
    printf("FAIL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define IPA(page, word) (ARENA_IPA + (page) * 0x1000UL + (word) * 4UL)

static u32 original(int i)
{
    return MRS_PMCCNTR | (i % 31);
}

/* How many words of a page are no longer what was written there */
static int changed(int page)
{
    int n = 0;

    for (int i = 0; i < WORDS; i++)
        if (arena[page * WORDS + i] != original(i))
            n++;
    return n;
}

/* Every page but this one is as it was written and has never been scanned */
static void others_untouched(int page, const char *what)
{
    for (int n = 0; n < NR_PAGES; n++)
        if (n != page)
            check(changed(n) == 0 && state[n] == HV_XPAGE_UNSCANNED,
                  "%s: page %d, which is not page %d, has %d words changed and state %d", what, n,
                  page, changed(n), state[n]);
}

static bool iabort(u64 ipa)
{
    struct exc_info ctx = {.cpu_id = 0, .elr = ipa};

    test_esr = 0x8200000f; /* Instruction Abort from a lower EL, permission fault, level 3 */
    test_far = ipa;
    return hv_xtrack_iabort(&ctx);
}

static int store(u64 ipa, u64 pc)
{
    struct exc_info ctx = {.cpu_id = 0, .elr = pc};

    test_esr = 0x9200004f; /* Data Abort from a lower EL, a write, permission fault, level 3 */
    test_far = ipa;
    return hv_xtrack_dabort(&ctx, ipa, true);
}

static int load(u64 ipa, u64 pc)
{
    struct exc_info ctx = {.cpu_id = 0, .elr = pc};

    test_esr = 0x9200000f;
    test_far = ipa;
    return hv_xtrack_dabort(&ctx, ipa, false);
}

/* A call of the probe: returns x0, or 0x7777 if the call was not taken for one of its own */
static struct exc_info xo;

static s64 xo_call(u64 fn, u64 a1, u64 a2)
{
    memset(&xo, 0, sizeof(xo));
    xo.regs[0] = fn;
    xo.regs[1] = a1;
    xo.regs[2] = a2;
    xo.elr = 0x4004;
    if (!hv_xoprobe_hvc(&xo, 0))
        return 0x7777;
    return xo.regs[0];
}

/* The nth fault that the probe has recorded is this one */
static bool xo_fault_is(u64 n, u64 count, u64 esr, u64 ipa, u64 pc, u64 flags)
{
    return xo_call(XO_FN_QUERY, n, 0) == (s64)count && xo.regs[1] == esr && xo.regs[2] == ipa &&
           xo.regs[3] == ipa && xo.regs[4] == pc && xo.regs[6] == flags;
}

static void test_xoprobe(void)
{
    /* Page 5 has executed and page 6 has not. Without the word of the host there is no probe */
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_SKIP) == 0x7777 &&
              state[5] == HV_XPAGE_SCANNED,
          "the probe answers before it is turned on: page 5 in state %d", state[5]);
    test_xtrack_on = false;
    check(hv_xoprobe_enable() < 0 && xo_call(XO_FN_RESET, 0, 0) == 0x7777,
          "the probe can be turned on while execution is not tracked");
    test_xtrack_on = true;
    check(hv_xoprobe_enable() == 0 && xo_call(XO_FN_RESET, 0, 0) == 0, "the probe is not on");

    /* What is not one of its calls is left to PSCI */
    check(xo_call(0x84000000, 0, 0) == 0x7777 && xo_call(XO_FN_QUERY + 1, 0, 0) == 0x7777 &&
              xo_call(XO_FN_RESET - 1, 0, 0) == 0x7777,
          "the probe takes a call that is not its own");
    xo.regs[0] = XO_FN_RESET;
    check(!hv_xoprobe_hvc(&xo, 1), "the probe takes an hvc #1");

    /* Only a page that has executed, and only a mode there is */
    check(xo_call(XO_FN_ARM, IPA(6, 0), HV_XOPROBE_SKIP) == XO_INVALID &&
              state[6] == HV_XPAGE_UNSCANNED,
          "a page that never ran becomes execute-only: state %d", state[6]);
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_EMULATE + 1) == XO_INVALID &&
              xo_call(XO_FN_ARM, 0x1000000000, HV_XOPROBE_SKIP) == XO_INVALID &&
              state[5] == HV_XPAGE_SCANNED,
          "ARM with a mode or a page there is not: state %d", state[5]);

    /* Nothing armed: an abort that nothing explains is not the business of the probe */
    check(load(IPA(6, 4), IPA(1, 0)) == HV_XTRACK_NONE && xo_call(XO_FN_QUERY, 0, 0) == 0,
          "a load from page 6 with no page armed");

    /* SKIP: the accesses do not happen, the page stays as it is, and each is recorded */
    check(xo_call(XO_FN_ARM, IPA(5, 0x80), HV_XOPROBE_SKIP) == 0 && state[5] == HV_XPAGE_XONLY,
          "page 5 does not become execute-only: state %d", state[5]);
    check(load(IPA(5, 64), IPA(1, 3)) == HV_XTRACK_SKIP && state[5] == HV_XPAGE_XONLY,
          "a load from the execute-only page 5 is not skipped: state %d", state[5]);
    check(xo_fault_is(0, 1, 0x9200000f, IPA(5, 64), IPA(1, 3), 0), "the load was not recorded");
    check(store(IPA(5, 65), IPA(5, 0)) == HV_XTRACK_SKIP && state[5] == HV_XPAGE_XONLY,
          "a store into the execute-only page 5 from within it is not skipped: state %d",
          state[5]);
    check(xo_fault_is(1, 2, 0x9200004f, IPA(5, 65), IPA(5, 0), XO_FLAG_WRITE) &&
              xo_fault_is(0, 2, 0x9200000f, IPA(5, 64), IPA(1, 3), 0),
          "the store was not recorded, or the load before it is gone");
    check(xo_call(XO_FN_QUERY, 2, 0) == 2 && xo.regs[1] == 0 && xo.regs[4] == 0,
          "a fault that has not happened has an ESR");
    check(iabort(IPA(5, 0)) && state[5] == HV_XPAGE_XONLY,
          "an instruction abort on the execute-only page 5: state %d", state[5]);

    /* ...and so is an abort on another page that nothing explains: FAR may be anywhere */
    check(load(IPA(6, 1023), IPA(1, 3)) == HV_XTRACK_SKIP && state[6] == HV_XPAGE_UNSCANNED &&
              xo_fault_is(2, 3, 0x9200000f, IPA(6, 1023), IPA(1, 3), XO_FLAG_STRAY),
          "a load next to the execute-only page that the page tables allow");
    check(store(IPA(4, 1023), IPA(1, 3)) == HV_XTRACK_SKIP && state[4] == HV_XPAGE_UNSCANNED &&
              xo_fault_is(3, 4, 0x9200004f, IPA(4, 1023), IPA(1, 3),
                          XO_FLAG_STRAY | XO_FLAG_WRITE),
          "a store next to the execute-only page that the page tables allow");

    /* More faults than there is room for are counted */
    for (int i = 0; i < 6; i++)
        load(IPA(5, 64), IPA(1, 3));
    check(xo_call(XO_FN_QUERY, 7, 0) == 10 && xo.regs[1] == 0x9200000f &&
              xo_call(XO_FN_QUERY, 8, 0) == 10 && xo.regs[1] == 0 &&
              xo_call(XO_FN_QUERY, ~0UL, 0) == 10 && xo.regs[1] == 0,
          "ten faults: the count or what is known of the 8th, the 9th and a silly one");
    check(xo_call(XO_FN_RESET, 0, 0) == 0 && xo_call(XO_FN_QUERY, 0, 0) == 0 && xo.regs[1] == 0,
          "RESET leaves faults behind");

    /* DEMOTE: the page is readable again, the instruction runs again, the probe is done */
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_DEMOTE) == 0 && state[5] == HV_XPAGE_XONLY,
          "ARM of a page that is execute-only already: state %d", state[5]);
    check(load(IPA(6, 1023), IPA(1, 3)) == HV_XTRACK_NONE && xo_call(XO_FN_QUERY, 0, 0) == 0,
          "a load from page 6 is taken up by the probe in the mode DEMOTE");
    check(load(IPA(5, 64), IPA(1, 3)) == HV_XTRACK_RETRY && state[5] == HV_XPAGE_SCANNED &&
              xo_fault_is(0, 1, 0x9200000f, IPA(5, 64), IPA(1, 3), 0),
          "a load from page 5 in the mode DEMOTE: state %d", state[5]);
    check(load(IPA(5, 64), IPA(1, 3)) == HV_XTRACK_NONE && xo_call(XO_FN_QUERY, 0, 0) == 1,
          "a load from page 5 after it was made readable again");
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_SKIP) == 0 &&
              xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_DEMOTE) == 0,
          "page 5 armed again");
    check(load(IPA(6, 0), IPA(1, 3)) == HV_XTRACK_NONE, "a stray load in the mode DEMOTE");
    /* A store: readable and read-only first, and from there the way of every store */
    check(store(IPA(5, 65), IPA(6, 2)) == HV_XTRACK_RETRY && state[5] == HV_XPAGE_SCANNED,
          "a store into page 5 in the mode DEMOTE: state %d", state[5]);
    check(store(IPA(5, 65), IPA(6, 2)) == HV_XTRACK_RETRY && state[5] == HV_XPAGE_UNSCANNED &&
              xo_call(XO_FN_QUERY, 0, 0) == 2,
          "the same store again: state %d", state[5]);

    /* EMULATE: EL2 is to carry the access out, the page stays, a store is scanned */
    check(iabort(IPA(5, 0)) && state[5] == HV_XPAGE_SCANNED, "page 5 runs again");
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_EMULATE) == 0 && state[5] == HV_XPAGE_XONLY,
          "page 5 armed for EMULATE: state %d", state[5]);
    check(load(IPA(5, 64), IPA(1, 3)) == HV_XTRACK_EMULATE &&
              store(IPA(5, 65), IPA(1, 3)) == HV_XTRACK_EMULATE && state[5] == HV_XPAGE_XONLY,
          "a load and a store in the mode EMULATE: state %d", state[5]);
    arena[5 * WORDS + 65] = MRS_PMCCNTR | 7;
    hv_xtrack_stored(IPA(5, 65));
    check(arena[5 * WORDS + 65] == (MRS_CNTVCT | 7),
          "an access stored into the execute-only page 5 stays: 0x%08x", arena[5 * WORDS + 65]);

    /* DISARM, twice, and of a page that was never armed */
    check(xo_call(XO_FN_DISARM, IPA(5, 9), 0) == 0 && state[5] == HV_XPAGE_SCANNED,
          "DISARM: state %d", state[5]);
    check(xo_call(XO_FN_DISARM, IPA(5, 9), 0) == 0 && xo_call(XO_FN_DISARM, IPA(6, 0), 0) == 0 &&
              state[5] == HV_XPAGE_SCANNED && state[6] == HV_XPAGE_UNSCANNED,
          "DISARM of pages that are not armed: states %d and %d", state[5], state[6]);
    check(xo_call(XO_FN_ARM, IPA(5, 0), HV_XOPROBE_SKIP) == 0 &&
              xo_call(XO_FN_DISARM, IPA(5, 0), 0) == 0 &&
              load(IPA(6, 0), IPA(1, 3)) == HV_XTRACK_NONE,
          "a stray load in the mode SKIP after the only armed page was disarmed");

    /* Room for four pages */
    for (int n = 0; n < 5; n++)
        check(iabort(IPA(n, 0)), "page %d does not run", n);
    for (int n = 0; n < 4; n++)
        check(xo_call(XO_FN_ARM, IPA(n, 0), HV_XOPROBE_SKIP) == 0 && state[n] == HV_XPAGE_XONLY,
              "page %d is not armed", n);
    check(xo_call(XO_FN_ARM, IPA(4, 0), HV_XOPROBE_SKIP) == XO_INVALID &&
              state[4] == HV_XPAGE_SCANNED,
          "a fifth page is armed: state %d", state[4]);
    check(xo_call(XO_FN_DISARM, IPA(1, 0), 0) == 0 &&
              xo_call(XO_FN_ARM, IPA(4, 0), HV_XOPROBE_SKIP) == 0 && state[4] == HV_XPAGE_XONLY &&
              state[1] == HV_XPAGE_SCANNED,
          "one disarmed and another armed: states %d and %d", state[1], state[4]);
}

/* ---- the count of reads of pages with replaced instructions ---- */

#define I_NOP 0xd503201f

/* ESR_EL2 of a load that the syndrome describes: 1 << sas bytes into register rt */
static u64 syn(int sas, bool sse, int rt, bool sf)
{
    return 0x9200000f | BIT(24) | ((u64)sas << 22) | (sse ? BIT(21) : 0) | ((u64)rt << 16) |
           (sf ? BIT(15) : 0);
}

/* A load that ESR_EL2 does not describe: a pair, say */
#define NOSYN 0x9200000fUL

static struct exc_info rd;

/* A load at ipa by the instruction at pc, with this ESR_EL2. The registers are in rd */
static int xo_load(u64 ipa, u64 pc, u64 esr)
{
    for (int i = 0; i < 32; i++)
        rd.regs[i] = 0x5a5a000000000000UL + i;
    rd.cpu_id = 0;
    rd.elr = pc;
    test_esr = esr;
    test_far = ipa;
    return hv_xtrack_dabort(&rd, ipa, false);
}

static u8 *bytes(int page)
{
    return (u8 *)&arena[page * WORDS];
}

static void test_count_reads(void)
{
    hv_pmu_init();
    hv_xtrack_init();
    hv_xoprobe_init();
    test_CNTFRQ_EL0 = 1000;
    test_CNTPCT_EL0 = 0;
    for (int n = 0; n < NR_PAGES; n++) {
        state[n] = HV_XPAGE_UNSCANNED;
        for (int i = 0; i < WORDS; i++)
            arena[n * WORDS + i] = I_NOP;
    }
    /* Pages 2 and 3 have an access each, page 4 a read of CNTVCT_EL0 of the guest's own */
    arena[2 * WORDS + 6] = MRS_PMCCNTR | 3;
    arena[3 * WORDS + 6] = MRS_PMCCNTR | 4;
    arena[4 * WORDS + 6] = MRS_CNTVCT | 5;
    arena[2 * WORDS + 8] = 0x44332211;
    arena[2 * WORDS + 9] = 0x88776655;
    arena[2 * WORDS + 10] = 0x80000001;
    arena[2 * WORDS + 11] = 0x7f81f00d;

    /* Said off, nothing is different: a page with an access becomes read-only */
    check(!hv_xtrack_counts_reads() && iabort(IPA(3, 0)) && state[3] == HV_XPAGE_SCANNED &&
              load(IPA(3, 8), IPA(1, 0)) == HV_XTRACK_NONE,
          "reads are counted before anybody said so: state %d", state[3]);
    state[3] = HV_XPAGE_UNSCANNED;
    arena[3 * WORDS + 6] = MRS_PMCCNTR | 4;

    hv_xtrack_count_reads(true);
    hv_xtrack_init();
    check(hv_xtrack_counts_reads(), "hv_xtrack_init() turned the count off");
    check(hv_xoprobe_enable() < 0, "the probe can be turned on while reads are counted");

    /* Which pages */
    check(iabort(IPA(1, 0)) && state[1] == HV_XPAGE_SCANNED,
          "a page with nothing to replace: state %d", state[1]);
    check(iabort(IPA(4, 0)) && state[4] == HV_XPAGE_SCANNED,
          "a page with a read of CNTVCT_EL0 that was never replaced: state %d", state[4]);
    check(iabort(IPA(2, 0)) && state[2] == HV_XPAGE_XONLY &&
              arena[2 * WORDS + 6] == (MRS_CNTVCT | 3),
          "a page with an access: state %d, the access now 0x%08x", state[2],
          arena[2 * WORDS + 6]);
    check(rc_stats.armed == 1 && rc_stats.armed_max == 1 && rc_page(IPA(2, 0), false) &&
              rc_page(IPA(2, 0), false)->arms == 1 && !rc_page(IPA(1, 0), false),
          "one page is execute-only: %lu now, %lu at most", rc_stats.armed, rc_stats.armed_max);
    check(iabort(IPA(2, 4)) && state[2] == HV_XPAGE_XONLY,
          "an instruction abort on the execute-only page: state %d", state[2]);

    /* Loads that ESR_EL2 describes are carried out, and the page stays as it is */
    check(xo_load(IPA(2, 8), IPA(1, 3), syn(3, false, 5, true)) == HV_XTRACK_SKIP &&
              rd.regs[5] == 0x8877665544332211UL && rd.regs[4] == 0x5a5a000000000004UL &&
              state[2] == HV_XPAGE_XONLY,
          "8 bytes into x5: 0x%lx, state %d", rd.regs[5], state[2]);
    check(xo_load(IPA(2, 10), IPA(1, 3), syn(2, true, 0, true)) == HV_XTRACK_SKIP &&
              rd.regs[0] == 0xffffffff80000001UL,
          "4 bytes, sign-extended into x0: 0x%lx", rd.regs[0]);
    check(xo_load(IPA(2, 10), IPA(1, 3), syn(2, false, 30, false)) == HV_XTRACK_SKIP &&
              rd.regs[30] == 0x80000001UL,
          "4 bytes into w30: 0x%lx", rd.regs[30]);
    check(xo_load(IPA(2, 11) + 2, IPA(1, 3), syn(0, true, 7, false)) == HV_XTRACK_SKIP &&
              rd.regs[7] == 0xffffff81UL,
          "1 byte, sign-extended into w7: 0x%lx", rd.regs[7]);
    check(xo_load(IPA(2, 11) + 3, IPA(1, 3), syn(0, true, 7, true)) == HV_XTRACK_SKIP &&
              rd.regs[7] == 0x7f,
          "1 byte that is not negative, sign-extended into x7: 0x%lx", rd.regs[7]);
    check(xo_load(IPA(2, 11), IPA(1, 3), syn(1, true, 8, true)) == HV_XTRACK_SKIP &&
              rd.regs[8] == 0xfffffffffffff00dUL,
          "2 bytes, sign-extended into x8: 0x%lx", rd.regs[8]);
    check(xo_load(IPA(2, 11), IPA(1, 3), syn(1, false, 8, false)) == HV_XTRACK_SKIP &&
              rd.regs[8] == 0xf00d,
          "2 bytes into w8: 0x%lx", rd.regs[8]);
    check(xo_load(IPA(2, 8), IPA(1, 3), syn(3, false, 31, true)) == HV_XTRACK_SKIP &&
              rd.regs[31] == 0x5a5a00000000001fUL,
          "8 bytes into the zero register: slot 31 is now 0x%lx", rd.regs[31]);
    /* What is in the page now, the replaced instruction included */
    check(xo_load(IPA(2, 6), IPA(1, 3), syn(2, false, 1, false)) == HV_XTRACK_SKIP &&
              rd.regs[1] == (MRS_CNTVCT | 3),
          "the replaced instruction read as data: 0x%lx", rd.regs[1]);
    /* The last 8 bytes of the page, and its first byte */
    bytes(2)[0xfff] = 0xc3;
    bytes(2)[0] = 0x3c;
    check(xo_load(IPA(2, 1022), IPA(1, 3), syn(3, false, 2, true)) == HV_XTRACK_SKIP &&
              rd.regs[2] >> 56 == 0xc3 && state[2] == HV_XPAGE_XONLY,
          "the last 8 bytes of the page: 0x%lx", rd.regs[2]);
    check(xo_load(IPA(2, 0), IPA(1, 3), syn(0, false, 2, false)) == HV_XTRACK_SKIP &&
              rd.regs[2] == 0x3c,
          "the first byte of the page: 0x%lx", rd.regs[2]);
    check(rc_stats.reads == 11 && rc_stats.carried == 11 && rc_stats.lent == 0 &&
              rc_stats.same == 0 && rc_stats.el0 == 0 && rc_page(IPA(2, 0), false)->reads == 11,
          "11 reads carried out: %lu counted, %lu carried out, %lu for the page", rc_stats.reads,
          rc_stats.carried, rc_page(IPA(2, 0), false)->reads);

    /* By instruction, by where it came from, by time */
    check(rc_pc(IPA(1, 3))->reads == 11 && rc_pc(IPA(1, 3))->flags == RC_PC_CARRIED &&
              rc_pc(IPA(1, 3))->far == IPA(2, 0) &&
              rc_pc(IPA(1, 3))->esr == syn(0, false, 2, false),
          "the instruction in page 1: %lu reads, flags %u", rc_pc(IPA(1, 3))->reads,
          rc_pc(IPA(1, 3))->flags);
    test_spsr = 0; /* EL0t */
    test_CNTPCT_EL0 = 12 * 1000;
    check(xo_load(IPA(2, 8), IPA(2, 0), syn(3, false, 0, true)) == HV_XTRACK_SKIP &&
              rc_stats.el0 == 1 && rc_stats.same == 1 &&
              rc_pc(IPA(2, 0))->flags == (RC_PC_CARRIED | RC_PC_EL0 | RC_PC_SAME) &&
              rc_slots[0].carried == 11 && rc_slots[2].carried == 1,
          "a read at EL0 of the page the instruction is in, 12 s into the run: flags %u, slots "
          "%u %u",
          rc_pc(IPA(2, 0))->flags, rc_slots[0].carried, rc_slots[2].carried);
    test_spsr = 5;
    test_CNTPCT_EL0 = (u64)RC_SLOTS * RC_SLOT_SECONDS * 1000;
    check(xo_load(IPA(2, 8), IPA(1, 3), syn(3, false, 0, true)) == HV_XTRACK_SKIP &&
              rd.regs[0] == 0x8877665544332211UL && rc_stats.late == 1,
          "a read after the last slot is carried out and counted as late: %lu", rc_stats.late);
    test_CNTPCT_EL0 = 0;

    /* What ESR_EL2 does not describe: the page is lent, and taken back two entries later */
    check(xo_load(IPA(2, 8), IPA(1, 5), NOSYN) == HV_XTRACK_RETRY &&
              state[2] == HV_XPAGE_SCANNED && rd.regs[0] == 0x5a5a000000000000UL &&
              rc_stats.lent == 1 && rc_stats.lent_nosyn == 1 &&
              rc_pc(IPA(1, 5))->flags == RC_PC_LENT && rc_slots[0].lent == 1,
          "a load without a syndrome: state %d", state[2]);
    hv_xtrack_close();
    check(state[2] == HV_XPAGE_SCANNED && rc_stats.rearmed == 0,
          "the lent page is taken back at the next entry: state %d", state[2]);
    hv_xtrack_close();
    check(state[2] == HV_XPAGE_XONLY && rc_stats.rearmed == 1 && rc_stats.armed == 1 &&
              rc_page(IPA(2, 0), false)->arms == 1,
          "the lent page is not taken back at the entry after: state %d", state[2]);

    /* An access that may have begun in the page before, or goes on into the next */
    check(xo_load(IPA(2, 0), IPA(1, 3), syn(3, false, 2, true)) == HV_XTRACK_RETRY &&
              state[2] == HV_XPAGE_SCANNED && rc_stats.lent_edge == 1,
          "8 bytes at the first byte of the page: state %d", state[2]);
    hv_xtrack_close();
    hv_xtrack_close();
    check(xo_load(IPA(2, 1023), IPA(1, 3), syn(3, false, 2, true)) == HV_XTRACK_RETRY &&
              state[2] == HV_XPAGE_SCANNED && rc_stats.lent_edge == 2,
          "8 bytes from the last 4 of the page: state %d", state[2]);
    hv_xtrack_close();
    hv_xtrack_close();
    check(state[2] == HV_XPAGE_XONLY, "page 2 is not execute-only again: state %d", state[2]);

    /* One instruction that reads two such pages gets both */
    check(iabort(IPA(3, 0)) && state[3] == HV_XPAGE_XONLY && rc_stats.armed == 2,
          "page 3 runs: state %d", state[3]);
    check(xo_load(IPA(2, 1023), IPA(1, 9), NOSYN) == HV_XTRACK_RETRY, "a pair across two pages");
    hv_xtrack_close();
    check(xo_load(IPA(3, 0), IPA(1, 9), NOSYN) == HV_XTRACK_RETRY &&
              state[2] == HV_XPAGE_SCANNED && state[3] == HV_XPAGE_SCANNED,
          "the second page of the pair: states %d and %d", state[2], state[3]);
    hv_xtrack_close();
    check(state[2] == HV_XPAGE_XONLY && state[3] == HV_XPAGE_SCANNED,
          "one entry later: states %d and %d", state[2], state[3]);
    hv_xtrack_close();
    check(state[3] == HV_XPAGE_XONLY && rc_stats.armed == 2 && rc_stats.armed_max == 2,
          "two entries later: state %d", state[3]);

    /* More pages lent than there is room for: the oldest goes back */
    for (int i = 0; i < RC_MAX_LENT + 3; i++) {
        state[3] = HV_XPAGE_XONLY;
        xo_load(IPA(3, 8), IPA(1, 9), NOSYN);
    }
    check(rc_nr_lent == RC_MAX_LENT && state[3] == HV_XPAGE_SCANNED,
          "%d pages lent at once: %u on the list", RC_MAX_LENT + 3, rc_nr_lent);
    hv_xtrack_close();
    hv_xtrack_close();
    check(rc_nr_lent == 0 && state[3] == HV_XPAGE_XONLY, "the list is not empty: %u", rc_nr_lent);

    /* A store from elsewhere: writable, as for a read-only page, and when it runs again... */
    u64 arms = rc_page(IPA(2, 0), false)->arms;
    check(store(IPA(2, 8), IPA(1, 0)) == HV_XTRACK_RETRY && state[2] == HV_XPAGE_UNSCANNED &&
              rc_stats.armed == 1 && rc_stats.writable == 1,
          "a store into the execute-only page 2 from page 1: state %d", state[2]);
    check(iabort(IPA(2, 0)) && state[2] == HV_XPAGE_XONLY && rc_stats.armed == 2 &&
              rc_page(IPA(2, 0), false)->arms == arms + 1,
          "...execute-only again, though nothing was replaced this time: state %d", state[2]);
    /* ...unless what was replaced is gone */
    check(store(IPA(2, 6), IPA(1, 0)) == HV_XTRACK_RETRY, "a second store into page 2");
    arena[2 * WORDS + 6] = I_NOP;
    check(iabort(IPA(2, 0)) && state[2] == HV_XPAGE_SCANNED && rc_stats.armed == 1,
          "page 2 with the replaced instruction overwritten: state %d", state[2]);
    check(xo_load(IPA(2, 8), IPA(1, 3), syn(3, false, 5, true)) == HV_XTRACK_NONE,
          "a load from page 2, which is readable, is taken for one that is counted");

    /* A lent page that is written to is not taken back */
    check(xo_load(IPA(3, 8), IPA(1, 9), NOSYN) == HV_XTRACK_RETRY &&
              store(IPA(3, 8), IPA(1, 0)) == HV_XTRACK_RETRY && state[3] == HV_XPAGE_UNSCANNED,
          "page 3 lent and then written to: state %d", state[3]);
    u64 rearmed = rc_stats.rearmed;
    hv_xtrack_close();
    hv_xtrack_close();
    check(state[3] == HV_XPAGE_UNSCANNED && rc_stats.rearmed == rearmed && rc_stats.armed == 0,
          "...and is taken back all the same: state %d", state[3]);

    /* A store from within: carried out by EL2, and the page is what the scan after it says */
    check(iabort(IPA(3, 0)) && state[3] == HV_XPAGE_XONLY, "page 3 runs again");
    check(store(IPA(3, 8), IPA(3, 0)) == HV_XTRACK_EMULATE && state[3] == HV_XPAGE_XONLY,
          "a store into the execute-only page 3 from within: state %d", state[3]);
    hv_xtrack_stored(IPA(3, 8));
    check(state[3] == HV_XPAGE_XONLY, "...after which its state is %d", state[3]);
    arena[3 * WORDS + 6] = I_NOP;
    hv_xtrack_stored(IPA(3, 6));
    check(state[3] == HV_XPAGE_SCANNED && rc_stats.armed == 0,
          "a store from within that overwrites the replaced instruction: state %d", state[3]);
    check(store(IPA(1, 5), IPA(1, 0)) == HV_XTRACK_EMULATE, "a store into page 1 from within");
    arena[1 * WORDS + 5] = MRS_PMCCNTR | 9;
    hv_xtrack_stored(IPA(1, 5));
    check(state[1] == HV_XPAGE_XONLY && arena[1 * WORDS + 5] == (MRS_CNTVCT | 9) &&
              rc_stats.armed == 1,
          "a store from within that writes an access into page 1: state %d", state[1]);

    /* A page opened for a store EL2 cannot carry out is execute-only again when it is closed */
    arms = rc_page(IPA(1, 0), false)->arms;
    check(hv_xtrack_open(IPA(1, 8), 0x28810520) && state[1] == HV_XPAGE_OPEN, "page 1 opened");
    hv_xtrack_close();
    check(state[1] == HV_XPAGE_XONLY && rc_page(IPA(1, 0), false)->arms == arms &&
              rc_stats.armed == 1,
          "page 1 closed: state %d", state[1]);

    /* No room in the table: the page stays readable, and that is counted */
    for (u32 i = 0; i < RC_MAX_PAGES; i++)
        if (!rc_pages[i].page)
            rc_pages[i].page = 0x7000000000UL + i * 0x1000UL;
    arena[5 * WORDS + 6] = MRS_PMCCNTR | 1;
    check(iabort(IPA(5, 0)) && state[5] == HV_XPAGE_SCANNED && rc_stats.pages_lost == 1 &&
              arena[5 * WORDS + 6] == (MRS_CNTVCT | 1),
          "a page with an access and no room in the table: state %d, %lu lost", state[5],
          rc_stats.pages_lost);

    /* Said off again */
    hv_xtrack_count_reads(false);
    hv_xtrack_init();
    state[6] = HV_XPAGE_UNSCANNED;
    arena[6 * WORDS + 6] = MRS_PMCCNTR | 2;
    check(iabort(IPA(6, 0)) && state[6] == HV_XPAGE_SCANNED && hv_xoprobe_enable() == 0,
          "the count said off: state %d", state[6]);
}

int main(void)
{
    struct exc_info ctx = {.cpu_id = 0};

    hv_pmu_init();
    hv_xtrack_init();
    for (int i = 0; i < NR_PAGES * WORDS; i++)
        arena[i] = original(i % WORDS);

    /* Pages 4 to 7 share 16 KB. Page 5 runs */
    check(iabort(IPA(5, 16)), "the first instruction of page 5 is not fetched again");
    check(changed(5) == WORDS && arena[5 * WORDS + 3] == (MRS_CNTVCT | 3),
          "page 5 ran and %d of its %d words were replaced", changed(5), WORDS);
    check(state[5] == HV_XPAGE_SCANNED, "page 5 ran and its state is %d", state[5]);
    others_untouched(5, "page 5 ran");

    check(iabort(IPA(5, 900)) && state[5] == HV_XPAGE_SCANNED,
          "a second fault on page 5, which is executable already");

    /* Code in page 6 writes to page 5: page 5 becomes writable, the store is not carried out here */
    check(store(IPA(5, 4), IPA(6, 2)) == HV_XTRACK_RETRY && state[5] == HV_XPAGE_UNSCANNED,
          "a store into page 5 from page 6: state %d", state[5]);
    others_untouched(5, "a store into page 5 from page 6");

    /* Code in page 5 writes to page 5: carried out here, and the page is scanned again */
    check(iabort(IPA(5, 0)) && state[5] == HV_XPAGE_SCANNED, "page 5 runs again");
    check(store(IPA(5, 4), IPA(5, 8)) == HV_XTRACK_EMULATE && state[5] == HV_XPAGE_SCANNED,
          "a store into page 5 from page 5: state %d", state[5]);
    arena[5 * WORDS + 7] = MRS_PMCCNTR | 9;
    hv_xtrack_stored(IPA(5, 7));
    check(arena[5 * WORDS + 7] == (MRS_CNTVCT | 9), "an access stored into page 5 stays: 0x%08x",
          arena[5 * WORDS + 7]);
    others_untouched(5, "a store into page 5 from page 5");

    /* A store EL2 cannot carry out: page 5 is opened, and closed again with what was written */
    check(hv_xtrack_open(IPA(5, 8), 0x28810520) && state[5] == HV_XPAGE_OPEN,
          "page 5 is not opened: state %d", state[5]);
    check(store(IPA(5, 4), IPA(5, 8)) == HV_XTRACK_RETRY, "a store into the open page 5");
    arena[5 * WORDS + 9] = MRS_PMCCNTR | 11;
    hv_xtrack_close();
    check(state[5] == HV_XPAGE_SCANNED && arena[5 * WORDS + 9] == (MRS_CNTVCT | 11),
          "page 5 closed: state %d, the word written meanwhile 0x%08x", state[5],
          arena[5 * WORDS + 9]);
    others_untouched(5, "page 5 opened and closed");

    /* Nothing to do with execution tracking */
    check(hv_xtrack_dabort(&ctx, IPA(5, 4), false) == HV_XTRACK_NONE, "a load from page 5");
    check(store(IPA(2, 4), IPA(5, 8)) == HV_XTRACK_NONE, "a store into page 2, which never ran");
    others_untouched(5, "a load, and a store elsewhere");

    /* Execution where there is no memory, and an abort that is not about permissions */
    check(!iabort(0x1000000000), "the guest executes device registers and is not stopped");
    test_esr = 0x82000007; /* translation fault */
    test_far = IPA(1, 0);
    check(!hv_xtrack_iabort(&ctx) && state[1] == HV_XPAGE_UNSCANNED,
          "a translation fault on page 1 is taken for a first execution");

    test_xoprobe();
    test_count_reads();

    printf("xtrack-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
