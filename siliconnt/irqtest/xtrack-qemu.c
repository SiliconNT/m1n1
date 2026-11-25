// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Runs a test guest (pmutest.bin or xitest.bin) on QEMU's "virt" machine under an EL2 of its
 * own, to check the guest together with the execution tracking of the hypervisor before
 * either goes near the M4: `make qemu-pmu-el2`, `make qemu-pmu-el2-nosyn`, `make qemu-xi-el2`,
 * `make qemu-xi-el2-nosyn`, and `make qemu-xi-broken` with a hv_xtrack.c that has something
 * wrong in it (XTRACK_INC).
 *
 * src/hv/hv_xtrack.c, src/hv/hv_xinsn.c and src/hv/hv_pmu.c are the real ones: the Makefile
 * drops their #include lines, and this file stands in for the rest of the hypervisor. It has
 *
 *  - a stage 2 with 4 KB pages for the memory the guest is in, with the page states of
 *    src/hv/hv_vm.c: data and code, which point to the guest's page, and execute-only, which
 *    points to a shadow page; a page goes from one to another through an invalid entry, and
 *    hv_pt_set_scanned() refuses any other way, as the one of hv_vm.c does;
 *  - the heap that the shadow pages come from (memalign()), in memory that the guest's stage 2
 *    does not map;
 *  - what hv_exc_sync() and hv_handle_dabort() do with an instruction abort, a data abort
 *    and an hvc;
 *  - an interrupt of its own every millisecond, as the hypervisor has;
 *  - for pmutest, an hvc that the hypervisor does not have (HVC_DEVICE): it writes a word of
 *    the guest's memory from outside stage 2, as a device would, and tells the guest what
 *    execution tracking counted. The guest is told that it is there (ARG_DEVICE).
 *
 * QEMU's processor has no caches to keep, and it carries out DC IVAC without asking stage 2
 * for permission to write. So a DC IVAC of the guest's on a page that it may not write is made
 * to fault here as the architecture has it (Arm DDI 0487 M.d, D7.5.9.2): HCR_EL2.TPCP traps the
 * data cache maintenance to the point of coherency to EL2 (D24.2.61), and shim_dc_poc() hands
 * hv_xtrack.c the data abort that the core would have taken.
 *
 * QEMU's processor has the PMUv3 registers. The guest's accesses are replaced all the same,
 * and it is told to insist on that (ARG_STRICT). And QEMU's processor is the architecture as
 * QEMU reads it, not the M4: what this run shows is that the guest and the hypervisor's code
 * agree with each other and with that reading.
 *
 * With NO_SYNDROME, ESR_EL2 never describes an access (ISV is taken out of it), so that every
 * access that hv_xtrack.c carries out is one that it takes apart from the instruction
 * (`make qemu-pmu-el2-nosyn`).
 */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
/* bool, true and false are the compiler's own (C23) */
#define NULL          ((void *)0)
#define BIT(n)        (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctzl(mask))
#define MASK(n)   (BIT(n) - 1)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define MAX_CPUS 1
#define SPSR_M   GENMASK(4, 0)

/* What hv_xtrack.c and hv_pmu.c use of the real one */
struct exc_info {
    u64 regs[32];
    u64 elr;
    u64 sp[3];
    u64 cpu_id;
};

/* What lower_entry in xtrack-qemu-head.S puts on the stack */
struct frame {
    u64 regs[31];
    u64 elr;
};

#include "hv_xpage.h" /* HV_XPAGE_* and HV_XTRACK_* of the real hv.h */

#define mrs(reg)                                                                                   \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #reg : "=r"(_v));                                                  \
        _v;                                                                                        \
    })
