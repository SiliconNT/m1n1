// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * pmutest: an EL1 guest that uses the PMUv3 cycle counter the way Windows does and that
 * brings code into being the ways an operating system does, for src/hv/hv_pmu.c and
 * src/hv/hv_xtrack.c. Under the hypervisor with execution tracking on, on a core without
 * PMUv3 registers, every one of its accesses has been replaced before it runs, at EL1 and at
 * EL0: a read of the cycle counter by a read of CNTVCT_EL0, anything else by an hvc. On a
 * core that has the registers nothing is replaced. The guest passes on both; with ARG_STRICT
 * it insists that the cycle counter is the count of the generic timer, which it is only if
 * the read of it was replaced. Either way it reads its own code back as data, from the page
 * the code is in and from others, and insists that it is what it wrote: what was replaced is
 * the hypervisor's to keep out of sight.
 *
 * The instructions under test must not sit in this image as they are: the image's own code
 * and constants share pages that execute, and those pages are scanned. So the test code is
 * kept as words XORed with a key that the compiler cannot see through, and written out at
 * run time into pages of an arena that nothing has executed.
 *
 * Raw image, MMU off, EL1h, entry points and image header of sgitest (sgitest-head.S). It
 * ends with PSCI SYSTEM_OFF, which stops the guest under the hypervisor.
 */

#define GUEST_NAME "pmutest"

#include "sgitest.h"
#include "gic.h" /* for u64 and u32 */
#include "guest.h"

extern u64 hdr_arg;

#define ARG_STRICT 0x100 /* the code must have been replaced */

u64 run_el0(u64 entry, u64 sp, u64 *esr);
void psci_system_off(void);
extern char el0_vectors[];

