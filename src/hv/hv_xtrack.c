/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Execution tracking: every page of the guest's memory is scanned before code in it runs, and
 * what is replaced in it is kept from the guest's eyes.
 *
 * hv_pmu.c replaces the instructions of the guest that the core cannot execute. The guest is
 * Windows with a UEFI firmware in front of it: it loads its own boot manager, loader, kernel
 * and drivers, each stage the next, and nothing tells EL2 when code has arrived somewhere.
 * And Windows reads its code as data: constants that the compiler put between functions, and
 * checksums of its kernel that it takes again later and stops (bug check 0x109) when they
 * differ. So the second stage of translation is made to tell when code runs, and to keep
 * what was replaced out of every read. The rule: what the guest reads from memory as data is
 * what it wrote there. Only the core, fetching instructions, sees what was replaced.
 *
 * A page of memory is in one of three states (hv_pt_scanned(), hv_pt_set_scanned()):
 *
 *   HV_XPAGE_UNSCANNED  data: read and written, not executed. What the guest wrote.
 *   HV_XPAGE_SCANNED    code with nothing replaced: read and executed, not written. What the
 *                       guest wrote.
 *   HV_XPAGE_XONLY      code with instructions replaced: executed, and neither read nor
 *                       written. For such a page there is a record here of which of its 1024
 *                       words were replaced. What they were is not kept: it can be had back
 *                       from what they are now (hv_pmu_original_word()).
 *
 * All memory starts out as data (hv_pt_xtrack()). From there:
 *
 *  - The first instruction fetched from a page of data is a stage 2 permission fault, an
 *    Instruction Abort (hv_xtrack_iabort()). The page is scanned, the instructions in question
 *    are replaced and recorded, and the page becomes code of the one kind or the other.
 *  - A read of an execute-only page is a Data Abort (hv_xtrack_dabort()) and is carried out
 *    here: the guest gets the bytes of the page with every recorded word put back to what it
 *    was. Where ESR_EL2 describes the access (a load of one general-purpose register without
 *    writeback) and all of it is in the page, that is all there is to it. Otherwise the
 *    instruction is read and taken apart (hv_xinsn.c); an access across two pages is carried
 *    out page by page.
 *  - A store into a page of code from an instruction in another page: the page becomes data
 *    again, with every recorded word put back first, and the guest stores again. When code in
 *    it next runs, it is scanned again.
 *  - A store into a page of code from an instruction in that very page (code and writable
 *    data that share a page) cannot go that way: the store needs the page writable and the
 *    instruction that stores needs it executable. It is carried out here, and only the words
 *    that it wrote to are looked at afterwards: a word that is now an access is replaced and
 *    recorded, a recorded word that is no longer one is released. The atomic operations and
 *    CAS are carried out the same way, which is atomic because this runs with the hypervisor
 *    lock held and every other access to such a page comes here or reads what a single store
 *    left.
 *  - An instruction that hv_xinsn.c does not take: if it is in another page, the page it
 *    accesses becomes data and the guest executes it again. If it is in the page it accesses,
 *    there is no way, and the guest is stopped with a line that says which instruction.
 *  - DC IVAC on a page of code faults because it asks for write permission (Arm DDI 0487 M.d,
 *    D7.5.9.2). DC CIVAC on the same line is carried out for it: with stage 2 on, that is
 *    what the guest's DC IVAC does anyway (same section).
 *
 * Between data and code a page is nobody's (HV_XPAGE_NONE, and the TLBs of all cores told)
 * while its words are changed: no core executes an access that is not replaced yet, and none
 * reads a page that is half done.
 *
 * The guest is also stopped when there is no room for another record, when a walk of its own
 * stage 1 tables is refused by stage 2 (then one of those tables is in a page of code; which
 * one is not looked for), and when its stage 1 does not translate an address that is needed
 * to carry an access out. It is never left to go on with a replaced word where it can
 * read it.
 *
 * What gets past this: memory written from outside the guest's stage 2 mappings while it is
 * executable. That is the host of the hypervisor writing to guest memory, and devices doing
 * DMA. A recorded word that such a write has changed is given to the guest as it is now.
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
#define XT_PAGE  0x1000UL
#define XT_WORDS (XT_PAGE / 4)

/* The most bytes that one instruction of hv_xinsn.c accesses: DC ZVA of a 64-byte block */
#define XT_MAX_ACCESS 64

/*
 * ESR_ELx.ISS of an abort (Arm DDI 0487 M.d, D24.2.45): the fault status code is in bits 5:0,
 * 0b0011xx is a permission fault
 */
#define ISS_FSC     GENMASK(5, 0)
#define FSC_PERM(x) (((x) & 0x3c) == 0x0c)
#define ISS_S1PTW   BIT(7)
#define ISS_ISV     BIT(24)
#define ISS_SAS     GENMASK(23, 22)
#define ISS_SSE     BIT(21)
#define ISS_SRT     GENMASK(20, 16)
#define ISS_SF      BIT(15)

