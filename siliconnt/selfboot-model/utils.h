/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>

#include "types.h"

/* As in src/utils.h */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* The registers of the CPU are run_test.c */
u64 mock_mrs(int reg);
void mock_msr(int reg, u64 val);
#define mrs(reg)      mock_mrs(reg)
#define msr(reg, val) mock_msr(reg, val)
#define sysop(op)     ((void)0)
/* Names that m1n1 leaves to the assembler */
#define HCR_EL2  sys_reg(3, 4, 1, 1, 0)
#define MDCR_EL2 sys_reg(3, 4, 1, 1, 1)

extern u64 mem_size_actual;
#endif
