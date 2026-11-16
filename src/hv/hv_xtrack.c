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
 *  - If the store comes from the very page it writes to (code and writable data that share a
 *    page; stage 2 has the 4 KB pages of the guest), flipping would never end: the store
 *    needs the page writable and the instruction that stores needs it executable. Then the
 *    store is carried out here, with the page left as it is, and the page is scanned again.
 *  - If that store is an instruction the MMIO emulation does not know (pair stores with
 *    writeback of vector or 32-bit registers, exclusive stores, atomics), the page is opened:
 *    executable and writable, so that the guest carries the store out itself. The next time
 *    any core comes to EL2 with the lock, which the 1 kHz tick of the hypervisor sees to,
 *    open pages are scanned and made read-only again.
 *
 * What gets past this: memory written from outside the guest's stage 2 mappings while it is
 * executable. That is the host of the hypervisor writing to guest memory, and devices doing
 * DMA. And an access that is written into an open page and executed before the page is
 * closed again: within a millisecond, by code that shares its page.
 *
 * Device registers are not memory here: a guest that executes from them is stopped
 * (hv_pt_scanned() knows nothing below the start of RAM).
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

/* A page of stage 2, which is a page of the guest: see hv_vm.c */
#define XTRACK_PAGE 0x1000UL

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
    u64 opens;       // pages opened for a store that could not be carried out here
    u64 open_hits;   // ...in which there was something to replace when they were closed
    u32 open_insn;   // the last instruction a page was opened for
    u64 ticks;       // time spent scanning, in timer ticks
    u64 longest;
} xtrack_stats;

static struct {
    u64 page;
    u32 repeats;
} xtrack_last[MAX_CPUS];

/* Pages that are open, by IPA */
#define XTRACK_MAX_OPEN 16
static u64 xtrack_open[XTRACK_MAX_OPEN];
static u32 xtrack_nr_open;

/* How many of the instructions that pages are opened for get a line of their own */
#define XTRACK_LOG_OPEN 16

/*
 * Counting reads, for an experiment (SiliconNT R27) and no part of the above.
 *
 * The guest reads its own code: constants that the compiler put between functions, and
 * Windows checks copies of its code against what it knows they should be, which is how it
 * finds the instructions replaced here. A way out would be to make the pages that
 * instructions were replaced in execute-only, so that every read of them comes to EL2 (the
 * M4 has such pages: hv_xoprobe.c). What that would cost depends on how often they are read,
 * and this counts it. With hv_xtrack_count_reads(true), said before the guest starts:
 *
 *  - A page that instructions were replaced in becomes execute-only instead of read-only
 *    when it has been scanned, and again whenever it is scanned later while words that an
 *    access becomes are still in it.
 *  - A read of such a page is counted: in all, for the page, for the address of the
 *    instruction that reads, and for the RC_SLOT_SECONDS of the run that it falls in.
 *  - It is then carried out here, from what ESR_EL2 says of the instruction (Arm DDI 0487
 *    M.d, D24.2.45, ISS encoding for an exception from a Data Abort: ISV, SAS, SSE, SRT, SF),
 *    which the M4 gives for a load of one general-purpose register (SiliconNT R26). The guest
 *    gets what is in the page now, replaced instructions and all: this counts reads, it hides
 *    nothing.
 *  - Where ESR_EL2 does not say (pairs, vector registers, writeback, exclusives), or the
 *    address is the first byte of the page and the access is longer than a byte (it may have
 *    begun in the page before), or the access goes on beyond the end of the page, the page is
 *    lent to the guest instead: readable and executable until the EL2 entry after the next
 *    one. Not the next one: an instruction that reads two such pages comes here once for each
 *    and needs the first still readable when it has got the second. While a page is lent,
 *    reads of it are not seen, so what is counted is faults, a lower bound of the reads.
 *  - A store is what it is for a read-only page (hv_xtrack_dabort()).
 *
 * The switch stays as it is said until it is said again: hv_xtrack_init() leaves it alone,
 * so that it can be said before m1n1 loads a guest by itself (src/selfboot_run.c).
 */
#define ISS_ISV BIT(24)
#define ISS_SAS GENMASK(23, 22)
#define ISS_SSE BIT(21)
#define ISS_SRT GENMASK(20, 16)
#define ISS_SF  BIT(15)

#define RC_MAX_PAGES    2048 // these two are powers of two
#define RC_MAX_PCS      2048
#define RC_SLOT_SECONDS 5
#define RC_SLOTS        720
#define RC_MAX_LENT     16

