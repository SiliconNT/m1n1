// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * xotest: an EL1 guest that finds out what the core does with a page that the second stage of
 * translation lets it execute and neither read nor write, with the execute-only probe of the
 * hypervisor (src/hv/hv_xoprobe.c, which says what the calls are and why anybody asks).
 *
 * The page is in an arena, between two pages of plain data. It holds two functions, a literal
 * that one of them loads, and a pattern. The guest executes it, reads and writes it from
 * elsewhere and from within, by eight bytes and by one, across either end, at EL0 and through
 * a second address, and asks the probe after each access what came to EL2. Then it measures
 * what a trip to EL2 costs, and last it tries the cache maintenance instructions, which have
 * permission checks of their own.
 *
 * It runs with its own stage 1: an access that crosses a page boundary is not aligned, and
 * with the MMU off that is a fault of its own. The map is 1 GB blocks: everything where it is,
 * the memory of the image a second time at KALIAS() for EL1, and a third time at UALIAS() for
 * EL0. The data cache stays off: nothing here is about it, and what was written without it is
 * not read through it.
 *
 * Raw image, MMU off at the start, EL1h, entry points and image header of sgitest
 * (sgitest-head.S). It ends with PSCI SYSTEM_OFF, with SCTLR_EL1 as it found it: the
 * hypervisor leaves the EL1 registers of one guest to the next, and a guest that starts with
 * the MMU on and this one's tables gone does not get to its first instruction (SiliconNT
 * R26). `make qemu-xo` runs it on QEMU under a
 * small EL2 of its own that links the real probe (xotest-qemu.c).
 */

#include "sgitest.h"
#include "gic.h" /* for u64, u32 and u8 */

typedef long s64;

extern char image_start[], image_end[];
extern u64 hdr_uart_base;

