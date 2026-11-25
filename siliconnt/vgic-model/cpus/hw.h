/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Included before anything else of src/hv/hv.c and src/smp.c when cpus_test.c compiles them:
 * m1n1's own utils.h with the system registers and the instructions of the CPU taken out (the
 * Makefile does that to a copy), and what it reads and writes of the hardware.
 */

#ifndef CPUS_HW_H
#define CPUS_HW_H

#include "../build/cpus/utils.h"

u64 mock_read64(u64 addr);
void mock_write64(u64 addr, u64 val);
void mock_write32(u64 addr, u32 val);

#define read64  mock_read64
#define write64 mock_write64
#define write32 mock_write32

/* hv_rendezvous() gives up after so many looks: let the others run in between */
int sched_yield(void);
#define __atomic_load_n(ptr, order) (sched_yield(), __atomic_load_n(ptr, order))

#endif