#define RC_PAGE_ARMED BIT(0) // execute-only, or lent for the moment

#define RC_PC_CARRIED BIT(0) // a read by this instruction was carried out in EL2
#define RC_PC_LENT    BIT(1) // ...a page was lent for one
#define RC_PC_EL0     BIT(2) // ...one came from EL0
#define RC_PC_SAME    BIT(3) // ...one was of the page the instruction is in

static bool rc_on;

/* By IPA of the page; page == 0 is a free entry */
static struct rc_page {
    u64 page;
    u64 reads;
    u32 arms; // how often it became execute-only
    u32 flags;
} rc_pages[RC_MAX_PAGES];

/* By the address of the instruction as the guest has it; reads == 0 is a free entry */
static struct rc_pc {
    u64 pc;
    u64 far; // what the last read was of
    u64 reads;
    u32 esr; // ...and what ESR_EL2 said of it
    u32 flags;
} rc_pcs[RC_MAX_PCS];

static struct {
    u32 carried;
    u32 lent;
} rc_slots[RC_SLOTS];

/* Pages that are lent, oldest first, and the entry to EL2 that lent them */
static struct {
    u64 page;
    u64 seq;
} rc_lent[RC_MAX_LENT];
static u32 rc_nr_lent;
static u64 rc_seq;
static u64 rc_t0;

static struct {
    u64 reads;
    u64 el0;        // ...of which came from EL0
    u64 same;       // ...and of which were of the page the instruction is in
    u64 carried;    // reads carried out here
    u64 lent;       // reads that a page was lent for
    u64 lent_nosyn; // ...because ESR_EL2 does not describe the instruction
    u64 lent_edge;  // ...because the access may not be within the page
    u64 rearmed;    // lent pages taken back
    u64 writable;   // pages that a store made writable again
    u64 armed;      // pages that are execute-only or lent now
    u64 armed_max;
    u64 pages_lost; // pages that rc_pages had no room for: they stayed readable
    u64 pcs_lost;   // reads by instructions that rc_pcs had no room for
    u64 late;       // reads after the last slot
} rc_stats;

static void rc_init(void)
{
    for (u32 i = 0; i < RC_MAX_PAGES; i++) {
        rc_pages[i].page = 0;
        rc_pages[i].reads = 0;
        rc_pages[i].arms = 0;
        rc_pages[i].flags = 0;
    }
    for (u32 i = 0; i < RC_MAX_PCS; i++) {
        rc_pcs[i].pc = 0;
        rc_pcs[i].far = 0;
        rc_pcs[i].reads = 0;
        rc_pcs[i].esr = 0;
        rc_pcs[i].flags = 0;
    }
    for (u32 i = 0; i < RC_SLOTS; i++) {
        rc_slots[i].carried = 0;
        rc_slots[i].lent = 0;
    }
    rc_nr_lent = 0;
    rc_seq = 0;
    rc_t0 = mrs(CNTPCT_EL0);
    rc_stats.reads = 0;
    rc_stats.el0 = 0;
    rc_stats.same = 0;
    rc_stats.carried = 0;
    rc_stats.lent = 0;
    rc_stats.lent_nosyn = 0;
    rc_stats.lent_edge = 0;
    rc_stats.rearmed = 0;
    rc_stats.writable = 0;
    rc_stats.armed = 0;
    rc_stats.armed_max = 0;
    rc_stats.pages_lost = 0;
    rc_stats.pcs_lost = 0;
    rc_stats.late = 0;
}

void hv_xtrack_count_reads(bool on)
{
    rc_on = on;
    printf("HV: execution tracking: reads of pages with replaced instructions are %s\n",
           on ? "counted, and such pages are execute-only" : "not counted");
}

bool hv_xtrack_counts_reads(void)
{
    return rc_on;
}

static u32 rc_hash(u64 key, u32 size)
{
    return ((key * 0x9e3779b97f4a7c15UL) >> 40) & (size - 1);
}

static struct rc_page *rc_page(u64 page, bool add)
{
    u32 i = rc_hash(page >> 12, RC_MAX_PAGES);

    for (u32 n = 0; n < RC_MAX_PAGES; n++, i = (i + 1) & (RC_MAX_PAGES - 1)) {
        if (rc_pages[i].page == page)
            return &rc_pages[i];
        if (rc_pages[i].page)
            continue;
        if (!add)
            return NULL;
        rc_pages[i].page = page;
        return &rc_pages[i];
    }
    return NULL;
}