void uart_putc(int c);
u64 run_el0(u64 entry, u64 sp, u64 *esr, u64 x0, u64 x1);
void psci_system_off(void);
s64 xo_hvc(u64 fn, u64 a1, u64 a2, u64 *out);
u64 probe_ldr(u64 addr, u64 sentinel);
void probe_str(u64 addr, u64 val);
void probe_strb(u64 addr, u64 val);
void probe_dc_cvau(u64 addr);
void probe_dc_cvac(u64 addr);
void probe_dc_civac(u64 addr);
void probe_dc_ivac(u64 addr);
void probe_ic_ivau(u64 addr);
extern char el0_vectors[], probe_ldr_at[], probe_str_at[], probe_strb_at[];

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })
#define write_sysreg(r, v)                                                                         \
    do {                                                                                           \
        u64 _v = (v);                                                                              \
        asm volatile("msr " #r ", %0" : : "r"(_v));                                                \
    } while (0)

/* The calls of the probe, and what it does with a data abort: src/hv/hv_xoprobe.c, src/hv/hv.h */
#define XO_FN_RESET  0xc6005800UL
#define XO_FN_ARM    0xc6005801UL
#define XO_FN_DISARM 0xc6005802UL
#define XO_FN_QUERY  0xc6005803UL

#define XO_DEMOTE  0
#define XO_SKIP    1
#define XO_EMULATE 2

#define XO_FLAG_STRAY 1
#define XO_FLAG_WRITE 2

struct fault {
    u64 esr, far, ipa, elr, hpfar, flags;
};

#define PAGE  0x1000UL
#define WORDS (PAGE / 4)
#define GB    (1UL << 30)

/* The same memory at other addresses: for EL1, and for EL0 */
#define SLOT(va)   ((va) & ((1UL << 39) - 1))
#define KALIAS(va) (SLOT((u64)(va)) | (5UL << 39))
#define UALIAS(va) (SLOT((u64)(va)) | (6UL << 39))

/* Pages of the arena */
enum {
    PG_BEFORE, /* data */
    PG_XO,     /* the page under test */
    PG_AFTER,  /* data */
    PG_EL0,    /* code for EL0 */
    NR_PAGES,
};

/* One more page than is used, so that the pages can start on a boundary */
static u8 arena[(NR_PAGES + 1) * PAGE];
static u8 el0_stack[4096] __attribute__((aligned(16)));

/* Translation tables: one of level 0 and up to six of level 1 */
#define NR_TABLES 7
static u8 table_pool[(NR_TABLES + 1) * PAGE];
static u8 l1_of[8];
static u32 nr_tables = 1;

/* What is in the page under test, by word */
#define I_LDR_X0_LIT 0x58000080 /* ldr x0, <16 bytes on> */
#define I_RET        0xd65f03c0
#define I_MOV_X0     0xd280bc00 /* mov x0, #0x5e0 */
#define MARK         0x5e0UL
#define W_LOAD       0 /* u64 f(u64): returns the literal */
#define W_MARK       2 /* u64 f(void): returns MARK */
#define W_LIT        4
#define W_DATA       64 /* eight bytes that are read and written from elsewhere */
#define LIT          0x1122334455667788UL
#define SENTINEL     0x5e5e5e5e00c0ffeeUL
#define NEW          0x0123456789abcdefUL

/* For EL0: ldr x0, [x1]; svc #0 */
#define I_LDR_X0_X1 0xf9400020
#define I_SVC       0xd4000001

#define NR_TIMED 1000

/* ---- output ---- */

static u32 nr_pass, nr_fail;

static void put(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_hex(u64 v, int digits)
{
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        uart_putc("0123456789abcdef"[(v >> shift) & 0xf]);
}

static void put_dec(u64 v)
{
    char buf[20];
    int n = 0;

    do {
        buf[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}

static void line(const char *s)
{
    put("HVLOG: xotest: ");
    put(s);
}

static void result(const char *name, int ok)
{
    line(ok ? "PASS " : "FAIL ");
    put(name);
    put("\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;
}

static void value(const char *name, u64 v)
{
    line("     ");
    put(name);
    put(" = 0x");
    put_hex(v, 16);
    put("\n");
}

/* ---- exception handlers, called from sgitest-head.S and from the vectors below ---- */

void irq_handler(void)
{
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

void bad_exception(u64 vector)
{
    line("EXCEPTION cpu vector ESR ELR FAR SPSR = ");
    put_hex(read_sysreg(tpidr_el1), 2);
    put(" ");
    put_hex(vector, 2);
    put(" ");
    put_hex(read_sysreg(esr_el1), 8);
    put(" ");
    put_hex(read_sysreg(elr_el1), 16);
    put(" ");
    put_hex(read_sysreg(far_el1), 16);
    put(" ");
    put_hex(read_sysreg(spsr_el1), 8);
    put("\n");
    for (;;)
        ;
}

void secondary_main(int slot)
{
    (void)slot;
    for (;;)
        ;
}

/*
 * A second vector table, in use only while code runs at EL0: a synchronous exception from
 * there ends run_el0(), which returns what EL0 left in x0 and stores ESR_EL1 through esr.
 * Then the call of the probe, and the instructions under test: each is the one instruction at
 * its _at label, so that the probe can be asked whether that is where the fault was. The
 * loads and stores are forms that the hypervisor's emulation knows.
 */
asm("    .text\n"
    "    .macro  el0_bad num\n"
    "    .balign 0x80\n"
    "    mov     x0, #\\num\n"
    "    bl      bad_exception\n"
    "    .endm\n"
    "    .balign 0x800\n"
    "    .globl  el0_vectors\n"
    "el0_vectors:\n"
    "    el0_bad 32\n"
    "    el0_bad 33\n"
    "    el0_bad 34\n"
    "    el0_bad 35\n"
    "    el0_bad 36\n"
    "    el0_bad 37\n"
    "    el0_bad 38\n"
    "    el0_bad 39\n"
    "    .balign 0x80\n"
    "    b       el0_return\n" /* synchronous, from EL0 */
    "    el0_bad 41\n"
    "    el0_bad 42\n"
    "    el0_bad 43\n"
    "    el0_bad 44\n"
    "    el0_bad 45\n"
    "    el0_bad 46\n"
    "    el0_bad 47\n"
    "    .balign 0x80\n"
    "    .globl  run_el0\n"
    "run_el0:\n"
    "    stp     x29, x30, [sp, #-112]!\n"
    "    stp     x19, x20, [sp, #16]\n"
    "    stp     x21, x22, [sp, #32]\n"
    "    stp     x23, x24, [sp, #48]\n"
    "    stp     x25, x26, [sp, #64]\n"
    "    stp     x27, x28, [sp, #80]\n"
    "    str     x2, [sp, #96]\n"
    "    msr     sp_el0, x1\n"
    "    msr     elr_el1, x0\n"
    "    mov     x9, #0x3c0\n" /* EL0t, interrupts masked */
    "    msr     spsr_el1, x9\n"
    "    mov     x0, x3\n"
    "    mov     x1, x4\n"
    "    isb\n"
    "    eret\n"
    "el0_return:\n"
    "    mrs     x1, esr_el1\n"
    "    ldr     x9, [sp, #96]\n"
    "    str     x1, [x9]\n"
    "    ldp     x19, x20, [sp, #16]\n"
    "    ldp     x21, x22, [sp, #32]\n"
    "    ldp     x23, x24, [sp, #48]\n"
    "    ldp     x25, x26, [sp, #64]\n"
    "    ldp     x27, x28, [sp, #80]\n"
    "    ldp     x29, x30, [sp], #112\n"
    "    ret\n"
    "    .globl  psci_system_off\n"
    "psci_system_off:\n"
    "    mov     x0, #0x84000000\n"
    "    orr     x0, x0, #8\n"
    "    hvc     #0\n"
    "    ret\n"
    "    .globl  xo_hvc\n"
    "xo_hvc:\n"
    "    mov     x9, x3\n"
    "    mov     x3, #0\n"
    "    mov     x4, #0\n"
    "    mov     x5, #0\n"
    "    mov     x6, #0\n"
    "    hvc     #0\n"
    "    cbz     x9, 1f\n"
    "    stp     x1, x2, [x9]\n"
    "    stp     x3, x4, [x9, #16]\n"
    "    stp     x5, x6, [x9, #32]\n"
    "1:  ret\n"
    "    .globl  probe_ldr, probe_ldr_at\n"
    "probe_ldr:\n"
    "    mov     x9, x0\n"
    "    mov     x0, x1\n"
    "probe_ldr_at:\n"
    "    ldr     x0, [x9]\n"
    "    ret\n"
    "    .globl  probe_str, probe_str_at\n"
    "probe_str:\n"
    "probe_str_at:\n"
    "    str     x1, [x0]\n"
    "    ret\n"
    "    .globl  probe_strb, probe_strb_at\n"
    "probe_strb:\n"
    "probe_strb_at:\n"
    "    strb    w1, [x0]\n"
    "    ret\n"
    "    .globl  probe_dc_cvau, probe_dc_cvac, probe_dc_civac, probe_dc_ivac, probe_ic_ivau\n"
    "probe_dc_cvau:\n"
    "    dc      cvau, x0\n"
    "    ret\n"
    "probe_dc_cvac:\n"
    "    dc      cvac, x0\n"
    "    ret\n"
    "probe_dc_civac:\n"
    "    dc      civac, x0\n"
    "    ret\n"
    "probe_dc_ivac:\n"
    "    dc      ivac, x0\n"
    "    ret\n"
    "probe_ic_ivau:\n"
    "    ic      ivau, x0\n"
    "    ret\n");

/* ---- helpers ---- */

static u32 *page(int n)
{
    u64 base = ((u64)arena + PAGE - 1) & ~(PAGE - 1);

    return (u32 *)(base + n * PAGE);
}

#define XO ((u64)page(PG_XO))

/* After writing code: what real hardware needs before the code may run, at each of its addresses */
static void sync_code(u32 *at, int words)
{
    for (int i = 0; i < words; i++) {
        asm volatile("dc cvau, %0" : : "r"(at + i));
        asm volatile("ic ivau, %0" : : "r"(at + i));
        asm volatile("ic ivau, %0" : : "r"(KALIAS(at + i)));
        asm volatile("ic ivau, %0" : : "r"(UALIAS(at + i)));
    }
    asm volatile("dsb ish\nisb");
}

typedef u64 (*fn0)(void);
typedef u64 (*fn1)(u64);

static u64 now(void)
{
    asm volatile("isb");
    return read_sysreg(cntvct_el0);
}

static u32 before_word(int i)
{
    return 0xa0000000 | i;
}

static u32 xo_word(int i)
{
    return 0xc0000000 | i;
}

static u32 after_word(int i)
{
    return 0xb0000000 | i;
}

/* The eight bytes at this word of the page under test, as they were written */
static u64 xo_quad(int i)
{
    return ((u64)xo_word(i + 1) << 32) | xo_word(i);
}

static void fill(void)
{
    u32 *xo = page(PG_XO);

    for (u64 i = 0; i < WORDS; i++) {
        page(PG_BEFORE)[i] = before_word(i);
        xo[i] = xo_word(i);
        page(PG_AFTER)[i] = after_word(i);
    }
    xo[W_LOAD] = I_LDR_X0_LIT;
    xo[W_LOAD + 1] = I_RET;
    xo[W_MARK] = I_MOV_X0;
    xo[W_MARK + 1] = I_RET;
    xo[W_LIT] = (u32)LIT;
    xo[W_LIT + 1] = LIT >> 32;
    sync_code(xo, 6);

    page(PG_EL0)[0] = I_LDR_X0_X1;
    page(PG_EL0)[1] = I_SVC;
    sync_code(page(PG_EL0), 2);
}

/* ---- stage 1 ---- */

static u64 *table(int n)
{
    u64 base = ((u64)table_pool + PAGE - 1) & ~(PAGE - 1);

    return (u64 *)(base + n * PAGE);
}

/* Block descriptors of level 1: AF, inner shareable; index 0 of MAIR_EL1 is memory, 1 a device */
#define S1_BLOCK 0x701UL
#define S1_UXN   (1UL << 54)
#define S1_PXN   (1UL << 53)
#define S1_KERN  (S1_BLOCK | S1_UXN)                     /* EL1 reads, writes and executes */
#define S1_USER  (S1_BLOCK | S1_PXN | (1UL << 6))        /* EL0 does, and EL1 does not execute */
#define S1_DEV   (S1_BLOCK | S1_UXN | S1_PXN | (1UL << 2))

static int map_gb(u64 va, u64 pa, u64 attr)
{
    u64 i0 = (va >> 39) & 7;

    if (!l1_of[i0]) {
        if (nr_tables == NR_TABLES)
            return 0;
        l1_of[i0] = nr_tables++;
        table(0)[i0] = (u64)table(l1_of[i0]) | 3;
    }
    table(l1_of[i0])[(va >> 30) & 511] = (pa & ~(GB - 1)) | attr;
    return 1;
}

static int mmu_on(void)
{
    u64 first = (u64)image_start & ~(GB - 1);
    u64 last = ((u64)image_end - 1) & ~(GB - 1);
    u64 uart = hdr_uart_base & ~(GB - 1);
    int ok = 1;

    /* The UART is not in the memory of the image, and both are below the aliases */
    if (uart >= first && uart <= last)
        return 0;
    if ((u64)image_end >> 39 >= 5 || hdr_uart_base >> 39 >= 5)
        return 0;

    for (u64 gb = first; gb <= last; gb += GB) {
        ok &= map_gb(gb, gb, S1_KERN);
        ok &= map_gb(KALIAS(gb), gb, S1_KERN);
        ok &= map_gb(UALIAS(gb), gb, S1_USER);
    }
    ok &= map_gb(uart, uart, S1_DEV);
    if (!ok)
        return 0;

    u64 parange = read_sysreg(id_aa64mmfr0_el1) & 0xf;
    if (parange > 5)
        parange = 5;
    write_sysreg(mair_el1, 0x00ff);
    /* T0SZ 22 (42 bits of address), 4 KB granule, write-back, inner shareable; no TTBR1 */
    write_sysreg(tcr_el1, 22 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 23) |
                              (parange << 32));
    write_sysreg(ttbr0_el1, (u64)table(0));
    asm volatile("isb\ntlbi vmalle1\ndsb nsh\nisb");

    /*
     * M on. No alignment checks (A), no data cache (C), and what is writable stays
     * executable (WXN)
     */
    u64 sctlr = read_sysreg(sctlr_el1);
    sctlr |= 1UL;
    sctlr &= ~((1UL << 1) | (1UL << 2) | (1UL << 19));
    write_sysreg(sctlr_el1, sctlr);
    asm volatile("isb");
    return 1;
}

/* ---- the probe ---- */

static u64 faults(struct fault *f, u64 n)
{
    return xo_hvc(XO_FN_QUERY, n, 0, (u64 *)f);
}

static u64 nr_faults(void)
{
    return xo_hvc(XO_FN_QUERY, 0, 0, 0);
}

static void show(const char *what, const struct fault *f)
{
    line("     ");
    put(what);
    put(": ESR ");
    put_hex(f->esr, 8);
    put(" FAR ");
    put_hex(f->far, 16);
    put(" IPA ");
    put_hex(f->ipa, 16);
    put(" PC ");
    put_hex(f->elr, 16);
    put(" HPFAR ");
    put_hex(f->hpfar, 16);
    put(f->flags & XO_FLAG_STRAY ? " (not on the page)\n" : "\n");
}

/*
 * A Data Abort from a lower Exception level (EC 0x24), a stage 2 permission fault at level 3
 * (DFSC 0b001111), not on a stage 1 table walk (S1PTW 0), for a read or a write (WnR)
 */
static int s2_perm(const struct fault *f, int write)
{
    return (f->esr >> 26) == 0x24 && (f->esr & 0x3f) == 0x0f && !(f->esr & 0x80) &&
           ((f->esr >> 6) & 1) == (u64)write && !!(f->flags & XO_FLAG_WRITE) == write;
}

/* ...on the page under test, at this address, which is this address of the image, from this instruction */
static int fault_at(const struct fault *f, int write, u64 far, u64 ipa, u64 pc)
{
    return s2_perm(f, write) && !(f->flags & XO_FLAG_STRAY) && f->far == far && f->ipa == ipa &&
           f->elr == pc;
}

/* The page under test becomes execute-only. Running it first is what gets it scanned */
static int arm(u64 mode)
{
    if (((fn0)(XO + W_MARK * 4))() != MARK)
        return 0;
    if (xo_hvc(XO_FN_RESET, 0, 0, 0) != 0)
        return 0;
    return xo_hvc(XO_FN_ARM, XO, mode, 0) == 0;
}

static void disarm(void)
{
    xo_hvc(XO_FN_DISARM, XO, 0, 0);
}

#define ARM_OR_FAIL(mode, name)                                                                    \
    if (!arm(mode)) {                                                                              \
        result(name ": the page could not be made execute-only", 0);                              \
        return;                                                                                    \
    }

/* ---- the tests ---- */

static void test_execute(void)
{
    ARM_OR_FAIL(XO_SKIP, "execute");
    u64 a = ((fn0)(XO + W_MARK * 4))();
    u64 b = ((fn0)KALIAS(XO + W_MARK * 4))();
    result("code in an execute-only page runs, at either of its addresses",
           a == MARK && b == MARK);
    result("...and nothing of it comes to EL2 as a data abort", nr_faults() == 0);
}

static void test_read(void)
{
    struct fault f;
    u64 at = XO + W_DATA * 4;

    ARM_OR_FAIL(XO_SKIP, "read");
    u64 v = probe_ldr(at, SENTINEL);
    u64 n = faults(&f, 0);
    show("read from elsewhere", &f);
    result("a read of the page from elsewhere does not happen", v == SENTINEL);
    result("...and is one stage 2 permission fault on a read, at the address read",
           n == 1 && fault_at(&f, 0, at, at, (u64)probe_ldr_at));
    v = probe_ldr(at, SENTINEL);
    result("a second read is a second fault", v == SENTINEL && nr_faults() == 2);

    ARM_OR_FAIL(XO_DEMOTE, "read");
    v = probe_ldr(at, SENTINEL);
    result("once the page is readable again the read returns what is there, after one fault",
           v == xo_quad(W_DATA) && nr_faults() == 1);
    v = probe_ldr(at, SENTINEL);
    result("...and reads from then on are no faults", v == xo_quad(W_DATA) && nr_faults() == 1);
}

static void test_literal(void)
{
    struct fault f;

    ARM_OR_FAIL(XO_SKIP, "literal");
    u64 v = ((fn1)(XO + W_LOAD * 4))(SENTINEL);
    u64 n = faults(&f, 0);
    show("literal in the page", &f);
    result("an instruction in the page that loads a literal from the page runs and does not "
           "read",
           v == SENTINEL);
    result("...one fault, the instruction and the literal both in the page",
           n == 1 && fault_at(&f, 0, XO + W_LIT * 4, XO + W_LIT * 4, XO + W_LOAD * 4));

    ARM_OR_FAIL(XO_DEMOTE, "literal");
    v = ((fn1)(XO + W_LOAD * 4))(SENTINEL);
    result("...and loads the literal once the page is readable, after one fault",
           v == LIT && nr_faults() == 1);
}

static void test_write(void)
{
    struct fault f, g;
    u64 at = XO + W_DATA * 4;
    u32 *xo = page(PG_XO);

    ARM_OR_FAIL(XO_SKIP, "write");
    probe_str(at, NEW);
    u64 n = faults(&f, 0);
    show("store of eight bytes", &f);
    result("a store into the page is one stage 2 permission fault on a write",
           n == 1 && fault_at(&f, 1, at, at, (u64)probe_str_at));
    probe_strb(at + 9, 0xee);
    n = faults(&g, 1);
    show("store of one byte", &g);
    result("...and so is a store of one byte",
           n == 2 && fault_at(&g, 1, at + 9, at + 9, (u64)probe_strb_at));
    disarm();
    result("...and neither has changed the page",
           xo[W_DATA] == xo_word(W_DATA) && xo[W_DATA + 1] == xo_word(W_DATA + 1) &&
               xo[W_DATA + 2] == xo_word(W_DATA + 2));

    ARM_OR_FAIL(XO_DEMOTE, "write");
    probe_strb(at + 9, 0xee);
    n = nr_faults();
    result("once the page takes stores a store of one byte arrives, after one fault",
           n == 1 && xo[W_DATA + 2] == ((xo_word(W_DATA + 2) & ~0xff00u) | 0xee00));
    xo[W_DATA + 2] = xo_word(W_DATA + 2);
}

/*
 * Eight bytes, four in the page under test and four in the page before or after it, which
 * anybody may read and write. Where the fault is reported is the processor's choice: "an
 * unaligned access that occurs across a page boundary can generate an abort on either side
 * of the page boundary" (Arm DDI 0487 M.d, B2.8). Returns whether it was reported on the
 * page.
 */
static int cross_read(const char *name, const char *name2, u64 at)
{
    struct fault f;

    if (!arm(XO_SKIP)) {
        result(name, 0);
        return 0;
    }
    u64 v = probe_ldr(at, SENTINEL);
    u64 n = faults(&f, 0);
    show("read across the boundary", &f);
    result(name, v == SENTINEL && n == 1);
    result(name2, s2_perm(&f, 0) && f.far >= at && f.far < at + 8 && f.elr == (u64)probe_ldr_at);
    return n == 1 && !(f.flags & XO_FLAG_STRAY);
}

static void test_cross(void)
{
    struct fault f;
    u32 *xo = page(PG_XO);
    u32 *before = page(PG_BEFORE);
    u64 head = XO - 4, tail = XO + PAGE - 4;

    int on_page = cross_read("a read that begins in the page before does not happen, and is "
                             "one fault",
                             "...a stage 2 permission fault on a read, at an address within "
                             "the access",
                             head);
    on_page &= cross_read("a read that ends in the page after does not happen, and is one "
                          "fault",
                          "...a stage 2 permission fault on a read, at an address within the "
                          "access",
                          tail);

    ARM_OR_FAIL(XO_SKIP, "store across the boundary");
    probe_str(head, NEW);
    u64 n = faults(&f, 0);
    show("store across the boundary", &f);
    result("a store that begins in the page before is one fault on a write",
           n == 1 && s2_perm(&f, 1) && f.far >= head && f.far < head + 8);
    line("     the four bytes of it in the page before: ");
    put(before[WORDS - 1] == before_word(WORDS - 1) ? "not written\n" : "written\n");
    before[WORDS - 1] = before_word(WORDS - 1);
    disarm();
    result("...and has not changed the page", xo[0] == I_LDR_X0_LIT);

    /* With the fault somewhere else, EL2 would not know which page to make readable */
    if (!on_page) {
        line("     not run: a read across the boundary once the page is readable\n");
        return;
    }
    ARM_OR_FAIL(XO_DEMOTE, "read across the boundary");
    u64 v = probe_ldr(head, SENTINEL);
    result("once the page is readable the read across the boundary returns both halves",
           v == (((u64)I_LDR_X0_LIT << 32) | before_word(WORDS - 1)) && nr_faults() == 1);
}

static void test_el0(void)
{
    struct fault f;
    u64 at = XO + W_DATA * 4;
    u64 vbar = read_sysreg(vbar_el1);
    u64 esr = 0;

    ARM_OR_FAIL(XO_SKIP, "EL0");
    write_sysreg(vbar_el1, (u64)el0_vectors);
    asm volatile("isb");
    u64 x0 = run_el0(UALIAS(page(PG_EL0)), UALIAS(el0_stack + sizeof(el0_stack)), &esr, SENTINEL,
                     UALIAS(at));
    write_sysreg(vbar_el1, vbar);
    asm volatile("isb");

    u64 n = faults(&f, 0);
    show("read at EL0", &f);
    value("ESR_EL1 of the way back from EL0", esr);
    /* EC 0x15 is the svc behind the read; 0x24 would be the read, as a fault for EL1 */
    result("a read of the page at EL0 does not happen, and EL0 goes on",
           x0 == SENTINEL && (esr >> 26) == 0x15);
    result("...and is one stage 2 permission fault that came to EL2, not to EL1",
           n == 1 && fault_at(&f, 0, UALIAS(at), at, UALIAS(page(PG_EL0))));
}

static void test_alias(void)
{
    struct fault f;
    u64 at = XO + W_DATA * 4;

    ARM_OR_FAIL(XO_SKIP, "second address");
    u64 v = probe_ldr(KALIAS(at), SENTINEL);
    u64 n = faults(&f, 0);
    show("read through the second address", &f);
    result("a read of the page through its second address does not happen",
           v == SENTINEL && n == 1);
    result("...and the fault has the address that was used, and the page that it is",
           fault_at(&f, 0, KALIAS(at), at, (u64)probe_ldr_at));
}

static void timing(const char *what, u64 ticks, u64 count)
{
    line("     ");
    put(what);
    put(": ");
    put_dec(ticks * 1000000000 / read_sysreg(cntfrq_el0) / count);
    put(count == 1 ? " ns\n" : " ns each\n");
}

static void test_timing(void)
{
    u64 at = XO + W_DATA * 4;
    u64 plain = (u64)page(PG_BEFORE) + W_DATA * 4;
    int ok = 1;

    u64 t0 = now();
    for (int i = 0; i < NR_TIMED; i++)
        ok &= probe_ldr(plain, SENTINEL) == (((u64)before_word(W_DATA + 1) << 32) |
                                             before_word(W_DATA));
    u64 t1 = now();
    result("1000 reads of a page that anybody may read", ok);
    timing("reads that are not faults", t1 - t0, NR_TIMED);

    ARM_OR_FAIL(XO_SKIP, "timing");
    ok = 1;
    t0 = now();
    for (int i = 0; i < NR_TIMED; i++)
        ok &= probe_ldr(at, SENTINEL) == SENTINEL;
    t1 = now();
    result("1000 reads of the page, each a fault that EL2 does nothing about",
           ok && nr_faults() == NR_TIMED);
    timing("reads that EL2 skips", t1 - t0, NR_TIMED);

    ARM_OR_FAIL(XO_EMULATE, "timing");
    ok = 1;
    t0 = now();
    for (int i = 0; i < NR_TIMED; i++)
        ok &= probe_ldr(at, SENTINEL) == xo_quad(W_DATA);
    t1 = now();
    result("1000 reads of the page that EL2 carries out return what is there",
           ok && nr_faults() == NR_TIMED);
    timing("reads that EL2 carries out", t1 - t0, NR_TIMED);

    ARM_OR_FAIL(XO_DEMOTE, "timing");
    t0 = now();
    u64 v = probe_ldr(at, SENTINEL);
    t1 = now();
    result("one read that makes the page readable again",
           v == xo_quad(W_DATA) && nr_faults() == 1);
    timing("that one read", t1 - t0, 1);
}

static void cache_op(const char *name, void (*op)(u64))
{
    struct fault f;

    xo_hvc(XO_FN_RESET, 0, 0, 0);
    op(XO + W_DATA * 4);
    u64 n = faults(&f, 0);
    line("     ");
    put(name);
    if (!n) {
        put(" on the page: no fault\n");
        return;
    }
    put(" on the page: ");
    put_dec(n);
    put(" fault(s)\n");
    show(name, &f);
}

/* Nothing is expected here but that the guest gets through; what happens is what is asked */
static void test_cache(void)
{
    u32 *xo = page(PG_XO);

    ARM_OR_FAIL(XO_SKIP, "cache maintenance");
    cache_op("dc cvau", probe_dc_cvau);
    cache_op("dc cvac", probe_dc_cvac);
    cache_op("dc civac", probe_dc_civac);
    cache_op("dc ivac", probe_dc_ivac);
    cache_op("ic ivau", probe_ic_ivau);
    result("cache maintenance by address on the page: the guest goes on", 1);

    disarm();
    xo_hvc(XO_FN_RESET, 0, 0, 0);
    result("after DISARM the page is read and run as before, and as it was written",
           probe_ldr(XO + W_DATA * 4, SENTINEL) == xo_quad(W_DATA) &&
               ((fn0)(XO + W_MARK * 4))() == MARK && xo[W_LIT] == (u32)LIT &&
               xo[WORDS - 1] == xo_word(WORDS - 1) && nr_faults() == 0);
}

void boot_main(void)
{
    u64 sctlr = read_sysreg(sctlr_el1);

    line("start\n");
    line("CurrentEL = ");
    put_dec((read_sysreg(CurrentEL) >> 2) & 3);
    put("\n");
    value("the page under test", XO);
    value("SCTLR_EL1 as the guest found it", sctlr);

    if (!mmu_on()) {
        result("stage 1: no room for the map of this image and this UART", 0);
    } else if (xo_hvc(XO_FN_RESET, 0, 0, 0) != 0) {
        /* -1 is what PSCI says to a function it does not have */
        value("RESET", xo_hvc(XO_FN_RESET, 0, 0, 0));
        result("the hypervisor has the execute-only probe, and it is on", 0);
    } else {
        value("SCTLR_EL1 with the map on", read_sysreg(sctlr_el1));
        result("the hypervisor has the execute-only probe, and it is on", 1);
        /* With the map there: the code is made known at each of its addresses */
        fill();
        test_execute();
        test_read();
        test_literal();
        test_write();
        test_cross();
        test_el0();
        test_alias();
        test_timing();
        test_cache();
    }

    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");

    /* Everything is mapped where it is, so the map can go while this runs */
    write_sysreg(sctlr_el1, sctlr);
    asm volatile("isb");
    line("SCTLR_EL1 is as it was\n");

    psci_system_off();
    line("FAIL SYSTEM_OFF returned\n");
    for (;;)
        ;
}
