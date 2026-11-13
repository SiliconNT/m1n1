/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#include "types.h"

struct exc_info;
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);

#define HV_VGIC_INTID_VTIMER 27
#define HV_VGIC_DIST_BASE    0x5000000000UL
#define HV_VGIC_DIST_SIZE    0x10000UL
#define HV_VGIC_REDIST_BASE  0x5100000000UL
#define HV_VGIC_REDIST_SIZE  0x20000UL
void hv_vgic_init(void);
void hv_vgic_ppi_level(u32 intid, bool high);
bool hv_vgic_handle_irq(void);
void hv_vgic_send_sgi(u64 val);
void hv_vgic_deliver(void);
void hv_vgic_report(void);