#define msr(reg, v)                                                                                \
    do {                                                                                           \
        u64 _v = (v);                                                                              \
        asm volatile("msr " #reg ", %0" : : "r"(_v));                                              \
    } while (0)
#define sysop(op) asm volatile(op)

u64 shim_smc(u64 x0, u64 x1, u64 x2, u64 x3);
extern u8 guest[], guest_end[];

#define PL011_BASE 0x09000000UL
#define GICD_BASE  0x08000000UL
#define GICR_BASE  0x080a0000UL
#define RAM_BASE   0x40000000UL
#define PAGE       0x1000UL
/* The memory that has pages of its own in stage 2: the first 4 MB, which the guest is in */
#define NR_L3     2
#define PAGED_END (RAM_BASE + NR_L3 * 512 * PAGE)
/* The heap of this EL2: 8 MB at 32 MB into RAM, two blocks that the guest's stage 2 leaves out */
#define HEAP_BASE (RAM_BASE + (32UL << 20))
#define HEAP_END  (HEAP_BASE + (8UL << 20))

/* Offsets in the image, see sgitest-head.S and sgitest.h (MAX_CPUS there is 4) */
#define HDR_UART_BASE 0x10
#define HDR_NR_CPUS   0x18
#define HDR_ARG       0x78
#define ARG_STRICT    0x100
#define ARG_DEVICE    0x400
/*
 * What the guest is told: pmutest insists on replaced code, and may ask for HVC_DEVICE. xitest
 * takes neither bit; other bits are the Makefile's
 */
#ifndef GUEST_ARG
#define GUEST_ARG (ARG_STRICT | ARG_DEVICE)
#endif

/* The hvc of this EL2 alone: its immediate, and x0 for the two calls; pmutest.c has them too */
#define HVC_DEVICE        0x7e57
#define HVC_DEVICE_WRITE  0 // x1: an address of the guest, x2: the word to write there
#define HVC_DEVICE_COUNTS 1 // x1: an address; back in x0 to x2: stale, ivac, state of its page

/* ---- output ---- */

static void shim_putc(int c)
{
    *(volatile u32 *)PL011_BASE = c;
}

static void shim_puts(const char *s)
{
    while (*s)
        shim_putc(*s++);
}

static void shim_num(u64 v, int base, int width)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (width-- > n)
        shim_putc('0');
    while (n)
        shim_putc(buf[--n]);
}

/* What the two files print: %d, %x, %ld, %lx, a width with a leading zero, %s and %p */
static int printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    shim_puts("SHIM> ");
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            shim_putc(*fmt);
            continue;
        }

        int width = 0;
        bool l = false;

        fmt++;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + *fmt++ - '0';
        if (*fmt == 'l') {
            l = true;
            fmt++;
        }
        if (*fmt == 's') {
            shim_puts(__builtin_va_arg(ap, const char *));
        } else if (*fmt == 'p') {
            void *p = __builtin_va_arg(ap, void *);

            shim_puts("0x");
            shim_num((u64)p, 16, width);
        } else if (*fmt == 'x') {
            shim_num(l ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32), 16, width);
        } else {
            s64 v = l ? __builtin_va_arg(ap, s64) : __builtin_va_arg(ap, int);

            if (v < 0) {
                shim_putc('-');
                v = -v;
            }
            shim_num(v, 10, width);
        }
    }
    __builtin_va_end(ap);
    return 0;
}

/* ---- stage 2 ---- */

/* MemAttr write-back memory, inner shareable, AF; then S2AP and XN, as src/hv/hv_vm.c has them */
#define S2_MEM   0x73cUL
#define S2_RW    (3UL << 6)
#define S2_RO    (1UL << 6)
#define S2_XN    BIT(54)
#define S2_BLOCK 1UL
#define S2_PAGE  3UL
#define S2_VALID 1UL
#define S2_OA    0x0000fffffffff000UL
/* MemAttr Device-nGnRE, S2AP read and write, AF, XN */
#define S2_DEV (0x4c4UL | BIT(54))
/* Left to software, as in src/hv/hv_vm.c */
#define SW_SCANNED BIT(56)
#define SW_XONLY   BIT(58)

static u64 s2_l1[512] __attribute__((aligned(4096)));
static u64 s2_l2_dev[512] __attribute__((aligned(4096)));
static u64 s2_l2_ram[512] __attribute__((aligned(4096)));
static u64 s2_l3[NR_L3][512] __attribute__((aligned(4096)));

/* The page that hv_pt_set_scanned() has given an invalid entry, on its way to another state */
#define NO_PAGE (~0UL)
static u64 s2_switching = NO_PAGE;
/* ...and how many times it has done that */
static u64 s2_switches;