#define DC_IVAC 0xd5087620 // dc ivac, x0

/* How often a core may fault on the same page that is executable already before it is stopped */
#define XTRACK_MAX_REPEATS 16

/* Pages with replaced instructions that there is room to record: a power of two */
#ifndef XT_MAX_PAGES
#define XT_MAX_PAGES 8192
#endif

/* By IPA of the page; page == 0 is a free entry. A bit of map for each word of the page */
static struct xt_rec {
    u64 page;
    u64 map[XT_WORDS / 64];
} xt_recs[XT_MAX_PAGES];
static u32 xt_nr_recs;

/* The instructions that were taken apart here, by where the guest has them */
#define XT_MAX_PCS 1024

#define XT_PC_READ      BIT(0) // it read
#define XT_PC_WRITE     BIT(1) // it wrote
#define XT_PC_SAME      BIT(2) // ...the page that it is in
#define XT_PC_EL0       BIT(3)
#define XT_PC_UNKNOWN   BIT(4) // hv_xinsn.c does not take it
#define XT_PC_TWO_PAGES BIT(5)

/* count == 0 is a free entry */
static struct xt_pc {
    u64 pc;
    u64 count;
    u32 insn;
    u32 flags;
} xt_pcs[XT_MAX_PCS];

static struct {
    u64 scans;     // instruction aborts that scanned a page
    u64 scans_el0; // ...of which came from EL0
    u64 hits;      // ...and of which found something to replace
    u64 done;      // instruction aborts on a page that was executable already
    u64 ticks;     // time spent scanning, in timer ticks
    u64 longest;
    u64 recs_max;     // most pages with replaced instructions at one time
    u64 to_data;      // pages of code that became data again
    u64 to_data_read; // ...for an instruction elsewhere that is not taken apart here
    u64 reads_syn;    // reads carried out from what ESR_EL2 says
    u64 reads_el0;    // ...of which came from EL0
    u64 writes_syn;   // stores into the instruction's own page, likewise
    u64 reads_insn;   // reads carried out from the instruction
    u64 writes_insn;  // stores into the instruction's own page, likewise
    u64 atomics;      // ...of which were atomic operations
    u64 rerecorded;   // stores that changed which words of a page are replaced
    u64 ivac;         // DC IVAC on a page of code
    u64 insn_ticks;   // time spent on what was taken apart
    u64 stale;        // recorded words found changed by somebody else
    u64 pcs_lost;     // instructions that xt_pcs had no room for
    u64 stops;
} xt_stats;

static struct {
    u64 page;
    u32 repeats;
} xtrack_last[MAX_CPUS];

void hv_xtrack_init(void)
{
    for (u32 i = 0; i < XT_MAX_PAGES; i++)
        xt_recs[i].page = 0;
    xt_nr_recs = 0;
    for (u32 i = 0; i < XT_MAX_PCS; i++)
        xt_pcs[i].count = 0;

    xt_stats.scans = 0;
    xt_stats.scans_el0 = 0;
    xt_stats.hits = 0;
    xt_stats.done = 0;
    xt_stats.ticks = 0;
    xt_stats.longest = 0;
    xt_stats.recs_max = 0;
    xt_stats.to_data = 0;
    xt_stats.to_data_read = 0;
    xt_stats.reads_syn = 0;
    xt_stats.reads_el0 = 0;
    xt_stats.writes_syn = 0;
    xt_stats.reads_insn = 0;
    xt_stats.writes_insn = 0;
    xt_stats.atomics = 0;
    xt_stats.rerecorded = 0;
    xt_stats.ivac = 0;
    xt_stats.insn_ticks = 0;
    xt_stats.stale = 0;
    xt_stats.pcs_lost = 0;
    xt_stats.stops = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        xtrack_last[i].page = 0;
        xtrack_last[i].repeats = 0;
    }
}

/* ---- the record of which words of a page were replaced ---- */

static u32 xt_hash(u64 key)
{
    return ((key * 0x9e3779b97f4a7c15UL) >> 40) & (XT_MAX_PAGES - 1);
}

static struct xt_rec *xt_find(u64 page)
{
    u32 i = xt_hash(page >> 12);

    for (u32 n = 0; n < XT_MAX_PAGES; n++, i = (i + 1) & (XT_MAX_PAGES - 1)) {
        if (xt_recs[i].page == page)
            return &xt_recs[i];
        if (!xt_recs[i].page)
            return NULL;
    }
    return NULL;
}

/* A record for a page that has none, with no word marked; NULL if there is no room */
static struct xt_rec *xt_add(u64 page)
{
    u32 i = xt_hash(page >> 12);