static struct rc_pc *rc_pc(u64 pc)
{
    u32 i = rc_hash(pc >> 2, RC_MAX_PCS);

    for (u32 n = 0; n < RC_MAX_PCS; n++, i = (i + 1) & (RC_MAX_PCS - 1)) {
        if (rc_pcs[i].reads && rc_pcs[i].pc != pc)
            continue;
        rc_pcs[i].pc = pc;
        return &rc_pcs[i];
    }
    return NULL;
}

/* The page is no longer one that executes with replaced instructions in it. Was it one? */
static bool rc_gone(u64 page)
{
    struct rc_page *p = rc_on ? rc_page(page, false) : NULL;

    if (!p || !(p->flags & RC_PAGE_ARMED))
        return false;
    p->flags &= ~RC_PAGE_ARMED;
    rc_stats.armed--;
    return true;
}

/*
 * What a page becomes that has just been scanned, with n instructions replaced this time:
 * read-only, or execute-only if reads are counted and it is one of the pages in question.
 */
static int rc_scanned(u64 page, u64 pa, u32 n)
{
    if (!rc_on)
        return HV_XPAGE_SCANNED;

    struct rc_page *p = rc_page(page, n != 0);
    if (!p) {
        if (n)
            rc_stats.pages_lost++;
        return HV_XPAGE_SCANNED;
    }
    /* Replaced in an earlier scan: is any of it still there? */
    if (!n && !hv_pmu_count_replaced((u32 *)pa, XTRACK_PAGE / 4)) {
        rc_gone(page);
        return HV_XPAGE_SCANNED;
    }
    if (!(p->flags & RC_PAGE_ARMED)) {
        p->flags |= RC_PAGE_ARMED;
        p->arms++;
        if (++rc_stats.armed > rc_stats.armed_max)
            rc_stats.armed_max = rc_stats.armed;
    }
    return HV_XPAGE_XONLY;
}

/* A page that was lent is execute-only again, unless something else has become of it since */
static void rc_rearm(u64 page)
{
    u64 pa;
    struct rc_page *p = rc_page(page, false);

    if (!p || !(p->flags & RC_PAGE_ARMED) || hv_pt_scanned(page, &pa) != HV_XPAGE_SCANNED)
        return;
    if (hv_pt_set_scanned(page, HV_XPAGE_XONLY) == 0)
        rc_stats.rearmed++;
}

/*
 * A read of the execute-only page at pa, which is the page of this IPA; ctx->elr is where the
 * instruction is. Returns what hv_xtrack_dabort() returns.
 */
static int rc_read(struct exc_info *ctx, u64 ipa, u64 pa)
{
    u64 esr = hv_get_esr();
    u64 page = ipa & ~(XTRACK_PAGE - 1);
    u64 off = ipa & (XTRACK_PAGE - 1);
    u64 size = 1UL << FIELD_GET(ISS_SAS, esr);
    bool syn = esr & ISS_ISV;
    bool edge = syn && ((off == 0 && size > 1) || off + size > XTRACK_PAGE);
    bool carry = syn && !edge;
    u32 flags = carry ? RC_PC_CARRIED : RC_PC_LENT;

    rc_stats.reads++;
    if (FIELD_GET(SPSR_M, hv_get_spsr()) >> 2 == 0) {
        flags |= RC_PC_EL0;
        rc_stats.el0++;
    }
    u64 pc = hv_translate(ctx->elr, true, false, NULL);
    if (pc && (pc & ~(XTRACK_PAGE - 1)) == page) {
        flags |= RC_PC_SAME;
        rc_stats.same++;
    }

    struct rc_page *p = rc_page(page, false);
    if (p)
        p->reads++;
    struct rc_pc *c = rc_pc(ctx->elr);
    if (c) {
        c->reads++;
        c->far = hv_get_far();
        c->esr = esr;
        c->flags |= flags;
    } else {
        rc_stats.pcs_lost++;
    }

    u64 freq = mrs(CNTFRQ_EL0);
    u64 slot = freq ? (mrs(CNTPCT_EL0) - rc_t0) / (freq * RC_SLOT_SECONDS) : 0;
    if (slot >= RC_SLOTS)
        rc_stats.late++;

    if (carry) {
        u64 val = 0;
        u32 rt = FIELD_GET(ISS_SRT, esr);

        for (u64 i = 0; i < size; i++)
            val |= (u64)((u8 *)pa)[off + i] << (8 * i);
        if ((esr & ISS_SSE) && size < 8 && (val >> (8 * size - 1)) & 1)
            val |= ~0UL << (8 * size);
        if (!(esr & ISS_SF))
            val &= 0xffffffffUL;
        /* Rt == 31 is the zero register */
        if (rt != 31)
            ctx->regs[rt] = val;

        rc_stats.carried++;
        if (slot < RC_SLOTS)
            rc_slots[slot].carried++;
        return HV_XTRACK_SKIP;
    }

    /* No room: the one lent longest ago goes back, which is not the one of this instruction */
    if (rc_nr_lent == RC_MAX_LENT) {
        rc_rearm(rc_lent[0].page);
        for (u32 i = 1; i < rc_nr_lent; i++)
            rc_lent[i - 1] = rc_lent[i];
        rc_nr_lent--;
    }
    if (hv_pt_set_scanned(page, HV_XPAGE_SCANNED) < 0)
        return HV_XTRACK_NONE;
    rc_lent[rc_nr_lent].page = page;
    rc_lent[rc_nr_lent].seq = rc_seq;
    rc_nr_lent++;

    rc_stats.lent++;
    if (edge)
        rc_stats.lent_edge++;
    else
        rc_stats.lent_nosyn++;
    if (slot < RC_SLOTS)
        rc_slots[slot].lent++;
    return HV_XTRACK_RETRY;
}