static u64 *s2_pte(u64 ipa)
{
    u64 n = (ipa - RAM_BASE) / PAGE;

    return &s2_l3[n / 512][n % 512];
}

static bool hv_pt_xtrack_on(void)
{
    return true;
}

/*
 * As in hv_vm.c: the state of the page at this IPA, and in *pa the physical page that its entry
 * points to, the one that the core executes. -1 where there is no memory with pages of its own,
 * and for a page while its entry is invalid.
 */
static int hv_pt_scanned(u64 ipa, u64 *pa)
{
    ipa &= ~(PAGE - 1);
    if (ipa < RAM_BASE || ipa >= PAGED_END)
        return -1;

    u64 pte = *s2_pte(ipa);
    if (!(pte & S2_VALID))
        return -1;
    *pa = pte & S2_OA;
    if (pte & SW_XONLY)
        return HV_XPAGE_XONLY;
    return pte & SW_SCANNED ? HV_XPAGE_SCANNED : HV_XPAGE_UNSCANNED;
}

static void s2_check(u64 ipa, int state, u64 pa);

/*
 * As in hv_vm.c: the page at this IPA into a state, its entry pointing to the physical page pa.
 * HV_XPAGE_INVALID for a valid page first, then a valid state for that page and no other; -1
 * with nothing changed otherwise. Whether pa is the right page is checked as well, which
 * hv_vm.c leaves to its callers (s2_check()).
 */
static int hv_pt_set_scanned(u64 ipa, int state, u64 pa)
{
    u64 attrs, was;

    ipa &= ~(PAGE - 1);

    if (state == HV_XPAGE_INVALID) {
        if (s2_switching != NO_PAGE || hv_pt_scanned(ipa, &was) < 0)
            return -1;
        *s2_pte(ipa) = 0;
        s2_switching = ipa;
        s2_switches++;
        asm volatile("dsb ishst\ntlbi vmalls12e1is\ndsb ish\nisb");
        return 0;
    }

    if (ipa != s2_switching || (pa & (PAGE - 1)))
        return -1;

    switch (state) {
        case HV_XPAGE_UNSCANNED:
            attrs = S2_RW | S2_XN;
            break;
        case HV_XPAGE_SCANNED:
            attrs = S2_RO | SW_SCANNED;
            break;
        case HV_XPAGE_XONLY:
            attrs = SW_SCANNED | SW_XONLY;
            break;
        default:
            return -1;
    }
    s2_check(ipa, state, pa);
    *s2_pte(ipa) = pa | S2_MEM | attrs | S2_PAGE;
    s2_switching = NO_PAGE;
    asm volatile("dsb ishst\ntlbi vmalls12e1is\ndsb ish\nisb");
    return 0;
}

/* As in hv_vm.c */
static u64 hv_pt_switch_count(void)
{
    return s2_switches;
}

/*
 * A data abort that the core did not take, and this file hands to hv_xtrack.c as if it had:
 * shim_dc_poc()
 */
static bool shim_faking;
static u64 shim_fake_esr, shim_fake_far;

static u64 hv_get_esr(void)
{
    u64 esr = shim_faking ? shim_fake_esr : mrs(esr_el2);

#ifdef NO_SYNDROME
    /* A data abort (EC 0b10010x) without ISV: the instruction has to say what the access is */
    if ((esr >> 27) == 0x12)
        esr &= ~BIT(24);
#endif
    return esr;
}

static u64 hv_get_far(void)
{
    return shim_faking ? shim_fake_far : mrs(far_el2);
}

static u64 hv_get_spsr(void)
{
    return mrs(spsr_el2);
}

/* Stage 1 of the guest, for the Exception level it came from: the IPA of an address, or 0 */
static u64 hv_translate(u64 addr, bool s1, bool w, u64 *par_out)
{
    (void)s1;
    (void)w;
    (void)par_out;

    if (!(mrs(sctlr_el1) & 1))
        return addr;

    u64 save = mrs(par_el1);
    if ((mrs(spsr_el2) & 0xf) == 0)
        asm volatile("at s1e0r, %0" : : "r"(addr));
    else
        asm volatile("at s1e1r, %0" : : "r"(addr));
    asm volatile("isb");
    u64 par = mrs(par_el1);
    msr(par_el1, save);

    if (par & 1)
        return 0;
    return (par & 0x0000fffffffff000UL) | (addr & (PAGE - 1));
}

