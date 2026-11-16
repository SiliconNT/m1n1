/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The execute-only probe: a test guest finds out what this core does with a page that the
 * second stage of translation lets it execute and neither read nor write.
 *
 * hv_xtrack.c replaces instructions in the pages of the guest, and Windows reads its own code:
 * it checks copies of it against what it knows they should be. A way out is a page that
 * executes with the replaced instructions in it and cannot be read, so that every read comes
 * to EL2 and can be given something else. The architecture has such a page (Arm DDI 0487 M.d,
 * D8.4.2.1.1, table D8-76: S2AP 0b00 is no data access; D8.4.2.1.2, table D8-78: XN 0b00 is
 * executable; J1.2.4.35, AArch64_S2CheckPermissions: a fetch is checked against the one and
 * not the other). Whether the M4 does, what it reports when the page is read, and what a trip
 * here costs, is what xotest (siliconnt/irqtest/xotest.c) is for. This file is its other half
 * and nothing more: it carries out no design, and Windows never gets here.
 *
 * The host turns the probe on (P_HV_XOPROBE); execution has to be tracked. From then on the
 * guest has four calls, hvc #0 with a function ID from the range of the SMC calling convention
 * that belongs to the hypervisor (Arm DEN 0028 1.7 H, 2.5, table 2-1, and 6.1: vendor specific
 * hypervisor service calls, SMC64):
 *
 *   RESET   forget the faults recorded so far
 *   ARM     x1 = an address in a page, x2 = what to do with a data abort on it (below). The
 *           page must have executed, so that it has been scanned. It becomes execute-only.
 *           The mode is one for all pages.
 *   DISARM  x1 = an address in a page. The page is as it was before ARM.
 *   QUERY   x1 = n. Returns in x0 how many faults were recorded since RESET, and of the nth,
 *           if it is one of the first XO_MAX_FAULTS: x1 = ESR_EL2, x2 = FAR_EL2, x3 = the IPA
 *           that FAR_EL2 translates to, x4 = where the instruction is, x5 = HPFAR_EL2,
 *           x6 = XO_FLAG_*.
 *
 * They return 0 in x0 or XO_INVALID. Without the probe they are functions that PSCI does not
 * have, and return -1.
 *
 * What becomes of a data abort on an execute-only page:
 *
 *   HV_XOPROBE_DEMOTE   the page is what it was before ARM, readable and executable, and the
 *                       guest executes the instruction again
 *   HV_XOPROBE_SKIP     nothing: the guest goes on behind the instruction, which has not read
 *                       or written anything
 *   HV_XOPROBE_EMULATE  EL2 carries the access out, with the code that does so for stores into
 *                       a page from within it (hv_handle_dabort()), as far as that code knows
 *                       the instruction
 *
 * An access that crosses into such a page may be reported with an address that is not in it:
 * "an unaligned access that occurs across a page boundary can generate an abort on either
 * side of the page boundary" (B2.8, Alignment support), and HPFAR_EL2 does not have to say
 * where a stage 2 permission fault was (D1.4.2.1, rule RFKLWR: it is UNKNOWN for a fault that
 * is not in the list there). Then no page state explains the abort, and
 * hv_handle_dabort() would have the guest execute the instruction again for ever. So while
 * pages are armed and the mode is SKIP, such an abort is recorded and skipped as well, with
 * XO_FLAG_STRAY.
 *
 * Everything here runs with the hypervisor lock held.
 */

#include "hv.h"
#include "cpu_regs.h"
#include "utils.h"

#define XO_FN_RESET  0xc6005800
#define XO_FN_ARM    0xc6005801
#define XO_FN_DISARM 0xc6005802
#define XO_FN_QUERY  0xc6005803

/* Not -1, which is what a hypervisor without the probe answers */
#define XO_INVALID (-2L)

#define XO_FLAG_STRAY BIT(0) // the abort was not on an execute-only page
#define XO_FLAG_WRITE BIT(1) // ESR_EL2 says it was a write

#define XO_PAGE       0x1000UL
#define XO_MAX_PAGES  4
#define XO_MAX_FAULTS 8

static bool xo_enabled;
static u64 xo_mode;

/* The pages that are execute-only, by IPA */
static u64 xo_pages[XO_MAX_PAGES];
static u32 xo_nr_pages;

static struct {
    u64 esr, far, ipa, elr, hpfar, flags;
} xo_faults[XO_MAX_FAULTS];
static u64 xo_nr_faults;

static struct {
    u64 arms;
    u64 demoted;
    u64 skipped;
    u64 emulated;
    u64 stray;
} xo_stats;

void hv_xoprobe_init(void)
{
    xo_enabled = false;
    xo_mode = HV_XOPROBE_DEMOTE;
    xo_nr_pages = 0;
    xo_nr_faults = 0;
    xo_stats.arms = 0;
    xo_stats.demoted = 0;
    xo_stats.skipped = 0;
    xo_stats.emulated = 0;
    xo_stats.stray = 0;
}

/* The host says the guest is xotest. Returns 0, or -1 if execution is not tracked */
int hv_xoprobe_enable(void)
{
    if (!hv_pt_xtrack_on()) {
        printf("HV: execute-only probe: execution is not tracked\n");
        return -1;
    }
    xo_enabled = true;
    printf("HV: execute-only probe: on\n");
    return 0;
}

static void xo_forget(u64 page)
{
    for (u32 i = 0; i < xo_nr_pages; i++) {
        if (xo_pages[i] != page)
            continue;
        xo_pages[i] = xo_pages[--xo_nr_pages];
        return;
    }
}

