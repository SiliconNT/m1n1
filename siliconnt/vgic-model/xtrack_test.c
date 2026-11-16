/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/hv/hv_xtrack.c, with the real src/hv/hv_pmu.c and
 * src/hv/hv_xinsn.c linked in.
 *
 * The rule under test: whatever the guest does, what it reads from its memory as data is what
 * it wrote there, and no page that it may execute has an access to a PMUv3 register in it.
 *
 * So there are two copies of a few pages of memory. arena is the memory, which hv_xtrack.c
 * changes; ref is what the guest wrote, which only this file changes. A guest is played that
 * does things in random order: it executes pages, and it runs random instructions that read
 * and write them, from the page they access and from others, within a page and across two.
 * The instructions are random words that the decoder takes, with registers set up so that
 * they access where the test wants. A core is played too: an access that the state of the
 * page allows happens here, on both copies; one that it does not allow is a Data Abort for
 * hv_xtrack_dabort(), and what that does to the registers and to the memory is compared with
 * what the instruction does when this file carries it out (execute()) on ref.
 *
 * After every step, for every page:
 *   - a page of data, and a page of code with nothing replaced, is byte for byte what the
 *     guest wrote;
 *   - a page of code has no access in it, and it is execute-only exactly if what the guest
 *     wrote there has one;
 *   - read through hv_xtrack_dabort(), an execute-only page is byte for byte what the guest
 *     wrote.
 *
 * The Makefile drops the #include lines of hv_xtrack.c; this file stands in for them and for
 * the page tables: one state for each 4 KB page. The table of pages with replaced
 * instructions has room for XT_MAX_PAGES here, fewer than there are pages, so that it fills.
 *
 * With BROKEN defined to a number the Makefile has changed hv_xtrack.c on its way here: the
 * test must fail then (make xtrack-broken).
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "hv.h"
#include "arm_cpu_regs.h"
#include "hv_xinsn.h"
#include "hv_xpage.h" /* HV_XPAGE_* and HV_XTRACK_* of the real hv.h, see the Makefile */
#include "smp.h"

#define XT_MAX_PAGES 4

/* The registers that hv_xtrack.c reads and writes */
static u64 test_CNTPCT_EL0, test_CNTFRQ_EL0 = 1000, test_SP_EL0, test_SP_EL1;
static u64 test_DCZID_EL0 = 4; /* blocks of 64 bytes */
#define mrs(reg)      (test_##reg)
#define msr(reg, val) (test_##reg = (val))
#define sysop(op)     ((void)0)
#define min(a, b)     ((a) < (b) ? (a) : (b))

#define ARENA_IPA 0x10100000000UL
#define NR_PAGES  8
#define PAGE      0x1000UL
#define WORDS     1024 /* in a page */

#define MRS_PMCCNTR 0xd53b9d00 /* mrs x0, PMCCNTR_EL0 */
#define MRS_PMCR    0xd53b9c00 /* mrs x0, PMCR_EL0 */
#define MSR_PMCR    0xd51b9c00 /* msr PMCR_EL0, x0 */
#define MRS_CNTVCT  0xd53be040 /* mrs x0, CNTVCT_EL0 */
#define I_LDR       0xf9400020 /* ldr x0, [x1] */
#define I_LDXR      0xc85f7c20 /* ldxr x0, [x1] */
#define I_STXR      0xc8027c20 /* stxr w2, x0, [x1] */
#define I_LD1       0x4c407020 /* ld1 {v0.16b}, [x1] */
#define I_DC_IVAC   0xd5087620 /* dc ivac, x0 */

static u8 arena[NR_PAGES * PAGE] __attribute__((aligned(0x1000)));
static u8 ref[NR_PAGES * PAGE] __attribute__((aligned(0x1000)));
static int state[NR_PAGES];

static u64 test_esr, test_far, test_spsr = 5;
static u32 test_insn;
static bool test_insn_unreadable;
static int test_unmapped = -1; /* a page that stage 1 of the guest does not map */
static u8 test_simd[32][16];
static u64 test_civac;
static bool test_xtrack_on = true;

static unsigned long checks, failures;
/* The step that is being checked, for whoever reads a failure */
static char doing[200] = "nothing yet";

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    if (failures++ > 30)
        return;
    printf("FAIL (%s): ", doing);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ---- what hv_xtrack.c uses of the hypervisor ---- */

static void dc_cvau_range(void *addr, size_t size)
{
    UNUSED(addr);
    UNUSED(size);
}

