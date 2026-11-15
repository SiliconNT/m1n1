/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Execution tracking: every page of the guest's memory is scanned before code in it runs.
 *
 * hv_pmu.c replaces the instructions of the guest that the core cannot execute. The guest is
 * Windows with a UEFI firmware in front of it: it loads its own boot manager, loader, kernel
 * and drivers, each stage the next, and nothing tells EL2 when code has arrived somewhere. So
 * the second stage of translation is made to tell:
 *
 *  - Memory starts out writable and not executable (hv_pt_xtrack()).
 *  - The first instruction fetched from a page is a stage 2 permission fault, an Instruction
 *    Abort that comes here. The page is scanned, the instructions in question are replaced,
 *    and the page becomes executable and read-only. The guest fetches the instruction again.
 *  - A store into such a page is a stage 2 permission fault too, a Data Abort. The page goes
 *    back to writable and not executable and the guest stores again; when code in it next
 *    runs, it is scanned again. So a page is never executable while something that has not
 *    been scanned can be in it.
 *  - If the store comes from the very page it writes to (code and writable data share a 16 KB
 *    stage 2 page while the guest thinks in 4 KB pages), flipping would never end: the store
 *    needs the page writable and the instruction that stores needs it executable. Then the
 *    store is carried out here, with the page left as it is, and the page is scanned again.
 *
 * What gets past this: memory written from outside the guest's stage 2 mappings while it is
 * executable. That is the host of the hypervisor writing to guest memory, and devices doing
 * DMA.
 *
 * Everything here runs with the hypervisor lock held. The stage 2 tables are shared by all
 * cores, and a change of permissions is followed by a TLB invalidation on all of them
 * (hv_pt_set_scanned()), so a core that faults on a page another core has just dealt with
 * finds it done and simply goes on.
 */

#include "hv.h"
#include "cpu_regs.h"
#include "memory.h"
#include "smp.h"
#include "utils.h"

#define XTRACK_PAGE 0x4000UL

/* ESR_ELx.ISS of an abort: the fault status code is in bits 5:0, 0b0011xx is a permission fault */
#define ISS_FSC      GENMASK(5, 0)
#define FSC_PERM(x)  (((x) & 0x3c) == 0x0c)
#define ISS_S1PTW    BIT(7)

/* How often a core may fault on the same page that is executable already before it is stopped */
#define XTRACK_MAX_REPEATS 16

static struct {
    u64 scans;       // instruction aborts that scanned a page
    u64 scans_el0;   // ...of which came from EL0
    u64 hits;        // ...and of which found something to replace
    u64 done;        // instruction aborts on a page that was executable already
    u64 writes;      // stores that made a page writable again
    u64 stores;      // stores carried out here
    u64 restores;    // ...after which the page had to be patched again
    u64 ticks;       // time spent scanning, in timer ticks
    u64 longest;
} xtrack_stats;

static struct {
    u64 page;
    u32 repeats;
} xtrack_last[MAX_CPUS];

void hv_xtrack_init(void)
{
    xtrack_stats.scans = 0;
    xtrack_stats.scans_el0 = 0;
    xtrack_stats.hits = 0;
    xtrack_stats.done = 0;
    xtrack_stats.writes = 0;
    xtrack_stats.stores = 0;
    xtrack_stats.restores = 0;
    xtrack_stats.ticks = 0;
    xtrack_stats.longest = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        xtrack_last[i].page = 0;
        xtrack_last[i].repeats = 0;
    }
}

/* Replace what has to be replaced in the page at pa. Returns how many instructions that was */
static u32 hv_xtrack_scan(u64 pa)
{
    u32 n = hv_pmu_patch_range((u32 *)pa, XTRACK_PAGE / 4);

    if (n) {
        /*
         * The guest may have executed these words before, through any address. Clean them to
         * the point of coherency and drop every instruction cache entry on every core.
         */
        dc_cvau_range((void *)pa, XTRACK_PAGE);
        sysop("dsb ish");
        ic_ialluis();
        sysop("dsb ish");
        sysop("isb");
    }
    return n;
}

/*
 * The guest could not fetch an instruction because of stage 2. Returns true if it is to fetch
 * it again; ctx->elr is not to be moved.
 */
