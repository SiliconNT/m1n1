/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for the stage 2 page tables of src/hv/hv_vm.c.
 *
 * The Makefile cuts the page table half of that file, everything before the MMIO emulation,
 * into build/hv_vm_pt.inc and drops its #include lines; this file stands in for those and
 * includes the rest, so that the real code builds the tables in the memory of this process.
 *
 * Two things look at the tables it builds. s2_walk() below is the hardware: a stage 2 walk
 * with the 4 KB granule written from the specification (Arm DDI 0487 M.d, D8.2.2, D8.2.8
 * tables D8-16, D8-17, D8-19 and D8-22, D8.3 table D8-53), sharing no constant with
 * hv_vm.c. hv_pt_walk() is what the hypervisor itself reads back. Both are compared, address
 * by address, with a list of what was asked for (expect()).
 *
 * The mappings are those of the M4 MacBook Pro as the firmware loader makes them: the list
 * the host printed in run R19 of the SiliconNT project. 60 GB of it is device registers and
 * 15.6 GB is memory, which matters for how much the tables take once execution is tracked.
 */

#include <assert.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "hv.h"           /* the stand-in: types, struct exc_info, hv_hook_t */
#include "arm_cpu_regs.h" /* the real one, copied to build/ */
#include "hv_xpage.h"     /* HV_XPAGE_* of the real hv.h, see the Makefile */

/* ---- what the #include lines of hv_vm.c would have brought ---------------------------- */

/* src/utils.h, word for word */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define min(a, b)        (((a) < (b)) ? (a) : (b))
#define max(a, b)        (((a) > (b)) ? (a) : (b))
#define dprintf(...)                                                                               \
    do {                                                                                           \
    } while (0)

static u64 reg_mmfr0, reg_vtcr, reg_vttbr;
static unsigned long tlb_flushes;

static u64 mock_mrs(const char *reg)
{
    if (!strcmp(reg, "ID_AA64MMFR0_EL1"))
        return reg_mmfr0;
    return 0;
}

static void mock_msr(const char *reg, u64 val)
{
    if (!strcmp(reg, "VTCR_EL2"))
        reg_vtcr = val;
    else if (!strcmp(reg, "VTTBR_EL2"))
        reg_vttbr = val;
}

static void mock_sysop(const char *op)
{
    if (!strncmp(op, "tlbi", 4))
        tlb_flushes++;
}

#define mrs(reg)      mock_mrs(#reg)
#define msr(reg, val) mock_msr(#reg, (u64)(val))
#define sysop(op)     mock_sysop(op)
#define asm(...)      ((void)0) /* the AT instructions of hv_translate(), which nothing here calls */

static u64 hv_get_spsr(void)
{
    return 0;
}

static void memset64(void *dst, u64 value, size_t size)
{
    u64 *p = dst;
    for (size_t i = 0; i < size / 8; i++)
        p[i] = value;
}

/* The heap of m1n1 is small: count what the tables take */
#define NR_SLOTS  (1u << 18)
#define TOMBSTONE ((void *)1)
static struct {
    void *p;
    size_t size;
} slots[NR_SLOTS];
static size_t live_bytes;

static void *mock_memalign(size_t align, size_t size)
{
    void *p = memalign(align, size);
    assert(p && !((u64)p & (align - 1)));

    unsigned i = ((u64)p >> 12) % NR_SLOTS;
    while (slots[i].p && slots[i].p != TOMBSTONE)
        i = (i + 1) % NR_SLOTS;
    slots[i].p = p;
    slots[i].size = size;
    live_bytes += size;
    return p;
}

static void mock_free(void *p)
{
    if (!p)
        return;

    unsigned i = ((u64)p >> 12) % NR_SLOTS;
    while (slots[i].p != p) {
        assert(slots[i].p); /* freed something that was never allocated */
        i = (i + 1) % NR_SLOTS;
    }
    slots[i].p = TOMBSTONE;
    live_bytes -= slots[i].size;
    free(p);
}

#define memalign(a, s) mock_memalign(a, s)
#define free(p)        mock_free(p)

uint64_t ram_base;
static u64 m1n1_base;
#define _base ((char *)m1n1_base)

u64 hv_pt_walk(u64 addr);

#include "hv_vm_pt.inc"

