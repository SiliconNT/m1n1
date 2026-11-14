// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the PSCI of src/hv/hv_psci.c: it makes the calls the Windows kernel
 * makes while it starts (10.0.26300.9457: PsciLibraryInit and what it calls), looks at every
 * register afterwards, and ends with a call that must not come back.
 *
 * What it expects depends on bit 8 of the argument the loader leaves in the image header:
 *
 *   0  what the Windows kernel needs from any PSCI: a version of 0.2 or later, PSCI_FEATURES
 *      that says the six functions are there, x4 to x30 left alone (Arm DEN 0028 1.7 H 2.7).
 *      This is what QEMU's own PSCI is held to (`make qemu-psci`, `make qemu-psci-reset`),
 *      which checks the guest itself.
 *   1  exactly the answers of the hypervisor: version 1.1, the six functions and no others,
 *      -1 for everything else, only x0 changed, the upper halves of x0 and x1 ignored.
 *
 * Bits 3:0 of the argument choose the call it ends with, see enum last_call.
 *
 * It runs on the boot CPU only, with interrupts masked, and touches neither the GIC nor a
 * timer. It prints on the hypervisor's virtual UART; lines start with "HVLOG: " so that the
 * hypervisor copies them to its own console. Entry points and vectors are those of sgitest
 * (sgitest-head.S).
 */

#include "sgitest.h"
#include "gic.h" /* for u64 and u32 */

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_arg;

#define ARG_LAST   0xf
#define ARG_STRICT 0x100

enum last_call {
    LAST_SYSTEM_OFF,
    LAST_SYSTEM_RESET,
    LAST_HVC_1,   // hvc #1 with PSCI_VERSION in x0: not PSCI, the hypervisor must not answer
    LAST_CPU_ON,  // only under the hypervisor, which cannot do it yet and must not answer
    LAST_CPU_OFF, // the same
    LAST_NONE,
};

void uart_putc(int c);

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })

/* Arm DEN 0022 F.b 5.1, DEN 0028 1.7 H 7.2 to 7.5 */
#define FN_PSCI_VERSION  0x84000000UL
#define FN_CPU_OFF       0x84000002UL
#define FN_CPU_ON        0xc4000003UL
#define FN_SYSTEM_OFF    0x84000008UL
#define FN_SYSTEM_RESET  0x84000009UL
#define FN_PSCI_FEATURES 0x8400000aUL
#define FN_SMCCC_VERSION 0x80000000UL

/* The six the kernel takes for granted on PSCI 1.1 (PsciPlatformInfo_v1_1, state 2) */
static const u32 taken_for_granted[] = {
    FN_PSCI_VERSION, FN_CPU_ON, FN_CPU_OFF, FN_PSCI_FEATURES, FN_SYSTEM_RESET, FN_SYSTEM_OFF,
};

/* ...and the ones it asks PSCI_FEATURES about on 1.1 and 1.3 (state 1), in its order */
static const u32 asked_about[] = {
    0xc4000001, /* CPU_SUSPEND */
    0x8400000b, /* CPU_FREEZE */
    0xc400000c, /* CPU_DEFAULT_SUSPEND */
    0x8400000f, /* PSCI_SET_SUSPEND_MODE */
    0xc4000010, /* PSCI_STAT_RESIDENCY */
    0xc4000011, /* PSCI_STAT_COUNT */
    0xc4000012, /* SYSTEM_RESET2 */
    0x84000013, /* MEM_PROTECT */
    0xc4000014, /* MEM_PROTECT_CHECK_RANGE */
    0x80000000, /* SMCCC_VERSION */
    0xc4000015, /* SYSTEM_OFF2 */
};

/*
 * Functions the hypervisor does not have, called outright. Only under the hypervisor: on
 * something that has them, some of these would put the CPU to sleep or change its state.
 */