    if (xt_nr_recs == XT_MAX_PAGES)
        return NULL;
    while (xt_recs[i].page)
        i = (i + 1) & (XT_MAX_PAGES - 1);

    xt_recs[i].page = page;
    for (u32 n = 0; n < XT_WORDS / 64; n++)
        xt_recs[i].map[n] = 0;
    if (++xt_nr_recs > xt_stats.recs_max)
        xt_stats.recs_max = xt_nr_recs;
    return &xt_recs[i];
}

/* Records move when one is deleted: no pointer to one is good afterwards */
static void xt_del(struct xt_rec *r)
{
    u32 i = r - xt_recs, j = i;

    /* Close the gap, so that a search that stops at a free entry has missed nothing */
    for (u32 n = 1; n < XT_MAX_PAGES; n++) {
        j = (j + 1) & (XT_MAX_PAGES - 1);
        if (!xt_recs[j].page)
            break;

        /* The record at j stays if its search starts behind the gap: in (i, j], cyclically */
        u32 k = xt_hash(xt_recs[j].page >> 12);

        if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
            continue;
        xt_recs[i] = xt_recs[j];
        i = j;
    }
    xt_recs[i].page = 0;
    xt_nr_recs--;
}

static bool xt_marked(const struct xt_rec *r, u32 i)
{
    return r->map[i / 64] & BIT(i % 64);
}

static bool xt_empty(const struct xt_rec *r)
{
    for (u32 n = 0; n < XT_WORDS / 64; n++)
        if (r->map[n])
            return false;
    return true;
}

/*
 * Word i of the page at pa as the guest wrote it: the word, or what it was before it was
 * replaced. A recorded word that is not what an access becomes was written by somebody who
 * does not come through stage 2: it is the guest's as it is, and no longer recorded.
 */
static u32 xt_view(struct xt_rec *r, u64 pa, u32 i)
{
    u32 word = ((u32 *)pa)[i];

    if (!r || !xt_marked(r, i))
        return word;

    u32 orig = hv_pmu_original_word(word);

    if (orig)
        return orig;
    r->map[i / 64] &= ~BIT(i % 64);
    xt_stats.stale++;
    return word;
}

/* A page with a record and nothing recorded in it any more is code with nothing replaced */
static void xt_settle(u64 page)
{
    struct xt_rec *r = xt_find(page);

    if (!r || !xt_empty(r))
        return;
    xt_del(r);
    hv_pt_set_scanned(page, HV_XPAGE_SCANNED);
}

/* len bytes at off of the page at this IPA and pa, as the guest wrote them */
static void xt_read(u64 page, u64 pa, u32 off, u32 len, u8 *buf)
{
    struct xt_rec *r = xt_find(page);
    u64 stale = xt_stats.stale;

    for (u32 n = 0; n < len; n++, off++)
        buf[n] = xt_view(r, pa, off / 4) >> (8 * (off % 4));
    if (xt_stats.stale != stale)
        xt_settle(page);
}

/* A store as the guest would have done it: in one piece where the architecture has it so */
static void xt_put(u64 pa, const u8 *buf, u32 len)
{
    u64 val = 0;

    if (len == 2 || len == 4 || len == 8)
        for (u32 i = 0; i < len; i++)
            val |= (u64)buf[i] << (8 * i);

    switch (len) {
        case 2:
            *(volatile u16 *)pa = val;
            return;
        case 4:
            *(volatile u32 *)pa = val;
            return;
        case 8:
            *(volatile u64 *)pa = val;
            return;
        default:
            for (u32 i = 0; i < len; i++)
                ((volatile u8 *)pa)[i] = buf[i];
            return;
    }
}

static void xt_sync_code(void *addr, u64 size)
{
    /*
     * The guest may have executed these words before, through any address. Clean them to the
     * point of coherency and drop every instruction cache entry on every core.
     */
    dc_cvau_range(addr, size);
    sysop("dsb ish");
    ic_ialluis();
    sysop("dsb ish");
    sysop("isb");
}

/*
 * The guest writes len bytes at off into a page of code, at this IPA and pa, that stays code:
 * the store of an instruction that is in the page. The words written to are what the guest
 * says, but for those that are accesses now, which are replaced and recorded. Returns -1,
 * with nothing written, if there is no room for the record that this needs.
 */
