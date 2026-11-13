/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#include "types.h"
#define MAX_CPUS 32
int smp_id(void);
void smp_send_ipi(int cpu);
bool smp_is_alive(int cpu);
uint64_t smp_get_mpidr(int cpu);
extern int boot_cpu_idx;