static const u32 not_there[] = {
    0x80000000, /* SMCCC_VERSION */
    0x80000001, /* SMCCC_ARCH_FEATURES */
    0x80008000, /* SMCCC_ARCH_WORKAROUND_1 */
    0xc4000001, /* CPU_SUSPEND */
    0xc4000012, /* SYSTEM_RESET2 */
    0x84000013, /* MEM_PROTECT */
    0x84000003, /* CPU_ON, SMC32 */
    0xc4000004, /* AFFINITY_INFO */
    0x86000000, /* a vendor specific hypervisor service */
    0xffffffff,
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the call ---- */

#define NR_REGS 31

struct regs {
    u64 x[NR_REGS];
};

/*
 * void hvc_0(struct regs *r), void hvc_1(struct regs *r): x0 to x29 from r, hvc with that
 * immediate, x0 to x30 back into r. x30 holds r across the hvc, so that is what comes back
 * in r->x[30] if the other side left it alone.
 */
asm("    .text\n"
    "    .macro hvc_fn name, imm\n"
    "    .globl \\name\n"
    "\\name:\n"
    "    stp     x29, x30, [sp, #-112]!\n"
    "    stp     x19, x20, [sp, #16]\n"
    "    stp     x21, x22, [sp, #32]\n"
    "    stp     x23, x24, [sp, #48]\n"
    "    stp     x25, x26, [sp, #64]\n"
    "    stp     x27, x28, [sp, #80]\n"
    "    str     x0, [sp, #96]\n"
    "    mov     x30, x0\n"
    "    ldp     x0, x1, [x30]\n"
    "    ldp     x2, x3, [x30, #16]\n"
    "    ldp     x4, x5, [x30, #32]\n"
    "    ldp     x6, x7, [x30, #48]\n"
    "    ldp     x8, x9, [x30, #64]\n"
    "    ldp     x10, x11, [x30, #80]\n"
    "    ldp     x12, x13, [x30, #96]\n"
    "    ldp     x14, x15, [x30, #112]\n"
    "    ldp     x16, x17, [x30, #128]\n"
    "    ldp     x18, x19, [x30, #144]\n"
    "    ldp     x20, x21, [x30, #160]\n"
    "    ldp     x22, x23, [x30, #176]\n"
    "    ldp     x24, x25, [x30, #192]\n"
    "    ldp     x26, x27, [x30, #208]\n"
    "    ldp     x28, x29, [x30, #224]\n"
    "    hvc     #\\imm\n"
    "    str     x30, [sp, #104]\n"
    "    ldr     x30, [sp, #96]\n"
    "    stp     x0, x1, [x30]\n"
    "    stp     x2, x3, [x30, #16]\n"
    "    stp     x4, x5, [x30, #32]\n"
    "    stp     x6, x7, [x30, #48]\n"
    "    stp     x8, x9, [x30, #64]\n"
    "    stp     x10, x11, [x30, #80]\n"
    "    stp     x12, x13, [x30, #96]\n"
    "    stp     x14, x15, [x30, #112]\n"
    "    stp     x16, x17, [x30, #128]\n"
    "    stp     x18, x19, [x30, #144]\n"
    "    stp     x20, x21, [x30, #160]\n"
    "    stp     x22, x23, [x30, #176]\n"
    "    stp     x24, x25, [x30, #192]\n"
    "    stp     x26, x27, [x30, #208]\n"
    "    stp     x28, x29, [x30, #224]\n"
    "    ldr     x0, [sp, #104]\n"
    "    str     x0, [x30, #240]\n"
    "    ldp     x19, x20, [sp, #16]\n"
    "    ldp     x21, x22, [sp, #32]\n"
    "    ldp     x23, x24, [sp, #48]\n"
    "    ldp     x25, x26, [sp, #64]\n"
    "    ldp     x27, x28, [sp, #80]\n"
    "    ldp     x29, x30, [sp], #112\n"
    "    ret\n"
    "    .endm\n"
    "    hvc_fn  hvc_0, 0\n"
    "    hvc_fn  hvc_1, 1\n");

void hvc_0(struct regs *r);
void hvc_1(struct regs *r);

static struct regs before, after;
static int strict;
static u32 nr_pass, nr_fail;

/* A struct assignment would be a call to memcpy, which is not there */
static void reload(void)
{
    for (int i = 0; i < NR_REGS; i++)
        after.x[i] = before.x[i];
}

/* x1 to x29 as a caller might have left them: no two alike, none that looks like an answer */
static void call(u64 x0, u64 x1)
{
    for (int i = 0; i < NR_REGS - 1; i++)
        before.x[i] = 0xa5a5000000000000UL | ((u64)i << 32) | (0x2000 + i);
    before.x[0] = x0;
    before.x[1] = x1;
    before.x[30] = (u64)&after;
    reload();
    hvc_0(&after);
}

/* The first register from x<from> up that the call changed, or 0 */
static int changed(int from)
{
    for (int i = from; i < NR_REGS; i++)
        if (after.x[i] != before.x[i])
            return i;
    return 0;
}

/* The answer as the Windows kernel reads it: w0 */
static u32 w0(void)
{
    return after.x[0];
}

/* ---- output ---- */

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
    put("HVLOG: pscitest: ");
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

/* What a call answered: "name(arg) x0 = value" */
static void answered(const char *name, u64 arg)
{
    line("     ");
    put(name);
    put("(");
    put_hex(arg, 8);
    put(") x0 = ");
    put_hex(after.x[0], 16);
    put("\n");
}

static void changed_reg(int reg)
{
    line("     changed: x");
    put_dec(reg);
    put(" = ");
    put_hex(after.x[reg], 16);
    put(", was ");
    put_hex(before.x[reg], 16);
    put("\n");
}

/* ---- exception handlers, called from sgitest-head.S ---- */

/* Interrupts stay masked and nothing enables one */
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

/* ---- the tests ---- */

/* From which register on a call has to leave things alone */
static int first_kept(void)
{
    /* x1 to x3 are result registers a function may use or clear (DEN 0028 2.6, 2.7) */
    return strict ? 1 : 4;
}

static void test_version(void)
{
    u32 version;
    int reg, same = 1;

    call(FN_PSCI_VERSION, 0);
    version = w0();
    answered("PSCI_VERSION", 0);
    if (strict) {
        result("PSCI_VERSION is 1.1", after.x[0] == 0x00010001);
    } else {
        /* PscipInitDetectPlatform: below 0.2 there is no PSCI for the kernel */
        result("PSCI_VERSION is 0.2 or later", !(version >> 31) && version >= 2);
    }
    reg = changed(first_kept());
    result(strict ? "PSCI_VERSION changed nothing but x0" : "PSCI_VERSION left x4 to x30 alone",
           !reg);
    if (reg)
        changed_reg(reg);

    /* HAL, power manager and the speculation checks each ask again */
    for (int i = 0; i < 1000; i++) {
        call(FN_PSCI_VERSION, i);
        if (w0() != version || changed(first_kept()))
            same = 0;
    }
    result("1000 more PSCI_VERSION calls answered the same", same);
}

static void test_features(void)
{
    int ok = 1, kept = 1, reg;

    for (u64 i = 0; i < COUNT(taken_for_granted); i++) {
        call(FN_PSCI_FEATURES, taken_for_granted[i]);
        answered("PSCI_FEATURES", taken_for_granted[i]);
        /* Bit 31 is 0 for a function that is there; none of these has feature flags */
        if (strict ? after.x[0] != 0 : w0() >> 31)
            ok = 0;
        reg = changed(first_kept());
        if (reg) {
            kept = 0;
            changed_reg(reg);
        }
    }
    result("PSCI_FEATURES says the six functions of PSCI 1.1 are there", ok);

    ok = 1;
    for (u64 i = 0; i < COUNT(asked_about); i++) {
        call(FN_PSCI_FEATURES, asked_about[i]);
        answered("PSCI_FEATURES", asked_about[i]);
        /* PscipInitDiscoverFeatures only looks at bit 31; -1 is NOT_SUPPORTED */
        if (strict && after.x[0] != (u64)-1)
            ok = 0;
        reg = changed(first_kept());
        if (reg) {
            kept = 0;
            changed_reg(reg);
        }
    }
    if (strict)
        result("PSCI_FEATURES says the eleven the kernel asks about are not there", ok);
    result(strict ? "PSCI_FEATURES changed nothing but x0" : "PSCI_FEATURES left x4 to x30 alone",
           kept);
}

/* Only under the hypervisor */
static void test_strict(void)
{
    int ok = 1, kept = 1, reg;

    for (u64 i = 0; i < COUNT(not_there); i++) {
        call(not_there[i], 0x1234);
        answered("call", not_there[i]);
        if (after.x[0] != (u64)-1)
            ok = 0;
        reg = changed(1);
        if (reg) {
            kept = 0;
            changed_reg(reg);
        }
    }
    result("a function that is not there answers -1", ok);
    result("...and changes nothing but x0", kept);

    /* The function ID is w0 and psci_func_id is a uint32 (DEN 0028 2.6, DEN 0022 5.1.15) */
    call(0xdeadbeef00000000UL | FN_PSCI_VERSION, 0);
    ok = after.x[0] == 0x00010001;
    call(FN_PSCI_FEATURES, 0xdeadbeef00000000UL | FN_SYSTEM_OFF);
    ok = ok && after.x[0] == 0;
    call(FN_PSCI_FEATURES, 0xdeadbeef00000000UL | FN_SMCCC_VERSION);
    ok = ok && after.x[0] == (u64)-1;
    result("the upper halves of x0 and x1 do not count", ok);
}

/* A call that must not come back. If it does, say so */
static void last_call(void)
{
    u64 last = hdr_arg & ARG_LAST;
    const char *name;

    /* Not a table: the image does not know where it runs, so it cannot hold pointers */
    switch (last) {
        case LAST_SYSTEM_OFF:
            name = "SYSTEM_OFF";
            break;
        case LAST_SYSTEM_RESET:
            name = "SYSTEM_RESET";
            break;
        case LAST_HVC_1:
            name = "hvc #1";
            break;
        case LAST_CPU_ON:
            name = "CPU_ON";
            break;
        case LAST_CPU_OFF:
            name = "CPU_OFF";
            break;
        default:
            return;
    }

    line("last: ");
    put(name);
    put(", which must not come back\n");

    switch (last) {
        case LAST_SYSTEM_OFF:
            call(FN_SYSTEM_OFF, 0);
            break;
        case LAST_SYSTEM_RESET:
            call(FN_SYSTEM_RESET, 0);
            break;
        case LAST_HVC_1:
            call(FN_PSCI_VERSION, 0);
            reload();
            hvc_1(&after);
            break;
        case LAST_CPU_ON:
            /* target_cpu, entry_point_address, context_id: a CPU that does not exist */
            call(FN_CPU_ON, 0xffffff);
            break;
        case LAST_CPU_OFF:
            call(FN_CPU_OFF, 0);
            break;
    }

    line("FAIL ");
    put(name);
    put(" came back, x0 = ");
    put_hex(after.x[0], 16);
    put("\n");
}

void boot_main(void)
{
    strict = !!(hdr_arg & ARG_STRICT);

    line("start, arg=");
    put_hex(hdr_arg, 4);
    put(strict ? ", expecting the hypervisor's answers\n" : ", expecting any PSCI\n");
    line("CurrentEL = ");
    put_dec((read_sysreg(CurrentEL) >> 2) & 3);
    put("\n");

    test_version();
    test_features();
    if (strict)
        test_strict();

    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");

    last_call();
}