/* As src/memory.c has them: from addr on, every 64 bytes, with no alignment first */
static void dc_cvac_range(void *addr, u64 size)
{
    for (u64 p = (u64)addr; p < (u64)addr + size; p += 64)
        asm volatile("dc cvac, %0" : : "r"(p));
}

static void dc_civac_range(void *addr, u64 size)
{
    for (u64 p = (u64)addr; p < (u64)addr + size; p += 64)
        asm volatile("dc civac, %0" : : "r"(p));
}

static void ic_ialluis(void)
{
    asm volatile("ic ialluis");
}

/*
 * Where EL2 finds the instruction at this address of the guest: as in hv_vm.c, in the page that
 * stage 2 points to, the one that the core executes (for an execute-only page, its shadow
 * page). 0 outside the memory that has pages of its own, where the guest has no code, and for a
 * page while its entry is invalid.
 */
static u64 hv_translate_insn(u64 addr)
{
    u64 ipa = hv_translate(addr, true, false, NULL);
    u64 pa;

    if (!ipa || hv_pt_scanned(ipa, &pa) < 0)
        return 0;
    return pa | (ipa & (PAGE - 1));
}

/* The SIMD registers of the guest, which nothing here uses: xtrack-qemu-head.S */
void get_simd_state(void *state);
void put_simd_state_masked(const void *state, u32 mask);

/* The compiler asks for this to copy a structure */
void *memcpy(void *dst, const void *src, u64 n)
{
    for (u64 i = 0; i < n; i++)
        ((u8 *)dst)[i] = ((const u8 *)src)[i];
    return dst;
}

void *memset(void *dst, int c, u64 n)
{
    for (u64 i = 0; i < n; i++)
        ((u8 *)dst)[i] = c;
    return dst;
}

/* The heap: what memalign() of m1n1 gives, but NULL where m1n1 would stop for want of room */
static u64 heap_top = HEAP_BASE;

static void *memalign(u64 align, u64 size)
{
    u64 p = (heap_top + align - 1) & ~(align - 1);

    if (p + size > HEAP_END)
        return NULL;
    heap_top = p + size;
    return (void *)p;
}

/*
 * This EL2 is built for the tiny code model, all of it within 1 MB: a smaller table than the
 * real one, and 4 MB of shadow pages
 */
#define XT_MAX_PAGES 1024

#include "hv_xinsn_h.inc"
#include "hv_pmu.inc"
#include "hv_xinsn.inc"
#ifndef XTRACK_INC
#define XTRACK_INC "hv_xtrack.inc"
#endif
#include XTRACK_INC

/* ---- the hypervisor around them ---- */

static u64 nr_ticks;
/* Words that HVC_DEVICE_WRITE wrote: each may leave one shadow page out of date */
static u64 nr_device_writes;

static void __attribute__((noreturn)) shim_stop(void)
{
    shim_smc(0x84000008, 0, 0, 0); /* PSCI SYSTEM_OFF */
    for (;;)
        ;
}

void shim_bad(u64 vector)
{
    printf("unexpected exception, vector ESR ELR FAR = 0x%lx 0x%lx 0x%lx 0x%lx\n", vector,
           mrs(esr_el2), mrs(elr_el2), mrs(far_el2));
    hv_xtrack_report();
    shim_stop();
}

/*
 * What stage 2 is to point to, which only the model of the page tables checks otherwise
 * (vgic-model/xtrack_test.c): an execute-only page to its shadow page, and the other states to
 * the guest's page, which is at its IPA here, with no shadow page left.
 */
static void s2_check(u64 ipa, int state, u64 pa)
{
    struct xt_rec *r = xt_find(ipa);
    const char *why = NULL;

    if (state == HV_XPAGE_XONLY && (!r || pa != (u64)xt_shadow(r)))
        why = "is made execute-only, and does not point to its shadow page";
    else if (state != HV_XPAGE_XONLY && pa != ipa)
        why = "is made data or code, and does not point to the guest's page";
    else if (state != HV_XPAGE_XONLY && r)
        why = "is made data or code, and keeps a shadow page";
    if (!why)
        return;
    printf("FAIL stage 2: the page at IPA 0x%lx %s (0x%lx)\n", ipa, why, pa);
    shim_bad(0x200);
}