bool hv_xtrack_iabort(struct exc_info *ctx)
{
    u64 esr = hv_get_esr();
    u64 far = hv_get_far();

    if (!hv_pt_xtrack_on()) {
        printf("HV: instruction abort at 0x%lx (ESR 0x%lx) and execution is not tracked\n", far,
               esr);
        return false;
    }

    if (!FSC_PERM(esr & ISS_FSC) || (esr & ISS_S1PTW)) {
        printf("HV: instruction abort at 0x%lx that is not a stage 2 permission fault (ESR "
               "0x%lx)\n",
               far, esr);
        return false;
    }

    u64 ipa = hv_translate(far, true, false, NULL);
    if (!ipa) {
        printf("HV: instruction abort at 0x%lx: stage 1 translation failed\n", far);
        return false;
    }

    u64 page = ipa & ~(XTRACK_PAGE - 1);
    u64 pa;
    int state = hv_pt_scanned(page, &pa);

    if (state < 0) {
        printf("HV: the guest executes 0x%lx (IPA 0x%lx), where there is no memory\n", far, ipa);
        return false;
    }

    if (state == 1) {
        /* Another core got there first, or this one faulted on what was left in its TLB */
        if (xtrack_last[ctx->cpu_id].page == page) {
            if (++xtrack_last[ctx->cpu_id].repeats > XTRACK_MAX_REPEATS) {
                printf("HV: instruction abort at 0x%lx (IPA 0x%lx, ESR 0x%lx) again and again "
                       "although the page is executable\n",
                       far, ipa, esr);
                return false;
            }
        } else {
            xtrack_last[ctx->cpu_id].page = page;
            xtrack_last[ctx->cpu_id].repeats = 1;
        }
        xtrack_stats.done++;
        return true;
    }

    u64 t0 = mrs(CNTPCT_EL0);

    if (hv_xtrack_scan(pa))
        xtrack_stats.hits++;
    if (hv_pt_set_scanned(page, true) < 0) {
        printf("HV: could not make IPA 0x%lx executable\n", page);
        return false;
    }

    u64 t = mrs(CNTPCT_EL0) - t0;
    xtrack_stats.scans++;
    if (FIELD_GET(SPSR_M, hv_get_spsr()) >> 2 == 0)
        xtrack_stats.scans_el0++;
    xtrack_stats.ticks += t;
    if (t > xtrack_stats.longest)
        xtrack_stats.longest = t;

    xtrack_last[ctx->cpu_id].page = page;
    xtrack_last[ctx->cpu_id].repeats = 0;
    return true;
}

/*
 * A data abort on memory that has a hardware mapping. Is it a store into a page that is
 * read-only because it has been scanned, and what is to be done about it?
 */
int hv_xtrack_dabort(struct exc_info *ctx, u64 ipa, bool is_write)
{
    u64 pa;

    if (!is_write || !hv_pt_xtrack_on())
        return HV_XTRACK_NONE;

    u64 page = ipa & ~(XTRACK_PAGE - 1);
    if (hv_pt_scanned(page, &pa) != 1)
        return HV_XTRACK_NONE;

    /* ctx->elr is where the store is: the caller has not moved it yet */
    u64 pc = hv_translate(ctx->elr, true, false, NULL);
    if (pc && (pc & ~(XTRACK_PAGE - 1)) == page) {
        xtrack_stats.stores++;
        return HV_XTRACK_EMULATE;
    }

    if (hv_pt_set_scanned(page, false) < 0)
        return HV_XTRACK_NONE;
    xtrack_stats.writes++;
    return HV_XTRACK_RETRY;
}

/* A store was carried out by EL2 into the page at this IPA: if it is executable, scan it again */
void hv_xtrack_stored(u64 ipa)
{
    u64 pa;

    if (!hv_pt_xtrack_on() || hv_pt_scanned(ipa, &pa) != 1)
        return;
    if (hv_xtrack_scan(pa))
        xtrack_stats.restores++;
}

void hv_xtrack_report(void)
{
    if (!hv_pt_xtrack_on())
        return;

    u64 freq = mrs(CNTFRQ_EL0);
    u64 us = freq ? xtrack_stats.ticks * 1000000 / freq : 0;
    u64 longest = freq ? xtrack_stats.longest * 1000000 / freq : 0;

    printf("HV: execution tracking: %ld pages scanned before they ran (%ld of them for EL0, %ld "
           "had instructions to replace), %ld us in all, longest %ld us; %ld faults on pages "
           "already done; %ld pages made writable again; %ld stores carried out in EL2 (%ld "
           "put an instruction back)\n",
           xtrack_stats.scans, xtrack_stats.scans_el0, xtrack_stats.hits, us, longest,
           xtrack_stats.done, xtrack_stats.writes, xtrack_stats.stores, xtrack_stats.restores);
}
