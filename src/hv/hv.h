/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HV_H
#define HV_H

#include "exception.h"
#include "hv_xinsn.h"
#include "iodev.h"
#include "types.h"
#include "uartproxy.h"

enum hv_sptm_mode {
    SPTM_NONE = 0,
    SPTM_EMULATION = 1,
    SPTM_NESTED_VIRT = 2,
};

extern enum hv_sptm_mode hv_sptm_mode;
void hv_sptm_set_mode(enum hv_sptm_mode mode);

typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

#define MMIO_EVT_ATTR  GENMASK(31, 24)
#define MMIO_EVT_CPU   GENMASK(23, 16)
#define MMIO_EVT_SH    GENMASK(15, 14)
#define MMIO_EVT_MULTI BIT(6)
#define MMIO_EVT_WRITE BIT(5)
#define MMIO_EVT_WIDTH GENMASK(4, 0)

struct hv_evt_mmiotrace {
    u32 flags;
    u32 reserved;
    u64 pc;
    u64 addr;
    u64 data;
};

struct hv_evt_irqtrace {
    u32 flags;
    u16 type;
    u16 num;
};

#define HV_MAX_RW_SIZE  64
#define HV_MAX_RW_WORDS (HV_MAX_RW_SIZE >> 3)

struct hv_vm_proxy_hook_data {
    u32 flags;
    u32 id;
    u64 addr;
    u64 data[HV_MAX_RW_WORDS];
};

typedef enum _hv_entry_type {
    HV_HOOK_VM = 1,
    HV_VTIMER,
    HV_USER_INTERRUPT,
    HV_WDT_BARK,
    HV_CPU_SWITCH,
    HV_VIRTIO,
    HV_PANIC,
    HV_GUEST_RESET, // the guest called PSCI SYSTEM_RESET
    HV_GUEST_OFF,   // the guest called PSCI SYSTEM_OFF, or CPU_OFF on its last core
    HV_GUEST_HVC,   // the guest executed an hvc that must not return and cannot be carried out
} hv_entry_type;