static void tick_arm(void)
{
    msr(cnthp_tval_el2, mrs(cntfrq_el0) / 1000);
    msr(cnthp_ctl_el2, 1);
}

/* The hypervisor's own timer: nothing to do but to have come here */
void shim_irq(struct frame *f)
{
    (void)f;

    u64 intid = mrs(S3_0_C12_C12_0); /* ICC_IAR1_EL1 */
    nr_ticks++;
    tick_arm();
    msr(S3_0_C12_C12_1, intid); /* ICC_EOIR1_EL1 */
}

/*
 * A data abort: what hv_handle_dabort() does with one on memory. hv_vm.c also has the guest
 * try again after a translation fault on a page that is valid by then, which another core
 * made in between; with one core there is none, and this file takes it for a failure.
 */
static bool shim_dabort(struct exc_info *ctx)
{
    u64 esr = hv_get_esr();
    bool is_write = esr & BIT(6);
    u64 ipa = hv_translate(hv_get_far(), true, is_write, NULL);

    if (!ipa)
        return false;

    switch (hv_xtrack_dabort(ctx, ipa, is_write)) {
        case HV_XTRACK_RETRY:
            return true;
        case HV_XTRACK_SKIP:
            ctx->elr += 4;
            return true;
    }
    /* Stopped, with the reason printed; or an abort that nothing explains */
    return false;
}

/* ESR_EL2.ISS of a trapped system instruction (D24.2.45): all but Rt and the direction */
#define ISS_SYS_OP  0x3ffc1eUL
#define ISS_SYS_RT  GENMASK(9, 5)
#define ISS_DC_IVAC ((1UL << 20) | (1UL << 17) | (0UL << 14) | (7UL << 10) | (6UL << 1))
/* A data abort from below (EC 0x24): IL, CM, WnR, and a permission fault at level 3 */
#define ESR_DC_FAULT ((0x24UL << 26) | BIT(25) | BIT(8) | BIT(6) | 0x0fUL)

/*
 * DC IVAC, DC CIVAC or DC CVAC of the guest, trapped (HCR_EL2.TPCP). DC IVAC on a page that
 * stage 2 does not let it write is a permission fault (D7.5.9.2), which QEMU does not take:
 * hv_xtrack.c gets the data abort that the core would have, with no syndrome of the access,
 * CM set and WnR set (D24.2.45), and the address in FAR_EL2. Anything else is carried out here
 * on the page that stage 2 points to, as DC CIVAC, which is what DC IVAC of EL1 is with stage 2
 * on (D7.5.9.2); the guest goes on behind it.
 */
static bool shim_dc_poc(struct exc_info *ctx, u64 esr)
{
    u32 rt = FIELD_GET(ISS_SYS_RT, esr);
    u64 va = rt == 31 ? 0 : ctx->regs[rt];
    u64 ipa = hv_translate(va, true, false, NULL);
    u64 pa;

    if (!ipa)
        return false;

    int state = hv_pt_scanned(ipa, &pa);

    if ((esr & ISS_SYS_OP) == ISS_DC_IVAC &&
        (state == HV_XPAGE_SCANNED || state == HV_XPAGE_XONLY)) {
        shim_fake_esr = ESR_DC_FAULT;
        shim_fake_far = va;
        shim_faking = true;

        bool ok = shim_dabort(ctx);

        shim_faking = false;
        return ok;
    }

    pa = state < 0 ? ipa : pa | (ipa & (PAGE - 1));
    asm volatile("dc civac, %0" : : "r"(pa));
    ctx->elr += 4;
    return true;
}

/*
 * HVC_DEVICE, which only this EL2 has. HVC_DEVICE_WRITE writes the word x2 at the address x1 of
 * the guest into the guest's page, past stage 2, as a device would, with no cache maintenance
 * (QEMU has no caches to keep). HVC_DEVICE_COUNTS gives back in x0 to x2 how many shadow pages
 * were found out of date, how many pages DC IVAC made data, and the state of the page at the
 * address x1 (-1 if it is no memory).
 */