void hv_xtrack_init(void)
{
    rc_init();
    xtrack_stats.scans = 0;
    xtrack_stats.scans_el0 = 0;
    xtrack_stats.hits = 0;
    xtrack_stats.done = 0;
    xtrack_stats.writes = 0;
    xtrack_stats.stores = 0;
    xtrack_stats.restores = 0;
    xtrack_stats.opens = 0;
    xtrack_stats.open_hits = 0;
    xtrack_stats.open_insn = 0;
    xtrack_nr_open = 0;
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

    if (state != HV_XPAGE_UNSCANNED) {
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

    u32 n = hv_xtrack_scan(pa);
    if (n)
        xtrack_stats.hits++;
    if (hv_pt_set_scanned(page, rc_scanned(page, pa, n)) < 0) {
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
 *
 * An abort that is not such a store goes to the execute-only probe (hv_xoprobe.c), which
 * says HV_XTRACK_NONE unless a test guest has asked it for something else. While reads are
 * counted, the execute-only pages are not the probe's: a read of one is counted, and a store
 * into one is a store into a read-only page.
 */
int hv_xtrack_dabort(struct exc_info *ctx, u64 ipa, bool is_write)
{
    u64 pa;

    if (!hv_pt_xtrack_on())
        return HV_XTRACK_NONE;

    u64 page = ipa & ~(XTRACK_PAGE - 1);
    int state = hv_pt_scanned(page, &pa);
    bool counted = rc_on && state == HV_XPAGE_XONLY;

    if (counted && !is_write)
        return rc_read(ctx, ipa, pa);
    if (!counted && (state == HV_XPAGE_XONLY || !is_write))
        return hv_xoprobe_dabort(ctx, ipa, state, is_write);
    /* An open page takes stores: this one faulted on what was left in the TLB */
    if (state == HV_XPAGE_OPEN)
        return HV_XTRACK_RETRY;
    if (!counted && state != HV_XPAGE_SCANNED)
        return hv_xoprobe_dabort(ctx, ipa, state, is_write);

    /* ctx->elr is where the store is: the caller has not moved it yet */
    u64 pc = hv_translate(ctx->elr, true, false, NULL);
    if (pc && (pc & ~(XTRACK_PAGE - 1)) == page) {
        xtrack_stats.stores++;
        return HV_XTRACK_EMULATE;
    }

    if (hv_pt_set_scanned(page, false) < 0)
        return HV_XTRACK_NONE;
    xtrack_stats.writes++;
    if (rc_gone(page))
        rc_stats.writable++;
    return HV_XTRACK_RETRY;
}

/* A store was carried out by EL2 into the page at this IPA: if it is executable, scan it again */
void hv_xtrack_stored(u64 ipa)
{
    u64 pa;

    if (!hv_pt_xtrack_on())
        return;

    /* An open page is scanned when it is closed */
    int state = hv_pt_scanned(ipa, &pa);
    if (state != HV_XPAGE_SCANNED && state != HV_XPAGE_XONLY)
        return;

    u32 n = hv_xtrack_scan(pa);
    if (n)
        xtrack_stats.restores++;

    /* While reads are counted: the store may have made the page one of those, or no longer one */
    if (rc_on) {
        u64 page = ipa & ~(XTRACK_PAGE - 1);
        int want = rc_scanned(page, pa, n);

        if (want != state)
            hv_pt_set_scanned(page, want);
    }
}

/*
 * A store from the page it writes to that EL2 cannot carry out. Open the page: the guest
 * executes the store again and this time it goes through. Returns false if that cannot be.
 */
bool hv_xtrack_open(u64 ipa, u32 insn)
{
    u64 page = ipa & ~(XTRACK_PAGE - 1);

    if (xtrack_nr_open == XTRACK_MAX_OPEN)
        hv_xtrack_close();
    if (hv_pt_set_scanned(page, HV_XPAGE_OPEN) < 0)
        return false;

    xtrack_open[xtrack_nr_open++] = page;
    /* hv_xtrack_dabort() has counted it as a store carried out here */
    xtrack_stats.stores--;
    if (xtrack_stats.opens++ < XTRACK_LOG_OPEN)
        printf("HV: page 0x%lx opened for the store 0x%08x from within it\n", page, insn);
    xtrack_stats.open_insn = insn;
    return true;
}

/* Scan the open pages and make them read-only again. Called on every entry to EL2 that locks */
void hv_xtrack_close(void)
{
    u64 pa;

    while (xtrack_nr_open) {
        u64 page = xtrack_open[--xtrack_nr_open];

        if (hv_pt_scanned(page, &pa) != HV_XPAGE_OPEN)
            continue;

        u32 n = hv_xtrack_scan(pa);
        if (n)
            xtrack_stats.open_hits++;
        hv_pt_set_scanned(page, rc_scanned(page, pa, n));
    }

    /* Pages lent for a read are taken back at the second entry after the one that lent them */
    u32 keep = 0;

    rc_seq++;
    for (u32 i = 0; i < rc_nr_lent; i++) {
        if (rc_lent[i].seq + 1 >= rc_seq)
            rc_lent[keep++] = rc_lent[i];
        else
            rc_rearm(rc_lent[i].page);
    }
    rc_nr_lent = keep;
}

static void rc_report(void)
{
    if (!rc_on)
        return;

    printf("HV: reads of pages with replaced instructions: %ld (%ld from EL0, %ld of the page "
           "the instruction is in); %ld carried out in EL2; %ld that the page was lent for (%ld: "
           "ESR_EL2 does not describe the instruction, %ld: first byte of the page or beyond its "
           "end); %ld times a lent page was taken back\n",
           rc_stats.reads, rc_stats.el0, rc_stats.same, rc_stats.carried, rc_stats.lent,
           rc_stats.lent_nosyn, rc_stats.lent_edge, rc_stats.rearmed);
    printf("HV: pages with replaced instructions: %ld execute-only now, %ld at most; %ld times "
           "a store made one writable; no room for %ld pages and for %ld reads by instruction; "
           "%ld reads after the last slot\n",
           rc_stats.armed, rc_stats.armed_max, rc_stats.writable, rc_stats.pages_lost,
           rc_stats.pcs_lost, rc_stats.late);

    /* For the host to take apart: one line for each slot, page and instruction with reads */
    for (u32 i = 0; i < RC_SLOTS; i++)
        if (rc_slots[i].carried || rc_slots[i].lent)
            printf("HV: xo slot %d %d %d\n", i, rc_slots[i].carried, rc_slots[i].lent);
    for (u32 i = 0; i < RC_MAX_PAGES; i++)
        if (rc_pages[i].page)
            printf("HV: xo page 0x%lx %ld %d %d\n", rc_pages[i].page, rc_pages[i].reads,
                   rc_pages[i].arms, rc_pages[i].flags & RC_PAGE_ARMED ? 1 : 0);
    for (u32 i = 0; i < RC_MAX_PCS; i++)
        if (rc_pcs[i].reads)
            printf("HV: xo pc 0x%lx %ld %d 0x%x 0x%lx\n", rc_pcs[i].pc, rc_pcs[i].reads,
                   rc_pcs[i].flags, rc_pcs[i].esr, rc_pcs[i].far);
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
    printf("HV: execution tracking: %ld times a page was opened for a store EL2 could not "
           "carry out (%ld had instructions to replace when closed)",
           xtrack_stats.opens, xtrack_stats.open_hits);
    if (xtrack_stats.opens)
        printf(", the last such store 0x%08x", xtrack_stats.open_insn);
    printf("\n");
    rc_report();
}
