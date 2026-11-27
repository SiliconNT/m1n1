// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * xitest: an EL1 guest that executes every form of load and store that src/hv/hv_xinsn.c
 * takes apart, once where the hypervisor has no part in it and again where the hypervisor
 * carries it out (src/hv/hv_xtrack.c), and insists that the two leave the same registers and
 * the same memory behind.
 *
 * There are two stretches of memory of three pages each, A and B, filled alike. A is never
 * executed: it stays data, and what an instruction does to it is the core's own doing. That
 * is what everything else is compared with. The middle page of B and the page behind it are
 * executed before every run, so they are code: with the one filling, in which every third
 * word is an access to a PMUv3 register, they are execute-only under the hypervisor, and with
 * the other they are code that may be read and not written.
 *
 * Each instruction (xitest-insns.S) is run
 *
 *   - from a page of its own on A: the reference;
 *   - from the middle page of B on B: an instruction that reads or writes the page of code
 *     that it is in, which the hypervisor has to carry out;
 *   - from the page of its own on B: reads of an execute-only page are carried out, and a
 *     store makes the page data again.
 *
 * and each of that with the bytes it accesses in the middle of the page, at its start, at its
 * end, and across the boundary to the page before and to the page behind; with a tag in the
 * top byte of the address; and, if it stores, with values that are accesses themselves.
 *
 * Raw image, entry points and image header of sgitest (sgitest-head.S), EL1h. Unlike the other
 * guests it turns its MMU on, with every address mapped to itself: only then may an access be
 * unaligned, and Windows never runs without. It ends with PSCI SYSTEM_OFF, with SCTLR_EL1 and
 * CPACR_EL1 as it found them: the hypervisor hands the next guest what this one leaves.
 */

#define GUEST_NAME "xitest"

#include "sgitest.h"
#include "gic.h" /* for u64 and u32 */
#include "xitest.h"
#include "guest.h"

typedef long s64;

extern u64 hdr_arg;
extern u64 hdr_uart_base;

#define ARG_STOP 0x200 /* end at the first instruction that fails */

void psci_system_off(void);