static bool shim_device(struct exc_info *ctx)
{
    u64 ipa = hv_translate(ctx->regs[1], true, false, NULL);
    u64 pa;
    int state = ipa ? hv_pt_scanned(ipa, &pa) : -1;

    switch (ctx->regs[0]) {
        case HVC_DEVICE_WRITE:
            if (state < 0 || (ipa & 3))
                return false;
            /* The guest's page is at its IPA here, whatever stage 2 points to */
            *(volatile u32 *)ipa = ctx->regs[2];
            nr_device_writes++;
            ctx->regs[0] = 0;
            return true;
        case HVC_DEVICE_COUNTS:
            ctx->regs[0] = xt_stats.stale;
            ctx->regs[1] = xt_stats.ivac;
            ctx->regs[2] = state;
            return true;
    }
    return false;
}

void shim_sync(struct frame *f)
{
    struct exc_info ctx;
    u64 esr = mrs(esr_el2);
    u64 ec = esr >> 26;
    bool ok = false;

    for (int i = 0; i < 31; i++)
        ctx.regs[i] = f->regs[i];
    ctx.regs[31] = 0;
    ctx.elr = f->elr;
    ctx.cpu_id = 0;
    /*
     * As hv_get_context() of src/hv/hv_exc.c has them, and as the way out there puts them
     * back: a stack pointer of the guest that is written to the register in between is lost
     */
    ctx.sp[0] = mrs(sp_el0);
    ctx.sp[1] = mrs(sp_el1);
    ctx.sp[2] = 0;

    if (ec == 0x16) {
        /* hvc: the address to return to is behind the instruction */
        if (esr & BIT(15)) {
            ok = hv_pmu_hvc(&ctx, esr & 0xffff);
        } else if ((esr & 0xffff) == HVC_DEVICE) {
            ok = (GUEST_ARG & ARG_DEVICE) && shim_device(&ctx);
        } else {
            /* PSCI SYSTEM_OFF: what the hypervisor prints when the guest has ended */
            if ((u32)ctx.regs[0] == 0x84000008) {
                printf("%ld ticks\n", nr_ticks);
                hv_pmu_report();
                hv_xtrack_report();
            }
            ctx.regs[0] = shim_smc(ctx.regs[0], ctx.regs[1], ctx.regs[2], ctx.regs[3]);
            ok = true;
        }
    } else if (ec == 0x20) {
        ok = hv_xtrack_iabort(&ctx);
    } else if (ec == 0x24) {
        ok = shim_dabort(&ctx);
    } else if (ec == 0x18) {
        ok = shim_dc_poc(&ctx, esr);
    }

    /* A shadow page is out of date only after a device wrote; xtrack_test.c checks the same */
    if (xt_stats.stale > nr_device_writes) {
        printf("FAIL a shadow page was found out of date, and no device wrote\n");
        ok = false;
    }

    if (!ok)
        shim_bad(0x100);

    for (int i = 0; i < 31; i++)
        f->regs[i] = ctx.regs[i];
    f->elr = ctx.elr;
    msr(sp_el0, ctx.sp[0]);
    msr(sp_el1, ctx.sp[1]);
}

/* The interrupt of the EL2 physical timer, PPI 26, to this EL2 */
static void tick_init(void)
{
    volatile u32 *gicd = (volatile u32 *)GICD_BASE;
    volatile u32 *gicr = (volatile u32 *)GICR_BASE;
    volatile u32 *sgi = (volatile u32 *)(GICR_BASE + 0x10000);

    msr(S3_4_C12_C9_5, 0xf); /* ICC_SRE_EL2: system registers */
    asm volatile("isb");
    gicd[0] = BIT(4) | BIT(1) | BIT(0); /* GICD_CTLR: ARE, both groups */
    gicr[0x14 / 4] &= ~BIT(1);          /* GICR_WAKER.ProcessorSleep */
    while (gicr[0x14 / 4] & BIT(2))
        ;
    sgi[0x80 / 4] |= BIT(26);  /* GICR_IGROUPR0: group 1 */
    *((volatile u8 *)sgi + 0x400 + 26) = 0x80;
    sgi[0x100 / 4] = BIT(26);  /* GICR_ISENABLER0 */
    msr(S3_0_C4_C6_0, 0xff);   /* ICC_PMR_EL1 */
    msr(S3_0_C12_C12_7, 1);    /* ICC_IGRPEN1_EL1 */
    asm volatile("isb");
    tick_arm();
}

