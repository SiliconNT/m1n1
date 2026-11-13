/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#ifndef UTILS_H
#define UTILS_H
#include "types.h"
u64 mock_mrs(int reg);
void mock_msr(int reg, u64 val);
#define mrs(reg)      mock_mrs(reg)
#define msr(reg, val) mock_msr(reg, val)
#define sysop(op)     ((void)0)
#define TPIDR_EL2 sys_reg(3, 4, 13, 0, 2)
#define MPIDR_EL1 sys_reg(3, 0, 0, 0, 5)
#endif
