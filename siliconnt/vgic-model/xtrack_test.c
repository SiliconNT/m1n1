/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/hv/hv_xtrack.c: which page is scanned, and which page changes
 * state, when the guest executes or writes somewhere.
 *
 * The guest (Windows) has 4 KB pages. A page that runs is scanned and the accesses to PMUv3
 * registers in it are replaced (the real src/hv/hv_pmu.c, linked in). The pages next to it
 * are somebody else's, code or data, and are left alone: that is what is checked here, with
 * eight pages of memory that are all full of such accesses.
 *
 * The Makefile drops the #include lines of hv_xtrack.c; this file stands in for them and for
 * the page tables: one state for each 4 KB page.
 */

#include <stdarg.h>
#include <string.h>

#include "hv.h"
#include "arm_cpu_regs.h"
#include "hv_xpage.h" /* HV_XPAGE_* and HV_XTRACK_* of the real hv.h, see the Makefile */
#include "smp.h"

#define mrs(reg)  0UL
#define sysop(op) ((void)0)

#define ARENA_IPA 0x10100000000UL
#define NR_PAGES  8
#define WORDS     1024 /* in a page */

#define MRS_PMCCNTR 0xd53b9d00 /* mrs x0, PMCCNTR_EL0 */
#define MRS_CNTVCT  0xd53be040 /* mrs x0, CNTVCT_EL0 */

static u32 arena[NR_PAGES * WORDS] __attribute__((aligned(0x4000)));
static int state[NR_PAGES];
static u64 test_esr, test_far;

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
    return 5; /* EL1h */
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
    return true;
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

    return hv_xtrack_dabort(&ctx, ipa, true);
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

    printf("xtrack-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