static int xt_write(u64 page, u64 pa, u32 off, u32 len, const u8 *buf)
{
    struct xt_rec *r = xt_find(page);
    u32 *words = (u32 *)pa;
    u32 first = off / 4;
    u32 nr = (off + len + 3) / 4 - first;
    u32 raw[XT_MAX_ACCESS / 4 + 1];
    u64 is_now = 0, was = 0;

    for (u32 n = 0; n < nr; n++) {
        u32 word = xt_view(r, pa, first + n);

        if (r && xt_marked(r, first + n))
            was |= BIT(n);
        for (u32 b = 0; b < 4; b++) {
            u32 at = 4 * (first + n) + b;

            if (at >= off && at < off + len)
                word = (word & ~(0xffU << (8 * b))) | ((u32)buf[at - off] << (8 * b));
        }
        if (hv_pmu_is_access(word)) {
            is_now |= BIT(n);
            word = hv_pmu_patch_word(word);
        }
        raw[n] = word;
    }

    /* Nothing replaced before or after: the bytes go in as the guest's store would put them */
    if (!is_now && !was) {
        xt_put(pa + off, buf, len);
        if (r)
            xt_settle(page);
        return 0;
    }

    if (is_now && !r) {
        r = xt_add(page);
        if (!r)
            return -1;
        /* Not to be read from the moment that a replaced word is in it */
        if (hv_pt_set_scanned(page, HV_XPAGE_XONLY) < 0) {
            xt_del(r);
            return -1;
        }
    }

    for (u32 n = 0; n < nr; n++) {
        u32 i = first + n;

        if (words[i] != raw[n])
            *(volatile u32 *)&words[i] = raw[n];
        if (is_now & BIT(n))
            r->map[i / 64] |= BIT(i % 64);
        else
            r->map[i / 64] &= ~BIT(i % 64);
    }
    if (is_now)
        xt_sync_code(&words[first], 4 * nr);
    if (is_now != was)
        xt_stats.rerecorded++;

    /* ...and to be read again when the last one is gone */
    xt_settle(page);
    return 0;
}

/* A page of code becomes data: as the guest wrote it, every replaced word put back */
static int xt_to_data(u64 page, u64 pa)
{
    if (hv_pt_set_scanned(page, HV_XPAGE_NONE) < 0)
        return -1;

    struct xt_rec *r = xt_find(page);

    if (r) {
        u32 *words = (u32 *)pa;

        for (u32 i = 0; i < XT_WORDS; i++) {
            if (!r->map[i / 64]) {
                i |= 63;
                continue;
            }
            if (xt_marked(r, i))
                words[i] = xt_view(r, pa, i);
        }
        xt_del(r);
    }

    xt_stats.to_data++;
    return hv_pt_set_scanned(page, HV_XPAGE_UNSCANNED);
}

/*
 * A page of data becomes code: scanned, what has to be replaced in it replaced and recorded.
 * Returns the state it is in now, or -1 if it is data still: no room for the record.
 */
static int xt_to_code(u64 page, u64 pa)
{
    int state = HV_XPAGE_SCANNED;

    if (hv_pt_set_scanned(page, HV_XPAGE_NONE) < 0)
        return -1;

    if (hv_pmu_count_accesses((u32 *)pa, XT_WORDS)) {
        struct xt_rec *r = xt_add(page);

        if (!r) {
            hv_pt_set_scanned(page, HV_XPAGE_UNSCANNED);
            return -1;
        }
        hv_pmu_patch_range((u32 *)pa, XT_WORDS, r->map);
        xt_sync_code((void *)pa, XT_PAGE);
        state = HV_XPAGE_XONLY;
    }

    if (hv_pt_set_scanned(page, state) < 0)
        return -1;
    return state;
}

/* ---- instruction aborts ---- */

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

    u64 page = ipa & ~(XT_PAGE - 1);
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
        xt_stats.done++;
        return true;
    }

    u64 t0 = mrs(CNTPCT_EL0);

    state = xt_to_code(page, pa);
    if (state < 0) {
        printf("HV: execution tracking stops the guest: it executes 0x%lx (IPA 0x%lx), a page "
               "with instructions to replace, and there is room to record %d such pages\n",
               far, ipa, XT_MAX_PAGES);
        xt_stats.stops++;
        return false;
    }

    u64 t = mrs(CNTPCT_EL0) - t0;
    xt_stats.scans++;
    if (state == HV_XPAGE_XONLY)
        xt_stats.hits++;
    if (FIELD_GET(SPSR_M, hv_get_spsr()) >> 2 == 0)
        xt_stats.scans_el0++;
    xt_stats.ticks += t;
    if (t > xt_stats.longest)
        xt_stats.longest = t;

    xtrack_last[ctx->cpu_id].page = page;
    xtrack_last[ctx->cpu_id].repeats = 0;
    return true;
}

/* ---- data aborts ---- */

static int xt_stop(struct exc_info *ctx, u64 ipa, u32 insn, const char *why)
{
    printf("HV: execution tracking stops the guest: %s. The instruction 0x%08x at 0x%lx, the "
           "address 0x%lx (IPA 0x%lx), ESR 0x%lx\n",
           why, insn, ctx->elr, hv_get_far(), ipa, hv_get_esr());
    xt_stats.stops++;
    return HV_XTRACK_STOP;
}