static u64 el2_l1[512] __attribute__((aligned(4096)));

/*
 * The MMU of this EL2, every address mapped to itself: the hypervisor runs with its MMU on
 * (src/memory.c), and only then may it access memory of the guest at an address that is not
 * aligned, as it does when it carries out such an access of the guest.
 */
static void el2_mmu_on(void)
{
    /* Two blocks of 1 GB: devices (attribute 1, never executable), then memory (attribute 0) */
    el2_l1[0] = BIT(54) | BIT(10) | BIT(6) | (1UL << 2) | 1;
    el2_l1[1] = RAM_BASE | BIT(10) | (3UL << 8) | BIT(6) | 1;

    msr(mair_el2, 0x00ff);
    /* 39 bits of address from level 1, write-back walks, inner shareable, 40 bits of PA */
    msr(tcr_el2, 25 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 16) | BIT(23) | BIT(31));
    msr(ttbr0_el2, (u64)el2_l1);
    asm volatile("isb\ntlbi alle2\ndsb nsh\nisb");
    /* M, C and I, and nothing has to be aligned */
    msr(sctlr_el2, 0x30c50830UL | 0x1005);
    asm volatile("isb");
}

u64 shim_main(void)
{
    if ((u64)guest_end > PAGED_END) {
        shim_puts("SHIM> the guest does not fit the memory that has pages of its own\n");
        shim_stop();
    }
    el2_mmu_on();

    /* The guest writes characters to base + 0x20; the PL011 data register is at + 0 */
    *(u64 *)(guest + HDR_UART_BASE) = PL011_BASE - 0x20;
    *(u64 *)(guest + HDR_NR_CPUS) = 1;
    *(u64 *)(guest + HDR_ARG) = GUEST_ARG;

    /* 64 MB of memory, the first NR_L3 * 2 MB of it in pages, and the heap left out; the
     * devices below. No memory is executable: that is hv_pt_xtrack() */
    for (u64 i = 0; i < 32; i++) {
        u64 at = RAM_BASE + (i << 21);

        if (at < HEAP_BASE || at >= HEAP_END)
            s2_l2_ram[i] = at | S2_MEM | S2_RW | S2_XN | S2_BLOCK;
    }
    for (u64 i = 0; i < NR_L3 * 512; i++)
        s2_l3[i / 512][i % 512] = (RAM_BASE + i * PAGE) | S2_MEM | S2_RW | S2_XN | S2_PAGE;
    for (u64 i = 0; i < NR_L3; i++)
        s2_l2_ram[i] = (u64)s2_l3[i] | 3;
    for (u64 a = 0x08000000; a < 0x0a000000; a += 1 << 21)
        s2_l2_dev[a >> 21] = a | S2_DEV | S2_BLOCK;
    s2_l1[0] = (u64)s2_l2_dev | 3;
    s2_l1[1] = (u64)s2_l2_ram | 3;

    /* 39 bits of IPA from level 1, 4 KB granule, write-back, inner shareable, 40 bits of PA */
    msr(vtcr_el2, 25 | (1UL << 6) | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 16) | BIT(31));
    msr(vttbr_el2, (u64)s2_l1);
    /*
     * An AArch64 guest under stage 2; physical interrupts come to EL2 (IMO); data cache
     * maintenance to the point of coherency comes here (TPCP), see shim_dc_poc()
     */
    msr(hcr_el2, BIT(31) | BIT(23) | BIT(4) | BIT(0));
    msr(cnthctl_el2, 3);
    /* SIMD and floating point are the guest's, and get_simd_state() reads them here */
    msr(cptr_el2, 0x33ff);
    msr(cntvoff_el2, 0);
    msr(sctlr_el1, 0x30d00800);
    asm volatile("isb\ntlbi vmalls12e1is\ndsb ish\nisb");

    hv_pmu_init();
    hv_xtrack_init();
    /* What hv_pt_xtrack() asks for first */
    if (hv_xtrack_start() < 0)
        shim_stop();
    tick_init();
    return (u64)guest;
}
