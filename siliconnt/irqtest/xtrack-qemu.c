// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Runs a test guest (pmutest.bin) on QEMU's "virt" machine under an EL2 of its own, to check
 * the guest together with the execution tracking of the hypervisor before either goes near
 * the M4: `make qemu-pmu-el2` and `make qemu-pmu-el2-nosyn`.
 *
 * src/hv/hv_xtrack.c, src/hv/hv_xinsn.c and src/hv/hv_pmu.c are the real ones: the Makefile
 * drops their #include lines, and this file stands in for the rest of the hypervisor. It has
 *
 *  - a stage 2 with 4 KB pages for the memory the guest is in, with the page states of
 *    src/hv/hv_vm.c: data, code, execute-only, and no access at all;
 *  - what hv_exc_sync() and hv_handle_dabort() do with an instruction abort, a data abort
 *    and an hvc;
 *  - an interrupt of its own every millisecond, as the hypervisor has.
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

/* Offsets in the image, see sgitest-head.S and sgitest.h (MAX_CPUS there is 4) */
#define HDR_UART_BASE 0x10
#define HDR_NR_CPUS   0x18
#define HDR_ARG       0x78
#define ARG_STRICT    0x100

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

/* What the two files print: %d, %x, %ld, %lx, a width with a leading zero, and %s */
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
/* MemAttr Device-nGnRE, S2AP read and write, AF, XN */
#define S2_DEV (0x4c4UL | BIT(54))
/* Left to software, as in src/hv/hv_vm.c */
#define SW_SCANNED BIT(56)
#define SW_NONE    BIT(57)
#define SW_XONLY   BIT(58)

static u64 s2_l1[512] __attribute__((aligned(4096)));
static u64 s2_l2_dev[512] __attribute__((aligned(4096)));
static u64 s2_l2_ram[512] __attribute__((aligned(4096)));
static u64 s2_l3[NR_L3][512] __attribute__((aligned(4096)));

static u64 *s2_pte(u64 ipa)
{
    u64 n = (ipa - RAM_BASE) / PAGE;

    return &s2_l3[n / 512][n % 512];
}

static bool hv_pt_xtrack_on(void)
{
    return true;
}

static int hv_pt_scanned(u64 ipa, u64 *pa)
{
    ipa &= ~(PAGE - 1);
    if (ipa < RAM_BASE || ipa >= PAGED_END)
        return -1;
    *pa = ipa;

    u64 pte = *s2_pte(ipa);
    if (pte & SW_XONLY)
        return HV_XPAGE_XONLY;
    if (pte & SW_NONE)
        return HV_XPAGE_NONE;
    return pte & SW_SCANNED ? HV_XPAGE_SCANNED : HV_XPAGE_UNSCANNED;
}

static int hv_pt_set_scanned(u64 ipa, int state)
{
    u64 pa;

    ipa &= ~(PAGE - 1);
    if (hv_pt_scanned(ipa, &pa) < 0)
        return -1;

    u64 attrs = S2_RW | S2_XN;
    if (state == HV_XPAGE_SCANNED)
        attrs = S2_RO | SW_SCANNED;
    else if (state == HV_XPAGE_NONE)
        attrs = S2_XN | SW_NONE;
    else if (state == HV_XPAGE_XONLY)
        attrs = SW_SCANNED | SW_XONLY;
    *s2_pte(ipa) = ipa | S2_MEM | attrs | S2_PAGE;
    asm volatile("dsb ishst\ntlbi vmalls12e1is\ndsb ish\nisb");
    return 0;
}

static u64 hv_get_esr(void)
{
#ifdef NO_SYNDROME
    /* A data abort (EC 0b10010x) without ISV: the instruction has to say what the access is */
    if ((mrs(esr_el2) >> 27) == 0x12)
        return mrs(esr_el2) & ~BIT(24);
#endif
    return mrs(esr_el2);
}

static u64 hv_get_far(void)
{
    return mrs(far_el2);
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

static void dc_cvau_range(void *addr, u64 size)
{
    for (u64 p = (u64)addr & ~63UL; p < (u64)addr + size; p += 64)
        asm volatile("dc cvau, %0" : : "r"(p));
}

static void ic_ialluis(void)
{
    asm volatile("ic ialluis");
}

static void dc_civac_range(void *addr, u64 size)
{
    for (u64 p = (u64)addr & ~63UL; p < (u64)addr + size; p += 64)
        asm volatile("dc civac, %0" : : "r"(p));
}

/* Where EL2 finds the instruction at this address of the guest: memory is where it says it is */
static u64 hv_translate_insn(u64 addr)
{
    u64 ipa = hv_translate(addr, true, false, NULL);

    if (ipa < RAM_BASE || ipa >= PAGED_END)
        return 0;
    return ipa;
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

/* This EL2 is built for the tiny code model, all of it within 1 MB: a smaller table than the real one */
#define XT_MAX_PAGES 1024

#include "hv_xinsn_h.inc"
#include "hv_pmu.inc"
#include "hv_xinsn.inc"
#include "hv_xtrack.inc"

/* ---- the hypervisor around them ---- */

static u64 nr_ticks;

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

/* A data abort: what hv_handle_dabort() does with one on memory */
static bool shim_dabort(struct exc_info *ctx)
{
    u64 esr = mrs(esr_el2);
    bool is_write = esr & BIT(6);
    u64 ipa = hv_translate(mrs(far_el2), true, is_write, NULL);

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

    if (ec == 0x16) {
        /* hvc: the address to return to is behind the instruction */
        if (esr & BIT(15)) {
            ok = hv_pmu_hvc(&ctx, esr & 0xffff);
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
    }

    if (!ok)
        shim_bad(0x100);

    for (int i = 0; i < 31; i++)
        f->regs[i] = ctx.regs[i];
    f->elr = ctx.elr;
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

u64 shim_main(void)
{
    if ((u64)guest_end > PAGED_END) {
        shim_puts("SHIM> the guest does not fit the memory that has pages of its own\n");
        shim_stop();
    }

    /* The guest writes characters to base + 0x20; the PL011 data register is at + 0 */
    *(u64 *)(guest + HDR_UART_BASE) = PL011_BASE - 0x20;
    *(u64 *)(guest + HDR_NR_CPUS) = 1;
    *(u64 *)(guest + HDR_ARG) = ARG_STRICT;

    /* 64 MB of memory, the first NR_L3 * 2 MB of it in pages; the devices below. No memory is
     * executable: that is hv_pt_xtrack() */
    for (u64 i = 0; i < 32; i++)
        s2_l2_ram[i] = (RAM_BASE + (i << 21)) | S2_MEM | S2_RW | S2_XN | S2_BLOCK;
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
    /* An AArch64 guest under stage 2; physical interrupts come to EL2 (IMO) */
    msr(hcr_el2, BIT(31) | BIT(4) | BIT(0));
    msr(cnthctl_el2, 3);
    msr(cntvoff_el2, 0);
    msr(sctlr_el1, 0x30d00800);
    asm volatile("isb\ntlbi vmalls12e1is\ndsb ish\nisb");

    hv_pmu_init();
    hv_xtrack_init();
    tick_init();
    return (u64)guest;
}