static u64 xt_reg(struct exc_info *ctx, u32 r)
{
    /* Register 31 is the zero register wherever this is asked */
    return r == 31 ? 0 : ctx->regs[r];
}

static void xt_set_reg(struct exc_info *ctx, u32 r, u64 val)
{
    if (r != 31)
        ctx->regs[r] = val;
}

static u64 xt_le(const u8 *buf, u32 size)
{
    u64 val = 0;

    for (u32 i = 0; i < size; i++)
        val |= (u64)buf[i] << (8 * i);
    return val;
}

static u64 xt_sext(u64 val, u32 size)
{
    if (size < 8 && (val >> (8 * size - 1)) & 1)
        val |= ~0UL << (8 * size);
    return val;
}

/*
 * The stack pointer that the guest was using: SP_EL1 at EL1h, SP_EL0 at EL1t and at EL0. Both
 * are in the context while the hypervisor runs: hv_exc.c reads them on the way in and writes
 * them back on the way out, so what is to change has to change there.
 */
static u64 *xt_sp(struct exc_info *ctx)
{
    return &ctx->sp[(hv_get_spsr() & 0xf) == 5 ? 1 : 0];
}

union xt_simd {
    u8 b[16];
    u64 d[2];
};

/*
 * Carry out the instruction d, which is in the page pc_page, has faulted on stage 2 and is
 * one that reads an execute-only page or writes to the page it is in. Returns
 * HV_XTRACK_SKIP, or HV_XTRACK_STOP with nothing changed.
 */