#define write_sysreg(r, v)                                                                         \
    do {                                                                                           \
        u64 _v = (v);                                                                              \
        asm volatile("msr " #r ", %0" : : "r"(_v));                                                \
    } while (0)

#define PAGE  0x1000UL
#define WORDS (PAGE / 4)

struct xi_entry {
    u32 flags;
    int disp;
    u32 len;
    u32 pad;
    u64 x3;
    u32 test[XI_WORDS];
    u32 ref[XI_WORDS];
    char name[XI_NAME];
};

extern const struct xi_entry xi_entries[], xi_entries_end[];

/* x0 to x5, then q0 and q1 at offset 64: xi_call() in xitest-insns.S */
struct xi_regs {
    u64 x[6];
    u64 pad[2];
    u64 q[2][2];
};

void xi_call(struct xi_regs *regs, void *code);

/*
 * Pages of the arena. A span is five pages: a guard, the three pages of a stretch, a guard.
 * The last guard of A is the first of B.
 */
enum {
    PG_L0,     /* the translation tables of this guest */
    PG_L1_RAM,
    PG_L1_DEV,
    PG_SPAN_A,
    PG_SPAN_B = PG_SPAN_A + 4,
    PG_GUARD = PG_SPAN_B + 4,
    PG_CODE,   /* where an instruction runs that is not in the page it accesses */
    PG_GUARD_END,
    NR_PAGES,
};
#define SPAN_PAGES 5

/* One more page than is used, so that the pages can start on a boundary */
static u8 arena[(NR_PAGES + 1) * PAGE];

/* In the middle page of a stretch and in the page of code: a ret, and the words of an entry */
#define PRIME_OFF 0x400
#define STUB_OFF  0x440

#define GUARD_WORD 0x6a6a6a6a
#define TAG        (0x5aUL << 56)

/* ---- instructions, kept so that no word of this image is one of them ---- */

static volatile u32 key = 0xa5a5a5a5;
#define K(x)      ((x) ^ 0xa5a5a5a5u)
#define U(masked) ((masked) ^ key)

#define I_MRS_PMCCNTR 0xd53b9d00 /* mrs x0, PMCCNTR_EL0 */
#define I_MSR_PMCR    0xd51b9c00 /* msr PMCR_EL0, x0 */
#define I_MRS_PMCR    0xd53b9c00 /* mrs x0, PMCR_EL0 */
#define I_RET         0xd65f03c0

/* ---- output ---- */

static u32 nr_pass, nr_fail, nr_cases, nr_runs, nr_details;
static u64 sctlr_at_start, cpacr_at_start;

static void finish(void)
{
    line("     instructions cases runs = ");
    put_dec(nr_pass + nr_fail);
    put(" ");
    put_dec(nr_cases);
    put(" ");
    put_dec(nr_runs);
    put("\n");
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");

    write_sysreg(sctlr_el1, sctlr_at_start);
    write_sysreg(cpacr_el1, cpacr_at_start);
    asm volatile("isb");

    psci_system_off();
    line("FAIL SYSTEM_OFF returned\n");
    for (;;)
        ;
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

void secondary_main(int slot)
{
    (void)slot;
    for (;;)
        ;
}

/* The compiler asks for this to copy a structure */
void *memcpy(void *dst, const void *src, u64 n)
{
    for (u64 i = 0; i < n; i++)
        ((u8 *)dst)[i] = ((const u8 *)src)[i];
    return dst;
}

/* ---- helpers ---- */

static u8 *page(int n)
{
    u64 base = ((u64)arena + PAGE - 1) & ~(PAGE - 1);

    return (u8 *)(base + n * PAGE);
}

/* After writing code: what real hardware needs before the code may run */
static void sync_code(u32 *at, int words)
{
    for (int i = 0; i < words; i++) {
        asm volatile("dc cvau, %0" : : "r"(at + i));
        asm volatile("ic ivau, %0" : : "r"(at + i));
    }
    asm volatile("dsb ish\nisb");
}

/*
 * Every address mapped to itself with 1 GB blocks: memory everywhere but for the block that
 * the UART is in. 48 bits of address, a top byte that is ignored, caches on.
 */
static void mmu_on(void)
{
    u64 *l0 = (u64 *)page(PG_L0);
    u64 *l1_ram = (u64 *)page(PG_L1_RAM);
    u64 *l1_dev = (u64 *)page(PG_L1_DEV);
    u64 ram = (u64)page(0);
    u64 uart = hdr_uart_base;
    u64 parange = read_sysreg(id_aa64mmfr0_el1) & 0xf;

    if ((ram >> 48) || (uart >> 48) || (ram >> 30) == (uart >> 30)) {
        line("FAIL the UART and the image are too close together, or too far up\n");
        finish();
    }
    if (parange > 5)
        parange = 5;

    u64 *dev = (ram >> 39) == (uart >> 39) ? l1_ram : l1_dev;

    for (u64 i = 0; i < 512; i++) {
        l0[i] = 0;
        l1_dev[i] = 0;
        /* A block: memory of attribute 0, inner shareable, accessed; EL1 may do everything */
        l1_ram[i] = ((ram >> 39) << 39) | (i << 30) | (1UL << 10) | (3UL << 8) | 1;
    }
    /* A block: device of attribute 1, accessed, never executable */
    dev[(uart >> 30) & 511] = (uart & ~((1UL << 30) - 1)) | (3UL << 53) | (1UL << 10) | (1UL << 2) | 1;
    l0[ram >> 39] = (u64)l1_ram | 3;
    l0[uart >> 39] = (u64)dev | 3;

    write_sysreg(mair_el1, 0x00ff);
    /* T0SZ 16, write-back walks, inner shareable, no walks through TTBR1, IPS, TBI0 */
    write_sysreg(tcr_el1,
                 16 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 23) | (parange << 32) | (1UL << 37));
    write_sysreg(ttbr0_el1, (u64)l0);
    asm volatile("isb\ntlbi vmalle1\ndsb nsh\nisb");
    /* M, C and I; neither the stack pointer nor anything else has to be aligned */
    write_sysreg(sctlr_el1, 0x30d00800UL | 0x1005);
    asm volatile("isb");
}

static u64 rng_state;

static u64 rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* An access to a PMUv3 register, to any register there is: three kinds of them */
static u32 access_word(u32 n)
{
    u32 kind = (n / 3) % 3;
    u32 insn = kind == 0 ? U(K(I_MRS_PMCCNTR)) : kind == 1 ? U(K(I_MSR_PMCR)) : U(K(I_MRS_PMCR));

    return insn | (n % 31);
}

/* Word number idx of a stretch. Filling 0 has an access in every third word, filling 1 none */
static u32 fill_word(int filling, u32 idx)
{
    u32 w = (idx + 1) * 0x9e3779b9u;

    w ^= w >> 15;
    w *= 0x85ebca6bu;
    w ^= w >> 13;
    /* Nothing that is an access by chance: they all have 0xd5 on top */
    if ((w >> 24) == 0xd5)
        w ^= 0x01000000;
    if (filling == 0 && idx % 3 == 0)
        w = access_word(idx);
    return w;
}

/* Fill a span: the guards, the three pages, and in the middle page the words of the entry */
static void fill(u8 *span, int filling, const u32 *stub)
{
    u32 *w = (u32 *)span;
    u32 *mid = w + 2 * WORDS;

    for (u32 i = 0; i < WORDS; i++) {
        w[i] = GUARD_WORD;
        w[4 * WORDS + i] = GUARD_WORD;
    }
    for (u32 i = 0; i < 3 * WORDS; i++)
        w[WORDS + i] = fill_word(filling, i);

    for (u32 i = 0; i < XI_WORDS; i++)
        mid[STUB_OFF / 4 + i] = stub[i];
    mid[STUB_OFF / 4 + XI_WORDS] = U(K(I_RET));
    mid[PRIME_OFF / 4] = U(K(I_RET));
    mid[WORDS + PRIME_OFF / 4] = U(K(I_RET));
    sync_code(mid + STUB_OFF / 4, XI_WORDS + 1);
    sync_code(mid + PRIME_OFF / 4, 1);
    sync_code(mid + WORDS + PRIME_OFF / 4, 1);
}

/* Execute the middle page of a span and the page behind it: they are code from here on */
static void prime(u8 *span)
{
    ((void (*)(void))(span + 2 * PAGE + PRIME_OFF))();
    ((void (*)(void))(span + 3 * PAGE + PRIME_OFF))();
}

/* The words of an entry in the page of code */
static void place(const u32 *stub)
{
    u32 *at = (u32 *)(page(PG_CODE) + STUB_OFF);

    for (u32 i = 0; i < XI_WORDS; i++)
        at[i] = stub[i];
    at[XI_WORDS] = U(K(I_RET));
    sync_code(at, XI_WORDS + 1);
}

/* ---- the cases ---- */

enum { POS_MID, POS_TAGGED, POS_START, POS_END, POS_LOW, POS_HIGH, NR_POS };
enum { VAL_RANDOM, VAL_ACCESS, NR_VAL };

/*
 * Where in a stretch the first byte accessed is, from the start of its first page (the middle
 * page is at 0x1000). Returns 0 if the entry has no such case.
 */
static int target(const struct xi_entry *e, int pos, u32 *rel)
{
    u32 f = e->flags;
    int fixed = (f & (F_ALIGNED | F_ZVA)) != 0;
    u32 r;

    switch (pos) {
        case POS_MID:
            r = fixed || (f & F_LIT) ? 0x1a00 : 0x1a03;
            break;
        case POS_TAGGED:
            /* A literal has no address to tag */
            if (f & F_LIT)
                return 0;
            r = fixed ? 0x1a40 : 0x1a43;
            break;
        case POS_START:
            r = 0x1000;
            break;
        case POS_END:
            r = 0x2000 - e->len;
            break;
        case POS_LOW:
            if (fixed || e->len < 2)
                return 0;
            r = 0x1000 - e->len / 2;
            break;
        default:
            if (fixed || e->len < 2)
                return 0;
            r = 0x2000 - e->len / 2;
            break;
    }
    /* A literal is a number of words away from its instruction */
    if (f & F_LIT)
        r &= ~3U;
    *rel = r;
    return 1;
}

static void describe(const struct xi_entry *e, int filling, int pos, int val, const char *run)
{
    line("FAIL ");
    put(e->name);
    put(": ");
    put(run);
    put(", filling ");
    put_dec(filling);
    put(", position ");
    put_dec(pos);
    put(", values ");
    put_dec(val);
    put(": ");
}

/* What a run left behind against what the reference left behind. Returns 1 if they differ */
static int compare(const struct xi_entry *e, int filling, int pos, int val, const char *run,
                   const struct xi_regs *ref, const struct xi_regs *got)
{
    u8 *a = page(PG_SPAN_A), *b = page(PG_SPAN_B);
    int bad = 0;

    nr_runs++;

    for (int i = 0; i < 6 && !bad; i++) {
        u64 want = ref->x[i], have = got->x[i];

        /* The base register: how far it is from its span */
        if (i == 2) {
            want -= (u64)a;
            have -= (u64)b;
        }
        if (want == have)
            continue;
        bad = 1;
        if (nr_details++ < 40) {
            describe(e, filling, pos, val, run);
            put("x");
            put_dec(i);
            put(" is ");
            put_hex(have, 16);
            put(" and should be ");
            put_hex(want, 16);
            put("\n");
        }
    }

    for (int i = 0; i < 4 && !bad; i++) {
        if (ref->q[i / 2][i % 2] == got->q[i / 2][i % 2])
            continue;
        bad = 1;
        if (nr_details++ < 40) {
            describe(e, filling, pos, val, run);
            put("q");
            put_dec(i / 2);
            put(i % 2 ? " upper half is " : " lower half is ");
            put_hex(got->q[i / 2][i % 2], 16);
            put(" and should be ");
            put_hex(ref->q[i / 2][i % 2], 16);
            put("\n");
        }
    }

    for (u64 off = 0; off < SPAN_PAGES * PAGE && !bad; off += 8) {
        u64 want = *(volatile u64 *)(a + off);
        u64 have = *(volatile u64 *)(b + off);

        if (want == have)
            continue;
        bad = 1;
        if (nr_details++ < 40) {
            describe(e, filling, pos, val, run);
            put("memory at 0x");
            put_hex(off, 5);
            put(" of the span is ");
            put_hex(have, 16);
            put(" and should be ");
            put_hex(want, 16);
            put("\n");
        }
    }
    return bad;
}

/* Returns -1 if the entry has no such case, or how many of its runs failed */
static int run_case(const struct xi_entry *e, int filling, int pos, int val)
{
    u8 *a = page(PG_SPAN_A), *b = page(PG_SPAN_B);
    u32 f = e->flags;
    u32 rel;
    u32 stub[XI_WORDS];
    struct xi_regs init, ref, got;
    int failed = 0;

    if (!target(e, pos, &rel))
        return -1;
    if (val == VAL_ACCESS && !(f & F_WRITES))
        return -1;

    for (u32 i = 0; i < XI_WORDS; i++)
        stub[i] = e->test[i];
    if (f & F_LIT) {
        /* The distance in words from the instruction, where it will be in B, to the literal */
        s64 dist = (s64)rel - (s64)(PAGE + STUB_OFF + 4 * XI_INSN);

        stub[XI_INSN] = (stub[XI_INSN] & ~(0x7ffffU << 5)) | ((u32)((dist >> 2) & 0x7ffff) << 5);
    }

    for (int i = 0; i < 6; i++)
        init.x[i] = rng();
    for (int i = 0; i < 4; i++)
        init.q[i / 2][i % 2] = rng();
    init.pad[0] = init.pad[1] = 0;
    if (val == VAL_ACCESS) {
        /* What is stored is accesses: of two kinds, so that both ways of replacing are met */
        u64 two = access_word(rng() % 31) | ((u64)access_word(3 + rng() % 31) << 32);

        init.x[0] = init.x[1] = init.x[3] = init.x[4] = init.x[5] = two;
        init.q[0][0] = init.q[0][1] = init.q[1][0] = init.q[1][1] = two;
    }
    if (f & F_X3)
        init.x[3] = e->x3;

    /* The reference: from the page of code on A, which is data */
    fill(a, filling, stub);
    if (f & F_CASHIT) {
        init.x[3] = 0;
        for (int i = 7; i >= 0; i--)
            init.x[3] = (init.x[3] << 8) | a[PAGE + rel + i];
    }
    place(e->ref);
    ref = init;
    ref.x[2] = ((u64)a + PAGE + rel - e->disp) | (pos == POS_TAGGED ? TAG : 0);
    xi_call(&ref, page(PG_CODE) + STUB_OFF);

    /* From the middle page of B on B */
    if (!(f & F_OTHER)) {
        fill(b, filling, stub);
        prime(b);
        got = init;
        got.x[2] = ((u64)b + PAGE + rel - e->disp) | (pos == POS_TAGGED ? TAG : 0);
        xi_call(&got, b + 2 * PAGE + STUB_OFF);
        failed += compare(e, filling, pos, val, "from the page it accesses", &ref, &got);
    }

    /* From the page of code on B */
    if (!(f & F_LIT)) {
        fill(b, filling, stub);
        prime(b);
        got = init;
        got.x[2] = ((u64)b + PAGE + rel - e->disp) | (pos == POS_TAGGED ? TAG : 0);
        xi_call(&got, page(PG_CODE) + STUB_OFF);
        failed += compare(e, filling, pos, val, "from another page", &ref, &got);
    }

    nr_cases++;
    return failed;
}

void boot_main(void)
{
    line("start, arg=");
    put_hex(hdr_arg, 4);
    put("\n");
    line("CurrentEL = ");
    put_dec((read_sysreg(CurrentEL) >> 2) & 3);
    put("\n");

    sctlr_at_start = read_sysreg(sctlr_el1);
    cpacr_at_start = read_sysreg(cpacr_el1);

    /* SIMD and floating point at EL1 */
    write_sysreg(cpacr_el1, 3UL << 20);
    asm volatile("isb");
    mmu_on();

    line("     A B code = ");
    put_hex((u64)page(PG_SPAN_A) + PAGE, 16);
    put(" ");
    put_hex((u64)page(PG_SPAN_B) + PAGE, 16);
    put(" ");
    put_hex((u64)page(PG_CODE), 16);
    put("\n");

    for (u32 i = 0; i < WORDS; i++) {
        ((u32 *)page(PG_GUARD))[i] = GUARD_WORD;
        ((u32 *)page(PG_GUARD_END))[i] = GUARD_WORD;
    }
    rng_state = 0x243f6a8885a308d3UL;

    for (const struct xi_entry *e = xi_entries; e < xi_entries_end; e++) {
        u32 cases = 0, failed = 0;

        /* DCZID_EL0: DZP is 0 and a block is 2^4 words, or this is not the test for DC ZVA */
        if ((e->flags & F_ZVA) && (read_sysreg(dczid_el0) & 0x1f) != 4) {
            line("SKIP ");
            put(e->name);
            put(": a block is not 64 bytes here\n");
            continue;
        }

        for (int filling = 0; filling < 2; filling++)
            for (int pos = 0; pos < NR_POS; pos++)
                for (int val = 0; val < NR_VAL; val++) {
                    int r = run_case(e, filling, pos, val);

                    if (r < 0)
                        continue;
                    cases++;
                    failed += r;
                }

        line(failed ? "FAIL " : "PASS ");
        put(e->name);
        put(" (");
        put_dec(cases);
        put(" cases)\n");
        if (failed)
            nr_fail++;
        else
            nr_pass++;
        if (failed && (hdr_arg & ARG_STOP))
            break;
    }

    int ok = 1;
    for (u32 i = 0; i < WORDS; i++)
        ok &= ((u32 *)page(PG_GUARD))[i] == GUARD_WORD && ((u32 *)page(PG_GUARD_END))[i] == GUARD_WORD;
    line(ok ? "PASS " : "FAIL ");
    put("the pages around the page of code are as they were\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;

    finish();
}