#define write_sysreg(r, v)                                                                         \
    do {                                                                                           \
        u64 _v = (v);                                                                              \
        asm volatile("msr " #r ", %0" : : "r"(_v));                                                \
    } while (0)

#define PAGE  0x1000UL /* a stage 2 page of the hypervisor, the size of a page of Windows */
#define WORDS (PAGE / 4)

/* Pages of the arena, each for one purpose */
enum {
    PG_FRESH,    /* code written into a page that never ran, then rewritten again and again */
    PG_STORE,    /* code that stores into its own page */
    PG_PATCH,    /* code that writes an instruction into its own page and runs it */
    PG_EL0,      /* code that runs at EL0 first and at EL1 afterwards */
    PG_EL0_READ, /* code that reads the cycle counter at EL0 */
    PG_READ,     /* code with an access, and constants that it and code elsewhere read */
    PG_OPEN,    /* code that stores into its own page with a pair store that has writeback */
    /*
     * Looks like accesses all over and is never executed. It has to stay between two pages
     * that are: a hypervisor that scans more than the 4 KB that run replaces what is in here.
     */
    PG_UNTOUCHED,
    PG_TIMING,   /* the first of NR_TIMING pages that are each run once */
};
/* The image is built for the tiny code model: all of it within 1 MB, the arena included */
#define NR_TIMING 32
#define NR_PAGES    (PG_TIMING + NR_TIMING)
#define NR_REWRITES 16
#define NR_STORES   1000

/* One more page than is used, so that the pages can start on a boundary */
static u8 arena[(NR_PAGES + 1) * PAGE];
static u8 el0_stack[4096] __attribute__((aligned(16)));

/* ---- instructions, kept so that no word of this image is one of them ---- */

static volatile u32 key = 0xa5a5a5a5;
#define K(x) ((x) ^ 0xa5a5a5a5u)
/* U(K(x)) is x, and the compiler does not know */
#define U(masked) ((masked) ^ key)

#define I_MRS_X0_PMCCNTR 0xd53b9d00
#define I_MRS_X0_PMCR    0xd53b9c00
#define I_RET            0xd65f03c0
#define I_NOP            0xd503201f

/* mrs x0, PMCCNTR_EL0; ret; then a word of data that looks like the first instruction */
static const u32 tpl_ccntr[] = {K(I_MRS_X0_PMCCNTR), K(I_RET), K(I_MRS_X0_PMCCNTR)};
/* mrs x0, PMCR_EL0; ret; data */
static const u32 tpl_pmcr[] = {K(I_MRS_X0_PMCR), K(I_RET), K(I_MRS_X0_PMCCNTR)};
/* adr x9, data; str x0, [x9]; mrs x0, PMCCNTR_EL0; ret; data: .quad 0 */
static const u32 tpl_store[] = {K(0x10000089), K(0xf9000120), K(I_MRS_X0_PMCCNTR), K(I_RET), K(0),
                                K(0)};
#define STORE_DATA 4
/*
 * adr x9, slot; str w0, [x9]; dc cvau, x9; dsb ish; ic ivau, x9; dsb ish; isb; slot: nop; ret
 * Called with an instruction in w0, it writes it into the slot and runs it.
 */
static const u32 tpl_patch[] = {K(0x100000e9), K(0xb9000120), K(0xd50b7b29), K(0xd5033b9f),
                                K(0xd50b7529), K(0xd5033b9f), K(0xd5033fdf), K(I_NOP),  K(I_RET)};
#define PATCH_SLOT 7
/*
 * adr x9, slot; stp w0, w1, [x9], #8; ret; nop; nop; slot: nop; ret
 * Called with two instructions in w0 and w1, it writes them into the slot and behind it. The
 * core says nothing of a pair store with writeback in ESR_EL2: the hypervisor has to take
 * the instruction apart to carry it out.
 */
static const u32 tpl_open[] = {K(0x100000a9), K(0x28810520), K(I_RET), K(I_NOP),
                               K(I_NOP),      K(I_NOP),      K(I_RET)};
#define OPEN_SLOT 5
/* For EL0: mov x0, #0x4c30; svc #0. For EL1, four bytes further on: mov x0, #0x1e1; ret */
static const u32 tpl_el0[] = {K(0xd2898600), K(0xd4000001), K(0xd2803c20), K(I_RET)};
#define EL0_MARK 0x4c30
#define EL1_MARK 0x1e1
/* For EL0: mrs x0, PMCCNTR_EL0; svc #0 */
static const u32 tpl_el0_read[] = {K(I_MRS_X0_PMCCNTR), K(0xd4000001)};

/*
 * A page that Windows has many of: instructions, one of them an access, and constants between
 * them that the instructions read.
 *    0: ldr x0, c64;  ret      2: ldrsw x0, c32;  ret      4: ldr w0, c32;  ret
 *    6: mrs x0, PMCCNTR_EL0;  ret
 *    8: c64: .quad 0x8877665544332211     10: c32: .word 0x80000001     11: .word 0x7f81f00d
 *   12: str w0, [x1];  ret                14: .word 0x0badc0de, 0
 */
static const u32 tpl_read[] = {K(0x58000100), K(I_RET),     K(0x98000100),       K(I_RET),
                               K(0x180000c0), K(I_RET),     K(I_MRS_X0_PMCCNTR), K(I_RET),
                               K(0x44332211), K(0x88776655), K(0x80000001),      K(0x7f81f00d),
                               K(0xb9000020), K(I_RET),     K(0x0badc0de),       K(0)};
#define READ_LDR64  0
#define READ_LDRSW  2
#define READ_LDR32  4
#define READ_ACCESS 6
#define READ_C64    8
#define READ_C32    10
#define READ_BYTES  11
#define READ_STORE  12
#define READ_SPARE  14
#define C64         0x8877665544332211UL

/* The transmit register of the UART, from its base in the image header (sgitest-head.S) */
#define UTXH 0x20
extern u64 hdr_uart_base;

/* ---- output ---- */

static int strict;
static u32 nr_pass, nr_fail;

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

void secondary_main(int slot)
{
    (void)slot;
    for (;;)
        ;
}

/*
 * A second vector table, in use only while a page runs at EL0: a synchronous exception from
 * there ends run_el0(), which returns what EL0 left in x0 and stores ESR_EL1 through esr.
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
    "    ret\n");

/* ---- helpers ---- */

static u32 *page(int n)
{
    u64 base = ((u64)arena + PAGE - 1) & ~(PAGE - 1);

    return (u32 *)(base + n * PAGE);
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

static void emit(u32 *at, const u32 *tpl, int words)
{
    u32 k = key;

    for (int i = 0; i < words; i++)
        at[i] = tpl[i] ^ k;
    sync_code(at, words);
}

#define EMIT(at, tpl) emit(at, tpl, sizeof(tpl) / sizeof((tpl)[0]))

typedef u64 (*fn0)(void);
typedef u64 (*fn1)(u64);
typedef u64 (*fn2)(u64, u64);

static u64 call0(u32 *at)
{
    return ((fn0)at)();
}

static u64 call1(u32 *at, u64 arg)
{
    return ((fn1)at)(arg);
}

/*
 * The instruction at this address, read as data, is the access that was written there,
 * whatever the core executes in its place. masked is K(the access).
 */
static int as_expected(const u32 *at, u32 masked)
{
    return *(volatile const u32 *)at == U(masked);
}

static void spin(u64 ticks)
{
    u64 t0 = read_sysreg(cntvct_el0);

    while (read_sysreg(cntvct_el0) - t0 < ticks)
        ;
}

/* ---- the tests ---- */

/* What winload.efi and bootmgfw.efi do before they use the counter (BlpArchInitialize) */
static void test_setup(void)
{
    u64 pmcr = read_sysreg(pmcr_el0);

    value("PMCR_EL0 at the start", pmcr);
    if (!(pmcr & 1)) {
        write_sysreg(pmcntenclr_el0, 0xffffffff);
        write_sysreg(pmovsclr_el0, 0xffffffff);
        write_sysreg(pmintenclr_el1, 0xffffffff);
    }
    write_sysreg(pmcr_el0, (pmcr & ~0x28UL) | 0x51);
    write_sysreg(pmccfiltr_el0, 0x8000000);
    write_sysreg(pmcntenset_el0, 0x80000000);
    write_sysreg(pmuserenr_el0, 0);
    asm volatile("isb");

    result("PMCR_EL0 keeps E and LC", (read_sysreg(pmcr_el0) & 0x41) == 0x41);
    result("the cycle counter is enabled", (read_sysreg(pmcntenset_el0) >> 31) & 1);

    u64 a = read_sysreg(pmccntr_el0);
    spin(read_sysreg(cntfrq_el0) / 100);
    u64 b = read_sysreg(pmccntr_el0);
    value("PMCCNTR_EL0", a);
    value("10 ms later", b);
    result("PMCCNTR_EL0 goes up", b > a);

    /* HalpTimerInitSystem */
    write_sysreg(pmccntr_el0, 0);
    asm volatile("isb");
    u64 c = read_sysreg(pmccntr_el0);
    value("after writing 0", c);
    if (strict) {
        /* The count is the generic timer's: a write goes through and changes nothing */
        result("PMCCNTR_EL0 can be written, and the count runs on", c >= b);

        u64 t0 = read_sysreg(cntvct_el0);
        u64 d = read_sysreg(pmccntr_el0);
        u64 t1 = read_sysreg(cntvct_el0);
        value("CNTVCT_EL0", t0);
        result("PMCCNTR_EL0 is the count of the generic timer", t0 <= d && d <= t1);
    } else {
        result("PMCCNTR_EL0 can be written", c < b);
    }
}

static void test_fresh_page(void)
{
    u32 *pg = page(PG_FRESH);

    EMIT(pg, tpl_ccntr);
    u64 a = call0(pg);
    spin(1000);
    u64 b = call0(pg);
    result("code written into a page that never ran: reads the counter", b > a);
    result("...and reads as it was written afterwards", as_expected(pg, K(I_MRS_X0_PMCCNTR)));
    result("...and so does data behind it that looks like the instruction",
           as_expected(pg + 2, K(I_MRS_X0_PMCCNTR)));

    /* The page is executable now. Write other code into it and run that, over and over */
    int ok = 1, seen = 1;
    for (int i = 0; i < NR_REWRITES; i++) {
        if (i & 1) {
            EMIT(pg, tpl_ccntr);
            u64 c = call0(pg);
            ok &= c > b;
            b = c;
            seen &= as_expected(pg, K(I_MRS_X0_PMCCNTR));
        } else {
            EMIT(pg, tpl_pmcr);
            ok &= (call0(pg) & 0x41) == 0x41;
            seen &= as_expected(pg, K(I_MRS_X0_PMCR));
        }
    }
    result("the page rewritten 16 times: each version does what it says", ok);
    result("...and is as expected afterwards", seen);
}

static void test_store_same_page(void)
{
    u32 *pg = page(PG_STORE);
    u64 *data = (u64 *)(pg + STORE_DATA);
    int ok = 1;
    u64 last = 0;

    EMIT(pg, tpl_store);
    for (u64 i = 1; i <= NR_STORES; i++) {
        u64 c = call1(pg, 0x5700000000000000UL + i);
        ok &= *data == 0x5700000000000000UL + i && c > last;
        last = c;
    }
    result("code that stores into its own page, 1000 times: the stores arrive", ok);
    result("...and the code is as expected afterwards", as_expected(pg + 2, K(I_MRS_X0_PMCCNTR)));

    /* A store from elsewhere into that page, and the code again */
    *data = 1;
    u64 c = call1(pg, 2);
    result("after a store from another page: the code still works", *data == 2 && c > last);
}

static void test_patch_same_page(void)
{
    u32 *pg = page(PG_PATCH);
    u32 insn = U(K(I_MRS_X0_PMCCNTR));

    EMIT(pg, tpl_patch);
    u64 a = call1(pg, insn);
    spin(1000);
    u64 b = call1(pg, insn);
    value("first", a);
    value("second", b);
    result("code that writes an access into its own page and runs it", a != insn && b > a);
    result("...and the access is as expected afterwards",
           as_expected(pg + PATCH_SLOT, K(I_MRS_X0_PMCCNTR)));
}

static void test_open_page(void)
{
    u32 *pg = page(PG_OPEN);
    u32 insn = U(K(I_MRS_X0_PMCCNTR));
    u64 freq = read_sysreg(cntfrq_el0);

    EMIT(pg, tpl_open);
    ((fn2)pg)(insn, U(K(I_RET)));
    sync_code(pg + OPEN_SLOT, 2);
    result("a pair store with writeback into its own page arrives",
           pg[OPEN_SLOT + 1] == U(K(I_RET)) && pg[OPEN_SLOT] == insn);

    /* Not at once: several ticks of the hypervisor later */
    spin(freq / 200);
    u64 a = call0(pg + OPEN_SLOT);
    spin(1000);
    u64 b = call0(pg + OPEN_SLOT);
    result("...and the access it wrote works 5 ms later", b > a);
    result("...and is as expected afterwards", as_expected(pg + OPEN_SLOT, K(I_MRS_X0_PMCCNTR)));
}

static void test_el0(void)
{
    u32 *pg = page(PG_EL0);
    u64 vbar = read_sysreg(vbar_el1);

    EMIT(pg, tpl_el0);
    write_sysreg(vbar_el1, (u64)el0_vectors);
    asm volatile("isb");
    u64 el0_esr = 0;
    u64 x0 = run_el0((u64)pg, (u64)el0_stack + sizeof(el0_stack), &el0_esr);
    write_sysreg(vbar_el1, vbar);
    asm volatile("isb");

    value("ESR_EL1 of the way back from EL0", el0_esr);
    result("a page that EL1 never ran executes at EL0", x0 == EL0_MARK && (el0_esr >> 26) == 0x15);
    result("...and at EL1 afterwards", call0(pg + 2) == EL1_MARK);
}

/* What smss.exe does first: read the cycle counter at EL0 */
static void test_el0_read(void)
{
    u32 *pg = page(PG_EL0_READ);
    u64 vbar = read_sysreg(vbar_el1);

    /* HalpGitInitialize: EL0 may read the counts of the generic timer (EL0PCTEN, EL0VCTEN) */
    write_sysreg(cntkctl_el1, read_sysreg(cntkctl_el1) | 3);
    /* HalpCycleCounterInitialize: EL0 may read the cycle counter (CR) */
    write_sysreg(pmuserenr_el0, read_sysreg(pmuserenr_el0) | 4);
    asm volatile("isb");

    EMIT(pg, tpl_el0_read);
    write_sysreg(vbar_el1, (u64)el0_vectors);
    asm volatile("isb");
    u64 el0_esr = 0;
    u64 before = read_sysreg(pmccntr_el0);
    u64 x0 = run_el0((u64)pg, (u64)el0_stack + sizeof(el0_stack), &el0_esr);
    u64 after = read_sysreg(pmccntr_el0);
    write_sysreg(vbar_el1, vbar);
    asm volatile("isb");

    value("ESR_EL1 of the way back from EL0", el0_esr);
    value("PMCCNTR_EL0 read at EL0", x0);
    /* EC 0x15 is the svc behind the read; 0x00 would be the read itself, undefined */
    result("EL0 reads the cycle counter", (el0_esr >> 26) == 0x15);
    result("...and gets the count that EL1 gets", before < x0 && x0 < after);
    result("...and the read is as expected afterwards", as_expected(pg, K(I_MRS_X0_PMCCNTR)));
}

/*
 * Code and constants in one page, read by the code itself and from elsewhere. Under the
 * hypervisor the page is execute-only once it has run, because an access in it was replaced,
 * and every read below is the hypervisor's doing.
 */
static void test_read_code(void)
{
    u32 *pg = page(PG_READ);

    EMIT(pg, tpl_read);
    u64 a = call0(pg + READ_ACCESS);
    spin(1000);
    u64 b = call0(pg + READ_ACCESS);
    result("a page of code with an access and constants: the access works", b > a);

    result("its code reads a 64-bit constant of the page", call0(pg + READ_LDR64) == C64);
    result("...a 32-bit one, sign-extended", call0(pg + READ_LDRSW) == 0xffffffff80000001UL);
    result("...and a 32-bit one into a 32-bit register", call0(pg + READ_LDR32) == 0x80000001UL);

    u64 v1 = *(volatile u8 *)(pg + READ_BYTES);
    u64 v2 = *(volatile unsigned short *)(pg + READ_BYTES);
    u64 v4 = *(volatile u32 *)(pg + READ_C32);
    u64 v8 = *(volatile u64 *)(pg + READ_C64);
    result("code elsewhere reads 1, 2, 4 and 8 bytes of it",
           v1 == 0x0d && v2 == 0xf00d && v4 == 0x80000001 && v8 == C64);

    long s1 = *(volatile signed char *)((u8 *)(pg + READ_BYTES) + 2);
    long s2 = *(volatile short *)(pg + READ_BYTES);
    long s4 = *(volatile int *)(pg + READ_C32);
    result("...and 1, 2 and 4 bytes sign-extended", s1 == -127 && s2 == -4083 && s4 == -2147483647);

    /* At the first byte of a page the access may have begun in the page before */
    u64 first = *(volatile u64 *)pg;
    result("...and the first 8 bytes of the page", first == (((u64)U(K(I_RET)) << 32) | 0x58000100));

    /* A pair: the syndrome does not describe it */
    u64 lo, hi;
    asm volatile("ldp %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(pg + READ_C64) : "memory");
    result("...and two registers at once", lo == C64 && hi == 0x7f81f00d80000001UL);
    /* ...the access itself and the ret behind it: 8 bytes, half of them replaced */
    asm volatile("ldp %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(pg + READ_ACCESS) : "memory");
    result("the access read as data, two registers at once, is what was written",
           lo == (((u64)U(K(I_RET)) << 32) | U(K(I_MRS_X0_PMCCNTR))) && hi == C64);
    result("...and so it is read as 4 bytes",
           as_expected(pg + READ_ACCESS, K(I_MRS_X0_PMCCNTR)));

    /* A store from elsewhere makes the page writable; it is scanned when it next runs */
    pg[READ_SPARE] = 0x600dc0de;
    result("a store into it from elsewhere arrives", *(volatile u32 *)(pg + READ_SPARE) == 0x600dc0de);
    result("...and its code reads the constant again", call0(pg + READ_LDR64) == C64);

    /* The hypervisor has to read the instruction to carry out the store: from a page like this */
    line("     a character written by code in that page: ");
    ((fn2)(pg + READ_STORE))('Y', hdr_uart_base + UTXH);
    put("\n");
    result("its code writes to a device register", 1);
}

static void test_timing(void)
{
    u64 freq = read_sysreg(cntfrq_el0);
    for (int i = 0; i < NR_TIMING; i++) {
        page(PG_TIMING + i)[0] = U(K(I_RET));
        sync_code(page(PG_TIMING + i), 1);
    }

    u64 t0 = read_sysreg(cntvct_el0);
    for (int i = 0; i < NR_TIMING; i++)
        call0(page(PG_TIMING + i));
    u64 t1 = read_sysreg(cntvct_el0);
    for (int i = 0; i < NR_TIMING; i++)
        call0(page(PG_TIMING + i));
    u64 t2 = read_sysreg(cntvct_el0);

    line("     32 pages, each run for the first time: ");
    put_dec((t1 - t0) * 1000000000 / freq / NR_TIMING);
    put(" ns a page; run again: ");
    put_dec((t2 - t1) * 1000000000 / freq / NR_TIMING);
    put(" ns a page\n");
    result("32 pages run for the first time", 1);
}

static void test_untouched(void)
{
    u32 *pg = page(PG_UNTOUCHED);
    u32 insn = U(K(I_MRS_X0_PMCCNTR));
    int ok = 1;

    for (u64 i = 0; i < WORDS; i++)
        ok &= pg[i] == insn + (i & 31);
    result("a page full of accesses that never ran, between two that did, is as it was written",
           ok);
}

void boot_main(void)
{
    u32 insn = U(K(I_MRS_X0_PMCCNTR));

    strict = !!(hdr_arg & ARG_STRICT);

    line("start, arg=");
    put_hex(hdr_arg, 4);
    put(strict ? ", expecting replaced code\n" : ", expecting real PMUv3 registers\n");
    line("CurrentEL = ");
    put_dec((read_sysreg(CurrentEL) >> 2) & 3);
    put("\n");
    value("arena", (u64)page(0));

    /* Before anything else: fill the page that is never to run, every Rt there is */
    for (u64 i = 0; i < WORDS; i++)
        page(PG_UNTOUCHED)[i] = insn + (i & 31);

    test_setup();
    test_fresh_page();
    test_store_same_page();
    test_patch_same_page();
    test_open_page();
    test_el0();
    test_el0_read();
    test_read_code();
    test_timing();
    test_untouched();

    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");

    psci_system_off();
    line("FAIL SYSTEM_OFF returned\n");
    for (;;)
        ;
}