static int xt_carry(struct exc_info *ctx, const struct hv_xinsn *d, u32 insn, u64 ipa, u64 pc_page,
                    u32 *flags)
{
    u64 *sp = xt_sp(ctx);
    u64 va = hv_xinsn_addr(d, ctx->regs, *sp, ctx->elr);
    u32 len = d->size * d->count;
    bool rd = d->kind == XI_LOAD || d->kind == XI_ATOMIC || d->kind == XI_CAS;
    bool wr = d->kind != XI_LOAD;

    if (d->kind == XI_ZVA) {
        /* DCZID_EL0.BS: the binary logarithm of the block size in words */
        len = 4 << (mrs(DCZID_EL0) & 0xf);
        if (len > XT_MAX_ACCESS)
            return xt_stop(ctx, ipa, insn, "DC ZVA of a block that is larger than 64 bytes");
        va &= ~(u64)(len - 1);
    }

    /*
     * The core says where it faulted, and that has to be a byte that the instruction accesses,
     * or the instruction read here is not the one that faulted. Bits 63:56 of the address may
     * be a tag of the guest's, which FAR_EL2 does not have to show.
     */
    if (((hv_get_far() - va) & MASK(56)) >= len)
        return xt_stop(ctx, ipa, insn, "the instruction does not access the address that faulted");

    /* The page that each part of the access is in: two at most */
    struct {
        u64 page;
        u64 pa;
        u32 off;
        u32 len;
    } part[2];
    u32 parts = 0;

    for (u32 done = 0; done < len; parts++) {
        u64 at = hv_translate(va + done, true, wr, NULL);
        u64 pa;

        if (!at)
            return xt_stop(ctx, ipa, insn,
                           "the guest's stage 1 does not translate part of the access");
        if (hv_pt_scanned(at, &pa) < 0)
            return xt_stop(ctx, ipa, insn, "part of the access is not in memory");

        part[parts].page = at & ~(XT_PAGE - 1);
        part[parts].pa = pa;
        part[parts].off = at & (XT_PAGE - 1);
        part[parts].len = min(len - done, XT_PAGE - part[parts].off);
        done += part[parts].len;
    }
    if (parts == 2)
        *flags |= XT_PC_TWO_PAGES;

    /* As strong as the ordering of any instruction that this stands in for */
    sysop("dmb ish");

    u8 old[XT_MAX_ACCESS];
    u8 new[XT_MAX_ACCESS];
    union xt_simd simd[32];
    u64 old_val = 0;

    if (rd)
        for (u32 i = 0, done = 0; i < parts; done += part[i++].len)
            xt_read(part[i].page, part[i].pa, part[i].off, part[i].len, old + done);
    if (d->simd)
        get_simd_state(simd);

    switch (d->kind) {
        case XI_STORE:
            for (u32 k = 0; k < d->count; k++) {
                u32 r = k ? d->rt2 : d->rt;
                u64 val = d->simd ? 0 : xt_reg(ctx, r);

                for (u32 i = 0; i < d->size; i++)
                    new[k * d->size + i] = d->simd ? simd[r].b[i] : val >> (8 * i);
            }
            break;
        case XI_ZVA:
            for (u32 i = 0; i < len; i++)
                new[i] = 0;
            break;
        case XI_ATOMIC: {
            u64 mask = d->size == 8 ? ~0UL : BIT(8 * d->size) - 1;
            u64 rs = xt_reg(ctx, d->rs) & mask;
            u64 val;

            old_val = xt_le(old, d->size);
            switch (d->op) {
                case XI_OP_ADD:
                    val = old_val + rs;
                    break;
                case XI_OP_CLR:
                    val = old_val & ~rs;
                    break;
                case XI_OP_EOR:
                    val = old_val ^ rs;
                    break;
                case XI_OP_SET:
                    val = old_val | rs;
                    break;
                case XI_OP_SMAX:
                    val = (s64)xt_sext(old_val, d->size) > (s64)xt_sext(rs, d->size) ? old_val : rs;
                    break;
                case XI_OP_SMIN:
                    val = (s64)xt_sext(old_val, d->size) < (s64)xt_sext(rs, d->size) ? old_val : rs;
                    break;
                case XI_OP_UMAX:
                    val = old_val > rs ? old_val : rs;
                    break;
                case XI_OP_UMIN:
                    val = old_val < rs ? old_val : rs;
                    break;
                default:
                    /* XI_OP_SWP */
                    val = rs;
                    break;
            }
            for (u32 i = 0; i < d->size; i++)
                new[i] = val >> (8 * i);
            break;
        }
        case XI_CAS: {
            u64 mask = d->size == 8 ? ~0UL : BIT(8 * d->size) - 1;
            u64 val = xt_reg(ctx, d->rt);

            old_val = xt_le(old, d->size);
            /* Not what was expected: nothing is stored */
            if (old_val != (xt_reg(ctx, d->rs) & mask))
                wr = false;
            for (u32 i = 0; i < d->size; i++)
                new[i] = val >> (8 * i);
            break;
        }
        default:
            break;
    }

    if (wr) {
        /* The part in the instruction's own page first: the one there may be no room for */
        for (u32 i = 0, done = 0; i < parts; done += part[i++].len)
            if (part[i].page == pc_page &&
                xt_write(part[i].page, part[i].pa, part[i].off, part[i].len, new + done) < 0)
                return xt_stop(ctx, ipa, insn,
                               "a store puts an instruction to replace into a page that had "
                               "none, and there is no room to record another such page");

        for (u32 i = 0, done = 0; i < parts; done += part[i++].len) {
            u64 pa;

            if (part[i].page == pc_page)
                continue;
            /* The other page of the two: if it is code, it is data from now on */
            if (hv_pt_scanned(part[i].page, &pa) != HV_XPAGE_UNSCANNED &&
                xt_to_data(part[i].page, part[i].pa) < 0)
                return xt_stop(ctx, ipa, insn, "a page of code could not be made data");
            xt_put(part[i].pa + part[i].off, new + done, part[i].len);
        }
    }

    /* Memory is done. Now the registers */
    switch (d->kind) {
        case XI_LOAD:
            for (u32 k = 0; k < d->count; k++) {
                u32 r = k ? d->rt2 : d->rt;
                const u8 *from = old + k * d->size;

                if (d->simd) {
                    simd[r].d[0] = 0;
                    simd[r].d[1] = 0;
                    for (u32 i = 0; i < d->size; i++)
                        simd[r].b[i] = from[i];
                    continue;
                }

                u64 val = xt_le(from, d->size);

                if (d->sext)
                    val = xt_sext(val, d->size);
                if (!d->reg64)
                    val &= 0xffffffffUL;
                xt_set_reg(ctx, r, val);
            }
            if (d->simd)
                put_simd_state_masked(simd, BIT(d->rt) | (d->count == 2 ? BIT(d->rt2) : 0));
            break;
        case XI_ATOMIC:
            xt_set_reg(ctx, d->rt, old_val);
            xt_stats.atomics++;
            break;
        case XI_CAS:
            xt_set_reg(ctx, d->rs, old_val);
            xt_stats.atomics++;
            break;
        default:
            break;
    }

    if ((d->kind == XI_LOAD || d->kind == XI_STORE) && (d->mode == XI_PRE || d->mode == XI_POST)) {
        if (d->rn != 31)
            ctx->regs[d->rn] += d->imm;
        else
            *sp += d->imm;
    }

    sysop("dmb ish");

    if (d->kind == XI_LOAD)
        xt_stats.reads_insn++;
    else
        xt_stats.writes_insn++;
    return HV_XTRACK_SKIP;
}

