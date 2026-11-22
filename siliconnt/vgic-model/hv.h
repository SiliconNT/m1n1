/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#ifndef HV_H
#define HV_H

#include "types.h"

/* src/exception.h */
struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
    u64 esr;
    u64 far;
    u64 afsr1;
    u64 sp[3];
    u64 cpu_id;
    u64 mpidr;
    u64 elr_phys;
    u64 far_phys;
    u64 sp_phys;
    void *extra;
};

/* src/uartproxy.h */
typedef enum _uartproxy_start_reason_t {
    START_BOOT,
    START_EXCEPTION,
    START_EXCEPTION_LOWER,
    START_HV,
} uartproxy_boot_reason_t;

typedef enum _hv_entry_type {
    HV_HOOK_VM = 1,
    HV_VTIMER,
    HV_USER_INTERRUPT,
    HV_WDT_BARK,
    HV_CPU_SWITCH,
    HV_VIRTIO,
    HV_PANIC,
    HV_GUEST_RESET,
    HV_GUEST_OFF,
    HV_GUEST_HVC,
} hv_entry_type;

typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);

/* From the real header, by the Makefile: the table of AIC lines and SPIs */
#include "hv_aicmap.h"
int hv_aicmap(struct hv_aicmap *map);
bool hv_vgic_has_aicmap(void);

#define HV_VGIC_INTID_VTIMER 27
#define HV_VGIC_DIST_BASE    0x5000000000UL
#define HV_VGIC_DIST_SIZE    0x10000UL
#define HV_VGIC_REDIST_BASE  0x5100000000UL
#define HV_VGIC_REDIST_SIZE  0x20000UL
void hv_vgic_init(void);
void hv_vgic_start(void);
void hv_vgic_stop(void);
void hv_vgic_ppi_level(u32 intid, bool high);
bool hv_vgic_handle_irq(void);
void hv_vgic_send_sgi(u64 val);
void hv_vgic_deliver(void);
bool hv_vgic_irq_waiting(void);
void hv_vgic_report(void);
void hv_wfi_init(void);
bool hv_handle_wfx(u64 iss);
void hv_wfi_report(void);
void hv_update_fiq(void);
void hv_psci_init(void);
bool hv_psci_hvc(struct exc_info *ctx, u16 imm);
void hv_psci_report(void);
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra);
void hv_pmu_init(void);
bool hv_pmu_is_access(u32 insn);
u32 hv_pmu_patch_word(u32 insn);
u32 hv_pmu_unpatch_word(u32 insn);
u32 hv_pmu_count_accesses(const u32 *words, u32 count);
u32 hv_pmu_patch_range(const u32 *from, u32 *to, u32 count);
bool hv_pmu_hvc(struct exc_info *ctx, u16 imm);
void hv_pmu_report(void);
void hv_dbg_init(void);
bool hv_dbg_handle_msr(struct exc_info *ctx, u64 iss);
void hv_dbg_report(void);

#endif
