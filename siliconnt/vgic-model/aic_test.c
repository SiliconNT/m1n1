// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side test for aic_mask(), aic_unmask() and aic_mask_all() of src/aic.c: which
 * registers of AIC they write, and what. The real file is compiled for the build machine
 * against the stand-in headers of aic/ (write32() writes down what is written); the
 * structure that aic_init() would fill in is made here, for each version:
 *
 *   - AIC3 of the M4: what aic23_init() makes of its device tree (SiliconNT docs/findings/0058,
 *     evidence 1: extint-baseaddress 0x10000, the three strides 0x4a00) and of what m1n1
 *     printed there ("AIC3 with 1/2 dies, 2048/4096 IRQs", findings/0011); and the same with
 *     two dies.
 *   - An AIC2 with two dies, numbers made up, laid out the way aic23_init() lays it out.
 *   - AIC1, whose registers are at fixed places (src/aic_regs.h), with a number of lines that
 *     is not a multiple of 32.
 *
 * The offsets expected are written out, not worked out again the way aic.c works them out.
 */
#include <stdlib.h>
#include <string.h>

#include "aic.h"
#include "utils.h"

/* ---- what aic.c writes ---- */

static struct {
    u64 addr;
    u32 val;
} writes[1024];
static int nr_writes;

void write32(u64 addr, u32 val)
{
    if (nr_writes == sizeof(writes) / sizeof(writes[0])) {
        printf("CHECK FAILED: more writes than there is room for\n");
        exit(1);
    }
    writes[nr_writes].addr = addr;
    writes[nr_writes++].val = val;
}

u32 read32(u64 addr)
{
    printf("CHECK FAILED: read32(0x%lx): the functions under test read nothing\n", addr);
    exit(1);
}

u32 mask32(u64 addr, u32 clear, u32 set)
{
    printf("CHECK FAILED: mask32(0x%lx, 0x%x, 0x%x)\n", addr, clear, set);
    exit(1);
}

/* aic_init() is not called here: what it reads, it does not get */
void *adt;

int adt_path_offset_trace(const void *dt, const char *path, int *offsets)
{
    exit(2);
}

bool adt_is_compatible(const void *dt, int nodeoffset, const char *compat)
{
    exit(2);
}

int adt_get_reg(const void *dt, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    exit(2);
}

const void *adt_getprop(const void *dt, int nodeoffset, const char *name, u32 *lenp)
{
    exit(2);
}

int adt_getprop_copy(const void *dt, int nodeoffset, const char *name, void *out, size_t len)
{
    exit(2);
}

/* ---- checks ---- */

static int checks, failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
        }                                                                                          \
    } while (0)

/* Exactly one write, of this value to this register */
static bool wrote_once(u64 addr, u32 val)
{
    return nr_writes == 1 && writes[0].addr == addr && writes[0].val == val;
}

/* Was this written, and how often */
static int written(u64 addr, u32 val)
{
    int n = 0;

    for (int i = 0; i < nr_writes; i++)
        n += writes[i].addr == addr && writes[i].val == val;
    return n;
}

static void mask(u32 irq)
{
    nr_writes = 0;
    aic_mask(irq);
}

static void unmask(u32 irq)
{
    nr_writes = 0;
    aic_unmask(irq);
}

/*
 * aic_mask_all(): every word of every die, masked and with its software triggers cleared,
 * each once, nothing else. mask_set, sw_clr: where die 0 has them; stride: from one die to
 * the next, the same for both here
 */
static void check_mask_all(u64 mask_set, u64 sw_clr, u32 stride, u32 dies, u32 words)
{
    bool all = true;

    nr_writes = 0;
    aic_mask_all();
    CHECK(nr_writes == (int)(2 * dies * words));
    for (u32 die = 0; die < dies; die++) {
        for (u32 i = 0; i < words; i++) {
            all &= written(mask_set + die * stride + 4 * i, 0xffffffff) == 1;
            all &= written(sw_clr + die * stride + 4 * i, 0xffffffff) == 1;
        }
    }
    CHECK(all);
}

#define BASE 0x281000000UL /* made up */