static void xt_count_pc(u64 pc, u32 insn, u32 flags)
{
    u32 i = xt_hash(pc >> 2) & (XT_MAX_PCS - 1);

    for (u32 n = 0; n < XT_MAX_PCS; n++, i = (i + 1) & (XT_MAX_PCS - 1)) {
        if (xt_pcs[i].count && xt_pcs[i].pc != pc)
            continue;
        xt_pcs[i].pc = pc;
        xt_pcs[i].insn = insn;
        if (!xt_pcs[i].count)
            xt_pcs[i].flags = 0;
        xt_pcs[i].flags |= flags;
        xt_pcs[i].count++;
        return;
    }
    xt_stats.pcs_lost++;
}

/*
 * An access to a page of code that ESR_EL2 does not describe, or not well enough. same: the
 * instruction is in the page, whose state is given.
 */
static int xt_from_insn(struct exc_info *ctx, u64 ipa, u64 pa, int state, bool same, bool is_write)
{
    u64 t0 = mrs(CNTPCT_EL0);
    u64 page = ipa & ~(XT_PAGE - 1);
    u64 insn_pa = hv_translate_insn(ctx->elr);

    if (!insn_pa)
        return xt_stop(ctx, ipa, 0, "the instruction cannot be read");

    u32 insn = *(u32 *)insn_pa;
    u32 flags = (same ? XT_PC_SAME : 0) | (is_write ? XT_PC_WRITE : XT_PC_READ) |
                (FIELD_GET(SPSR_M, hv_get_spsr()) >> 2 == 0 ? XT_PC_EL0 : 0);
    struct hv_xinsn d;
    int ret;

    if ((insn & ~31U) == DC_IVAC) {
        dc_civac_range((void *)(pa + (ipa & (XT_PAGE - 1))), 1);
        xt_stats.ivac++;
        ret = HV_XTRACK_SKIP;
    } else if (!hv_xinsn_decode(insn, &d)) {
        flags |= XT_PC_UNKNOWN;
        if (same)
            return xt_stop(ctx, ipa, insn,
                           "an instruction that EL2 does not know how to carry out reads or "
                           "writes the page of code that it is in");
        if (xt_to_data(page, pa) < 0)
            return xt_stop(ctx, ipa, insn, "a page of code could not be made data");
        if (!is_write)
            xt_stats.to_data_read++;
        ret = HV_XTRACK_RETRY;
    } else if (d.kind == XI_LOAD) {
        /* A load that faults on a page that may be read, or as a store: not this instruction */
        if (state != HV_XPAGE_XONLY || is_write)
            return HV_XTRACK_NONE;
        ret = xt_carry(ctx, &d, insn, ipa, same ? page : 0, &flags);
    } else if (!same) {
        /* An atomic operation from elsewhere, on a page that is not to be read */
        flags |= XT_PC_WRITE;
        if (xt_to_data(page, pa) < 0)
            return xt_stop(ctx, ipa, insn, "a page of code could not be made data");
        ret = HV_XTRACK_RETRY;
    } else {
        flags |= XT_PC_WRITE;
        ret = xt_carry(ctx, &d, insn, ipa, page, &flags);
    }

    xt_count_pc(ctx->elr, insn, flags);
    xt_stats.insn_ticks += mrs(CNTPCT_EL0) - t0;
    return ret;
}

/*
 * A data abort on memory that has a hardware mapping; ipa is where stage 1 of the guest puts
 * the address that faulted, and ctx->elr where the instruction is. Returns
 *
 *   HV_XTRACK_SKIP   the access was carried out here: the guest goes on behind the instruction
 *   HV_XTRACK_RETRY  the guest is to execute the instruction again
 *   HV_XTRACK_STOP   the guest cannot go on; a line has been printed that says why
 *   HV_XTRACK_NONE   nothing here explains the abort
 */
