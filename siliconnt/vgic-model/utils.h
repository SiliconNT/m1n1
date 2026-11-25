/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#ifndef UTILS_H
#define UTILS_H
#include <pthread.h>

#include "types.h"
u64 mock_mrs(int reg);
void mock_msr(int reg, u64 val);
void mock_sysop(const char *op);
#define mrs(reg)      mock_mrs(reg)
#define msr(reg, val) mock_msr(reg, val)
/* "wfe" and what masks or unmasks SErrors are played (test.c); barriers do nothing here */
#define sysop(op) mock_sysop(op)
typedef pthread_mutex_t spinlock_t;
#define spin_lock(l)   pthread_mutex_lock(l)
#define spin_unlock(l) pthread_mutex_unlock(l)
#define TPIDR_EL2   sys_reg(3, 4, 13, 0, 2)
#define MPIDR_EL1   sys_reg(3, 0, 0, 0, 5)
#define HCR_EL2     sys_reg(3, 4, 1, 1, 0)
#define ISR_EL1     sys_reg(3, 0, 12, 1, 0)
#define CNTFRQ_EL0  sys_reg(3, 3, 14, 0, 0)
#define CNTPCT_EL0  sys_reg(3, 3, 14, 0, 1)
#define CNTHCTL_EL2 sys_reg(3, 4, 14, 1, 0)
#define CNTVOFF_EL2 sys_reg(3, 4, 14, 0, 3)
#endif