/* VM */
void hv_pt_init(void);
int hv_map(u64 from, u64 to, u64 size, u64 incr);
int hv_unmap(u64 from, u64 size);
int hv_map_hw(u64 from, u64 to, u64 size);
int hv_map_hw_ro(u64 from, u64 to, u64 size);
int hv_map_sw(u64 from, u64 to, u64 size);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
bool hv_pt_is_ram(u64 ipa);
int hv_pt_set_writable(u64 ipa, bool writable);
u64 hv_translate(u64 addr, bool s1only, bool w, u64 *par_out);
u64 hv_pt_walk(u64 addr);
u64 hv_translate_insn(u64 addr);
int hv_pt_check_ctrr(void);
bool hv_emulate_pt_store(struct exc_info *ctx, u64 far, u64 ipa, u64 *bytes);
bool hv_handle_dabort(struct exc_info *ctx);
bool hv_pa_write(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_read(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_rw(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

/*
 * What is where in the physical address space of the guest (hv_layout.c).
 *
 * The devices of the machine: the ranges of /arm-io of the device tree, as addresses of the
 * CPU. hv_io_ranges() returns -1 after printing why the tree has none that can be used;
 * hv_io_ranges_next() returns false after the last one.
 */
struct hv_io_ranges {
    const u8 *next, *end;
    u32 bus_cells, cpu_cells, size_cells;
};
int hv_io_ranges(struct hv_io_ranges *it);
bool hv_io_ranges_next(struct hv_io_ranges *it, u64 *base, u64 *size);
/*
 * Whether the machine has neither a device nor memory where the guest finds the Distributor
 * and the Redistributors (HV_VGIC_DIST_BASE, HV_VGIC_REDIST_BASE). Prints what is in the way
 * of which of the two. hv_start() does not enter a guest unless this is true.
 */
bool hv_vgic_ranges_free(void);
/*
 * The AIC lines a guest can have, and the SPI each of them is: every value in "interrupts" of
 * a node of the device tree that goes to AIC by "interrupt-parent", as die * max_irq + line,
 * that is a line this AIC has; in ascending order, each once. The k-th is SPI
 * HV_AICMAP_FIRST_SPI + k.
 * The firmware is handed the same table, version 1: the version, the count, the lines, each
 * 32 bits (SiliconNT docs/DESIGN.md: the vGIC of the EL2 layer, and the firmware).
 */
#define HV_AICMAP_VERSION   1
#define HV_AICMAP_FIRST_SPI 32
#define HV_AICMAP_MAX       988 // SPIs 32-1019
struct hv_aicmap {
    u32 version;
    u32 count;
    u32 line[HV_AICMAP_MAX];
};
/*
 * Writes the table to map. Returns its size in bytes, or -1 after printing why there is none:
 * no AIC, or more lines than a guest has SPIs, with how many there are.
 */
int hv_aicmap(struct hv_aicmap *map);

/* AIC events through tracing the MMIO event address */
bool hv_trace_irq(u32 type, u32 num, u32 count, u32 flags);
void hv_aic_init(void);
bool hv_aic_hook(void);

/* Interrupts for a guest that expects a GICv3, through the virtual CPU interface */
#define HV_VGIC_INTID_VTIMER 27 // PPI of the EL1 virtual timer
/*
 * Where the guest finds the Distributor, and the Redistributor of the core with m1n1 index n:
 * HV_VGIC_REDIST_BASE + n * HV_VGIC_REDIST_SIZE. The same on every machine, so the machine
 * must have nothing of its own there: hv_vgic_ranges_free().
 */
#define HV_VGIC_DIST_BASE   0x5000000000UL
#define HV_VGIC_DIST_SIZE   0x10000UL
#define HV_VGIC_REDIST_BASE 0x5100000000UL
#define HV_VGIC_REDIST_SIZE 0x20000UL
void hv_vgic_init(void);
void hv_vgic_init_cpu(void);
void hv_vgic_cpu_off(void);
/* AIC enabled for the guest, right before it is entered; as it was, once it has been left */
void hv_vgic_start(void);
void hv_vgic_stop(void);
/* Whether hv_vgic_init() has the table of hv_aicmap(): hv_start() does not enter a guest unless */
bool hv_vgic_has_aicmap(void);
void hv_vgic_ppi_level(u32 intid, bool high);
bool hv_vgic_handle_irq(void);
void hv_vgic_send_sgi(u64 val);
void hv_vgic_deliver(void);
bool hv_vgic_irq_waiting(void);
void hv_vgic_report(void);

/* WFI of the guest: trapped, the core waits in EL2 */
void hv_wfi_init(void);
bool hv_handle_wfx(u64 iss);
void hv_wfi_ipi_sent(int cpu);
void hv_wfi_report(void);

/* PSCI for the guest, over hvc #0 */
void hv_psci_init(void);
bool hv_psci_hvc(struct exc_info *ctx, u16 imm);
void hv_psci_report(void);

/* The PMUv3 registers for the guest: accesses replaced by hvc with bit 15 of the immediate set */
void hv_pmu_init(void);
void hv_pmu_init_cpu(int cpu);
bool hv_pmu_is_access(u32 insn);
u32 hv_pmu_patch_word(u32 insn);
u32 hv_pmu_unpatch_word(u32 insn);
u32 hv_pmu_count_accesses(const u32 *words, u32 count);
u32 hv_pmu_patch_range(const u32 *from, u32 *to, u32 count);
bool hv_pmu_hvc(struct exc_info *ctx, u16 imm);
void hv_pmu_report(void);

/* The breakpoint and watchpoint registers for the guest: kept per core, never armed */
void hv_dbg_init(void);
void hv_dbg_init_cpu(int cpu);
bool hv_dbg_handle_msr(struct exc_info *ctx, u64 iss);
void hv_dbg_report(void);

/*
 * Execution tracking: a page of the guest is scanned before it first executes, and the core
 * executes a copy of it in which what had to be replaced is replaced (hv_xtrack.c)
 */
int hv_pt_xtrack(void);
bool hv_pt_xtrack_on(void);
#define HV_XPAGE_UNSCANNED 0 // data: read and written, not executed; the guest's page
#define HV_XPAGE_SCANNED   1 // code, nothing replaced: read and executed; the guest's page
#define HV_XPAGE_INVALID   2 // an invalid entry, while the page goes from one state to another
#define HV_XPAGE_XONLY     3 // code with replaced instructions: executed only; a shadow page
int hv_pt_scanned(u64 ipa, u64 *pa);
int hv_pt_set_scanned(u64 ipa, int state, u64 pa);
u64 hv_pt_switch_count(void);
#define HV_XTRACK_NONE  0
#define HV_XTRACK_RETRY 1
#define HV_XTRACK_STOP  2
#define HV_XTRACK_SKIP  3
/* The smallest data cache line that cache maintenance here can do with, see hv_xtrack_line() */
#define HV_XTRACK_LINE 64UL
void hv_xtrack_init(void);
void hv_xtrack_init_cpu(int cpu);
u64 hv_xtrack_line(void);
int hv_xtrack_start(void);
bool hv_xtrack_iabort(struct exc_info *ctx);
int hv_xtrack_dabort(struct exc_info *ctx, u64 ipa, bool is_write);
void hv_xtrack_report(void);

/* AMCC interrupt filtering */
bool hv_amcc_init(void);
bool hv_amcc_handle_irq(u32 irq);

/* Virtual peripherals */
void hv_vuart_poll(void);
void hv_map_vuart(u64 base, int irq, iodev_id_t iodev);
struct virtio_conf;
void hv_map_virtio(u64 base, struct virtio_conf *conf);
void virtio_put_buffer(u64 base, int qu, u32 id, u32 len);

/*
 * Without a host (hv_stop.c): whether there is one, and what is done instead of telling it.
 * hv_stop_if_no_host() returns at once if there is a host, and does not return if not.
 */
bool hv_has_host(void);
void hv_stop_if_no_host(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type);
/* Restarting and powering off the machine (hv_power.c): they return only if it did not happen */
void hv_power_reset(void);
void hv_power_off(void);

/* Exceptions */
void hv_exc_init(void);
void hv_exc_init_cpu(int cpu);
void hv_lock(void);
void hv_exc_report(void);
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra);
void hv_maybe_switch_cpu(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type,
                         void *extra);
void hv_set_time_stealing(bool enabled, bool reset);
void hv_add_time(s64 time);
void hv_write_cntvoff(void);
u64 hv_pmcr0_read(void);
void hv_pmcr0_write(u64 val);

/* WDT */
void hv_wdt_pet(void);
void hv_wdt_suspend(void);
void hv_wdt_resume(void);
void hv_wdt_init(void);
void hv_wdt_start(int cpu);
void hv_wdt_stop(void);
void hv_wdt_breadcrumb(char c);
void hv_do_panic(void);

#define hv_panic(fmt, ...)                                                                         \
    do {                                                                                           \
        debug_printf("HV panic:" fmt, ##__VA_ARGS__);                                              \
        hv_do_panic();                                                                             \
        flush_and_reboot();                                                                        \
    } while (0)

/* Utilities */
void hv_update_fiq(void);
void hv_write_hcr(u64 val);
void hv_write_hacr(u64 val);
u64 hv_get_spsr(void);
void hv_set_spsr(u64 val);
u64 hv_get_esr(void);
u64 hv_get_far(void);
u64 hv_get_elr(void);
u64 hv_get_afsr1(void);
void hv_set_elr(u64 val);

bool hv_is_nested(void);
extern bool hv_genter_trap_enabled;
void hv_set_genter_trap(bool enabled);
void hv_enable_genter_trap(void);
void hv_disable_genter_trap(void);

/* HV main */
void hv_init(void);
bool hv_cpus_ready(void);
bool hv_cpu_power_init(void);
void hv_start(void *entry, u64 regs[4]);
bool hv_start_secondary(int cpu, void *entry, u64 regs[4]);
void hv_cpu_off(struct exc_info *ctx);
void hv_exit_cpu(int cpu);
void hv_exit_all(void) __attribute__((noreturn));
void hv_leave_guest(void) __attribute__((noreturn));
bool hv_is_leaving(void);
void hv_rendezvous(void);
u64 hv_get_started_cpus(void);
int hv_lead_cpu(void);
bool hv_switch_cpu(int cpu);
void hv_pin_cpu(int cpu);
void hv_arm_tick(bool secondary);
void hv_take_over_tick(void);
void hv_rearm(void);
void hv_maybe_exit(void);
void hv_tick(struct exc_info *ctx);

#endif