int hv_xtrack_dabort(struct exc_info *ctx, u64 ipa, bool is_write)
{
    u64 pa;

    if (!hv_pt_xtrack_on())
        return HV_XTRACK_NONE;

    u64 esr = hv_get_esr();

    if (!FSC_PERM(esr & ISS_FSC))
        return HV_XTRACK_NONE;

    /*
     * Stage 2 refused an access of a walk of the guest's stage 1 tables: one of them is in a
     * page of code. The address that faulted is not in that page. HPFAR_EL2 is to hold it for
     * this kind of fault (Arm DDI 0487 M.d, D1.4.2.1, rule RFKLWR); whether it does on the M4
     * has not been tried, and for the other kinds it does not. So the page is not looked for.
     */
    if (esr & ISS_S1PTW)
        return xt_stop(ctx, ipa, 0,
                       "stage 2 refused a walk of the guest's stage 1 tables, one of which must "
                       "be in a page of code");

    u64 page = ipa & ~(XT_PAGE - 1);
    int state = hv_pt_scanned(page, &pa);

    /* Only code is kept from being written, and only code with replaced words from being read */
    if (state != HV_XPAGE_SCANNED && state != HV_XPAGE_XONLY)
        return HV_XTRACK_NONE;
    if (state == HV_XPAGE_SCANNED && !is_write)
        return HV_XTRACK_NONE;

    /*
     * Does ESR_EL2 describe the access, and is all of it in this page? The address that the
     * core reports is the first byte that faulted: at the start of a page that may be the
     * middle of an access that began in the page before.
     */
    u32 off = ipa & (XT_PAGE - 1);
    u32 size = 1 << FIELD_GET(ISS_SAS, esr);
    u32 rt = FIELD_GET(ISS_SRT, esr);
    bool whole = (esr & ISS_ISV) && !(off == 0 && size > 1) && off + size <= XT_PAGE;
    u8 buf[8];

    if (whole && !is_write) {
        xt_read(page, pa, off, size, buf);

        u64 val = xt_le(buf, size);

        if (esr & ISS_SSE)
            val = xt_sext(val, size);
        if (!(esr & ISS_SF))
            val &= 0xffffffffUL;
        xt_set_reg(ctx, rt, val);

        xt_stats.reads_syn++;
        if (FIELD_GET(SPSR_M, hv_get_spsr()) >> 2 == 0)
            xt_stats.reads_el0++;
        return HV_XTRACK_SKIP;
    }

    /* Where the instruction is: in this page or in another */
    u64 pc = hv_translate(ctx->elr, true, false, NULL);

    if (!pc)
        return xt_stop(ctx, ipa, 0, "the guest's stage 1 does not say where the instruction is");

    bool same = (pc & ~(XT_PAGE - 1)) == page;

    /*
     * A store from another page: the page becomes data and the guest stores again. Not for a
     * cache maintenance instruction (ISS bit 8, CM), which writes nothing.
     */
    if (is_write && !same && !(esr & BIT(8))) {
        if (xt_to_data(page, pa) < 0)
            return xt_stop(ctx, ipa, 0, "a page of code could not be made data");
        return HV_XTRACK_RETRY;
    }

    if (whole && is_write) {
        u64 val = xt_reg(ctx, rt);

        for (u32 i = 0; i < size; i++)
            buf[i] = val >> (8 * i);
        if (xt_write(page, pa, off, size, buf) < 0)
            return xt_stop(ctx, ipa, 0,
                           "a store puts an instruction to replace into a page that had none, "
                           "and there is no room to record another such page");
        xt_stats.writes_syn++;
        return HV_XTRACK_SKIP;
    }

    return xt_from_insn(ctx, ipa, pa, state, same, is_write);
}

void hv_xtrack_report(void)
{
    if (!hv_pt_xtrack_on())
        return;

    u64 freq = mrs(CNTFRQ_EL0);
    u64 us = freq ? xt_stats.ticks * 1000000 / freq : 0;
    u64 longest = freq ? xt_stats.longest * 1000000 / freq : 0;
    u64 insn_us = freq ? xt_stats.insn_ticks * 1000000 / freq : 0;

    printf("HV: execution tracking: %ld pages scanned before they ran (%ld of them for EL0, %ld "
           "had instructions to replace), %ld us in all, longest %ld us; %ld faults on pages "
           "already done\n",
           xt_stats.scans, xt_stats.scans_el0, xt_stats.hits, us, longest, xt_stats.done);
    printf("HV: execution tracking: %d pages with replaced instructions now, %ld at most, room "
           "for %d; %ld times a page of code became data again (%ld of them for a read that "
           "EL2 does not know how to carry out)\n",
           xt_nr_recs, xt_stats.recs_max, XT_MAX_PAGES, xt_stats.to_data, xt_stats.to_data_read);
    printf("HV: execution tracking: reads of pages with replaced instructions carried out in "
           "EL2: %ld from what ESR_EL2 says (%ld from EL0), %ld from the instruction; stores "
           "into the page that the instruction is in: %ld and %ld (%ld atomic); %ld stores "
           "changed which words are replaced; %ld dc ivac\n",
           xt_stats.reads_syn, xt_stats.reads_el0, xt_stats.reads_insn, xt_stats.writes_syn,
           xt_stats.writes_insn, xt_stats.atomics, xt_stats.rerecorded, xt_stats.ivac);
    printf("HV: execution tracking: %ld us in all for what was done from the instruction; %ld "
           "replaced words found changed from outside; %ld times the guest was stopped; no "
           "room to list %ld instructions\n",
           insn_us, xt_stats.stale, xt_stats.stops, xt_stats.pcs_lost);

    /* For the host to take apart: one line for each instruction that was taken apart here */
    for (u32 i = 0; i < XT_MAX_PCS; i++)
        if (xt_pcs[i].count)
            printf("HV: xt pc 0x%lx 0x%08x %ld %d\n", xt_pcs[i].pc, xt_pcs[i].insn, xt_pcs[i].count,
                   xt_pcs[i].flags);
}