static void dc_civac_range(void *addr, size_t size)
{
    UNUSED(size);
    test_civac = (u64)addr;
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

static int page_of(u64 ipa)
{
    if (ipa < ARENA_IPA || ipa >= ARENA_IPA + NR_PAGES * PAGE)
        return -1;
    return (ipa - ARENA_IPA) >> 12;
}

/* Stage 1 of the guest maps every address to itself, but for one page when a test says so */
static u64 hv_translate(u64 addr, bool s1, bool w, u64 *par_out)
{
    UNUSED(s1);
    UNUSED(w);
    UNUSED(par_out);
    /* The top byte is a tag that stage 1 ignores */
    addr &= MASK(56);
    if (test_unmapped >= 0 && page_of(addr) == test_unmapped)
        return 0;
    return addr;
}

/* The instruction that the guest is executing is not in the arena: it is whatever the test says */
static u64 hv_translate_insn(u64 addr)
{
    UNUSED(addr);
    return test_insn_unreadable ? 0 : (u64)&test_insn;
}

static bool hv_pt_xtrack_on(void)
{
    return test_xtrack_on;
}

static int hv_pt_scanned(u64 ipa, u64 *pa)
{
    int n = page_of(ipa);

    if (n < 0)
        return -1;
    *pa = (u64)&arena[n * PAGE];
    return state[n];
}

static bool is_code(int st)
{
    return st == HV_XPAGE_SCANNED || st == HV_XPAGE_XONLY;
}

static int hv_pt_set_scanned(u64 ipa, int st)
{
    int n = page_of(ipa);

    if (n < 0)
        return -1;
    /* Between data and code, a page is nobody's for a while: its content is being changed */
    check(!(state[n] == HV_XPAGE_UNSCANNED && is_code(st)) &&
              !(is_code(state[n]) && st == HV_XPAGE_UNSCANNED),
          "page %d goes from state %d to %d without being taken from the guest first", n,
          state[n], st);
    state[n] = st;
    return 0;
}

static void get_simd_state(void *st)
{
    memcpy(st, test_simd, sizeof(test_simd));
}

static void put_simd_state_masked(const void *st, u32 mask)
{
    for (int i = 0; i < 32; i++)
        if (mask & BIT(i))
            memcpy(test_simd[i], (const u8 *)st + 16 * i, 16);
}

#ifndef XTRACK_INC
#define XTRACK_INC "hv_xtrack.inc"
#endif
#include XTRACK_INC

/* ---- helpers ---- */

static u64 rnd_state = 0x853c49e6748fea9bUL;

static u64 rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

static u32 rnd_below(u32 n)
{
    return (rnd() >> 16) % n;
}

static u64 ipa_of(int page, u32 off)
{
    return ARENA_IPA + page * PAGE + off;
}

static u32 ref_word(int page, int i)
{
    u32 w;

    memcpy(&w, &ref[page * PAGE + 4 * i], 4);
    return w;
}

static u32 arena_word(int page, int i)
{
    u32 w;

    memcpy(&w, &arena[page * PAGE + 4 * i], 4);
    return w;
}

static bool has_access(int page)
{
    for (int i = 0; i < WORDS; i++)
        if (hv_pmu_is_access(ref_word(page, i)))
            return true;
    return false;
}

static int nr_xonly(void)
{
    int n = 0;

    for (int p = 0; p < NR_PAGES; p++)
        n += state[p] == HV_XPAGE_XONLY;
    return n;
}

/* An access to a PMUv3 register, of one kind or another */
static u32 an_access(void)
{
    switch (rnd_below(4)) {
        case 0:
            return MRS_PMCCNTR | rnd_below(32);
        case 1:
            return MRS_PMCR | rnd_below(32);
        case 2:
            return MSR_PMCR | rnd_below(32);
        default:
            /* op1 3, CRn 9, CRm 12 to 14, any op2, read or write */
            return 0xd51b9000 | (rnd_below(2) << 21) | ((12 + rnd_below(3)) << 8) |
                   (rnd_below(8) << 5) | rnd_below(32);
    }
}

/* A word that is neither an access nor what one becomes */
static u32 a_plain_word(void)
{
    for (;;) {
        u32 w = rnd();

        if (!hv_pmu_is_access(w) && !hv_pmu_original_word(w))
            return w;
    }
}

/* ---- the core ---- */

struct cpu {
    u64 x[31];
    u64 sp;
    u8 v[32][16];
};

static u64 get_le(const u8 *p, u32 size)
{
    u64 v = 0;

    for (u32 i = 0; i < size; i++)
        v |= (u64)p[i] << (8 * i);
    return v;
}

static void put_le(u8 *p, u64 v, u32 size)
{
    for (u32 i = 0; i < size; i++)
        p[i] = v >> (8 * i);
}

static s64 as_signed(u64 v, u32 size)
{
    return size == 8 ? (s64)v : (s64)(v << (64 - 8 * size)) >> (64 - 8 * size);
}

static u64 reg_of(const struct cpu *c, u32 r)
{
    return r == 31 ? 0 : c->x[r];
}

/* The bytes of mem (which starts at ARENA_IPA) that the instruction accesses */
static void span(const struct hv_xinsn *d, const struct cpu *c, u64 pc, u64 *va, u32 *len)
{
    *va = hv_xinsn_addr(d, c->x, c->sp, pc);
    *len = d->size * d->count;
    if (d->kind == XI_ZVA) {
        *len = 64;
        *va &= ~63UL;
    }
}

/*
 * What the instruction does: this file's own word on it, to hold against hv_xtrack.c. mem is
 * the memory, from ARENA_IPA on.
 */
static void execute(const struct hv_xinsn *d, struct cpu *c, u64 pc, u8 *mem)
{
    u64 va;
    u32 len;

    span(d, c, pc, &va, &len);
    u8 *p = mem + (va - ARENA_IPA);

    switch (d->kind) {
        case XI_LOAD:
            for (u32 k = 0; k < d->count; k++, p += d->size) {
                u32 r = k ? d->rt2 : d->rt;

                if (d->simd) {
                    memset(c->v[r], 0, 16);
                    memcpy(c->v[r], p, d->size);
                    continue;
                }
                u64 v = get_le(p, d->size);
                if (d->sext)
                    v = as_signed(v, d->size);
                if (!d->reg64)
                    v &= 0xffffffffUL;
                if (r != 31)
                    c->x[r] = v;
            }
            break;
        case XI_STORE:
            for (u32 k = 0; k < d->count; k++, p += d->size) {
                u32 r = k ? d->rt2 : d->rt;

                if (d->simd)
                    memcpy(p, c->v[r], d->size);
                else
                    put_le(p, reg_of(c, r), d->size);
            }
            break;
        case XI_ZVA:
            memset(p, 0, len);
            return;
        case XI_ATOMIC: {
            u64 mask = d->size == 8 ? ~0UL : BIT(8 * d->size) - 1;
            u64 old = get_le(p, d->size);
            u64 rs = reg_of(c, d->rs) & mask;
            u64 new;

            switch (d->op) {
                case XI_OP_ADD:
                    new = old + rs;
                    break;
                case XI_OP_CLR:
                    new = old & ~rs;
                    break;
                case XI_OP_EOR:
                    new = old ^ rs;
                    break;
                case XI_OP_SET:
                    new = old | rs;
                    break;
                case XI_OP_SMAX:
                    new = as_signed(old, d->size) > as_signed(rs, d->size) ? old : rs;
                    break;
                case XI_OP_SMIN:
                    new = as_signed(old, d->size) < as_signed(rs, d->size) ? old : rs;
                    break;
                case XI_OP_UMAX:
                    new = old > rs ? old : rs;
                    break;
                case XI_OP_UMIN:
                    new = old < rs ? old : rs;
                    break;
                default:
                    new = rs;
                    break;
            }
            put_le(p, new, d->size);
            if (d->rt != 31)
                c->x[d->rt] = old;
            return;
        }
        case XI_CAS: {
            u64 mask = d->size == 8 ? ~0UL : BIT(8 * d->size) - 1;
            u64 old = get_le(p, d->size);

            if (old == (reg_of(c, d->rs) & mask))
                put_le(p, reg_of(c, d->rt), d->size);
            if (d->rs != 31)
                c->x[d->rs] = old;
            return;
        }
    }

    if (d->mode == XI_PRE || d->mode == XI_POST) {
        if (d->rn == 31)
            c->sp += d->imm;
        else
            c->x[d->rn] += d->imm;
    }
}

/* What the core says of the access in ESR_EL2 when it can: one general-purpose register, no writeback */
static bool has_syndrome(const struct hv_xinsn *d)
{
    return (d->kind == XI_LOAD || d->kind == XI_STORE) && d->count == 1 && !d->simd &&
           d->mode != XI_PRE && d->mode != XI_POST;
}

static u64 syndrome(const struct hv_xinsn *d)
{
    int sas = d->size == 8 ? 3 : d->size == 4 ? 2 : d->size == 2 ? 1 : 0;
    bool sf = d->kind == XI_LOAD ? d->reg64 : d->size == 8;

    return BIT(24) | ((u64)sas << 22) | (d->sext ? BIT(21) : 0) | ((u64)d->rt << 16) |
           (sf ? BIT(15) : 0);
}

static struct {
    u64 native, carried, with_syndrome, retries, stops, two_pages, same_page;
    u64 loads, stores, atomics, cas, zva, simd, wback, literal;
    u64 execs, exec_full, dma, stale;
} seen;

static int data_abort(struct cpu *c, u32 insn, u64 pc, u64 far, u64 esr, bool el0)
{
    struct exc_info ctx = {.cpu_id = 0, .elr = pc};

    for (int i = 0; i < 31; i++)
        ctx.regs[i] = c->x[i];
    ctx.regs[31] = 0x31313131;
    test_esr = esr;
    test_far = far;
    test_insn = insn;
    test_spsr = el0 ? 0 : 5;
    test_SP_EL0 = el0 ? c->sp : 0x5e0;
    test_SP_EL1 = el0 ? 0x5e1 : c->sp;
    memcpy(test_simd, c->v, sizeof(test_simd));

    int ret = hv_xtrack_dabort(&ctx, far, esr & BIT(6));

    for (int i = 0; i < 31; i++)
        c->x[i] = ctx.regs[i];
    /* The address to return to is the caller's to move, and the other stack pointer nobody's */
    check(ctx.regs[31] == 0x31313131 && ctx.elr == pc &&
              (el0 ? test_SP_EL1 == 0x5e1 : test_SP_EL0 == 0x5e0),
          "0x%08x: what is not a register of the instruction was changed", insn);
    c->sp = el0 ? test_SP_EL0 : test_SP_EL1;
    memcpy(c->v, test_simd, sizeof(test_simd));
    return ret;
}

enum { R_DONE, R_STOPPED };

/*
 * The guest executes this instruction at pc, in a page that it may execute. Returns R_DONE
 * when it has been executed, by the core or by hv_xtrack.c, and R_STOPPED when hv_xtrack.c
 * has stopped the guest instead; nothing is changed then.
 */
static int run(u32 insn, u64 pc, struct cpu *c, bool el0, bool with_syndrome)
{
    struct hv_xinsn d;

    if (!hv_xinsn_decode(insn, &d)) {
        check(false, "0x%08x is not an instruction that the test can run", insn);
        return R_STOPPED;
    }

    for (int tries = 0; tries < 6; tries++) {
        u64 va;
        u32 len;

        span(&d, c, pc, &va, &len);

        int first = page_of(va), last = page_of(va + len - 1), fault = -1;
        bool rd = d.kind == XI_LOAD || d.kind == XI_ATOMIC || d.kind == XI_CAS;
        bool wr = d.kind != XI_LOAD;

        for (int p = first; p <= last; p++) {
            if ((rd && state[p] == HV_XPAGE_XONLY) || (wr && state[p] != HV_XPAGE_UNSCANNED)) {
                fault = p;
                break;
            }
        }

        if (fault < 0) {
            /* The core does it. The pages are readable, so they are what the guest wrote */
            execute(&d, c, pc, ref);
            if (wr)
                memcpy(&arena[va - ARENA_IPA], &ref[va - ARENA_IPA], len);
            seen.native++;
            return R_DONE;
        }

        /* The first byte of the access that is in the page that refuses it */
        u64 far = va > ipa_of(fault, 0) ? va : ipa_of(fault, 0);
        /* For an atomic operation: a read if a read would have faulted (D24.2.45, WnR) */
        bool wnr = d.kind == XI_LOAD ? false : rd ? state[fault] != HV_XPAGE_XONLY : true;
        u64 esr = 0x9200000f | (wnr ? BIT(6) : 0);
        bool syn = with_syndrome && has_syndrome(&d);

        if (syn)
            esr |= syndrome(&d);

        struct cpu before = *c;
        int ret = data_abort(c, insn, pc, far, esr, el0);

        if (ret == HV_XTRACK_RETRY) {
            check(!memcmp(&before, c, sizeof(before)),
                  "0x%08x: registers changed although the guest is to execute it again", insn);
            seen.retries++;
            continue;
        }
        if (ret == HV_XTRACK_STOP) {
            check(!memcmp(&before, c, sizeof(before)),
                  "0x%08x: registers changed although the guest was stopped", insn);
            seen.stops++;
            return R_STOPPED;
        }
        if (ret != HV_XTRACK_SKIP) {
            check(false, "0x%08x at 0x%lx, address 0x%lx, page state %d: answer %d", insn, pc, va,
                  state[fault], ret);
            return R_STOPPED;
        }

        /* hv_xtrack.c did it, it says: the registers must be what the instruction leaves */
        struct cpu want = before;

        execute(&d, &want, pc, ref);
        for (int i = 0; i < 31; i++)
            check(c->x[i] == want.x[i], "0x%08x at 0x%lx, address 0x%lx: x%d is 0x%lx, not 0x%lx",
                  insn, pc, va, i, c->x[i], want.x[i]);
        check(c->sp == want.sp, "0x%08x: the stack pointer is 0x%lx, not 0x%lx", insn, c->sp,
              want.sp);
        for (int i = 0; i < 32; i++)
            check(!memcmp(c->v[i], want.v[i], 16), "0x%08x at 0x%lx, address 0x%lx: v%d is wrong",
                  insn, pc, va, i);
        *c = want;

        seen.carried++;
        seen.with_syndrome += syn;
        seen.two_pages += first != last;
        seen.same_page += page_of(pc) == fault;
        seen.loads += d.kind == XI_LOAD;
        seen.stores += d.kind == XI_STORE;
        seen.atomics += d.kind == XI_ATOMIC;
        seen.cas += d.kind == XI_CAS;
        seen.zva += d.kind == XI_ZVA;
        seen.simd += d.simd;
        seen.wback += d.mode == XI_PRE || d.mode == XI_POST;
        seen.literal += d.mode == XI_LITERAL;
        return R_DONE;
    }

    check(false, "0x%08x at 0x%lx does not get done", insn, pc);
    return R_STOPPED;
}

/* The guest executes something in this page. Returns false if it was stopped */
static bool exec_page(int page)
{
    struct exc_info ctx = {.cpu_id = 0, .elr = ipa_of(page, 0)};

    if (state[page] != HV_XPAGE_UNSCANNED)
        return true;

    bool full = has_access(page) && nr_xonly() == XT_MAX_PAGES;

    snprintf(doing, sizeof(doing), "page %d runs", page);

    test_esr = 0x8200000f; /* Instruction Abort from a lower EL, permission fault, level 3 */
    test_far = ipa_of(page, 4 * rnd_below(WORDS));
    test_spsr = rnd_below(2) ? 5 : 0;

    bool ok = hv_xtrack_iabort(&ctx);

    seen.execs++;
    if (full) {
        check(!ok && state[page] == HV_XPAGE_UNSCANNED,
              "page %d runs although there is no room to record what is replaced in it", page);
        seen.exec_full++;
        return false;
    }
    check(ok && is_code(state[page]), "page %d does not run: state %d", page, state[page]);
    return ok;
}

/* ---- what must hold after every step ---- */

/* Read the page as the guest would, 8 bytes at a time, from another page */
static void read_through(int page, u8 *out)
{
    for (u32 off = 0; off < PAGE; off += 8) {
        struct cpu c = {0};
        /* Some reads with a syndrome and some without; the first never has one that counts */
        bool syn = (off / 8) % 3 != 1;

        c.x[1] = ipa_of(page, off);
        if (state[page] != HV_XPAGE_XONLY) {
            memcpy(out + off, &arena[page * PAGE + off], 8);
            continue;
        }
        int ret = data_abort(&c, I_LDR, ipa_of((page + 1) % NR_PAGES, 64), c.x[1],
                             0x9200000f | (syn ? BIT(24) | (3UL << 22) | BIT(15) : 0), false);
        if (ret == HV_XTRACK_NONE && state[page] != HV_XPAGE_XONLY) {
            /* The page has become readable on the way: nothing replaced was left in it */
            memcpy(out + off, &arena[page * PAGE + off], 8);
            continue;
        }
        check(ret == HV_XTRACK_SKIP, "reading page %d at 0x%x: answer %d", page, off, ret);
        memcpy(out + off, &c.x[0], 8);
    }
}

static void verify(const char *what, u64 step)
{
    static u8 got[PAGE];

    for (int p = 0; p < NR_PAGES; p++) {
        u8 *a = &arena[p * PAGE], *r = &ref[p * PAGE];

        /* What a read gives first: it may find that something replaced is gone (DMA) */
        if (state[p] == HV_XPAGE_XONLY) {
            read_through(p, got);
            for (u32 i = 0; i < PAGE; i++)
                if (got[i] != r[i]) {
                    check(false, "%s (step %lu): page %d read through EL2 has 0x%02x at 0x%x, the "
                                 "guest wrote 0x%02x",
                          what, step, p, got[i], i, r[i]);
                    break;
                }
        }

        check(state[p] != HV_XPAGE_NONE, "%s (step %lu): page %d is left with no access", what,
              step, p);
        if (state[p] == HV_XPAGE_UNSCANNED || state[p] == HV_XPAGE_SCANNED)
            for (u32 i = 0; i < PAGE; i++)
                if (a[i] != r[i]) {
                    check(false, "%s (step %lu): page %d, state %d, has 0x%02x at 0x%x, the guest "
                                 "wrote 0x%02x",
                          what, step, p, state[p], a[i], i, r[i]);
                    break;
                }
        if (is_code(state[p])) {
            for (int i = 0; i < WORDS; i++)
                if (hv_pmu_is_access(arena_word(p, i))) {
                    check(false, "%s (step %lu): page %d may be executed and has the access 0x%08x "
                                 "at word %d",
                          what, step, p, arena_word(p, i), i);
                    break;
                }
            check((state[p] == HV_XPAGE_XONLY) == has_access(p),
                  "%s (step %lu): page %d is in state %d and what the guest wrote there has %s "
                  "access",
                  what, step, p, state[p], has_access(p) ? "an" : "no");
        }
    }
    check(xt_nr_recs == (u32)nr_xonly(), "%s (step %lu): %u records for %d execute-only pages",
          what, step, xt_nr_recs, nr_xonly());
}

/* ---- the guest ---- */

/* A page that may be executed, made one if there is none. -1 if none can be had */
static int a_code_page(void)
{
    int start = rnd_below(NR_PAGES);

    for (int i = 0; i < NR_PAGES; i++)
        if (is_code(state[(start + i) % NR_PAGES]))
            return (start + i) % NR_PAGES;
    for (int i = 0; i < NR_PAGES; i++)
        if (exec_page((start + i) % NR_PAGES))
            return (start + i) % NR_PAGES;
    return -1;
}

/* A random instruction that reads or writes, at a random place, from this page or another */
static void step_instruction(u64 step)
{
    struct hv_xinsn d;
    struct cpu c;
    u32 insn;
    u64 pc, va;
    u32 len;

    for (;;) {
        insn = rnd_below(40) == 0 ? 0xd50b7420 | rnd_below(31)
                                  : ((u32)rnd() | BIT(27)) & ~(u32)BIT(25);
        if (!hv_xinsn_decode(insn, &d))
            continue;
        /* The base is set below: nothing else of the instruction may be in the same register */
        if (d.mode == XI_REG && d.rm == d.rn)
            continue;
        if ((d.kind == XI_ATOMIC || d.kind == XI_CAS) && (d.rs == d.rn || d.rt == d.rn))
            continue;
        if (d.kind == XI_STORE && !d.simd && d.rn != 31 &&
            (d.rt == d.rn || (d.count == 2 && d.rt2 == d.rn)))
            continue;

        for (int i = 0; i < 31; i++)
            c.x[i] = rnd();
        c.sp = rnd();
        for (int i = 0; i < 32; i++)
            for (int j = 0; j < 16; j++)
                c.v[i][j] = rnd();

        len = d.kind == XI_ZVA ? 64 : d.size * d.count;

        /* Where: anywhere, across the end of a page, at the start of one, or at an access */
        int page = rnd_below(NR_PAGES);
        u32 off = rnd_below(PAGE - len + 1);
        u32 how = rnd_below(8);

        if (how == 0 && len > 1 && page < NR_PAGES - 1)
            off = PAGE - 1 - rnd_below(len - 1);
        else if (how == 1)
            off = 0;
        else if (how <= 4) {
            int start = rnd_below(WORDS);

            for (int i = 0; i < WORDS; i++) {
                int w = (start + i) % WORDS;

                if (hv_pmu_is_access(ref_word(page, w))) {
                    s64 o = 4 * w + (s64)rnd_below(4) - (s64)rnd_below(len);

                    if (o >= 0 && o + len <= PAGE)
                        off = o;
                    break;
                }
            }
        }
        va = ipa_of(page, off);

        /* Stores write accesses now and then, whole and in part */
        if (d.kind != XI_LOAD && rnd_below(3) == 0) {
            u64 two = an_access() | ((u64)an_access() << 32);

            if (d.kind == XI_STORE && d.simd) {
                memcpy(c.v[d.rt], &two, 8);
                memcpy(c.v[d.rt2] + 8, &two, 8);
            } else {
                if (d.rt != 31)
                    c.x[d.rt] = two >> (8 * rnd_below(5));
                if (d.kind == XI_STORE && d.count == 2 && d.rt2 != 31)
                    c.x[d.rt2] = two;
                if (d.kind != XI_STORE && d.rs != 31)
                    c.x[d.rs] = two;
            }
        }

        /* From the page that it accesses half of the time, if code can be there */
        int from = a_code_page();

        if (from < 0)
            return;
        if (rnd_below(2) && (is_code(state[page]) || exec_page(page)))
            from = page;
        pc = ipa_of(from, 4 * rnd_below(WORDS));

        if (d.kind == XI_ZVA) {
            va &= ~63UL;
            c.x[d.rt] = va + rnd_below(64);
            break;
        }
        if (d.mode == XI_LITERAL) {
            /* The instruction has to be where the literal is in reach, and in a page of code */
            pc = va - d.imm;
            if (page_of(pc) < 0 || !is_code(state[page_of(pc)]))
                continue;
            break;
        }

        u64 base = va;

        if (d.mode == XI_BASE || d.mode == XI_PRE)
            base = va - d.imm;
        else if (d.mode == XI_REG)
            base = va - (hv_xinsn_addr(&d, c.x, 0, 0) - (d.rn == 31 ? 0 : c.x[d.rn]));
        if (d.rn == 31)
            c.sp = base;
        else
            c.x[d.rn] = base;
        break;
    }

    u64 at;
    u32 n;

    span(&d, &c, pc, &at, &n);
    if (at != va || n != len) {
        check(false, "the test set 0x%08x up for 0x%lx and it accesses 0x%lx", insn, va, at);
        return;
    }

    snprintf(doing, sizeof(doing), "0x%08x at 0x%lx on 0x%lx, %u bytes, pages in states %d %d",
             insn, pc, va, len, state[page_of(va)], state[page_of(va + len - 1)]);

    /* The condition for being stopped: a store into its own page that needs a record, and no room */
    bool full = xt_nr_recs == XT_MAX_PAGES;
    static u8 before[sizeof(ref)];

    memcpy(before, ref, sizeof(ref));
    if (run(insn, pc, &c, rnd_below(4) == 0, rnd_below(4) != 0) == R_STOPPED) {
        check(full && d.kind != XI_LOAD && page_of(pc) >= page_of(va) &&
                  page_of(pc) <= page_of(va + len - 1),
              "step %lu: 0x%08x at 0x%lx, address 0x%lx, stopped the guest", step, insn, pc, va);
        check(!memcmp(before, ref, sizeof(ref)), "the test changed memory for a stopped guest");
    }
}

/* A device writes into memory: not through stage 2, and nothing that looks like an access */
static void step_dma(void)
{
    int page = rnd_below(NR_PAGES);
    int n = 1 + rnd_below(3);

    snprintf(doing, sizeof(doing), "a device writes to page %d, state %d", page, state[page]);
    for (int k = 0; k < n; k++) {
        int w = rnd_below(WORDS);

        /* Now and then where something was replaced */
        if (rnd_below(2))
            for (int i = 0; i < WORDS; i++)
                if (hv_pmu_is_access(ref_word(page, (w + i) % WORDS))) {
                    w = (w + i) % WORDS;
                    seen.stale += state[page] == HV_XPAGE_XONLY;
                    break;
                }

        u32 word = a_plain_word();

        memcpy(&arena[page * PAGE + 4 * w], &word, 4);
        memcpy(&ref[page * PAGE + 4 * w], &word, 4);
    }
    seen.dma++;
}

/* Things that hv_xtrack.c answers without carrying anything out */
static void step_other(void)
{
    struct cpu c = {0};
    int page = rnd_below(NR_PAGES);
    int other = (page + 1 + rnd_below(NR_PAGES - 1)) % NR_PAGES;
    u64 at = ipa_of(page, 8 * rnd_below(PAGE / 8));
    int st = state[page];
    int ret;

    static u8 arena_before[sizeof(arena)];
    static int state_before[NR_PAGES];

    memcpy(arena_before, arena, sizeof(arena));
    memcpy(state_before, state, sizeof(state));
    c.x[1] = at;
    snprintf(doing, sizeof(doing), "an abort that is answered without carrying anything out, "
                                   "page %d, state %d",
             page, st);

    switch (rnd_below(7)) {
        case 0:
            /* DC IVAC on a page of code: done as DC CIVAC, and the guest goes on behind it */
            if (!is_code(st))
                return;
            test_civac = 0;
            ret = data_abort(&c, I_DC_IVAC | 1, ipa_of(other, 0), at, 0x9200014f, false);
            check(ret == HV_XTRACK_SKIP && test_civac == (u64)&arena[at - ARENA_IPA],
                  "dc ivac on page %d: answer %d, dc civac at 0x%lx", page, ret, test_civac);
            break;
        case 1:
            /* A walk of the guest's stage 1 that stage 2 refuses: no telling which page */
            ret = data_abort(&c, I_LDR, ipa_of(other, 0), at, 0x920000cf, false);
            check(ret == HV_XTRACK_STOP, "a refused walk of stage 1: answer %d", ret);
            break;
        case 2:
            /* An instruction that the decoder does not take reads an execute-only page */
            if (st != HV_XPAGE_XONLY)
                return;
            ret = data_abort(&c, rnd_below(2) ? I_LDXR : I_LD1, ipa_of(other, 0), at, 0x9200000f,
                             false);
            check(ret == HV_XTRACK_RETRY && state[page] == HV_XPAGE_UNSCANNED,
                  "ldxr of page %d from page %d: answer %d, state %d", page, other, ret,
                  state[page]);
            return;
        case 3:
            /* ...from the page itself: there is no way */
            if (st != HV_XPAGE_XONLY)
                return;
            ret = data_abort(&c, I_LDXR, ipa_of(page, 0), at, 0x9200000f, false);
            check(ret == HV_XTRACK_STOP, "ldxr of page %d from within: answer %d", page, ret);
            break;
        case 4:
            /* ...writes a page of code: from elsewhere the guest does it itself, on data */
            if (!is_code(st))
                return;
            ret = data_abort(&c, I_STXR, ipa_of(other, 0), at, 0x9200004f, false);
            check(ret == HV_XTRACK_RETRY && state[page] == HV_XPAGE_UNSCANNED,
                  "stxr into page %d from page %d: answer %d, state %d", page, other, ret,
                  state[page]);
            return;
        case 5:
            if (!is_code(st))
                return;
            ret = data_abort(&c, I_STXR, ipa_of(page, 0), at, 0x9200004f, false);
            check(ret == HV_XTRACK_STOP, "stxr into page %d from within: answer %d", page, ret);
            break;
        default:
            /* Faults that the page tables do not explain: what another core left in a TLB */
            if (st == HV_XPAGE_UNSCANNED)
                ret = data_abort(&c, I_LDR, ipa_of(other, 0), at, 0x9200004f | (rnd_below(2) << 6),
                                 false);
            else if (st == HV_XPAGE_SCANNED)
                ret = data_abort(&c, I_LDR, ipa_of(other, 0), at, 0x9200000f, false);
            else
                return;
            check(ret == HV_XTRACK_NONE, "a fault on page %d that its state %d does not explain: "
                                         "answer %d",
                  page, st, ret);
            if (is_code(st)) {
                struct exc_info ctx = {.cpu_id = 0, .elr = at};

                test_esr = 0x8200000f;
                test_far = at;
                check(hv_xtrack_iabort(&ctx), "an instruction abort on page %d, which runs", page);
            }
            break;
    }

    /* None of these changes anything */
    check(!memcmp(arena_before, arena, sizeof(arena)) && !memcmp(state_before, state, sizeof(state)),
          "something that changes nothing changed memory or the state of a page");
}

static void fill(void)
{
    for (int p = 0; p < NR_PAGES; p++) {
        u32 *words = (u32 *)&ref[p * PAGE];

        for (int i = 0; i < WORDS; i++)
            words[i] = a_plain_word();
        /* Reads of CNTVCT_EL0 and marked hvcs of the guest's own: they are not to be touched */
        for (int i = 0; i < 8; i++) {
            words[rnd_below(WORDS)] = MRS_CNTVCT | rnd_below(32);
            words[rnd_below(WORDS)] = hv_pmu_patch_word(MRS_PMCR | rnd_below(32));
        }
        /* Two pages in three have accesses, some next to each other and at the ends */
        if (p % 3 != 2) {
            for (int i = 0; i < 12; i++)
                words[rnd_below(WORDS)] = an_access();
            words[0] = an_access();
            words[1] = an_access();
            words[WORDS - 1] = an_access();
        }
        state[p] = HV_XPAGE_UNSCANNED;
    }
    memcpy(arena, ref, sizeof(arena));
}

/* The few things that are better said one by one */
static void test_directed(void)
{
    struct cpu c = {0};
    int ret;

    fill();
    hv_pmu_init();
    hv_xtrack_init();

    /* Page 2 has no access, page 1 has */
    check(exec_page(2) && state[2] == HV_XPAGE_SCANNED, "page 2 runs: state %d", state[2]);
    check(exec_page(1) && state[1] == HV_XPAGE_XONLY, "page 1 runs: state %d", state[1]);
    verify("two pages run", 0);

    /* Execution where there is no memory, and an abort that is not about permissions */
    struct exc_info ctx = {.cpu_id = 0, .elr = 0x1000000000};

    test_esr = 0x8200000f;
    test_far = 0x1000000000;
    check(!hv_xtrack_iabort(&ctx), "the guest executes device registers and is not stopped");
    test_esr = 0x82000007; /* translation fault */
    test_far = ipa_of(3, 0);
    check(!hv_xtrack_iabort(&ctx) && state[3] == HV_XPAGE_UNSCANNED,
          "a translation fault on page 3 is taken for a first execution");

    /* The instruction cannot be read */
    c.x[1] = ipa_of(1, 0);
    test_insn_unreadable = true;
    ret = data_abort(&c, I_LDR, ipa_of(2, 0), c.x[1], 0x9200000f, false);
    check(ret == HV_XTRACK_STOP, "an instruction that cannot be read: answer %d", ret);
    test_insn_unreadable = false;

    /* The address that faulted is not one that the instruction accesses */
    c.x[1] = ipa_of(1, 64);
    ret = data_abort(&c, I_LDR, ipa_of(2, 0), ipa_of(1, 128), 0x9200000f, false);
    check(ret == HV_XTRACK_STOP, "an instruction that does not access what faulted: answer %d", ret);

    /* ...but for the top byte of the address, which the guest may use for a tag */
    c.x[1] = ipa_of(1, 64) | 0xab00000000000000UL;
    ret = data_abort(&c, I_LDR, ipa_of(2, 0), ipa_of(1, 64), 0x9200000f, false);
    check(ret == HV_XTRACK_SKIP && c.x[0] == get_le(&ref[PAGE + 64], 8),
          "a load through a tagged address: answer %d", ret);

    /* An access that goes on into a page that stage 1 does not map */
    c.x[1] = ipa_of(1, PAGE - 4);
    test_unmapped = 2;
    ret = data_abort(&c, I_LDR, ipa_of(3, 0), c.x[1], 0x9200000f, false);
    check(ret == HV_XTRACK_STOP, "a load that goes on into an unmapped page: answer %d", ret);
    test_unmapped = -1;

    /* Tracking off: none of this is anybody's business */
    test_xtrack_on = false;
    c.x[1] = ipa_of(1, 64);
    ret = data_abort(&c, I_LDR, ipa_of(2, 0), c.x[1], 0x9200000f, false);
    check(ret == HV_XTRACK_NONE, "a data abort while execution is not tracked: answer %d", ret);
    test_xtrack_on = true;

    verify("the directed checks", 0);
}

int main(int argc, char **argv)
{
    u64 steps = argc > 1 ? strtoul(argv[1], NULL, 0) : 300000;

    test_directed();

    for (int round = 0; round < 4; round++) {
        fill();
        hv_pmu_init();
        hv_xtrack_init();
        for (u64 step = 0; step < steps / 4 && failures < 30; step++) {
            u32 what = rnd_below(100);

            if (what < 12)
                exec_page(rnd_below(NR_PAGES));
            else if (what < 88)
                step_instruction(step);
            else if (what < 92)
                step_dma();
            else
                step_other();
            verify(what < 12 ? "a page run" : what < 88 ? "an instruction" : what < 92 ? "DMA"
                                                                                       : "other",
                   step);
        }
    }

    printf("xtrack-model: %lu instructions by the core, %lu by EL2 (%lu with a syndrome, %lu across "
           "two pages, %lu from the page they access; %lu loads, %lu stores, %lu atomic, %lu cas, "
           "%lu dc zva; %lu SIMD, %lu with writeback, %lu literal), %lu sent back, %lu stops\n",
           seen.native, seen.carried, seen.with_syndrome, seen.two_pages, seen.same_page,
           seen.loads, seen.stores, seen.atomics, seen.cas, seen.zva, seen.simd, seen.wback,
           seen.literal, seen.retries, seen.stops);
    printf("xtrack-model: %lu pages run (%lu found the table full), %lu times a device wrote (%lu "
           "over something replaced)\n",
           seen.execs, seen.exec_full, seen.dma, seen.stale);
    /* The random guest has been everywhere it is meant to go */
    check(seen.native && seen.carried && seen.with_syndrome && seen.two_pages && seen.same_page &&
              seen.loads && seen.stores && seen.atomics && seen.cas && seen.zva && seen.simd &&
              seen.wback && seen.literal && seen.retries && seen.stops && seen.exec_full &&
              seen.stale,
          "the random guest left something out");
    hv_xtrack_report();

    printf("xtrack-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
