// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Runs xotest.bin on QEMU's "virt" machine under an EL2 of its own, to check the guest and
 * the execute-only probe together before either goes near the M4: `make qemu-xo`.
 *
 * The probe is the real one: the Makefile drops the #include lines of src/hv/hv_xoprobe.c,
 * and this file stands in for the rest of the hypervisor. It has a stage 2 with 4 KB pages
 * for the memory the guest is in, and of the page states of src/hv/hv_vm.c the two the probe
 * moves between: a page is execute-only, or it is what everything else is here, readable,
 * writable and executable (which the probe is told is HV_XPAGE_SCANNED: there is no execution
 * tracking). What becomes of a data abort is decided as hv_xtrack_dabort() and
 * hv_handle_dabort() decide it; of carrying an access out in EL2 there is just the one
 * instruction that xotest uses for it.
 *
 * QEMU's processor is the architecture as QEMU reads it, not the M4. What this run shows is
 * that the guest and the probe agree with each other and with that reading.
 *
 * With XO_LEAKY (`make qemu-xo-leaky`) an execute-only page can be read all the same, which
 * shows that the checks of the guest can fail.
 */

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
/* bool, true and false are the compiler's own (C23) */
#define NULL ((void *)0)
#define BIT(n) (1UL << (n))

/* What lower_sync in xotest-qemu-head.S puts on the stack */
struct exc_info {
    u64 regs[31];
    u64 elr;
};

#include "hv_xpage.h" /* HV_XPAGE_*, HV_XTRACK_* and HV_XOPROBE_* of the real hv.h */

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

u64 shim_smc(u64 x0, u64 x1, u64 x2, u64 x3);
extern u8 guest[], guest_end[];

#define PL011_BASE 0x09000000UL
#define RAM_BASE   0x40000000UL
#define PAGE       0x1000UL
/* The memory that has pages of its own in stage 2: the first 4 MB, which the guest is in */
#define NR_L3     2
#define PAGED_END (RAM_BASE + NR_L3 * 512 * PAGE)

/* Offsets in the image, see sgitest-head.S */
#define HDR_UART_BASE 0x10
#define HDR_NR_CPUS   0x18

/* ---- output ---- */

static void shim_puts(const char *s)
{
    while (*s)
        *(volatile u32 *)PL011_BASE = *s++;
}

static void shim_hex(u64 v)
{
    for (int shift = 60; shift >= 0; shift -= 4)
        *(volatile u32 *)PL011_BASE = "0123456789abcdef"[(v >> shift) & 0xf];
}

static void shim_dec(s64 v)
{
    char buf[20];
    int n = 0;
    u64 u = v < 0 ? -v : v;

    if (v < 0)
        *(volatile u32 *)PL011_BASE = '-';
    do {
        buf[n++] = '0' + u % 10;
        u /= 10;
    } while (u);
    while (n)
        *(volatile u32 *)PL011_BASE = buf[--n];
}

/* What the probe prints: %ld, %d and %lx, and nothing else of printf */
static int printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    shim_puts("SHIM> ");
    for (; *fmt; fmt++) {
        if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 'd') {
            shim_dec(__builtin_va_arg(ap, s64));
            fmt += 2;
        } else if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 'x') {
            shim_hex(__builtin_va_arg(ap, u64));
            fmt += 2;
        } else if (fmt[0] == '%' && fmt[1] == 'd') {
            shim_dec(__builtin_va_arg(ap, int));
            fmt += 1;
        } else {
            *(volatile u32 *)PL011_BASE = *fmt;
        }
    }
    __builtin_va_end(ap);
    return 0;
}

/* ---- stage 2 ---- */

/* MemAttr write-back memory, S2AP read and write, inner shareable, AF; XN 0 */
#define S2_MEM   0x7fcUL
#define S2_BLOCK 1UL
#define S2_PAGE  3UL
/* S2AP 0b00 and XN 0, or with XO_LEAKY S2AP 0b01, read-only */
#ifdef XO_LEAKY
#define S2_XONLY 0x77cUL
#else
#define S2_XONLY 0x73cUL
#endif
/* MemAttr Device-nGnRE, S2AP read and write, AF, XN */
#define S2_DEV (0x4c4UL | BIT(54))
/* Left to software, as in src/hv/hv_vm.c */
#define SW_XONLY BIT(58)

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
    return *s2_pte(ipa) & SW_XONLY ? HV_XPAGE_XONLY : HV_XPAGE_SCANNED;
}

