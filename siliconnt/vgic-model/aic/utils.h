/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, for src/aic.c: see aic_test.c */

#ifndef UTILS_H
#define UTILS_H
#include "types.h"

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

u32 read32(u64 addr);
void write32(u64 addr, u32 val);
u32 mask32(u64 addr, u32 clear, u32 set);
#endif