/* The page of the guest that this address of the guest is in, or 0 */
static u64 xo_page_of(u64 addr)
{
    return hv_translate(addr, true, false, NULL) & ~(XO_PAGE - 1);
}

static s64 xo_arm(u64 addr, u64 mode)
{
    u64 pa;
    u64 page = xo_page_of(addr);

    if (!page || mode > HV_XOPROBE_EMULATE) {
        printf("HV: execute-only probe: ARM(0x%lx, %ld) makes no sense\n", addr, mode);
        return XO_INVALID;
    }

    int state = hv_pt_scanned(page, &pa);
    if (state == HV_XPAGE_XONLY) {
        xo_mode = mode;
        return 0;
    }
    if (state != HV_XPAGE_SCANNED) {
        printf("HV: execute-only probe: ARM(0x%lx): the page at IPA 0x%lx is in state %d, not "
               "one that has executed\n",
               addr, page, state);
        return XO_INVALID;
    }
    if (xo_nr_pages == XO_MAX_PAGES || hv_pt_set_scanned(page, HV_XPAGE_XONLY) < 0) {
        printf("HV: execute-only probe: ARM(0x%lx): no room\n", addr);
        return XO_INVALID;
    }

    xo_pages[xo_nr_pages++] = page;
    xo_mode = mode;
    xo_stats.arms++;
    return 0;
}

static s64 xo_disarm(u64 addr)
{
    u64 pa;
    u64 page = xo_page_of(addr);

    if (!page)
        return XO_INVALID;
    /* A data abort in the mode DEMOTE has done it already */
    if (hv_pt_scanned(page, &pa) != HV_XPAGE_XONLY)
        return 0;
    if (hv_pt_set_scanned(page, HV_XPAGE_SCANNED) < 0)
        return XO_INVALID;
    xo_forget(page);
    return 0;
}

/*
 * The guest executed hvc with this immediate; ctx->elr is behind the instruction already.
 * Returns false if the call is not one of the probe's, or the probe is off.
 */
bool hv_xoprobe_hvc(struct exc_info *ctx, u16 imm)
{
    u32 fn = ctx->regs[0];
    s64 ret = XO_INVALID;

    if (!xo_enabled || imm != 0 || fn < XO_FN_RESET || fn > XO_FN_QUERY)
        return false;

    switch (fn) {
        case XO_FN_RESET:
            xo_nr_faults = 0;
            ret = 0;
            break;
        case XO_FN_ARM:
            ret = xo_arm(ctx->regs[1], ctx->regs[2]);
            break;
        case XO_FN_DISARM:
            ret = xo_disarm(ctx->regs[1]);
            break;
        case XO_FN_QUERY: {
            u64 n = ctx->regs[1];
            bool have = n < xo_nr_faults && n < XO_MAX_FAULTS;

            ctx->regs[1] = have ? xo_faults[n].esr : 0;
            ctx->regs[2] = have ? xo_faults[n].far : 0;
            ctx->regs[3] = have ? xo_faults[n].ipa : 0;
            ctx->regs[4] = have ? xo_faults[n].elr : 0;
            ctx->regs[5] = have ? xo_faults[n].hpfar : 0;
            ctx->regs[6] = have ? xo_faults[n].flags : 0;
            ret = xo_nr_faults;
            break;
        }
    }

    ctx->regs[0] = ret;
    return true;
}

/*
 * A data abort on memory that has a hardware mapping, and either the page is execute-only
 * (state is HV_XPAGE_XONLY) or no page state explains the abort. ctx->elr is where the
 * instruction is. Returns what hv_xtrack_dabort() returns.
 */
int hv_xoprobe_dabort(struct exc_info *ctx, u64 ipa, int state, bool is_write)
{
    bool stray = state != HV_XPAGE_XONLY;
    u64 page = ipa & ~(XO_PAGE - 1);

    /* Not the probe's business: hv_handle_dabort() does what it did before there was one */
    if (stray && (!xo_enabled || !xo_nr_pages || xo_mode != HV_XOPROBE_SKIP))
        return HV_XTRACK_NONE;

    if (xo_nr_faults < XO_MAX_FAULTS) {
        xo_faults[xo_nr_faults].esr = hv_get_esr();
        xo_faults[xo_nr_faults].far = hv_get_far();
        xo_faults[xo_nr_faults].ipa = ipa;
        xo_faults[xo_nr_faults].elr = ctx->elr;
        xo_faults[xo_nr_faults].hpfar = mrs(HPFAR_EL2);
        xo_faults[xo_nr_faults].flags =
            (stray ? XO_FLAG_STRAY : 0) | (is_write ? XO_FLAG_WRITE : 0);
    }
    xo_nr_faults++;

    if (stray) {
        xo_stats.stray++;
        return HV_XTRACK_SKIP;
    }

    switch (xo_mode) {
        case HV_XOPROBE_SKIP:
            xo_stats.skipped++;
            return HV_XTRACK_SKIP;
        case HV_XOPROBE_EMULATE:
            xo_stats.emulated++;
            return HV_XTRACK_EMULATE;
    }

    if (hv_pt_set_scanned(page, HV_XPAGE_SCANNED) < 0)
        return HV_XTRACK_NONE;
    xo_forget(page);
    xo_stats.demoted++;
    return HV_XTRACK_RETRY;
}

void hv_xoprobe_report(void)
{
    if (!xo_enabled)
        return;

    printf("HV: execute-only probe: %ld times a page was made execute-only; data aborts on such "
           "pages: %ld skipped, %ld carried out in EL2, %ld that made the page readable again; "
           "%ld aborts elsewhere skipped\n",
           xo_stats.arms, xo_stats.skipped, xo_stats.emulated, xo_stats.demoted, xo_stats.stray);
}
