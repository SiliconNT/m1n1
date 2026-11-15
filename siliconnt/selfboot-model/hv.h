/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see run_test.c */

#ifndef HV_H
#define HV_H

#include "iodev.h"
#include "types.h"

void hv_init(void);
void hv_write_hcr(u64 val);
void hv_write_hacr(u64 val);
int hv_map_hw(u64 from, u64 to, u64 size);
int hv_unmap(u64 from, u64 size);
void hv_map_vuart(u64 base, int irq, iodev_id_t iodev);
int hv_pt_xtrack(void);
void hv_start(void *entry, u64 regs[4]);
#endif
