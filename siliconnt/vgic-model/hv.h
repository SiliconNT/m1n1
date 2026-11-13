/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#include "types.h"
#define HV_VGIC_INTID_VTIMER 27
#define HV_VGIC_PRIO_DEFAULT 0x80
void hv_vgic_init(void);
bool hv_vgic_inject(u32 vintid, u8 priority);
void hv_vgic_retract(u32 vintid);
bool hv_vgic_handle_irq(void);
void hv_vgic_send_sgi(u64 val);
void hv_vgic_deliver_sgis(void);
void hv_vgic_report(void);