/* ---- checks --------------------------------------------------------------------------- */

static unsigned long checks, failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    if (failures++ < 25) {
        printf("FAIL: ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

/* ---- the hardware: a stage 2 walk with the 4 KB granule, from the specification -------- */

struct s2 {
    bool valid;
    int level;
    u64 pa;
    u64 attr; /* descriptor bits 11:2 */
    unsigned xn;
};

static struct s2 s2_walk(u64 ipa)
{
    struct s2 r = {false, 0, 0, 0, 0};
    unsigned t0sz = reg_vtcr & 0x3f;

    /* TG0 = 0b00: 4 KB. SL0 = 0b01: start at level 1, which takes T0SZ 21 to 33 (D8-19, D8-22) */
    if (((reg_vtcr >> 14) & 3) != 0 || ((reg_vtcr >> 6) & 3) != 1 || t0sz < 21 || t0sz > 33)
        return r;
    if (ipa >> (64 - t0sz))
        return r;

    /* Up to 16 tables side by side at level 1: the index is every IA bit from 30 up */
    u64 *table = (u64 *)reg_vttbr;
    u64 idx = ipa >> 30;

    for (int level = 1; level <= 3; level++) {
        unsigned shift = 12 + 9 * (3 - level);
        u64 d = table[idx];

        r.level = level;
        if (!(d & 1))
            return r;
        if (level < 3 && (d & 2)) {
            table = (u64 *)(d & GENMASK(47, 12));
            idx = (ipa >> (shift - 9)) & 0x1ff;
            continue;
        }
        if (level == 3 && !(d & 2))
            return r; /* a reserved encoding */
        if (d & GENMASK(shift - 1, 12))
            return r; /* address bits a block does not have */

        r.valid = true;
        r.pa = (d & GENMASK(47, shift)) | (ipa & (BIT(shift) - 1));
        r.attr = d & GENMASK(11, 2);
        r.xn = (d >> 53) & 3;
        return r;
    }
    return r;
}

/* ---- what was asked for ---------------------------------------------------------------- */

enum kind { K_NONE, K_HW, K_SW, K_HOOK };

struct op {
    u64 start, end;
    enum kind kind;
    u64 to;    /* where start goes, or the hook */
    int state; /* K_HW: HV_XPAGE_* */
};

#define MAX_OPS 256
static struct op ops[MAX_OPS];
static unsigned nr_ops;
static bool xtrack;

static void op_add(u64 start, u64 size, enum kind kind, u64 to, int state)
{
    assert(nr_ops < MAX_OPS);
    ops[nr_ops++] = (struct op){start, start + size, kind, to, state};
}

struct want {
    enum kind kind;
    u64 pa;
    int state;
};

static struct want expect(u64 ipa)
{
    struct want w = {K_NONE, 0, 0};
    u64 page = ipa & ~0xfffUL;

    for (unsigned i = 0; i < nr_ops; i++) {
        struct op *o = &ops[i];

        if (o->end <= page || o->start >= page + 0x1000)
            continue;
        /*
         * A mapping of part of a page takes the page apart into words. What was memory
         * before is from then on read and written by the hypervisor, word by word.
         */
        if ((o->start > page || o->end < page + 0x1000) && w.kind == K_HW)
            w.kind = K_SW;
        if (ipa < o->start || ipa >= o->end)
            continue;
        w.kind = o->kind;
        w.state = o->state;
        w.pa = o->kind == K_HOOK ? o->to : o->kind == K_NONE ? 0 : o->to + (ipa - o->start);
    }
    return w;
}

static u64 ia_top; /* the first address the guest does not have */

static void verify(u64 ipa)
{
    struct want w = expect(ipa);
    struct s2 hw = s2_walk(ipa);

    /* hv_pt_walk() is not to be asked about addresses past the end; its callers check */
    if (ipa >= ia_top) {
        check(!hw.valid && !hv_pt_is_ram(ipa), "0x%lx: mapped, past the end of the address space",
              ipa);
        return;
    }

    u64 sw = hv_pt_walk(ipa);

    switch (w.kind) {
        case K_HW: {
            unsigned xn = (xtrack && w.state == HV_XPAGE_UNSCANNED) ? 2 : 0;
            u64 ap = w.state == HV_XPAGE_SCANNED ? 1 : 3;
            /* MemAttr 0b1111, S2AP, SH 0b11, AF */
            u64 attr = (0xfUL << 2) | (ap << 6) | (3UL << 8) | BIT(10);

            check(hw.valid && hw.pa == w.pa, "0x%lx: the hardware gets to 0x%lx (valid %d), not to 0x%lx",
                  ipa, hw.pa, hw.valid, w.pa);
            check(hw.attr == attr && hw.xn == xn,
                  "0x%lx: attributes 0x%lx and XN %d, not 0x%lx and %d (page state %d)", ipa,
                  hw.attr, hw.xn, attr, xn, w.state);
            if (xtrack && ipa >= ram_base)
                check(hw.level == 3, "0x%lx: memory mapped at level %d while execution is tracked",
                      ipa, hw.level);
            check((sw & 1) && (sw & GENMASK(47, 12)) == (w.pa & ~0xfffUL) &&
                      (sw & 0xffc) == (ipa & 0xffc),
                  "0x%lx: hv_pt_walk() says 0x%lx, memory at 0x%lx was expected", ipa, sw, w.pa);
            break;
        }
        case K_SW:
            check(!hw.valid, "0x%lx: the hardware gets to 0x%lx, a software mapping was expected",
                  ipa, hw.pa);
            check(sw && !(sw & 1) && ((sw >> 50) & 7) == 0 &&
                      (sw & GENMASK(49, 2)) == (w.pa & ~3UL),
                  "0x%lx: hv_pt_walk() says 0x%lx, a software mapping to 0x%lx was expected", ipa,
                  sw, w.pa);
            break;
        case K_HOOK:
            check(!hw.valid, "0x%lx: the hardware gets to 0x%lx, a hook was expected", ipa, hw.pa);
            check(!(sw & 1) && ((sw >> 50) & 7) == 1 && (sw & GENMASK(49, 2)) == w.pa,
                  "0x%lx: hv_pt_walk() says 0x%lx, the hook 0x%lx was expected", ipa, sw, w.pa);
            break;
        case K_NONE:
            check(!hw.valid && sw == 0, "0x%lx: mapped (hardware valid %d, hv_pt_walk() 0x%lx)",
                  ipa, hw.valid, sw);
            break;
    }
}

static u64 rnd_state = 0x9e3779b97f4a7c15UL;

static u64 rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

static void verify_range(u64 start, u64 end)
{
    u64 a;

    /* Just outside, and every word of the 16 KB at either end */
    verify(start - 4);
    verify(end);
    for (a = start; a < end && a < start + 0x4000; a += 4)
        verify(a);
    for (a = end - start > 0x4000 ? end - 0x4000 : start; a < end; a += 4)
        verify(a);

    if (end - start <= (64UL << 20)) {
        /* Every page, at some word of it */
        for (a = start & ~0xfffUL; a + 0x1000 <= end; a += 0x1000)
            if (a >= start)
                verify(a + (rnd() & 0xffc));
        return;
    }

    /* Either side of every 2 MB boundary, and words anywhere */
    for (a = (start + 0x1fffff) & ~0x1fffffUL; a < end; a += 0x200000) {
        verify(a);
        if (a - 0x1000 >= start)
            verify(a - 0x1000 + (rnd() & 0xffc));
    }
    for (int i = 0; i < 4096; i++)
        verify(start + ((rnd() % (end - start)) & ~3UL));
}

static void verify_all(void)
{
    for (unsigned i = 0; i < nr_ops; i++)
        verify_range(ops[i].start, ops[i].end);
}

/* ---- the calls, each noted in the list -------------------------------------------------- */

static void t_map_hw(u64 ipa, u64 pa, u64 size)
{
    check(hv_map_hw(ipa, pa, size) == 0, "hv_map_hw(0x%lx, 0x%lx, 0x%lx) failed", ipa, pa, size);
    op_add(ipa, size, K_HW, pa, HV_XPAGE_UNSCANNED);
}

static void t_map_sw(u64 ipa, u64 pa, u64 size)
{
    check(hv_map_sw(ipa, pa, size) == 0, "hv_map_sw(0x%lx, 0x%lx, 0x%lx) failed", ipa, pa, size);
    op_add(ipa, size, K_SW, pa, 0);
}

static void t_hook(u64 ipa, u64 size, u64 hook)
{
    check(hv_map_hook(ipa, (hv_hook_t *)hook, size) == 0, "hv_map_hook(0x%lx, 0x%lx) failed", ipa,
          size);
    op_add(ipa, size, K_HOOK, hook, 0);
}

static void t_unmap(u64 ipa, u64 size)
{
    check(hv_unmap(ipa, size) == 0, "hv_unmap(0x%lx, 0x%lx) failed", ipa, size);
    op_add(ipa, size, K_NONE, 0, 0);
}

/* HV.map_hw() of proxyclient/m1n1/hv/__init__.py: the host asks in units of 16 KB */
static void py_map_hw(u64 ipa, u64 pa, u64 size)
{
    if ((ipa & 0x3fff) != (pa & 0x3fff)) {
        t_map_sw(ipa, pa, size);
        return;
    }

    u64 ipa_p = ALIGN_UP(ipa, 0x4000UL);
    if (ipa_p != ipa) {
        t_map_sw(ipa, pa, min(ipa_p - ipa, size));
        if (size <= ipa_p - ipa)
            return;
        pa += ipa_p - ipa;
        size -= ipa_p - ipa;
    }

    u64 size_p = ALIGN_DOWN(size, 0x4000UL);
    if (size_p)
        t_map_hw(ipa_p, pa, size_p);
    if (size_p != size)
        t_map_sw(ipa_p + size_p, pa + size_p, size - size_p);
}

static void t_set_scanned(u64 ipa, int state)
{
    u64 page = ipa & ~0xfffUL, pa = 0;
    unsigned long flushes = tlb_flushes;

    check(hv_pt_set_scanned(ipa, state) == 0, "hv_pt_set_scanned(0x%lx, %d) failed", ipa, state);
    op_add(page, 0x1000, K_HW, page, state);
    check(tlb_flushes > flushes, "0x%lx: page state %d without a TLB invalidation", ipa, state);
    check(hv_pt_scanned(page + 0xff8, &pa) == state && pa == page,
          "0x%lx: hv_pt_scanned() does not say state %d at 0x%lx (pa 0x%lx)", ipa, state, page, pa);

    /* The page, and the 16 KB to either side of it */
    for (u64 a = page - 0x4000; a < page + 0x5000; a += 0x1000) {
        verify(a);
        verify(a + (rnd() & 0xffc));
    }
}

static void init(u64 mmfr0, u64 ram, u64 m1n1, u64 top)
{
    reg_mmfr0 = mmfr0;
    reg_vtcr = reg_vttbr = 0;
    ram_base = ram;
    m1n1_base = m1n1;
    ia_top = top;
    nr_ops = 0;
    xtrack = false;
    hv_pt_init();
}

static void start_xtrack(void)
{
    unsigned long flushes = tlb_flushes;

    check(hv_pt_xtrack() == 0 && hv_pt_xtrack_on(), "hv_pt_xtrack() failed");
    check(tlb_flushes > flushes, "execution tracking starts without a TLB invalidation");
    xtrack = true;
}

/* What hv_pt_init() has to leave in VTCR_EL2 and VTTBR_EL2 for a guest with ia_bits of IPA */
static void check_config(unsigned pa_range, unsigned ia_bits)
{
    unsigned t0sz = reg_vtcr & 0x3f;
    u64 entries = 1UL << (ia_bits - 30), bytes = entries * 8;

    check(((reg_vtcr >> 14) & 3) == 0,
          "VTCR_EL2.TG0 is %ld: the granule of stage 2 is not 4 KB (VTCR_EL2 0x%lx)",
          (reg_vtcr >> 14) & 3, reg_vtcr);
    check(((reg_vtcr >> 6) & 3) == 1, "VTCR_EL2.SL0 is %ld: the walk does not start at level 1",
          (reg_vtcr >> 6) & 3);
    check(t0sz == 64 - ia_bits && t0sz >= 21 && t0sz <= 33,
          "VTCR_EL2.T0SZ is %d, %d was expected: a walk from level 1 takes 21 to 33", t0sz,
          64 - ia_bits);
    check(((reg_vtcr >> 16) & 7) == pa_range, "VTCR_EL2.PS is %ld, not %d", (reg_vtcr >> 16) & 7,
          pa_range);
    check(!(reg_vtcr & (BIT(32) | BIT(33))), "VTCR_EL2.DS or SL2 is set");
    check(vaddr_bits == ia_bits, "vaddr_bits is %ld, not %d", vaddr_bits, ia_bits);

    /* Side by side, the tables of the first level are aligned to the size of all of them */
    if (bytes < 0x1000)
        bytes = 0x1000;
    check(reg_vttbr && !(reg_vttbr & (bytes - 1)),
          "VTTBR_EL2 is 0x%lx: not aligned to the 0x%lx bytes of the first level", reg_vttbr, bytes);
    if (!reg_vttbr || ((reg_vtcr >> 14) & 3) != 0)
        return;

    u64 used = 0;
    for (u64 i = 0; i < entries; i++)
        used |= ((u64 *)reg_vttbr)[i];
    check(used == 0, "the first level does not start out empty");
}

#define M4_MMFR0 0x110012120f100023UL /* ID_AA64MMFR0_EL1 of the M4: PARange 3, 42 bits */
#define M4_RAM   0x10000000000UL
#define M4_M1N1  0x10004a60000UL
#define HOOK(n)  (0x70000000UL + (n) * 0x100)

static void test_m4(void)
{
    static const u64 mmio[][2] = {
        {0x200000000, 0x5a0000000},   {0x800000000, 0xae0000000},   {0xb80000000, 0xbe0000000},
        {0xc00000000, 0xe80000000},   {0x1000000000, 0x1280000000}, {0x1400000000, 0x1680000000},
        {0x1c50000000, 0x1c90000000}, {0x1cb0000000, 0x1cc0000000},
    };
    static const u64 ram[][2] = {
        {0x10000000000, 0x10002e70000},
        {0x10017e80000, 0x103db610000},
        {0x103dd680000, 0x103e6da4000},
        {0x103ffda4000, 0x10400000000},
    };
    /* The registers the host and the hypervisor answer for themselves, inside the first range */
    static const u64 hooks[][2] = {
        {0x211050000, 4}, {0x211050100, 8}, {0x23d29c044, 4},    {0x23d29c05c, 4},
        {0x380700000, 4}, {0x3807002d8, 4}, {0x380700378, 4},    {0x3807003d8, 4},
        {0x380734000, 0x20}, {0x3882800b0, 4}, {0x3882800c0, 4}, {0x3ad200000, 0x1000},
    };
    u64 pa = 0;

    printf("the M4, 42 bits of guest physical address\n");
    init(M4_MMFR0, M4_RAM, M4_M1N1, 1UL << 42);
    check_config(3, 42);

    for (unsigned i = 0; i < sizeof(mmio) / sizeof(mmio[0]); i++)
        py_map_hw(mmio[i][0], mmio[i][0], mmio[i][1] - mmio[i][0]);
    for (unsigned i = 0; i < sizeof(ram) / sizeof(ram[0]); i++)
        py_map_hw(ram[i][0], ram[i][0], ram[i][1] - ram[i][0]);
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        t_hook(hooks[i][0], hooks[i][1], HOOK(i));
    /* The emulated GIC: Distributor, and the Redistributors of ten cores */
    t_hook(0x5000000000, 0x10000, HOOK(20));
    t_hook(0x5100000000, 10 * 0x20000, HOOK(21));

    /* Ends that are not on a 16 KB boundary; a range that goes somewhere else */
    py_map_hw(0x600000060, 0x600000060, 0x23fa4);
    py_map_hw(0x700000000, 0x710001000, 0x8000);
    /* Memory in units of 4 KB, which is all the granule asks for, and not in less */
    t_map_hw(0x720001000, 0x720001000, 0x3000);
    check(hv_map_hw(0x730000800, 0x730000800, 0x1000) < 0, "memory mapped at half a page");
    verify(0x730000800);

    verify_all();
    check(hv_pt_is_ram(0x10100000000) && !hv_pt_is_ram(0x5000000000) &&
              !hv_pt_is_ram(0x103dc000000) && !hv_pt_is_ram(1UL << 42),
          "hv_pt_is_ram() is wrong about memory, a hook, a hole or the end of the address space");

    /* Execution tracking: memory page by page, and the tables still fit the heap of m1n1 */
    start_xtrack();
    verify_all();
    printf("  page tables while execution is tracked: %zu MiB\n", live_bytes >> 20);
    check(live_bytes < (48UL << 20),
          "the page tables take %zu MiB of a heap of 128 MiB: device registers taken apart "
          "into pages?",
          live_bytes >> 20);
    check(s2_walk(0x1000000000).level == 2 && s2_walk(0x1000000000).xn == 2,
          "device registers are not left as a block that cannot be executed");

    /* A page that is scanned, opened, written to again: that page and no other */
    t_set_scanned(0x10100005123, HV_XPAGE_SCANNED);
    t_set_scanned(0x10100005000, HV_XPAGE_OPEN);
    t_set_scanned(0x10100005ffc, HV_XPAGE_UNSCANNED);
    t_set_scanned(0x10100005000, HV_XPAGE_SCANNED);
    t_set_scanned(0x10100006000, HV_XPAGE_SCANNED);
    /* The last page of 2 MB and the first of the next; below m1n1, where there never were blocks */
    t_set_scanned(0x101001ff000, HV_XPAGE_SCANNED);
    t_set_scanned(0x10100200000, HV_XPAGE_OPEN);
    t_set_scanned(0x10000004000, HV_XPAGE_SCANNED);
    t_set_scanned(0x10000004000, HV_XPAGE_UNSCANNED);

    /* What is not memory has no state, and gets none */
    check(hv_pt_scanned(0x5000000000, &pa) < 0 && hv_pt_scanned(0x103dc000000, &pa) < 0 &&
              hv_pt_scanned(1UL << 42, &pa) < 0 && hv_pt_scanned(0x1000000000, &pa) < 0,
          "hv_pt_scanned() finds memory in a hook, a hole, past the end or in device registers");
    check(hv_pt_set_scanned(0x1000000000, HV_XPAGE_SCANNED) < 0 &&
              hv_pt_set_scanned(0x5000000000, HV_XPAGE_SCANNED) < 0,
          "hv_pt_set_scanned() takes device registers or a hook for memory");
    verify_range(0x1000000000, 0x1000400000);

    /* Memory mapped from now on cannot be executed either */
    t_map_hw(0x103dc000000, 0x103dc000000, 0x8000);
    /* Taking mappings away: whole pages, and two words of a page */
    t_unmap(0x1c50200000, 0x400000);
    t_unmap(0x1cb0001004, 8);
    verify_all();

    /* One mapping over what has been taken apart: the tables underneath are given back */
    size_t before = live_bytes;
    t_map_hw(0x1c50000000, 0x1c50000000, 0x40000000);
    t_map_hw(0x1cb0000000, 0x1cb0000000, 0x10000000);
    check(live_bytes < before, "tables under a new mapping are not freed (%zu, then %zu bytes)",
          before, live_bytes);
    verify_all();
}

/* Other sizes of physical address: PARange, bits, bits of guest physical address */
static void test_sizes(void)
{
    static const unsigned sizes[][3] = {{0, 32, 32}, {1, 36, 36}, {2, 40, 40},
                                        {3, 42, 42}, {4, 44, 43}, {5, 48, 43}};

    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned bits = sizes[i][2];
        u64 top = 1UL << bits, ram = top - (1UL << 30), pa = 0;

        printf("PARange %d: %d bits, %d bits of guest physical address\n", sizes[i][0],
               sizes[i][1], bits);
        init((M4_MMFR0 & ~0xfUL) | sizes[i][0], ram, ram + 0x800000, top);
        check_config(sizes[i][0], bits);

        py_map_hw(0x20000000, 0x20000000, 0x10000000);
        py_map_hw(ram, ram, top - ram);
        t_hook(0x20000100, 8, HOOK(1));
        t_hook(0x28000000, 0x1000, HOOK(2));
        verify_all();

        start_xtrack();
        verify_all();
        t_set_scanned(top - 0x1000, HV_XPAGE_SCANNED);
        t_set_scanned(ram + 0x20000000, HV_XPAGE_SCANNED);
        check(hv_pt_scanned(top, &pa) < 0 && !hv_pt_is_ram(top),
              "memory past the end of the address space");
        verify_all();
    }
}

int main(void)
{
    test_m4();
    test_sizes();

    printf("pt-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