static int hv_pt_set_scanned(u64 ipa, int state)
{
    u64 pa;

    ipa &= ~(PAGE - 1);
    if (hv_pt_scanned(ipa, &pa) < 0)
        return -1;
    if (state == HV_XPAGE_XONLY)
        *s2_pte(ipa) = ipa | S2_XONLY | S2_PAGE | SW_XONLY;
    else
        *s2_pte(ipa) = ipa | S2_MEM | S2_PAGE;
    asm volatile("dsb ishst\ntlbi vmalls12e1is\ndsb ish\nisb");
    return 0;
}

static u64 hv_get_esr(void)
{
    return mrs(esr_el2);
}

static u64 hv_get_far(void)
{
    return mrs(far_el2);
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

#include "hv_xoprobe.inc"

/* ---- the hypervisor around the probe ---- */

static void __attribute__((noreturn)) shim_stop(void)
{
    shim_smc(0x84000008, 0, 0, 0); /* PSCI SYSTEM_OFF */
    for (;;)
        ;
}

void shim_bad(u64 vector)
{
    shim_puts("SHIM> unexpected exception, vector ESR ELR FAR = ");
    shim_hex(vector);
    shim_puts(" ");
    shim_hex(mrs(esr_el2));
    shim_puts(" ");
    shim_hex(mrs(elr_el2));
    shim_puts(" ");
    shim_hex(mrs(far_el2));
    shim_puts("\n");
    shim_stop();
}

void shim_sync(struct exc_info *ctx)
{
    u64 esr = mrs(esr_el2);
    u64 ec = esr >> 26;

    if (ec == 0x16) {
        /* hvc: the address to return to is behind the instruction */
        if (hv_xoprobe_hvc(ctx, esr & 0xffff))
            return;
        /* PSCI SYSTEM_OFF: what the hypervisor prints when the guest has ended */
        if ((u32)ctx->regs[0] == 0x84000008)
            hv_xoprobe_report();
        ctx->regs[0] = shim_smc(ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[3]);
        return;
    }

    if (ec == 0x24) {
        bool is_write = esr & BIT(6);
        u64 pa;
        u64 ipa = hv_translate(mrs(far_el2), true, is_write, NULL);
        int state = ipa ? hv_pt_scanned(ipa, &pa) : -1;

        /* A store into a page that takes stores cannot fault here; the rest is the probe's */
        if (ipa && (state == HV_XPAGE_XONLY || !is_write || state != HV_XPAGE_SCANNED)) {
            switch (hv_xoprobe_dabort(ctx, ipa, state, is_write)) {
                case HV_XTRACK_RETRY:
                    return;
                case HV_XTRACK_SKIP:
                    ctx->elr += 4;
                    return;
                case HV_XTRACK_EMULATE: {
                    u64 pc = hv_translate(ctx->elr, true, false, NULL);

                    /* ldr x0, [x9], and EL2 reads memory where it is */
                    if (pc && !is_write && *(u32 *)pc == 0xf9400120) {
                        ctx->regs[0] = *(u64 *)ipa;
                        ctx->elr += 4;
                        return;
                    }
                }
            }
        }
    }

    shim_bad(0x100);
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

    /* 64 MB of memory in 2 MB blocks, the first NR_L3 of them in pages; the devices below */
    for (u64 i = 0; i < 32; i++)
        s2_l2_ram[i] = (RAM_BASE + (i << 21)) | S2_MEM | S2_BLOCK;
    for (u64 i = 0; i < NR_L3 * 512; i++)
        s2_l3[i / 512][i % 512] = (RAM_BASE + i * PAGE) | S2_MEM | S2_PAGE;
    for (u64 i = 0; i < NR_L3; i++)
        s2_l2_ram[i] = (u64)s2_l3[i] | 3;
    for (u64 a = 0x08000000; a < 0x0a000000; a += 1 << 21)
        s2_l2_dev[a >> 21] = a | S2_DEV | S2_BLOCK;
    s2_l1[0] = (u64)s2_l2_dev | 3;
    s2_l1[1] = (u64)s2_l2_ram | 3;

    /* 39 bits of IPA from level 1, 4 KB granule, write-back, inner shareable, 40 bits of PA */
    msr(vtcr_el2, 25 | (1UL << 6) | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 16) | BIT(31));
    msr(vttbr_el2, (u64)s2_l1);
    /* An AArch64 guest under stage 2, and nothing trapped */
    msr(hcr_el2, BIT(31) | BIT(0));
    msr(cnthctl_el2, 3);
    msr(cntvoff_el2, 0);
    msr(sctlr_el1, 0x30d00800);
    asm volatile("isb\ntlbi vmalls12e1is\ndsb ish\nisb");

    hv_xoprobe_init();
    hv_xoprobe_enable();
    return (u64)guest;
}
