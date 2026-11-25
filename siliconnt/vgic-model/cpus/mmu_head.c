/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What mmu_secondary_setup() and mmu_init_secondary() of src/memory.c stand on. The Makefile
 * puts the two, as they are there, after this.
 */

#include "cpu_regs.h"
#include "memory.h"
#include "smp.h"
#include "utils.h"

/* cpus_test.c */
void test_mmu_configured(void);
void test_ivac(void *addr, size_t len);

static u64 read_sctlr(void)
{
    return mrs(SCTLR_EL1);
}

static void write_sctlr(u64 val)
{
    msr(SCTLR_EL1, val);
}

static void mmu_configure(void)
{
    test_mmu_configured();
}

static void mmu_init_sprr(void)
{
}

bool supports_pan(void)
{
    return true;
}

void dc_ivac_range(void *addr, size_t len)
{
    test_ivac(addr, len);
}