static void test_aic3_of_the_m4(void)
{
    static struct aic m4 = {
        .base = BASE,
        .version = 3,
        .nr_irq = 2048,
        .nr_die = 1,
        .max_irq = 4096,
        .max_die = 2,
        .extintrcfg_stride = 0x4a00,
        .intmaskset_stride = 0x4a00,
        .intmaskclear_stride = 0x4a00,
        /* config 0x10000, then 4 * 4096 bytes of it, then 0x200 bytes each */
        .regs = {.config = 0x10000,
                 .sw_set = 0x14000,
                 .sw_clr = 0x14200,
                 .mask_set = 0x14400,
                 .mask_clr = 0x14600,
                 .event = 0x40000},
    };

    aic = &m4;
    mask(0);
    CHECK(wrote_once(BASE + 0x14400, 1));
    mask(969);
    CHECK(wrote_once(BASE + 0x14400 + 4 * 30, 1U << 9));
    mask(2047);
    CHECK(wrote_once(BASE + 0x14400 + 4 * 63, 1U << 31));
    unmask(0);
    CHECK(wrote_once(BASE + 0x14600, 1));
    unmask(1505);
    CHECK(wrote_once(BASE + 0x14600 + 4 * 47, 1U << 1));
    /* One die: 64 words of 32 lines */
    check_mask_all(BASE + 0x14400, BASE + 0x14200, 0x4a00, 1, 64);

    /* Two dies: line 5 of the second is 4096 + 5, its registers 0x4a00 further on */
    m4.nr_die = 2;
    mask(4096 + 5);
    CHECK(wrote_once(BASE + 0x4a00 + 0x14400, 1U << 5));
    unmask(4096 + 2047);
    CHECK(wrote_once(BASE + 0x4a00 + 0x14600 + 4 * 63, 1U << 31));
    check_mask_all(BASE + 0x14400, BASE + 0x14200, 0x4a00, 2, 64);
}

static void test_aic2_of_two_dies(void)
{
    /* config 0x2000 (AIC2_IRQ_CFG), 1024 lines a die: 0x1000 bytes of it, then 0x80 each */
    static struct aic two = {
        .base = BASE,
        .version = 2,
        .nr_irq = 1000,
        .nr_die = 2,
        .max_irq = 1024,
        .max_die = 2,
        .extintrcfg_stride = 0x1280,
        .intmaskset_stride = 0x1280,
        .intmaskclear_stride = 0x1280,
        .regs = {.config = 0x2000,
                 .sw_set = 0x3000,
                 .sw_clr = 0x3080,
                 .mask_set = 0x3100,
                 .mask_clr = 0x3180},
    };

    aic = &two;
    mask(33);
    CHECK(wrote_once(BASE + 0x3100 + 4, 1U << 1));
    mask(1024 + 999);
    CHECK(wrote_once(BASE + 0x1280 + 0x3100 + 4 * 31, 1U << 7));
    unmask(1024);
    CHECK(wrote_once(BASE + 0x1280 + 0x3180, 1));
    /* 1000 lines: 32 words, the last one in part */
    check_mask_all(BASE + 0x3100, BASE + 0x3080, 0x1280, 2, 32);
}

static void test_aic1(void)
{
    static struct aic one = {
        .base = BASE,
        .version = 1,
        .nr_irq = 900,
        .nr_die = 1,
        .max_irq = 0x400,
        .max_die = 1,
        .regs = {.sw_set = 0x4000, .sw_clr = 0x4080, .mask_set = 0x4100, .mask_clr = 0x4180},
    };

    aic = &one;
    mask(899);
    CHECK(wrote_once(BASE + 0x4100 + 4 * 28, 1U << 3));
    unmask(64);
    CHECK(wrote_once(BASE + 0x4180 + 4 * 2, 1));
    /* 900 lines: 29 words */
    check_mask_all(BASE + 0x4100, BASE + 0x4080, 0, 1, 29);
}

int main(void)
{
    test_aic3_of_the_m4();
    test_aic2_of_two_dies();
    test_aic1();

    printf("aic_test: %d checks, %d failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
