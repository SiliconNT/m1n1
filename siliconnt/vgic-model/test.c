// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_vgic.c and src/hv/hv_wfi.c. The files under test are the
 * real ones, compiled against stand-in headers; this file plays the hardware (list registers,
 * the Apple IPI, ISR_EL1, WFE), the rest of the hypervisor (the MMIO hooks, the exit paths) and
 * the guest (the Distributor and Redistributor accesses of the Windows kernel, acknowledge,
 * EOI, WFI), with one thread per core.
 *
 * What the "hypervisor exit paths" do here mirrors src/hv/hv_exc.c: an IPI is acknowledged
 * first (hv_exc_fiq), a maintenance interrupt goes to hv_vgic_handle_irq() (hv_exc_irq), a
 * trapped MMIO access runs its hook with the big hypervisor lock held (hv_exc_sync), and
 * every way back to the guest ends in hv_update_fiq(), which looks at the virtual timer and
 * calls hv_vgic_deliver(). A trapped WFI goes to hv_handle_wfx() with that lock held.
 *
 * WFE is the worst one the architecture allows: an interrupt that is masked does not end it,
 * only the event stream does. Every WFE here lasts one period of the event stream.
 *
 * AIC is played here too, behind the functions of src/aic.h that hv_vgic.c calls: lines that
 * a device holds high or not, a mask for each, and an event register that reports the lowest
 * line that is high and unmasked and masks it doing so (Linux, aic_irq_eoi()). A line that is
 * high and unmasked is a physical IRQ, on whichever core looks first. The table of AIC lines
 * and SPIs is one of this file's (hv_aicmap(), which hv_layout.c makes from the device tree
 * and siliconnt/selfboot-model tests).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aic.h"
#include "aic_regs.h"
#include "cpu_regs.h"
#include "hv.h"
#include "smp.h"
#include "utils.h"

#define NR_SGIS   16
#define NR_INTIDS 1020

extern spinlock_t bhl;

struct core {
    u64 lr[16];
    u64 mpidr;
    atomic_int ipi; /* the Apple IPI: stays pending until acknowledged */
    bool masked;
    u32 pmr;        /* ICC_PMR_EL1: only priorities below this are taken */
    u64 ich_hcr;    /* ICH_HCR_EL2 */
    bool timer;     /* the output of the guest's virtual timer */
    bool timer_fiq; /* ...raises a FIQ when high: VM_TMR_FIQ_ENA_EL2, see hv_update_fiq() */
    atomic_uint count[NR_INTIDS];
    /* IPIs taken, to see the path was used */
    u64 fiqs, irqs;
} ALIGNED(64);

static struct core cores[MAX_CPUS];
static int nr_lr = 8;
static u32 alive = 0x3ff; /* the cores that run m1n1 */
static __thread int cur;

bool hv_started_cpus[MAX_CPUS];
int boot_cpu_idx = 6;

static bool is_started(int cpu)
{
    return __atomic_load_n(&hv_started_cpus[cpu], __ATOMIC_SEQ_CST);
}

static void set_started(int cpu, bool on)
{
    __atomic_store_n(&hv_started_cpus[cpu], on, __ATOMIC_SEQ_CST);
}

/* src/hv/hv.c */
int hv_lead_cpu(void)
{
    if (is_started(boot_cpu_idx))
        return boot_cpu_idx;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (is_started(cpu))
            return cpu;
    return -1;
}

/* ---- AIC: two dies of 2048 lines, 4096 apart, as die * max_irq + line ---- */

#define AIC_LINES (2 * 4096)

static struct aic model_aic = {
    .version = 3,
    .nr_die = 2,
    .max_die = 2,
    .nr_irq = 2048,
    .max_irq = 4096,
};
struct aic *aic = &model_aic;

/* What the device holds the line at, and whether AIC masks it. Changed with bhl held. */
static bool line_high[AIC_LINES];
static bool line_masked[AIC_LINES];
/* Lines that are high and unmasked */
static atomic_int lines_ready;
static int mask_all_calls, unmask_calls;

/*
 * The global configuration, as the M4 had it under m1n1 (R33): bit 0, the enable, is 0.
 * Until it is 1, AIC reports nothing, which is what R33 saw (not known for certain).
 */
#define AIC_CONFIG_M4 0xe0000000U
static u32 aic_config = AIC_CONFIG_M4;
static atomic_bool aic_on;

/* What was done to AIC as a whole, in order */
enum aic_op { OP_MASK_ALL, OP_CONFIG_READ, OP_CONFIG_WRITE };
static struct {
    enum aic_op op;
    u32 val;
} aic_ops[64];
static int nr_aic_ops;

static void aic_op(enum aic_op op, u32 val)
{
    if (nr_aic_ops < (int)(sizeof(aic_ops) / sizeof(aic_ops[0]))) {
        aic_ops[nr_aic_ops].op = op;
        aic_ops[nr_aic_ops++].val = val;
    }
}

/* The one core that AIC tells of its lines, as the M4 does (R50); -1: whichever looks first */
static int aic_told = -1;

/* A physical IRQ is pending, on the core that looks */
static bool aic_irq(void)
{
    if (aic_told >= 0 && aic_told != cur)
        return false;
    return atomic_load(&aic_on) && atomic_load(&lines_ready);
}

static void line_set(u32 line, bool high, bool masked)
{
    bool was = line_high[line] && !line_masked[line];

    if (line >= AIC_LINES || line % 4096 >= 2048) {
        printf("CHECK FAILED: AIC has no line %u\n", line);
        exit(1);
    }
    line_high[line] = high;
    line_masked[line] = masked;
    if (was != (high && !masked))
        atomic_fetch_add(&lines_ready, high && !masked ? 1 : -1);
}

/* The device raises or lowers its line */
static void line_raise(u32 line, bool high)
{
    pthread_mutex_lock(&bhl);
    line_set(line, high, line_masked[line]);
    pthread_mutex_unlock(&bhl);
}

uint32_t aic_ack(void)
{
    if (!aic_irq())
        return 0;
    for (u32 line = 0; line < AIC_LINES; line++) {
        if (line_high[line] && !line_masked[line]) {
            line_set(line, true, true);
            return FIELD_PREP(AIC_EVENT_DIE, line / 4096) |
                   FIELD_PREP(AIC_EVENT_TYPE, AIC_EVENT_TYPE_HW) |
                   FIELD_PREP(AIC_EVENT_NUM, line % 4096);
        }
    }
    return 0;
}

void aic_mask(int irq)
{
    line_set(irq, line_high[irq], true);
}

void aic_unmask(int irq)
{
    unmask_calls++;
    line_set(irq, line_high[irq], false);
}

void aic_mask_all(void)
{
    mask_all_calls++;
    aic_op(OP_MASK_ALL, 0);
    for (u32 line = 0; line < AIC_LINES; line++)
        if (line % 4096 < 2048)
            line_set(line, line_high[line], true);
}

bool aic_read_config(u32 *val)
{
    aic_op(OP_CONFIG_READ, aic_config);
    *val = aic_config;
    return true;
}

void aic_write_config(u32 val)
{
    aic_op(OP_CONFIG_WRITE, val);
    aic_config = val;
    atomic_store(&aic_on, val & AIC2_CONFIG_ENABLE);
}

/* The table: 3, 40, 1505 and line 7 of the second die are SPIs 32 to 35 */
#define LINE_A 3
#define LINE_B 40
#define LINE_C 1505
#define LINE_D (4096 + 7)
static const u32 table_lines[] = {LINE_A, LINE_B, LINE_C, LINE_D};
static bool aicmap_fails;

int hv_aicmap(struct hv_aicmap *map)
{
    if (aicmap_fails)
        return -1;
    map->version = HV_AICMAP_VERSION;
    map->count = sizeof(table_lines) / sizeof(table_lines[0]);
    memcpy(map->line, table_lines, sizeof(table_lines));
    return 8 + sizeof(table_lines);
}

int smp_id(void)
{
    return cur;
}

void smp_send_ipi(int cpu)
{
    atomic_store(&cores[cpu].ipi, 1);
}

bool smp_is_alive(int cpu)
{
    return alive & BIT(cpu);
}

uint64_t smp_get_mpidr(int cpu)
{
    return cores[cpu].mpidr & 0xffffff;
}

static u64 eisr(struct core *c)
{
    u64 v = 0;

    for (int i = 0; i < nr_lr; i++)
        if (!FIELD_GET(ICH_LR_STATE, c->lr[i]) && !(c->lr[i] & ICH_LR_HW) &&
            (c->lr[i] & ICH_LR_EOI))
            v |= BIT(i);
    return v;
}

/* ISR_EL1 as EL2 reads it: the physical interrupts that are pending, masked or not */
static u64 isr(struct core *c)
{
    u64 v = 0;

    if (atomic_load(&c->ipi) || (c->timer && c->timer_fiq))
        v |= ISR_F;
    if (eisr(c) || aic_irq())
        v |= ISR_I; /* the maintenance interrupt, or AIC */
    return v;
}

static int lr_index(int reg)
{
    if (reg >= SYS_ICH_LR0_EL2 && reg <= SYS_ICH_LR7_EL2)
        return reg - SYS_ICH_LR0_EL2;
    if (reg >= SYS_ICH_LR8_EL2 && reg <= SYS_ICH_LR15_EL2)
        return 8 + reg - SYS_ICH_LR8_EL2;
    return -1;
}

/*
 * Something other cores do in the middle of what this one is doing: called once, at the
 * lr_read_countdown-th read of a list register from now.
 */
static void (*lr_read_hook)(void);
static int lr_read_countdown;

/* ...and right after it has read ISR_EL1 */
static void (*isr_read_hook)(void);

/* The counter and the WFEs of the core a thread plays */
#define EVENT_PERIOD 65536 /* ticks: the event stream of bit 15 */
static u64 cntfrq = 1000000000;
static u64 cnthctl;
static __thread u64 cntpct;
static __thread u64 wfe_count;
static void (*wfe_hook)(void);

void mock_wfe(void)
{
    wfe_count++;
    cntpct += EVENT_PERIOD;
    if (wfe_hook)
        wfe_hook();
}

u64 mock_mrs(int reg)
{
    struct core *c = &cores[cur];

    if (lr_index(reg) >= 0) {
        if (lr_read_hook && --lr_read_countdown == 0) {
            void (*hook)(void) = lr_read_hook;
            int self = cur;

            lr_read_hook = NULL;
            hook();
            cur = self;
        }
        return c->lr[lr_index(reg)];
    }
    switch (reg) {
        case TPIDR_EL2:
            return cur;
        case MPIDR_EL1:
            return c->mpidr;
        case SYS_ICC_SRE_EL2:
            return 0xf;
        case SYS_ICH_VTR_EL2:
            return FIELD_PREP(ICH_VTR_PREBITS, 4) | (nr_lr - 1);
        case SYS_ICH_EISR_EL2:
            return eisr(c);
        case SYS_ICH_MISR_EL2:
            return eisr(c) ? 1 : 0;
        case ISR_EL1: {
            u64 v = isr(c);

            if (isr_read_hook) {
                void (*hook)(void) = isr_read_hook;
                int self = cur;

                isr_read_hook = NULL;
                hook();
                cur = self;
            }
            return v;
        }
        case HCR_EL2:
            return 0; /* nothing in the model raises the virtual FIQ */
        case CNTFRQ_EL0:
            return cntfrq;
        case CNTPCT_EL0:
            return cntpct;
        case CNTHCTL_EL2:
            return cnthctl;
    }
    return 0;
}

void mock_msr(int reg, u64 val)
{
    if (lr_index(reg) >= 0)
        cores[cur].lr[lr_index(reg)] = val;
    else if (reg == SYS_ICH_HCR_EL2)
        cores[cur].ich_hcr = val;
    else if (reg == CNTHCTL_EL2)
        cnthctl = val;
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- the hypervisor around hv_vgic.c ---- */

static struct {
    u64 base, size;
    hv_hook_t *hook;
} hooks[4];
static int nr_hooks;

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    CHECK(nr_hooks < 4);
    hooks[nr_hooks].base = from;
    hooks[nr_hooks].size = size;
    hooks[nr_hooks].hook = hook;
    nr_hooks++;
    return 0;
}

/*
 * The big hypervisor lock: held around the MMIO hooks and hv_vgic_handle_irq(), as hv_exc.c
 * holds it there, not on the other paths used here
 */
spinlock_t bhl = PTHREAD_MUTEX_INITIALIZER;

/* hv_update_fiq(), on the current core. The timer's FIQ is off for as long as its output is high */
static void hv_exit(void)
{
    cores[cur].timer_fiq = !cores[cur].timer;
    hv_vgic_ppi_level(HV_VGIC_INTID_VTIMER, cores[cur].timer);
    hv_vgic_deliver();
}

void hv_update_fiq(void)
{
    hv_exit();
}

/* Take whatever physical interrupt is pending. Returns false if there was none. */
static bool physical_interrupt(void)
{
    struct core *c = &cores[cur];

    if (atomic_load(&c->ipi)) {
        /* hv_exc_fiq: acknowledge, then hv_exc_exit -> hv_update_fiq */
        atomic_store(&c->ipi, 0);
        c->fiqs++;
        hv_exit();
        return true;
    }
    if (c->timer && c->timer_fiq) {
        /* hv_exc_fiq for the guest's timer: nothing to acknowledge, hv_update_fiq injects it */
        hv_exit();
        return true;
    }
    if (eisr(c) || aic_irq()) {
        /* hv_exc_irq: hv_vgic_handle_irq, then hv_exc_exit -> hv_update_fiq */
        c->irqs++;
        pthread_mutex_lock(&bhl);
        CHECK(hv_vgic_handle_irq());
        pthread_mutex_unlock(&bhl);
        hv_exit();
        return true;
    }
    return false;
}

/* The guest wrote ICC_SGI1R_EL1: trap, then hv_update_fiq on the way out */
static void guest_send(u64 val)
{
    hv_vgic_send_sgi(val);
    hv_exit();
}

/* The guest accessed memory that is not mapped: data abort, hook, hv_exc_exit */
static bool guest_mmio(u64 addr, u64 *val, bool write, int width)
{
    for (int i = 0; i < nr_hooks; i++) {
        if (addr >= hooks[i].base && addr - hooks[i].base < hooks[i].size) {
            pthread_mutex_lock(&bhl);
            bool ok = hooks[i].hook(NULL, addr, val, write, width);
            pthread_mutex_unlock(&bhl);
            hv_exit();
            return ok;
        }
    }
    return false;
}

static u64 rd(u64 addr, int width)
{
    u64 val = 0xa5a5a5a5a5a5a5a5UL;

    CHECK(guest_mmio(addr, &val, false, width));
    /* The hook returns the value zero-extended */
    CHECK(width == 3 || !(val >> (8 << width)));
    return val;
}

static void wr(u64 addr, u64 val, int width)
{
    CHECK(guest_mmio(addr, &val, true, width));
}

/* (functions, so that a read may be made for its effect only) */
static inline u8 rd8(u64 addr)
{
    return rd(addr, 0);
}

static inline u32 rd32(u64 addr)
{
    return rd(addr, 2);
}

static inline u64 rd64(u64 addr)
{
    return rd(addr, 3);
}

#define wr8(a, v)  wr(a, v, 0)
#define wr32(a, v) wr(a, v, 2)
#define wr64(a, v) wr(a, v, 3)

#define GICD(off)      (HV_VGIC_DIST_BASE + (off))
#define GICR(cpu, off) (HV_VGIC_REDIST_BASE + (u64)(cpu) * HV_VGIC_REDIST_SIZE + (off))

/* Offsets, Arm IHI 0069H.b 12.8 and 12.10; the Redistributor ones include SGI_base */
#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ISPENDR    0x0200
#define GICD_ICPENDR    0x0280
#define GICD_IPRIORITYR 0x0400
#define GICD_ICFGR      0x0c00
#define GICD_IROUTER    0x6000
#define GICD_PIDR2      0xffe8
#define GICR_CTLR       0x0000
#define GICR_TYPER      0x0008
#define GICR_WAKER      0x0014
#define GICR_PIDR2      0xffe8
#define GICR_IGROUPR0   0x10080
#define GICR_ISENABLER0 0x10100
#define GICR_ICENABLER0 0x10180
#define GICR_ISPENDR0   0x10200
#define GICR_IPRIORITYR 0x10400
#define GICR_ICFGR0     0x10c00
#define GICR_ICFGR1     0x10c04

/* ---- the guest's side of the virtual CPU interface ---- */

/* ICC_IAR1_EL1: the pending interrupt with the highest priority, 1023 if none */
static u32 guest_ack(void)
{
    struct core *c = &cores[cur];
    int best = -1;
    u32 best_prio = c->pmr;

    for (int i = 0; i < nr_lr; i++) {
        u32 prio = FIELD_GET(ICH_LR_PRIORITY, c->lr[i]);

        if (FIELD_GET(ICH_LR_STATE, c->lr[i]) == ICH_LR_STATE_PENDING && prio < best_prio) {
            best = i;
            best_prio = prio;
        }
    }
    if (best < 0)
        return 1023;
    c->lr[best] = (c->lr[best] & ~ICH_LR_STATE) | FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_ACTIVE);
    return FIELD_GET(ICH_LR_VINTID, c->lr[best]);
}

/* ICC_EOIR1_EL1 with EOImode 0: the active interrupt is deactivated */
static void guest_eoi(u32 intid)
{
    struct core *c = &cores[cur];

    for (int i = 0; i < nr_lr; i++) {
        u64 state = FIELD_GET(ICH_LR_STATE, c->lr[i]);

        if ((state & ICH_LR_STATE_ACTIVE) && FIELD_GET(ICH_LR_VINTID, c->lr[i]) == intid) {
            c->lr[i] = (c->lr[i] & ~ICH_LR_STATE) |
                       FIELD_PREP(ICH_LR_STATE, state & ~(u64)ICH_LR_STATE_ACTIVE);
            return;
        }
    }
    CHECK(!"EOI of an interrupt that is not active");
}

/* What the guest does about an interrupt it has taken, before it ends it; NULL: nothing */
static void (*guest_handler)(u32 intid);

/* Take one virtual interrupt if the guest can. Returns the INTID or 1023. */
static u32 guest_irq(void)
{
    struct core *c = &cores[cur];
    u32 intid;
    int self = cur;

    if (c->masked)
        return 1023;
    intid = guest_ack();
    if (intid == 1023)
        return 1023;
    CHECK(intid < NR_INTIDS);
    atomic_fetch_add(&c->count[intid], 1);
    if (guest_handler) {
        guest_handler(intid);
        cur = self;
    }
    guest_eoi(intid);
    return intid;
}

/* Run the core until nothing more happens */
static void settle(int cpu)
{
    cur = cpu;
    while (physical_interrupt() || guest_irq() != 1023)
        ;
}

static u64 sgi_list(u64 mpidr, u32 list, u32 intid, u32 rs)
{
    return (((mpidr >> 32) & 0xff) << 48) | ((u64)rs << 44) | (((mpidr >> 16) & 0xff) << 32) |
           ((u64)intid << 24) | (((mpidr >> 8) & 0xff) << 16) | list;
}

static u64 sgi_to(int cpu, u32 intid)
{
    return sgi_list(cores[cpu].mpidr, 1U << (cores[cpu].mpidr & 0xff), intid, 0);
}

static int nr_valid_lrs(int cpu)
{
    int n = 0;

    for (int i = 0; i < nr_lr; i++)
        if (FIELD_GET(ICH_LR_STATE, cores[cpu].lr[i]))
            n++;
    return n;
}

/* The list register that holds the interrupt, 0 if none does */
static u64 lr_of(int cpu, u32 intid)
{
    for (int i = 0; i < nr_lr; i++)
        if (FIELD_GET(ICH_LR_STATE, cores[cpu].lr[i]) &&
            FIELD_GET(ICH_LR_VINTID, cores[cpu].lr[i]) == intid)
            return cores[cpu].lr[i];
    return 0;
}

static u32 total(int cpu)
{
    u32 n = 0;

    for (int i = 0; i < NR_INTIDS; i++)
        n += atomic_load(&cores[cpu].count[i]);
    return n;
}

static u32 taken(int cpu, u32 intid)
{
    return atomic_load(&cores[cpu].count[intid]);
}

/* The M4's layout: 0-5 in one cluster, 6-9 in another; the guest runs on 6, 0, 7 and 1 */
static const int started[] = {6, 0, 7, 1};
#define NR_STARTED 4

/* The hypervisor has started and entered the guest on the cores; the guest has done nothing */
static void reset_raw(int lrs)
{
    nr_lr = lrs;
    nr_hooks = 0;
    memset(cores, 0, sizeof(cores));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    for (int i = 0; i < 10; i++)
        cores[i].mpidr = i < 6 ? (0x80000000UL | i) : (0x80010100UL | (i - 6));
    for (int i = 0; i < MAX_CPUS; i++) {
        cores[i].pmr = 0x100;
        cores[i].timer_fiq = true;
    }
    /* hv_init() on the boot core, and hv_start_secondary() for the others */
    for (int i = 0; i < NR_STARTED; i++) {
        cur = started[i];
        if (cur == boot_cpu_idx)
            hv_vgic_init();
        else
            hv_vgic_init_cpu();
        set_started(cur, true);
    }
    /* A core the guest is not on, but which has an affinity: must never be a target */
    cur = 8;
    hv_vgic_init_cpu();
    CHECK(nr_hooks == 2);
}

/* ...and the guest has turned on the Distributor and, on every core, the SGIs and the timer */
static void reset(int lrs)
{
    reset_raw(lrs);
    cur = 6;
    wr32(GICD(GICD_CTLR), 0x2);
    for (int i = 0; i < NR_STARTED; i++) {
        cur = started[i];
        for (int n = 0; n < 32; n += 4)
            wr32(GICR(cur, GICR_IPRIORITYR + n), 0x80808080);
        wr32(GICR(cur, GICR_ISENABLER0), 0xffff | BIT(HV_VGIC_INTID_VTIMER));
    }
    for (int i = 0; i < NR_STARTED; i++) {
        settle(started[i]);
        cores[started[i]].fiqs = 0;
    }
}

static void expect_counts(const char *what, u32 intid, int c6, int c0, int c7, int c1)
{
    int want[] = {c6, c0, c7, c1};

    for (int i = 0; i < NR_STARTED; i++) {
        settle(started[i]);
        if ((int)taken(started[i], intid) != want[i]) {
            printf("FAIL %s: cpu %d intid %u got %u want %d\n", what, started[i], intid,
                   taken(started[i], intid), want[i]);
            exit(1);
        }
    }
    CHECK(total(8) == 0 && !atomic_load(&cores[8].ipi));
    printf("ok   %s\n", what);
}

static void sgi_tests(void)
{
    reset(8);

    cur = 6;
    guest_send(sgi_to(6, 1));
    CHECK(nr_valid_lrs(6) == 1); /* in a list register before the trap returns */
    CHECK(!atomic_load(&cores[0].ipi) && !atomic_load(&cores[7].ipi)); /* nobody else woken */
    expect_counts("self", 1, 1, 0, 0, 0);

    cur = 6;
    guest_send(sgi_to(0, 2));
    CHECK(atomic_load(&cores[0].ipi) && !atomic_load(&cores[7].ipi) && !atomic_load(&cores[1].ipi));
    expect_counts("another cluster", 2, 0, 1, 0, 0);

    cur = 0;
    guest_send(sgi_to(7, 3));
    expect_counts("E core to P core", 3, 0, 0, 1, 0);

    cur = 6;
    guest_send(BIT(40) | (4UL << 24));
    expect_counts("all but self", 4, 0, 1, 1, 1);

    cur = 0;
    guest_send(BIT(40) | (5UL << 24) | 0xffff00ffffffUL); /* the RES0 fields are ignored */
    expect_counts("all but self, from an E core", 5, 1, 0, 1, 1);

    cur = 6;
    guest_send(sgi_list(cores[6].mpidr, 0x3, 6, 0));
    expect_counts("target list, P cluster", 6, 1, 0, 1, 0);

    cur = 6;
    guest_send(sgi_list(cores[0].mpidr, 0xffff, 7, 0)); /* 2-5 and 6-15 are not there */
    expect_counts("target list, all 16 bits, E cluster", 7, 0, 1, 0, 1);

    cur = 6;
    guest_send(
        sgi_list(cores[6].mpidr, 0xffff, 8, 0)); /* CPU 8 exists but the guest is not on it */
    expect_counts("target list, all 16 bits, P cluster", 8, 1, 0, 1, 0);

    cur = 6;
    guest_send(sgi_list(cores[6].mpidr, 0, 9, 0));
    guest_send(sgi_list(cores[6].mpidr, 0x3, 9, 1));               /* RS = 1: Aff0 16 and 17 */
    guest_send(sgi_list(cores[6].mpidr ^ 0x4000, 0x3, 9, 0));      /* no such Aff1 */
    guest_send(sgi_list(cores[6].mpidr ^ 0x400000, 0x3, 9, 0));    /* no such Aff2 */
    guest_send(sgi_list(cores[6].mpidr | (1UL << 32), 0x3, 9, 0)); /* no such Aff3 */
    expect_counts("targets that do not exist", 9, 0, 0, 0, 0);

    /* More pending than list registers */
    reset(8);
    cur = 6;
    cores[6].masked = true;
    for (u32 i = 0; i < NR_SGIS; i++)
        guest_send(sgi_to(6, i));
    CHECK(nr_valid_lrs(6) == 8);
    settle(6);
    CHECK(total(6) == 0 && nr_valid_lrs(6) == 8);
    cores[6].masked = false;
    settle(6);
    for (u32 i = 0; i < NR_SGIS; i++)
        CHECK(taken(6, i) == 1);
    CHECK(nr_valid_lrs(6) == 0 && eisr(&cores[6]) == 0);
    printf("ok   16 pending with 8 list registers (%lu maintenance IRQs)\n", cores[6].irqs);

    /* The same from another core, into a core with 2 list registers */
    reset(2);
    cores[0].masked = true;
    cur = 6;
    for (u32 i = 0; i < NR_SGIS; i++)
        guest_send(sgi_to(0, i));
    settle(0);
    CHECK(total(0) == 0 && nr_valid_lrs(0) == 2);
    cores[0].masked = false;
    settle(0);
    for (u32 i = 0; i < NR_SGIS; i++)
        CHECK(taken(0, i) == 1);
    printf("ok   16 pending from another core with 2 list registers\n");

    /* Sent again while pending: once */
    reset(8);
    cur = 6;
    cores[6].masked = true;
    guest_send(sgi_to(6, 3));
    guest_send(sgi_to(6, 3));
    guest_send(sgi_to(6, 3));
    CHECK(nr_valid_lrs(6) == 1);
    cores[6].masked = false;
    settle(6);
    CHECK(taken(6, 3) == 1);
    printf("ok   sent three times while pending\n");

    /* Sent again while active: active and pending, taken a second time after the EOI */
    reset(8);
    cur = 6;
    guest_send(sgi_to(6, 5));
    CHECK(guest_ack() == 5);
    guest_send(sgi_to(6, 5));
    CHECK(nr_valid_lrs(6) == 1 && FIELD_GET(ICH_LR_STATE, cores[6].lr[0]) == 3);
    guest_send(sgi_to(6, 5)); /* and once more: still one pending state */
    CHECK(nr_valid_lrs(6) == 1 && FIELD_GET(ICH_LR_STATE, cores[6].lr[0]) == 3);
    CHECK(guest_ack() == 1023); /* (the model has no running priority: nothing else is pending) */
    guest_eoi(5);
    CHECK(eisr(&cores[6]) == 0); /* still pending: no maintenance interrupt yet */
    CHECK(guest_ack() == 5);
    guest_eoi(5);
    CHECK(eisr(&cores[6]) == 1);
    settle(6);
    CHECK(nr_valid_lrs(6) == 0 && cores[6].lr[0] == 0);
    printf("ok   sent again while active\n");

    /* The level-sensitive timer keeps its behaviour and shares the list registers */
    reset(2);
    cur = 6;
    cores[6].timer = true;
    hv_exit();
    hv_exit();
    CHECK(nr_valid_lrs(6) == 1);
    guest_send(sgi_to(6, 1));
    guest_send(sgi_to(6, 2));
    CHECK(nr_valid_lrs(6) == 2); /* timer and SGI 1; SGI 2 waits */
    cores[6].timer = false;
    hv_exit();
    CHECK(nr_valid_lrs(6) == 2); /* SGI 2 took the place of the timer */
    settle(6);
    CHECK(taken(6, 1) == 1 && taken(6, 2) == 1 && taken(6, HV_VGIC_INTID_VTIMER) == 0);
    printf("ok   timer and SGIs share the list registers\n");
}

/* ---- the Distributor and the Redistributors, used the way the Windows kernel uses them ---- */

/* What GICR_TYPER[63:32] and GICD_IROUTER hold for a core */
static u32 aff_of(int cpu)
{
    return (((cores[cpu].mpidr >> 32) & 0xff) << 24) | (cores[cpu].mpidr & 0xffffff);
}

/* HalpGic3ValidateIoUnit and HalpGic3InitializeIoUnit, findings/0004 sections 2 and 6 */
static void windows_dist_init(void)
{
    u32 pidr2 = rd32(GICD(GICD_PIDR2));
    u32 typer = rd32(GICD(GICD_TYPER));
    u32 ctlr;

    CHECK((pidr2 & 0xf0) >= 0x30);
    CHECK(!(typer & BIT(10)) && !(typer & BIT(17)) && !(typer & BIT(8)));
    CHECK(((typer & 0x1f) + 1) * 32 == 1024);

    ctlr = rd32(GICD(GICD_CTLR));
    CHECK(ctlr & BIT(6)); /* single Security state: DS has to read 1 */
    if (ctlr & BIT(0)) {
        ctlr &= ~BIT(0);
        wr32(GICD(GICD_CTLR), ctlr);
    }
    if (ctlr & BIT(1)) {
        ctlr &= ~BIT(1);
        wr32(GICD(GICD_CTLR), ctlr);
    }
    if (!(ctlr & BIT(4))) {
        wr32(GICD(GICD_CTLR), ctlr | BIT(4));
        ctlr = rd32(GICD(GICD_CTLR));
        CHECK(ctlr & BIT(4));
    }
    for (int n = 1; n < 32; n++) {
        wr32(GICD(GICD_ICENABLER + 4 * n), ~0U);
        wr32(GICD(GICD_IGROUPR + 4 * n), ~0U);
    }
    for (int n = 32; n < 1020; n++)
        wr64(GICD(GICD_IROUTER + 8 * n), 0);
    wr32(GICD(GICD_CTLR), ctlr | BIT(1));
}

/* HalpGic3FindRedistributor: walk the frames until GICR_TYPER has the affinity of this core */
static u64 windows_find_redist(void)
{
    u64 base = HV_VGIC_REDIST_BASE;

    for (;;) {
        u64 typer = rd64(base + GICR_TYPER);

        if ((u32)(typer >> 32) == aff_of(cur))
            return base;
        CHECK(!(typer & BIT(4))); /* Last */
        base += HV_VGIC_REDIST_SIZE;
    }
}

/* The Redistributor part of HalpGic3InitializeLocalUnit, on the current core */
static void windows_local_init(void)
{
    u64 base = windows_find_redist();

    CHECK(base == GICR(cur, 0));
    wr32(base + GICR_IGROUPR0, ~0U);
    wr32(base + GICR_ICENABLER0, ~0xff00U); /* SGIs 8-15 are left to the firmware */
}

/* HalpGic3SetLineState for an SGI or PPI of the current core */
static void windows_enable_local(u32 intid, u8 prio)
{
    u64 cfg = GICR(cur, intid < 16 ? GICR_ICFGR0 : GICR_ICFGR1);

    wr8(GICR(cur, GICR_IPRIORITYR + intid), prio);
    wr32(cfg, rd32(cfg) & ~(2U << (2 * (intid % 16))));
    wr32(GICR(cur, GICR_ISENABLER0), BIT(intid));
}

/* ...and for an SPI, routed to one core */
static void windows_enable_spi(u32 intid, u8 prio, int cpu, bool edge)
{
    u64 cfg = GICD(GICD_ICFGR + 4 * (intid / 16));
    u32 bit = 2U << (2 * (intid % 16));

    wr8(GICD(GICD_IPRIORITYR + intid), prio);
    wr32(cfg, edge ? rd32(cfg) | bit : rd32(cfg) & ~bit);
    wr64(GICD(GICD_IROUTER + 8 * intid), aff_of(cpu));
    wr32(GICD(GICD_ISENABLER + 4 * (intid / 32)), BIT(intid % 32));
}

/* HalpGic3RequestInterrupt for an SPI */
static void windows_request_spi(u32 intid)
{
    wr32(GICD(GICD_ISPENDR + 4 * (intid / 32)), BIT(intid % 32));
}

static bool spi_pending(u32 intid)
{
    return rd32(GICD(GICD_ISPENDR + 4 * (intid / 32))) & BIT(intid % 32);
}

static void settle_all(void)
{
    for (int round = 0; round < 3; round++)
        for (int i = 0; i < NR_STARTED; i++)
            settle(started[i]);
}

/*
 * While CPU 0 holds SPI 104 in its hands, looking for a list register: CPU 6 gives the SPI to
 * CPU 7, and CPU 7 looks for it at once.
 */
static void reroute_104_to_7(void)
{
    cur = 6;
    CHECK(!spi_pending(104)); /* CPU 0 has claimed it */
    wr64(GICD(GICD_IROUTER + 8 * 104), aff_of(7));
    cur = 7;
    CHECK(physical_interrupt());
    CHECK(!lr_of(7, 104));
}

static void dist_tests(void)
{
    /* The state the guest finds: nothing enabled, nothing delivered */
    reset_raw(8);
    cur = 6;
    CHECK((rd32(GICD(GICD_CTLR)) & 0x53) == 0x50); /* ARE and DS read 1, the groups are off */
    CHECK(rd32(GICR(6, GICR_ISENABLER0)) == 0);
    guest_send(sgi_to(6, 1));
    guest_send(sgi_to(0, 1));
    cores[6].timer = true;
    hv_exit();
    settle_all();
    CHECK(total(6) == 0 && total(0) == 0 && nr_valid_lrs(6) == 0 && nr_valid_lrs(0) == 0);
    cores[6].timer = false;
    printf("ok   nothing is delivered before the guest has enabled it\n");

    /* The start of the Windows kernel, boot core first */
    reset_raw(8);
    cur = 6;
    windows_dist_init();
    CHECK((rd32(GICD(GICD_CTLR)) & 0x53) == 0x52);
    for (int n = 1; n < 32; n++) {
        CHECK(rd32(GICD(GICD_ISENABLER + 4 * n)) == 0);
        /* Every interrupt is Group 1; 1020-1023 are not interrupts */
        CHECK(rd32(GICD(GICD_IGROUPR + 4 * n)) == (n == 31 ? 0x0fffffffU : ~0U));
    }
    for (int i = 0; i < NR_STARTED; i++) {
        cur = started[i];
        windows_local_init();
        CHECK(rd32(GICR(cur, GICR_IGROUPR0)) == ~0U);
        CHECK(rd32(GICR(cur, GICR_ISENABLER0)) == 0);
    }
    printf("ok   Distributor and Redistributor initialisation of the Windows kernel\n");

    /* The frames: one per core that runs m1n1, at its index, the last one marked */
    for (int cpu = 0; cpu < 10; cpu++) {
        u64 typer = rd64(GICR(cpu, GICR_TYPER));

        CHECK((u32)(typer >> 32) == aff_of(cpu));
        CHECK(((typer >> 8) & 0xffff) == (u64)cpu);
        CHECK(!!(typer & BIT(4)) == (cpu == 9));
        CHECK(!(typer & 0xf)); /* no LPIs of any kind */
        CHECK(rd32(GICR(cpu, GICR_TYPER)) == (u32)typer);
        CHECK(rd32(GICR(cpu, GICR_TYPER + 4)) == (u32)(typer >> 32));
        CHECK((rd32(GICR(cpu, GICR_PIDR2)) & 0xf0) == 0x30);
        CHECK(!(rd32(GICR(cpu, GICR_WAKER)) & 0x6)); /* awake */
    }
    {
        u64 val = 0;

        CHECK(!guest_mmio(GICR(10, 0), &val, false, 2)); /* nothing behind the last frame */
    }
    printf("ok   Redistributor frames\n");

    /* An SGI is held back until it is enabled, and has the priority the guest gave it */
    cur = 6;
    guest_send(sgi_to(6, 2));
    guest_send(sgi_to(0, 2));
    settle_all();
    CHECK(taken(6, 2) == 0 && taken(0, 2) == 0);
    CHECK(rd32(GICR(6, GICR_ISPENDR0)) == BIT(2));
    CHECK(rd32(GICR(0, GICR_ISPENDR0)) == BIT(2));
    cur = 6;
    windows_enable_local(2, 0x40);
    CHECK(rd32(GICR(6, GICR_ISENABLER0)) == BIT(2));
    CHECK(FIELD_GET(ICH_LR_PRIORITY, lr_of(6, 2)) == 0x40);
    CHECK(rd32(GICR(6, GICR_ISPENDR0)) == BIT(2)); /* pending, now in a list register */
    settle_all();
    CHECK(taken(6, 2) == 1 && taken(0, 2) == 0);
    CHECK(rd32(GICR(6, GICR_ISPENDR0)) == 0);
    cur = 0;
    windows_enable_local(2, 0xc0);
    CHECK(FIELD_GET(ICH_LR_PRIORITY, lr_of(0, 2)) == 0xc0);
    settle_all();
    CHECK(taken(6, 2) == 1 && taken(0, 2) == 1);
    printf("ok   SGI held back until enabled, priority from GICR_IPRIORITYR\n");

    /* Writing one priority byte leaves the other three alone; so does a 4 byte write elsewhere */
    cur = 6;
    wr32(GICR(6, GICR_IPRIORITYR + 4), 0x11223344);
    wr8(GICR(6, GICR_IPRIORITYR + 5), 0xa0);
    CHECK(rd32(GICR(6, GICR_IPRIORITYR + 4)) == 0x1122a044);
    CHECK(rd8(GICR(6, GICR_IPRIORITYR + 5)) == 0xa0 && rd8(GICR(6, GICR_IPRIORITYR + 2)) == 0x40);
    CHECK(rd32(GICR(6, GICR_IPRIORITYR)) == 0x00400000);
    wr32(GICD(GICD_IPRIORITYR + 40), 0x55667788);
    wr8(GICD(GICD_IPRIORITYR + 42), 0x10);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 40)) == 0x55107788);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 36)) == 0 && rd32(GICD(GICD_IPRIORITYR + 44)) == 0);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 0)) == 0); /* SGIs and PPIs are not in the Distributor */
    CHECK(rd32(GICR(0, GICR_IPRIORITYR)) == 0x00c00000); /* each core has its own */
    printf("ok   priority registers, by byte\n");

    /* Disabled while it waits in a list register: taken back, delivered once enabled again */
    cur = 6;
    cores[6].masked = true;
    guest_send(sgi_to(6, 2));
    CHECK(lr_of(6, 2));
    wr32(GICR(6, GICR_ICENABLER0), BIT(2));
    CHECK(!lr_of(6, 2) && rd32(GICR(6, GICR_ISENABLER0)) == 0);
    CHECK(rd32(GICR(6, GICR_ISPENDR0)) == BIT(2));
    cores[6].masked = false;
    settle_all();
    CHECK(taken(6, 2) == 1);
    wr32(GICR(6, GICR_ISENABLER0), BIT(2));
    settle_all();
    CHECK(taken(6, 2) == 2);
    printf("ok   SGI disabled while pending\n");

    /* Disabled while active and pending: the handler finishes, the second one waits */
    cur = 6;
    guest_send(sgi_to(6, 2));
    CHECK(guest_ack() == 2);
    guest_send(sgi_to(6, 2));
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, 2)) == 3);
    wr32(GICR(6, GICR_ICENABLER0), BIT(2));
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, 2)) == ICH_LR_STATE_ACTIVE);
    guest_eoi(2);
    settle_all();
    CHECK(taken(6, 2) == 2 && nr_valid_lrs(6) == 0);
    wr32(GICR(6, GICR_ISENABLER0), BIT(2));
    settle_all();
    CHECK(taken(6, 2) == 3);
    printf("ok   SGI disabled while active and pending\n");

    /* Another core enables it: this core has to be told */
    cur = 0;
    wr32(GICR(0, GICR_ICENABLER0), BIT(2));
    cur = 6;
    guest_send(sgi_to(0, 2));
    settle_all();
    CHECK(taken(0, 2) == 1);
    cur = 6;
    wr32(GICR(0, GICR_ISENABLER0), BIT(2));
    CHECK(atomic_load(&cores[0].ipi));
    settle_all();
    CHECK(taken(0, 2) == 2);
    printf("ok   SGI enabled from another core\n");

    /* The Group 1 enable of the Distributor gates everything, and takes pending ones back */
    cur = 6;
    cores[6].masked = true;
    guest_send(sgi_to(6, 2));
    CHECK(lr_of(6, 2));
    wr32(GICD(GICD_CTLR), 0x50);
    CHECK(!lr_of(6, 2));
    guest_send(sgi_to(0, 2));
    cores[6].masked = false;
    settle_all();
    CHECK(taken(6, 2) == 3 && taken(0, 2) == 2);
    cur = 6;
    wr32(GICD(GICD_CTLR), 0x52);
    settle_all();
    CHECK(taken(6, 2) == 4 && taken(0, 2) == 3);
    printf("ok   GICD_CTLR.EnableGrp1\n");

    /* Two SGIs with different priorities: the mask of the guest lets one through */
    cur = 6;
    windows_enable_local(3, 0xc0);
    cores[6].pmr = 0x80;
    guest_send(sgi_to(6, 3));
    guest_send(sgi_to(6, 2));
    CHECK(guest_irq() == 2 && guest_irq() == 1023);
    cores[6].pmr = 0x100;
    CHECK(guest_irq() == 3);
    settle_all();
    printf("ok   two priorities\n");

    /* The timer: level-sensitive, PPI 27 */
    cur = 6;
    cores[6].timer = true;
    hv_exit();
    CHECK(nr_valid_lrs(6) == 0);
    CHECK(rd32(GICR(6, GICR_ISPENDR0)) == BIT(27)); /* high, not enabled: pending */
    windows_enable_local(27, 0x20);
    CHECK(FIELD_GET(ICH_LR_PRIORITY, lr_of(6, 27)) == 0x20);
    CHECK(guest_ack() == 27);
    hv_exit();
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, 27)) == ICH_LR_STATE_ACTIVE); /* level: no second one */
    cores[6].timer = false; /* the handler reprogrammed the timer */
    guest_eoi(27);
    settle_all();
    CHECK(nr_valid_lrs(6) == 0);
    cur = 6;
    cores[6].timer = true;
    cores[6].masked = true;
    hv_exit();
    CHECK(lr_of(6, 27));
    wr32(GICR(6, GICR_ICENABLER0), BIT(27));
    CHECK(!lr_of(6, 27));
    wr32(GICR(6, GICR_ISENABLER0), BIT(27));
    CHECK(lr_of(6, 27));
    cores[6].timer = false;
    hv_exit();
    CHECK(!lr_of(6, 27) && rd32(GICR(6, GICR_ISPENDR0)) == 0);
    cores[6].masked = false;
    printf("ok   timer PPI follows its enable bit\n");

    /* A software-triggered SPI goes to the core GICD_IROUTER names */
    cur = 6;
    windows_enable_spi(40, 0x60, 6, true);
    CHECK(rd64(GICD(GICD_IROUTER + 8 * 40)) == aff_of(6));
    CHECK(rd32(GICD(GICD_IROUTER + 8 * 40)) == aff_of(6) &&
          rd32(GICD(GICD_IROUTER + 8 * 40 + 4)) == 0);
    CHECK(rd32(GICD(GICD_ISENABLER + 4)) == BIT(8) && rd32(GICD(GICD_ICENABLER + 4)) == BIT(8));
    CHECK(rd32(GICD(GICD_ICFGR + 4 * 2)) == (2U << 16));
    windows_request_spi(40);
    CHECK(FIELD_GET(ICH_LR_PRIORITY, lr_of(6, 40)) == 0x60 && (lr_of(6, 40) & ICH_LR_GROUP1));
    CHECK(spi_pending(40));
    settle_all();
    CHECK(taken(6, 40) == 1 && total(0) == 3 && !spi_pending(40));

    windows_enable_spi(41, 0x60, 0, false);
    settle_all(); /* enabling it called every core in */
    cur = 6;
    windows_request_spi(41);
    CHECK(!lr_of(6, 41) && atomic_load(&cores[0].ipi) && !atomic_load(&cores[7].ipi));
    CHECK(spi_pending(41));
    settle_all();
    CHECK(taken(0, 41) == 1 && taken(6, 41) == 0 && taken(7, 41) == 0 && taken(1, 41) == 0);
    printf("ok   software-triggered SPI, to this core and to another\n");

    /* Not enabled: pending until it is */
    cur = 7;
    wr8(GICD(GICD_IPRIORITYR + 100), 0x30);
    wr64(GICD(GICD_IROUTER + 8 * 100), aff_of(1));
    windows_request_spi(100);
    settle_all();
    CHECK(total(1) == 0 && spi_pending(100));
    cur = 6;
    wr32(GICD(GICD_ISENABLER + 4 * 3), BIT(4));
    settle_all();
    CHECK(taken(1, 100) == 1 && !spi_pending(100));
    printf("ok   SPI held back until enabled\n");

    /* Routed to a core the guest is not on: pending until the route changes */
    cur = 6;
    windows_enable_spi(101, 0x30, 8, true);
    windows_request_spi(101);
    settle_all();
    CHECK(spi_pending(101) && total(8) == 0 && !atomic_load(&cores[8].ipi));
    wr64(GICD(GICD_IROUTER + 8 * 101), aff_of(7));
    settle_all();
    CHECK(taken(7, 101) == 1 && !spi_pending(101));
    /* ...and an affinity that no core has */
    wr64(GICD(GICD_IROUTER + 8 * 101), 0x030303);
    windows_request_spi(101);
    settle_all();
    CHECK(spi_pending(101) && taken(7, 101) == 1);
    /* Interrupt_Routing_Mode: any core will do */
    wr64(GICD(GICD_IROUTER + 8 * 101), BIT(31));
    CHECK(rd64(GICD(GICD_IROUTER + 8 * 101)) & BIT(31));
    settle_all();
    CHECK(!spi_pending(101));
    CHECK(taken(6, 101) + taken(0, 101) + taken(7, 101) + taken(1, 101) == 2);
    printf("ok   SPI routes: a core without the guest, no such core, any core\n");

    /* Rerouted while it waits in a list register of the old target */
    cur = 6;
    windows_enable_spi(102, 0x30, 6, true);
    cores[6].masked = true;
    windows_request_spi(102);
    CHECK(lr_of(6, 102));
    wr64(GICD(GICD_IROUTER + 8 * 102), aff_of(1));
    CHECK(!lr_of(6, 102));
    cores[6].masked = false;
    settle_all();
    CHECK(taken(6, 102) == 0 && taken(1, 102) == 1);
    /* Disabled by another core while it waits here */
    cores[1].masked = true;
    cur = 6;
    windows_request_spi(102);
    settle_all();
    CHECK(lr_of(1, 102));
    cur = 6;
    wr32(GICD(GICD_ICENABLER + 4 * 3), BIT(6));
    settle_all();
    CHECK(!lr_of(1, 102) && spi_pending(102));
    cores[1].masked = false;
    settle_all();
    CHECK(taken(1, 102) == 1);
    wr32(GICD(GICD_ISENABLER + 4 * 3), BIT(6));
    settle_all();
    CHECK(taken(1, 102) == 2 && !spi_pending(102));
    printf("ok   SPI rerouted and disabled while pending\n");

    /* Requested again while pending: once. While active: once more after the EOI. */
    cur = 6;
    windows_enable_spi(103, 0x30, 6, true);
    cores[6].masked = true;
    windows_request_spi(103);
    windows_request_spi(103);
    cores[6].masked = false;
    settle_all();
    CHECK(taken(6, 103) == 1);
    cur = 6;
    windows_request_spi(103);
    CHECK(guest_ack() == 103);
    windows_request_spi(103);
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, 103)) == 3);
    guest_eoi(103);
    settle_all();
    CHECK(taken(6, 103) == 2 && nr_valid_lrs(6) == 0);
    printf("ok   SPI requested again while pending and while active\n");

    /* More SPIs than list registers, from another core */
    reset(2);
    cur = 6;
    for (u32 intid = 200; intid < 232; intid++)
        windows_enable_spi(intid, 0x80, 0, true);
    cores[0].masked = true;
    wr32(GICD(GICD_ISPENDR + 4 * 6), 0xffffff00);
    wr32(GICD(GICD_ISPENDR + 4 * 7), 0x000000ff);
    settle_all();
    CHECK(nr_valid_lrs(0) == 2 && total(0) == 0);
    cores[0].masked = false;
    settle_all();
    for (u32 intid = 200; intid < 232; intid++)
        CHECK(taken(0, intid) == 1);
    CHECK(total(0) == 32 && rd32(GICD(GICD_ISPENDR + 4 * 6)) == 0);
    printf("ok   32 SPIs pending with 2 list registers\n");

    /*
     * An SPI finds no free list register on its core. In that very moment it is rerouted, and
     * the new core has already looked for it: the old core has to tell the new one.
     */
    reset(2);
    cur = 6;
    windows_enable_spi(104, 0x80, 0, true);
    settle_all();
    cores[0].masked = true;
    cur = 6;
    guest_send(sgi_to(0, 1));
    guest_send(sgi_to(0, 2));
    settle_all();
    CHECK(nr_valid_lrs(0) == 2);
    cur = 6;
    windows_request_spi(104);
    /*
     * CPU 0 answers the IPI. With 2 list registers it reads them twice for the timer and twice
     * in vgic_resync(); the fifth read is vgic_inject() looking for a place for the SPI.
     */
    lr_read_hook = reroute_104_to_7;
    lr_read_countdown = 5;
    settle(0);
    CHECK(!lr_read_hook);
    cores[0].masked = false;
    settle_all();
    CHECK(taken(7, 104) == 1 && taken(0, 104) == 0 && !spi_pending(104));
    CHECK(taken(0, 1) == 1 && taken(0, 2) == 1);
    printf("ok   SPI rerouted while its core found no list register for it\n");

    /* What the registers accept and what they do not */
    reset_raw(8);
    cur = 6;
    wr32(GICD(GICD_CTLR), 0xffffffff);
    CHECK(rd32(GICD(GICD_CTLR)) == 0x53);
    wr32(GICD(GICD_CTLR), 0x52);
    CHECK(rd32(GICD(GICD_TYPER)) == 0x0078001f);
    CHECK(rd32(GICD(GICD_PIDR2)) == 0x30);
    wr32(GICD(GICD_IGROUPR + 4), 0); /* there is only Group 1 */
    CHECK(rd32(GICD(GICD_IGROUPR + 4)) == ~0U && rd32(GICD(GICD_IGROUPR)) == 0);
    wr32(GICR(6, GICR_IGROUPR0), 0);
    CHECK(rd32(GICR(6, GICR_IGROUPR0)) == ~0U);
    /* SGIs and PPIs are not in the Distributor when affinity routing is on */
    wr32(GICD(GICD_ISENABLER), ~0U);
    wr32(GICD(GICD_ISPENDR), ~0U);
    wr32(GICD(GICD_ICFGR), ~0U);
    wr32(GICD(GICD_ICFGR + 4), ~0U);
    CHECK(rd32(GICD(GICD_ISENABLER)) == 0 && rd32(GICD(GICD_ISPENDR)) == 0);
    CHECK(rd32(GICD(GICD_ICFGR)) == 0 && rd32(GICD(GICD_ICFGR + 4)) == 0);
    CHECK(rd32(GICR(6, GICR_ISENABLER0)) == 0);
    /* 1020-1023 are not interrupts */
    wr32(GICD(GICD_ISENABLER + 4 * 31), ~0U);
    CHECK(rd32(GICD(GICD_ISENABLER + 4 * 31)) == 0x0fffffff);
    wr32(GICD(GICD_ISPENDR + 4 * 31), 0xf0000000);
    CHECK(rd32(GICD(GICD_ISPENDR + 4 * 31)) == 0);
    wr32(GICD(GICD_IPRIORITYR + 1020), ~0U);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 1020)) == 0);
    wr32(GICD(GICD_IPRIORITYR + 1016), ~0U);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 1016)) == ~0U);
    wr64(GICD(GICD_IROUTER + 8 * 1020), 1);
    CHECK(rd64(GICD(GICD_IROUTER + 8 * 1020)) == 0 && rd64(GICD(GICD_IROUTER + 8 * 31)) == 0);
    /* Trigger: one bit per interrupt can be written; SGIs are always edge-triggered */
    wr32(GICD(GICD_ICFGR + 4 * 2), ~0U);
    CHECK(rd32(GICD(GICD_ICFGR + 4 * 2)) == 0xaaaaaaaa);
    wr32(GICD(GICD_ICFGR + 4 * 63), ~0U);
    CHECK(rd32(GICD(GICD_ICFGR + 4 * 63)) == 0x00aaaaaa);
    wr32(GICR(6, GICR_ICFGR0), 0);
    wr32(GICR(6, GICR_ICFGR1), ~0U);
    CHECK(rd32(GICR(6, GICR_ICFGR0)) == 0xaaaaaaaa && rd32(GICR(6, GICR_ICFGR1)) == 0xaaaaaaaa);
    CHECK(rd32(GICR(0, GICR_ICFGR1)) == 0);
    /* The route: Aff2.Aff1.Aff0 and the mode bit; in two halves as well */
    wr64(GICD(GICD_IROUTER + 8 * 50), ~0UL);
    CHECK(rd64(GICD(GICD_IROUTER + 8 * 50)) == 0x80ffffff);
    wr32(GICD(GICD_IROUTER + 8 * 50), 0x010101);
    wr32(GICD(GICD_IROUTER + 8 * 50 + 4), 0xff);
    CHECK(rd64(GICD(GICD_IROUTER + 8 * 50)) == 0x010101);
    /* Writes that are narrower than the register are ignored, except for priorities */
    wr32(GICD(GICD_ISENABLER + 4), 0);
    wr8(GICD(GICD_ISENABLER + 4), 0xff);
    wr(GICD(GICD_ISENABLER + 4), 0xffff, 1);
    CHECK(rd32(GICD(GICD_ISENABLER + 4)) == 0);
    wr(GICD(GICD_IPRIORITYR + 60), 0xbbaa, 1);
    CHECK(rd32(GICD(GICD_IPRIORITYR + 60)) == 0xbbaa &&
          rd(GICD(GICD_IPRIORITYR + 60), 1) == 0xbbaa);
    /* Set-pending of the Redistributor is not something Windows writes: ignored */
    wr32(GICR(6, GICR_ISENABLER0), ~0U);
    wr32(GICR(6, GICR_ISPENDR0), ~0U);
    settle_all();
    CHECK(total(6) == 0 && rd32(GICR(6, GICR_ISPENDR0)) == 0);
    printf("ok   register details\n");

    /*
     * A crash dump reads these ranges of the Distributor and of the Redistributor of each core
     * (HalpGicdReadableRanges, HalpGicrReadableRanges, findings/0004 sections 6 and 7). Every
     * read has to be answered; so does a write anywhere in the frames.
     */
    static const u32 gicd_ranges[][2] = {{0x0, 0x10},      {0x80, 0xf00},    {0xf10, 0xf30},
                                         {0x1000, 0x3700}, {0x6100, 0xa000}, {0xffd0, 0x10000}};
    static const u32 gicr_ranges[][2] = {
        {0x0, 0x20}, {0x70, 0x80}, {0xc0, 0xc8}, {0xffd0, 0x10000}, {0x10080, 0x10e00}};
    for (unsigned r = 0; r < sizeof(gicd_ranges) / sizeof(gicd_ranges[0]); r++)
        for (u32 off = gicd_ranges[r][0]; off < gicd_ranges[r][1]; off += 4)
            rd32(GICD(off));
    for (int i = 0; i < NR_STARTED; i++)
        for (unsigned r = 0; r < sizeof(gicr_ranges) / sizeof(gicr_ranges[0]); r++)
            for (u32 off = gicr_ranges[r][0]; off < gicr_ranges[r][1]; off += 4)
                rd32(GICR(started[i], off));
    for (u32 off = 0; off < 0x10000; off += 4) {
        if (off != GICD_CTLR)
            wr32(GICD(off), 0);
        rd32(GICD(off));
    }
    for (u32 off = 0; off < 0x20000; off += 4) {
        wr32(GICR(7, off), 0);
        rd32(GICR(7, off));
        rd8(GICR(7, off + 1));
        rd64(GICR(7, off & ~7U));
    }
    CHECK((rd32(GICD(GICD_CTLR)) & 0x53) == 0x52);
    printf("ok   crash dump ranges and every other offset\n");

    /* Only some cores run m1n1: their frames stay where they are, the others are empty */
    alive = BIT(0) | BIT(6) | BIT(7);
    reset_raw(8);
    CHECK(rd64(GICR(7, GICR_TYPER)) == (((u64)aff_of(7) << 32) | (7 << 8) | BIT(4)));
    CHECK(rd64(GICR(6, GICR_TYPER)) == (((u64)aff_of(6) << 32) | (6 << 8)));
    CHECK(rd64(GICR(3, GICR_TYPER)) == (3 << 8));
    wr32(GICR(3, GICR_ISENABLER0), ~0U);
    CHECK(rd32(GICR(3, GICR_ISENABLER0)) == 0 && rd32(GICR(3, GICR_PIDR2)) == 0x30);
    {
        u64 val = 0;

        CHECK(!guest_mmio(GICR(8, 0), &val, false, 2));
    }
    for (int i = 0; i < 3; i++) {
        cur = (int[]){6, 0, 7}[i];
        CHECK(windows_find_redist() == GICR(cur, 0));
    }
    alive = 0x3ff;
    printf("ok   frames when only some cores run m1n1\n");
}

/* ---- AIC lines, forwarded as SPIs ---- */

/*
 * The device behind a line, and the guest's handler for its SPI: it lowers the line once the
 * guest has handled it `times` times. It may disable the SPI, and enable it again, while it
 * handles it. It notes whether AIC had the line unmasked while the guest had it.
 */
static struct {
    u32 spi, line;
    int times, seen;
    bool disable, enable_again;
    bool pend_again; // the guest makes the SPI pending again, the first time it handles it
    bool all;        // ...any SPI of the table: its line is lowered at once
    bool unmasked_in_flight;
} dev;

static void device_handler(u32 intid)
{
    u64 word = 4 * (intid / 32);

    if (dev.all && intid >= 32 && intid < 32 + sizeof(table_lines) / sizeof(table_lines[0])) {
        dev.seen++;
        line_raise(table_lines[intid - 32], false);
        return;
    }
    if (intid != dev.spi)
        return;
    dev.seen++;
    if (dev.pend_again && dev.seen == 1)
        windows_request_spi(intid);
    dev.unmasked_in_flight |= !line_masked[dev.line];
    if (dev.disable)
        wr32(GICD(GICD_ICENABLER + word), BIT(intid % 32));
    if (dev.enable_again)
        wr32(GICD(GICD_ISENABLER + word), BIT(intid % 32));
    dev.unmasked_in_flight |= !line_masked[dev.line];
    if (dev.seen >= dev.times)
        line_raise(dev.line, false);
}

static void device(u32 spi, u32 line, int times)
{
    memset(&dev, 0, sizeof(dev));
    dev.spi = spi;
    dev.line = line;
    dev.times = times;
}

/* The hypervisor starts with every line unmasked by whoever came before, none of them high */
/* AIC as hv_init() left it, before hv_start() enabled it */
static int mask_all_at_init;

/*
 * The hypervisor starts with every line unmasked by whoever came before, none of them high,
 * and AIC not enabled; and the guest is entered (hv_start(): hv_vgic_start()), unless there
 * is no table, without which hv_start() does not enter it
 */
static void aic_reset(bool table, int lrs)
{
    memset(line_high, 0, sizeof(line_high));
    memset(line_masked, 0, sizeof(line_masked));
    atomic_store(&lines_ready, 0);
    mask_all_calls = unmask_calls = 0;
    aic_config = AIC_CONFIG_M4;
    atomic_store(&aic_on, false);
    nr_aic_ops = 0;
    aicmap_fails = !table;
    reset(lrs);
    mask_all_at_init = mask_all_calls;
    cur = 6;
    if (table)
        hv_vgic_start();
    guest_handler = device_handler;
    memset(&dev, 0, sizeof(dev));
}

/* What hv_vgic_report() says of the AIC events of one core, and of IRQs with neither */
struct aic_events {
    long forwarded, late, stray, spurious;
};

static struct aic_events aic_events(int cpu)
{
    struct aic_events e = {0, 0, 0, 0};
    char line[512], key[32];
    FILE *f = tmpfile();
    int saved;

    CHECK(f);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0 && dup2(fileno(f), STDOUT_FILENO) >= 0);
    hv_vgic_report();
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    snprintf(key, sizeof(key), "HV: vGIC: CPU %d: ", cpu);
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        char *at = strstr(line, "AIC events: ");

        if (!strncmp(line, key, strlen(key)) && at)
            CHECK(sscanf(at,
                         "AIC events: %ld forwarded, %ld late, %ld stray; %ld IRQs with neither",
                         &e.forwarded, &e.late, &e.stray, &e.spurious) == 4);
    }
    fclose(f);
    return e;
}

static void aic_tests(void)
{
    struct aic_events e, before;
    int unmasked;

    /* Every line is masked when the hypervisor is initialized, by the boot core, once */
    aic_reset(true, 8);
    CHECK(hv_vgic_has_aicmap());
    CHECK(mask_all_at_init == 1);
    for (u32 line = 0; line < AIC_LINES; line++)
        CHECK(line_masked[line] || line % 4096 >= 2048);
    CHECK(unmask_calls == 0);

    /* A line that is raised while the guest has not enabled its SPI does not arrive */
    line_raise(LINE_A, true);
    expect_counts("AIC: a raised line whose SPI is not enabled does not arrive", 32, 0, 0, 0, 0);
    CHECK(line_masked[LINE_A]);

    /* Enabled: the line is unmasked, AIC reports it, the guest takes SPI 32 of the table */
    device(32, LINE_A, 1);
    cur = 6;
    windows_enable_spi(32, 0x80, 6, false);
    expect_counts("AIC: line 3 arrives as SPI 32 once the SPI is enabled", 32, 1, 0, 0, 0);
    CHECK(!dev.unmasked_in_flight);
    CHECK(!line_masked[LINE_A]);
    e = aic_events(6);
    CHECK(e.forwarded == 1 && e.late == 0 && e.stray == 0);

    /* Still high when the guest ends it: it arrives again; once low, no more */
    device(32, LINE_A, 3);
    line_raise(LINE_A, true);
    expect_counts("AIC: a line still high at the end arrives again, until it is low", 32, 4, 0, 0,
                  0);
    CHECK(!dev.unmasked_in_flight);
    CHECK(!line_masked[LINE_A]);

    /* Disabled while the line waits: masked; raised then, it does not arrive; enabled, it does */
    cur = 6;
    wr32(GICD(GICD_ICENABLER + 4), BIT(0));
    CHECK(line_masked[LINE_A]);
    device(32, LINE_A, 1);
    line_raise(LINE_A, true);
    expect_counts("AIC: disabled, the line is masked and does not arrive", 32, 4, 0, 0, 0);
    cur = 6;
    wr32(GICD(GICD_ISENABLER + 4), BIT(0));
    expect_counts("AIC: enabled again, the line that is high arrives", 32, 5, 0, 0, 0);
    CHECK(!line_masked[LINE_A]);

    /*
     * Disabled by the guest while it handles it: once ended, it stays masked, though high, and
     * AIC does not report it again: it is forwarded once, and nothing is pending
     */
    before = aic_events(6);
    device(32, LINE_A, 100);
    dev.disable = true;
    line_raise(LINE_A, true);
    expect_counts("AIC: disabled while the guest has it, it stays masked after the end", 32, 6, 0,
                  0, 0);
    CHECK(line_masked[LINE_A] && line_high[LINE_A]);
    e = aic_events(6);
    CHECK(e.forwarded == before.forwarded + 1);
    cur = 6;
    CHECK(!spi_pending(32));
    device(32, LINE_A, 1);
    cur = 6;
    wr32(GICD(GICD_ISENABLER + 4), BIT(0));
    expect_counts("AIC: ...and arrives when enabled again", 32, 7, 0, 0, 0);

    /* Disabled and enabled again while the guest has it: masked until the end, then again */
    device(32, LINE_A, 2);
    dev.disable = dev.enable_again = true;
    line_raise(LINE_A, true);
    expect_counts("AIC: off and on while the guest has it: masked until the end", 32, 9, 0, 0, 0);
    CHECK(!dev.unmasked_in_flight);
    CHECK(!line_masked[LINE_A]);

    /* Routed to another core: the core that reads the event makes it pending there */
    device(33, LINE_B, 1);
    cur = 6;
    windows_enable_spi(33, 0x80, 0, false);
    line_raise(LINE_B, true);
    expect_counts("AIC: line 40 arrives as SPI 33 on the core it is routed to", 33, 0, 1, 0, 0);
    CHECK(!line_masked[LINE_B]);

    /* The second die: die 1, line 7 of the event register is line 4096 + 7, SPI 35 */
    device(35, LINE_D, 1);
    cur = 6;
    windows_enable_spi(35, 0x80, 7, false);
    line_raise(LINE_D, true);
    expect_counts("AIC: line 7 of the second die arrives as SPI 35", 35, 0, 0, 1, 0);

    /* Group 1 off: the line is in flight and masked, the SPI waits; on again, it arrives */
    cur = 6;
    wr32(GICD(GICD_CTLR), 0);
    device(34, LINE_C, 1);
    windows_enable_spi(34, 0x80, 6, false);
    line_raise(LINE_C, true);
    expect_counts("AIC: with Group 1 off the SPI waits", 34, 0, 0, 0, 0);
    CHECK(line_masked[LINE_C]);
    cur = 6;
    CHECK(spi_pending(34));
    wr32(GICD(GICD_CTLR), 2);
    expect_counts("AIC: ...and arrives once it is on", 34, 1, 0, 0, 0);
    CHECK(!line_masked[LINE_C]);

    /*
     * An event the guest does not wait for is not forwarded, and the line stays masked: one
     * that is in no table (stray), and one of the table whose SPI is disabled, which AIC had
     * on its way when the guest disabled it (late)
     */
    e = aic_events(6);
    CHECK(e.late == 0 && e.stray == 0);
    pthread_mutex_lock(&bhl);
    line_set(99, true, false);
    pthread_mutex_unlock(&bhl);
    cur = 6;
    wr32(GICD(GICD_ICENABLER + 4), BIT(1));
    pthread_mutex_lock(&bhl);
    line_set(LINE_B, true, false);
    pthread_mutex_unlock(&bhl);
    settle(6);
    CHECK(line_masked[99] && line_masked[LINE_B]);
    e = aic_events(6);
    CHECK(e.late == 1 && e.stray == 1);
    expect_counts("AIC: events the guest does not wait for are not forwarded", 33, 0, 1, 0, 0);
    line_raise(99, false);
    line_raise(LINE_B, false);

    /* SPIs that are no line of the table: enabling them unmasks nothing */
    unmasked = unmask_calls;
    cur = 6;
    windows_enable_spi(36, 0x80, 6, false);
    windows_enable_spi(1019, 0x80, 6, false);
    CHECK(unmask_calls == unmasked);

    /* No table: the hypervisor says so, every line is still masked, none is ever unmasked */
    aic_reset(false, 8);
    CHECK(!hv_vgic_has_aicmap());
    CHECK(mask_all_at_init == 1);
    device(32, LINE_A, 1);
    cur = 6;
    windows_enable_spi(32, 0x80, 6, false);
    line_raise(LINE_A, true);
    expect_counts("AIC: without a table no line is forwarded", 32, 0, 0, 0, 0);
    CHECK(unmask_calls == 0 && line_masked[LINE_A]);
    line_raise(LINE_A, false);

    guest_handler = NULL;
    aicmap_fails = false;
}

/* What hv_vgic_report() prints of AIC's global configuration, "" if nothing */
static const char *aic_config_report(void)
{
    static char said[512];
    char line[512];
    FILE *f = tmpfile();
    int saved;

    said[0] = 0;
    CHECK(f);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0 && dup2(fileno(f), STDOUT_FILENO) >= 0);
    hv_vgic_report();
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(f);
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "AIC global configuration"))
            snprintf(said, sizeof(said), "%s", line);
    fclose(f);
    return said;
}

/*
 * AIC's global configuration: enabled right before the guest is entered, after every line has
 * been masked and every software trigger cleared, nothing but the enable bit changed; and
 * after the guest, every line masked again and the configuration as it was before
 */
static void aic_config_tests(void)
{
    aic_reset(true, 8);
    /* hv_init(): masked all, and left the configuration alone */
    CHECK(aic_ops[0].op == OP_MASK_ALL);
    /* hv_start(): masked all again, then read the configuration and set the enable bit */
    CHECK(nr_aic_ops == 4);
    CHECK(aic_ops[1].op == OP_MASK_ALL);
    CHECK(aic_ops[2].op == OP_CONFIG_READ && aic_ops[2].val == AIC_CONFIG_M4);
    CHECK(aic_ops[3].op == OP_CONFIG_WRITE && aic_ops[3].val == (AIC_CONFIG_M4 | 1));
    CHECK(aic_config == (AIC_CONFIG_M4 | 1));
    for (u32 line = 0; line < AIC_LINES; line++)
        CHECK(line_masked[line] || line % 4096 >= 2048);
    CHECK(!strcmp(aic_config_report(), "HV: vGIC: AIC global configuration 0xe0000000 before the "
                                       "guest, 0xe0000001 while it ran\n"));

    /* A line the guest has enabled, one in flight, one high */
    device(32, LINE_A, 1);
    cur = 6;
    windows_enable_spi(32, 0x80, 6, false);
    windows_enable_spi(33, 0x80, 6, false);
    cores[6].masked = true;
    line_raise(LINE_B, true);
    settle(6);
    CHECK(!line_masked[LINE_A] && line_masked[LINE_B]);
    cores[6].masked = false;

    /* The guest has been left: everything masked, the configuration as it was */
    nr_aic_ops = 0;
    hv_vgic_stop();
    CHECK(nr_aic_ops == 2);
    CHECK(aic_ops[0].op == OP_MASK_ALL);
    CHECK(aic_ops[1].op == OP_CONFIG_WRITE && aic_ops[1].val == AIC_CONFIG_M4);
    CHECK(aic_config == AIC_CONFIG_M4);
    for (u32 line = 0; line < AIC_LINES; line++)
        CHECK(line_masked[line] || line % 4096 >= 2048);
    /* ...and nothing more is said of it, nor restored a second time */
    CHECK(!strcmp(aic_config_report(), ""));
    nr_aic_ops = 0;
    hv_vgic_stop();
    CHECK(nr_aic_ops == 1 && aic_ops[0].op == OP_MASK_ALL);
    line_raise(LINE_B, false);
    printf("ok   AIC: enabled after masking everything, as it was again after the guest\n");

    /* Before hv_start() AIC reports nothing, though a line is high and unmasked */
    aic_reset(false, 8);
    pthread_mutex_lock(&bhl);
    line_set(LINE_A, true, false);
    pthread_mutex_unlock(&bhl);
    cur = 6;
    CHECK(!physical_interrupt());
    line_raise(LINE_A, false);
    printf("ok   AIC: not enabled, it reports nothing\n");

    guest_handler = NULL;
    aicmap_fails = false;
}

/* VGIC_UNMAPPED_LIMIT and VGIC_SPURIOUS_LIMIT of hv_vgic.c */
#define UNMAPPED_LIMIT (2 * AIC_MAX_HW_NUM)
#define SPURIOUS_LIMIT 16

/* hv_vgic_handle_irq() on this core with nothing pending, n times: how often it went on */
static int irqs_with_nothing(int n)
{
    int went_on = 0;

    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&bhl);
        went_on += hv_vgic_handle_irq();
        pthread_mutex_unlock(&bhl);
    }
    return went_on;
}

/* The two limits after which the hypervisor gives up, and what does not count towards them */
static void aic_limit_tests(void)
{
    struct aic_events e;
    int stopped_at = 0;

    /*
     * The guest disables the SPI of a line that waits, and AIC has the line on its way:
     * whenever that happens, as often as it happens, the hypervisor goes on
     */
    aic_reset(true, 8);
    cur = 6;
    windows_enable_spi(33, 0x80, 6, false);
    wr32(GICD(GICD_ICENABLER + 4), BIT(1));
    for (int i = 0; i < UNMAPPED_LIMIT + 10; i++) {
        pthread_mutex_lock(&bhl);
        line_set(LINE_B, true, false);
        pthread_mutex_unlock(&bhl);
        settle(6);
    }
    e = aic_events(6);
    CHECK(e.late == UNMAPPED_LIMIT + 10 && e.stray == 0 && e.forwarded == 0);
    CHECK(line_masked[LINE_B]);
    line_raise(LINE_B, false);
    expect_counts("AIC: events of a line disabled just before do not stop the hypervisor", 33, 0, 0,
                  0, 0);

    /* Events that masking should have kept away: it gives up at the limit, not before */
    aic_reset(true, 8);
    cur = 6;
    for (int i = 1; i <= UNMAPPED_LIMIT && !stopped_at; i++) {
        bool went_on;

        pthread_mutex_lock(&bhl);
        line_set(99, true, false);
        went_on = hv_vgic_handle_irq();
        pthread_mutex_unlock(&bhl);
        if (!went_on)
            stopped_at = i;
    }
    CHECK(stopped_at == UNMAPPED_LIMIT);
    CHECK(line_masked[99]);
    line_raise(99, false);
    printf("ok   AIC: stray events stop the hypervisor at the limit\n");

    /*
     * IRQs with neither an AIC event nor a maintenance interrupt: one less than the limit in
     * a row is fine; a line forwarded, or a maintenance interrupt, starts the count again;
     * the limit in a row is not
     */
    aic_reset(true, 8);
    cur = 6;
    CHECK(irqs_with_nothing(SPURIOUS_LIMIT - 1) == SPURIOUS_LIMIT - 1);
    device(32, LINE_A, 1);
    windows_enable_spi(32, 0x80, 6, false);
    cores[6].masked = true;
    line_raise(LINE_A, true);
    settle(6);
    CHECK(lr_of(6, 32));
    cur = 6;
    CHECK(irqs_with_nothing(SPURIOUS_LIMIT - 1) == SPURIOUS_LIMIT - 1);
    cores[6].masked = false;
    settle(6);
    CHECK(taken(6, 32) == 1);
    cur = 6;
    CHECK(irqs_with_nothing(SPURIOUS_LIMIT - 1) == SPURIOUS_LIMIT - 1);
    CHECK(irqs_with_nothing(1) == 0);
    e = aic_events(6);
    CHECK(e.spurious == 3 * (SPURIOUS_LIMIT - 1) + 1);
    printf("ok   AIC: IRQs with neither stop the hypervisor only so many in a row\n");

    guest_handler = NULL;
}

/* Orders of things that the guest and AIC can do to a line in flight */
static void aic_order_tests(void)
{
    struct aic_events e;

    /*
     * The SPI is in a list register, not acknowledged, when the guest disables it: it is
     * taken back and pending, the line stays in flight and masked. Enabled again, it arrives
     * once; also when the device has lowered the line meanwhile (a GIC would not deliver it
     * then: SiliconNT docs/findings/0060, conclusion 8)
     */
    aic_reset(true, 8);
    device(32, LINE_A, 1);
    cur = 6;
    windows_enable_spi(32, 0x80, 6, false);
    cores[6].masked = true;
    line_raise(LINE_A, true);
    settle(6);
    CHECK(lr_of(6, 32) && line_masked[LINE_A]);
    cur = 6;
    wr32(GICD(GICD_ICENABLER + 4), BIT(0));
    settle(6);
    cur = 6;
    CHECK(!lr_of(6, 32) && spi_pending(32) && line_masked[LINE_A]);
    cores[6].masked = false;
    expect_counts("AIC: disabled in a list register, it is taken back and does not arrive", 32, 0,
                  0, 0, 0);
    cur = 6;
    wr32(GICD(GICD_ISENABLER + 4), BIT(0));
    expect_counts("AIC: ...enabled again, it arrives once", 32, 1, 0, 0, 0);
    CHECK(!line_masked[LINE_A] && !line_high[LINE_A]);

    device(32, LINE_A, 1);
    cores[6].masked = true;
    line_raise(LINE_A, true);
    settle(6);
    cur = 6;
    wr32(GICD(GICD_ICENABLER + 4), BIT(0));
    line_raise(LINE_A, false);
    cores[6].masked = false;
    cur = 6;
    wr32(GICD(GICD_ISENABLER + 4), BIT(0));
    expect_counts("AIC: ...and once when the line went low meanwhile", 32, 2, 0, 0, 0);
    CHECK(!line_masked[LINE_A]);

    /*
     * The guest makes the SPI of a line pending itself (GICD_ISPENDR): while the line waits,
     * it arrives and the line still waits; while AIC's event has it pending already, the two
     * are one; while the guest handles it, it arrives a second time, and only the end of
     * that one unmasks the line
     */
    device(32, LINE_A, 1);
    cur = 6;
    windows_request_spi(32);
    expect_counts("AIC: made pending by the guest while its line waits, it arrives", 32, 3, 0, 0,
                  0);
    CHECK(!line_masked[LINE_A]);
    cores[6].masked = true;
    line_raise(LINE_A, true);
    settle(6);
    cur = 6;
    windows_request_spi(32);
    cores[6].masked = false;
    expect_counts("AIC: ...made pending by the guest while AIC's event has it pending, it "
                  "arrives once",
                  32, 4, 0, 0, 0);
    CHECK(!line_masked[LINE_A]);
    device(32, LINE_A, 2);
    dev.pend_again = true;
    line_raise(LINE_A, true);
    expect_counts("AIC: ...made pending by the guest while it handles it, it arrives twice", 32, 6,
                  0, 0, 0);
    CHECK(!dev.unmasked_in_flight && !line_masked[LINE_A]);

    /* Routed elsewhere while in flight: it arrives once, on the new core, and is ended there */
    device(33, LINE_B, 1);
    cur = 6;
    windows_enable_spi(33, 0x80, 0, false);
    cores[0].masked = true;
    line_raise(LINE_B, true);
    settle(6);
    settle(0);
    CHECK(lr_of(0, 33) && line_masked[LINE_B]);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(7));
    cores[0].masked = false;
    expect_counts("AIC: routed elsewhere while in flight, it arrives once on the new core", 33, 0,
                  0, 1, 0);
    CHECK(!line_masked[LINE_B]);

    /* More lines in flight than list registers: each arrives once, each line waits again */
    aic_reset(true, 1);
    memset(&dev, 0, sizeof(dev));
    dev.all = true;
    cur = 6;
    for (u32 spi = 32; spi < 36; spi++)
        windows_enable_spi(spi, 0x80, 6, false);
    cores[6].masked = true;
    for (u32 k = 0; k < 4; k++)
        line_raise(table_lines[k], true);
    settle(6);
    CHECK(nr_valid_lrs(6) == 1);
    for (u32 k = 0; k < 4; k++)
        CHECK(line_masked[table_lines[k]]);
    e = aic_events(6);
    CHECK(e.forwarded == 4);
    cores[6].masked = false;
    settle_all();
    for (u32 spi = 32; spi < 36; spi++)
        CHECK(taken(6, spi) == 1);
    for (u32 k = 0; k < 4; k++)
        CHECK(!line_masked[table_lines[k]]);
    printf("ok   AIC: four lines in flight with one list register: each arrives once\n");

    guest_handler = NULL;
}

/* ---- stress: every core sends to every core, one thread per core ---- */

static int rounds;
static atomic_int failed;
static u64 sent[MAX_CPUS][MAX_CPUS][NR_SGIS]; /* [sender slot][target][intid] */
static u64 spis_sent[MAX_CPUS];               /* [sender], all SPIs */
static bool with_spis;

static double seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void serve(void)
{
    if (!physical_interrupt())
        guest_irq();
}

/* Wait for a core to have taken an interrupt, serving this one in the meantime */
static bool wait_taken(int target, u32 intid, u32 before, double start)
{
    struct core *c = &cores[cur];
    u64 spins = 0;

    while (taken(target, intid) == before) {
        serve();
        if (c->masked && ++spins > 200)
            c->masked = false;
        if (!(++spins & 0xfffff) && seconds() - start > 20) {
            printf("FAIL stress: cpu %d waited 20 s for cpu %d to take %u\n", cur, target, intid);
            atomic_store(&failed, 1);
            return false;
        }
    }
    return true;
}

/*
 * One SPI of this sender's own four, the way Windows would use it: route it to some core,
 * request it (sometimes while it is disabled: see that it stays away, then enable it;
 * sometimes disabling and enabling it right behind the request), see that it arrives exactly
 * once. Other cores do the same with their SPIs and send SGIs at the same time.
 */
static bool spi_round(int slot, unsigned *seed)
{
    int target = started[rand_r(seed) % NR_STARTED];
    u32 intid = 64 + 32 * slot + rand_r(seed) % 4;
    u64 enable = GICD(GICD_ISENABLER + 4 * (intid / 32));
    u64 disable = GICD(GICD_ICENABLER + 4 * (intid / 32));
    u32 before = taken(target, intid);
    int mode = rand_r(seed) % 3; /* 0: plain, 1: held back, 2: flapped */
    bool held = mode == 1;

    wr8(GICD(GICD_IPRIORITYR + intid), 0x40 + 0x10 * (rand_r(seed) % 8));
    wr64(GICD(GICD_IROUTER + 8 * intid), aff_of(target));
    if (!held)
        wr32(enable, BIT(intid % 32));
    windows_request_spi(intid);
    if (held) {
        for (int i = 0; i < 50; i++)
            serve();
        if (taken(target, intid) != before) {
            printf("FAIL stress: cpu %d took SPI %u while it was disabled\n", target, intid);
            atomic_store(&failed, 1);
            return false;
        }
        wr32(enable, BIT(intid % 32));
    }
    if (mode == 2) {
        /* Off and on again while the target may have it in a list register: still once */
        wr32(disable, BIT(intid % 32));
        wr32(enable, BIT(intid % 32));
    }
    spis_sent[cur]++;
    if (!wait_taken(target, intid, before, seconds()))
        return false;
    wr32(disable, BIT(intid % 32));
    if (taken(target, intid) != before + 1) {
        printf("FAIL stress: cpu %d took SPI %u twice\n", target, intid);
        atomic_store(&failed, 1);
        return false;
    }
    return true;
}

static void *core_thread(void *arg)
{
    int slot = (long)arg;
    unsigned seed = 1234 + slot;
    struct core *c;

    cur = started[slot];
    c = &cores[cur];

    for (int round = 0; round < rounds && !atomic_load(&failed); round++) {
        int target = started[rand_r(&seed) % NR_STARTED];
        int burst = 1 + rand_r(&seed) % 4;
        u32 before[4];
        double start = seconds();

        /* Each sender has its own four INTIDs, so nobody else's SGI is merged into them */
        for (int i = 0; i < burst; i++) {
            u32 intid = slot * 4 + i;

            before[i] = taken(target, intid);
            guest_send(sgi_to(target, intid));
            sent[slot][target][intid]++;
        }

        /* Sometimes keep IRQs masked for a while, so that interrupts pile up here */
        c->masked = rand_r(&seed) % 8 == 0;

        for (int i = 0; i < burst; i++)
            if (!wait_taken(target, slot * 4 + i, before[i], start))
                return NULL;

        if (with_spis && round % 4 == 0 && !spi_round(slot, &seed))
            return NULL;
    }

    /* Keep serving the others until they are done */
    c->masked = false;
    return NULL;
}

static atomic_int done_threads;

static void *core_thread_wrapper(void *arg)
{
    core_thread(arg);
    atomic_fetch_add(&done_threads, 1);
    cur = started[(long)arg];
    while (atomic_load(&done_threads) < NR_STARTED && !atomic_load(&failed))
        serve();
    return NULL;
}

static void stress(int lrs, int n, bool spis)
{
    pthread_t threads[NR_STARTED];
    u64 spis_total = 0, spis_taken = 0;

    reset(lrs);
    rounds = n;
    with_spis = spis;
    memset(sent, 0, sizeof(sent));
    memset(spis_sent, 0, sizeof(spis_sent));
    atomic_store(&done_threads, 0);

    for (long i = 0; i < NR_STARTED; i++)
        pthread_create(&threads[i], NULL, core_thread_wrapper, (void *)i);
    for (int i = 0; i < NR_STARTED; i++)
        pthread_join(threads[i], NULL);
    if (atomic_load(&failed))
        exit(1);

    for (int t = 0; t < NR_STARTED; t++) {
        settle(started[t]);
        for (int s = 0; s < NR_STARTED; s++)
            for (int i = 0; i < NR_SGIS; i++)
                if (i / 4 == s && taken(started[t], i) != sent[s][started[t]][i]) {
                    printf("FAIL stress: cpu %d took SGI %d %u times, %lu were sent\n", started[t],
                           i, taken(started[t], i), sent[s][started[t]][i]);
                    exit(1);
                }
        for (int i = 32; i < NR_INTIDS; i++)
            spis_taken += taken(started[t], i);
        spis_total += spis_sent[started[t]];
        CHECK(nr_valid_lrs(started[t]) == 0);
    }
    CHECK(spis_taken == spis_total);
    printf("ok   stress, %d list registers, %d rounds per core, %lu SPIs: FIQs %lu %lu %lu %lu, "
           "maintenance IRQs %lu %lu %lu %lu\n",
           lrs, n, spis_total, cores[6].fiqs, cores[0].fiqs, cores[7].fiqs, cores[1].fiqs,
           cores[6].irqs, cores[0].irqs, cores[7].irqs, cores[1].irqs);
    cur = 6;
    hv_vgic_report();
}

/*
 * ---- the guest turns cores off and on ----
 */

/* PSCI CPU_OFF on this core: hv_cpu_off() and hv_leave_guest() of src/hv/hv.c */
static void guest_cpu_off(int cpu)
{
    cur = cpu;
    pthread_mutex_lock(&bhl);
    set_started(cpu, false);
    hv_vgic_cpu_off();
    pthread_mutex_unlock(&bhl);
    for (int i = 0; i < nr_lr; i++)
        CHECK(!cores[cpu].lr[i]);
    CHECK(!cores[cpu].ich_hcr);
}

/* PSCI CPU_ON of a core, by another one: hv_start_secondary() */
static void guest_cpu_on(int cpu)
{
    cur = cpu;
    pthread_mutex_lock(&bhl);
    hv_vgic_init_cpu();
    set_started(cpu, true);
    pthread_mutex_unlock(&bhl);
    CHECK(cores[cpu].ich_hcr == ICH_HCR_EN);
}

/* What the boot core does about an IRQ while the guest has it turned off: hv_boot_cpu_waits() */
static bool boot_takes_irq(void)
{
    bool irq;

    cur = boot_cpu_idx;
    irq = mock_mrs(ISR_EL1) & ISR_I;
    if (irq) {
        pthread_mutex_lock(&bhl);
        CHECK(hv_vgic_handle_irq());
        pthread_mutex_unlock(&bhl);
    }
    /* The virtual CPU interface is as hv_vgic_cpu_off() has left it: off, and empty */
    for (int i = 0; i < nr_lr; i++)
        CHECK(!cores[cur].lr[i]);
    CHECK(!cores[cur].ich_hcr);
    return irq;
}

static void off_expect(const char *what, u32 intid, int c6, int c0, int c7, int c1)
{
    int want[] = {c6, c0, c7, c1};

    for (int round = 0; round < 3; round++)
        for (int i = 0; i < NR_STARTED; i++)
            if (is_started(started[i]))
                settle(started[i]);
    for (int i = 0; i < NR_STARTED; i++)
        if ((int)taken(started[i], intid) != want[i]) {
            printf("FAIL %s: cpu %d intid %u got %u want %d\n", what, started[i], intid,
                   taken(started[i], intid), want[i]);
            exit(1);
        }
    printf("ok   %s\n", what);
}

/* How many list registers of this core hold something the guest has ended and not told yet */
static int nr_ended(int cpu)
{
    return __builtin_popcountl(eisr(&cores[cpu]));
}

static void off_tests(void)
{
    u32 was;

    /* An AIC line that waits in a list register of core 0, and the guest moves it to core 7 */
    aic_reset(true, 8);
    device(33, LINE_B, 1);
    cur = 6;
    windows_enable_spi(33, 0x80, 0, false);
    cores[0].masked = true;
    line_raise(LINE_B, true);
    settle(6);
    settle(0);
    CHECK(lr_of(0, 33) && line_masked[LINE_B]);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(7));
    guest_cpu_off(0);
    off_expect("off: a line in a list register goes to the core it is routed to by then", 33, 0, 0,
               1, 0);
    CHECK(!line_masked[LINE_B]);
    /* ...and it comes again, on that core */
    device(33, LINE_B, 1);
    line_raise(LINE_B, true);
    off_expect("off: ...and comes again there", 33, 0, 0, 2, 0);

    /* Still routed to the core that is off: it waits, and comes when the guest moves it */
    cores[0].masked = false;
    guest_cpu_on(0);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(0));
    settle_all();
    cores[0].masked = true;
    device(33, LINE_B, 1);
    line_raise(LINE_B, true);
    settle(6);
    settle(0);
    CHECK(lr_of(0, 33));
    guest_cpu_off(0);
    off_expect("off: a line that is still routed to the core waits", 33, 0, 0, 2, 0);
    cur = 6;
    CHECK(spi_pending(33) && line_masked[LINE_B]);
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(1));
    off_expect("off: ...until it is moved", 33, 0, 0, 2, 1);
    CHECK(!line_masked[LINE_B]);

    /* ...or for the core to come back */
    cores[0].masked = false;
    guest_cpu_on(0);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(0));
    settle_all();
    guest_cpu_off(0);
    device(33, LINE_B, 1);
    line_raise(LINE_B, true);
    off_expect("off: a line for a core that is off waits", 33, 0, 0, 2, 1);
    guest_cpu_on(0);
    CHECK(!lr_of(0, 33)); /* in nobody's list register: the first return to the guest looks */
    cur = 0;
    hv_exit();
    CHECK(lr_of(0, 33));
    off_expect("off: ...and is there when it is on again", 33, 0, 1, 2, 1);
    CHECK(!line_masked[LINE_B]);

    /* The guest has taken it and not ended it: it counts as ended, and the line is not lost */
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(0));
    settle_all();
    device(33, LINE_B, 1);
    line_raise(LINE_B, true);
    settle(6);
    cur = 0;
    CHECK(physical_interrupt() && guest_ack() == 33);
    CHECK(line_masked[LINE_B]);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(7));
    settle(6);
    settle(7);
    guest_cpu_off(0);
    off_expect("off: one that was not ended counts as ended, and comes again", 33, 0, 1, 3, 1);
    CHECK(!line_masked[LINE_B]);

    /* Ended, with the maintenance interrupt still to come */
    guest_cpu_on(0);
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 33), aff_of(0));
    settle_all();
    device(33, LINE_B, 1);
    line_raise(LINE_B, true);
    settle(6);
    cur = 0;
    CHECK(physical_interrupt() && guest_irq() == 33);
    CHECK(nr_ended(0) == 1 && line_masked[LINE_B] && !line_high[LINE_B]);
    guest_cpu_off(0);
    CHECK(!line_masked[LINE_B]);
    cur = 6;
    CHECK(!spi_pending(33));
    off_expect("off: one that was ended, but for the maintenance interrupt", 33, 0, 2, 3, 1);

    /* Taken, and pending again: what is pending goes on */
    guest_cpu_on(0);
    cur = 6;
    windows_enable_spi(104, 0x80, 0, true);
    settle_all();
    cur = 6;
    windows_request_spi(104);
    cur = 0;
    CHECK(physical_interrupt() && guest_ack() == 104);
    cur = 6;
    windows_request_spi(104);
    cur = 0;
    CHECK(physical_interrupt());
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(0, 104)) ==
          (ICH_LR_STATE_PENDING | ICH_LR_STATE_ACTIVE));
    cur = 6;
    wr64(GICD(GICD_IROUTER + 8 * 104), aff_of(1));
    settle(1);
    guest_cpu_off(0);
    off_expect("off: active and pending: the second one arrives elsewhere", 104, 0, 0, 0, 1);

    /* SGIs and the timer are gone with the core, and it starts with none */
    guest_cpu_on(0);
    settle_all();
    cores[0].masked = true;
    cores[0].timer = true;
    was = total(0);
    cur = 6;
    guest_send(sgi_to(0, 3));
    settle(0);
    cur = 0;
    hv_exit();
    CHECK(lr_of(0, 3) && lr_of(0, HV_VGIC_INTID_VTIMER));
    cur = 6;
    for (u32 i = 0; i < NR_SGIS; i++) /* more than there are list registers */
        guest_send(sgi_to(0, i));
    settle(0);
    /* Some wait for a list register: the Redistributor of core 0 says so to another core */
    CHECK(rd32(GICR(0, GICR_ISPENDR0)) & 0xffff);
    guest_cpu_off(0);
    cores[0].timer = false;
    cores[0].masked = false;
    /* Nobody sends to it, or calls it, while it is off */
    atomic_store(&cores[0].ipi, 0);
    cur = 6;
    guest_send(sgi_to(0, 3));
    guest_send(BIT(40) | (4UL << 24));
    wr32(GICD(GICD_ISENABLER + 4 * 3), BIT(9));
    wr32(GICR(0, GICR_ICENABLER0), BIT(9));
    wr32(GICR(0, GICR_ISENABLER0), BIT(9));
    CHECK(!atomic_load(&cores[0].ipi));
    guest_cpu_on(0);
    settle(0);
    /* None of them, and not the timer: nothing at all */
    CHECK(total(0) == was && nr_valid_lrs(0) == 0);
    cur = 6;
    guest_send(sgi_to(0, 3));
    settle(0);
    CHECK(total(0) == was + 1);
    printf("ok   off: SGIs and the timer do not outlast it\n");

    /* An SPI that any core may take goes to the boot core, and without that to the first */
    aic_reset(true, 1);
    cur = 6;
    for (u32 i = 104; i < 108; i++) {
        windows_enable_spi(i, 0x80, 6, true);
        wr64(GICD(GICD_IROUTER + 8 * i), BIT(31));
    }
    settle_all();
    cur = 6;
    windows_request_spi(104);
    off_expect("off: an SPI for any core goes to the boot core", 104, 1, 0, 0, 0);
    /* Four that wait for the only list register, which has something else in it */
    cores[6].masked = true;
    cur = 6;
    guest_send(sgi_to(6, 1));
    for (u32 i = 104; i < 108; i++)
        windows_request_spi(i);
    settle(6);
    CHECK(nr_valid_lrs(6) == 1 && lr_of(6, 1));
    for (int i = 1; i < NR_STARTED; i++) {
        settle(started[i]);
        CHECK(!atomic_load(&cores[started[i]].ipi) && nr_valid_lrs(started[i]) == 0);
    }
    guest_cpu_off(6);
    CHECK(atomic_load(&cores[0].ipi)); /* the tick too, hv_exc_fiq() */
    CHECK(!atomic_load(&cores[7].ipi) && !atomic_load(&cores[1].ipi));
    off_expect("off: ...and when that is off to the first one", 104, 1, 1, 0, 0);
    for (u32 i = 105; i < 108; i++)
        CHECK(taken(0, i) == 1);
    /* The boot core is on again: those that come from then on are its */
    cores[6].masked = false;
    guest_cpu_on(6);
    cur = 7;
    windows_request_spi(105);
    off_expect("off: ...and to the boot core once that is on again", 105, 1, 1, 0, 0);
    /* Two off: the next one. This one was in a list register of each of them */
    cores[6].masked = cores[0].masked = true;
    cur = 7;
    windows_request_spi(106);
    settle(6);
    CHECK(lr_of(6, 106));
    guest_cpu_off(6);
    settle(0);
    CHECK(lr_of(0, 106));
    guest_cpu_off(0);
    off_expect("off: ...and to the next one if the first is off as well", 106, 0, 1, 0, 1);
    cores[6].masked = cores[0].masked = false;
    guest_cpu_on(0);
    guest_cpu_on(6);

    /* AIC has told this core of a line, and nobody else */
    aic_reset(true, 8);
    device(35, LINE_D, 1);
    cur = 6;
    windows_enable_spi(35, 0x80, 7, false);
    settle_all();
    line_raise(LINE_D, true);
    guest_cpu_off(0);
    CHECK(line_masked[LINE_D]); /* its event has been read */
    off_expect("off: a line that AIC has told the core of is forwarded first", 35, 0, 0, 1, 0);
    CHECK(aic_events(0).forwarded == 1);

    /* The boot core is off, and goes on taking the lines for the guest */
    guest_cpu_on(0);
    settle_all();
    guest_cpu_off(6);
    CHECK(!boot_takes_irq());
    for (int n = 0; n < 5; n++) {
        device(35, LINE_D, 1);
        line_raise(LINE_D, true);
        CHECK(boot_takes_irq());
        CHECK(!boot_takes_irq());
        CHECK(atomic_load(&cores[7].ipi));
        settle(7);
    }
    off_expect("off: the boot core takes the lines of the others while it is off", 35, 0, 0, 6, 0);
    CHECK(!line_masked[LINE_D] && nr_valid_lrs(6) == 0);
    CHECK(aic_events(6).forwarded == 5);
    /* One that is routed to the boot core waits for it */
    device(32, LINE_A, 1);
    cur = 7;
    windows_enable_spi(32, 0x80, 6, false);
    line_raise(LINE_A, true);
    CHECK(boot_takes_irq());
    off_expect("off: a line for the boot core waits for it", 32, 0, 0, 0, 0);
    guest_cpu_on(6);
    cur = 6;
    hv_exit();
    off_expect("off: ...and is there when it is on again", 32, 1, 0, 0, 0);

    /* What the guest has set up for itself is there after CPU_ON of the boot core as well */
    cur = 0;
    CHECK(rd32(GICD(GICD_CTLR)) & 2);
    CHECK(rd32(GICR(6, GICR_ISENABLER0)) == (0xffff | BIT(HV_VGIC_INTID_VTIMER)));
    CHECK(rd32(GICD(GICD_ISENABLER + 4)) & BIT(0));
    CHECK(nr_hooks == 2);
    cur = 0;
    guest_send(sgi_to(6, 5));
    off_expect("off: nothing the guest has set up is lost", 5, 1, 0, 0, 0);

    guest_handler = NULL;
}

/*
 * Cores 6 and 0 are turned on and off over and over, and 7 and 1 make SPIs that any core may
 * take pending, and have the lines of AIC raised: each arrives once, wherever it is taken, and
 * none is lost on the way.
 */
static struct {
    atomic_int on;    /* CPU_ON: the core is to come */
    atomic_int came;  /* ...and has */
    u64 times;        /* it was turned off so often */
} toggled[MAX_CPUS];
static atomic_int toggling_over;
static u64 off_sent[MAX_CPUS];

/* The lines of the table: the device lets go of the line */
static void off_handler(u32 intid)
{
    if (intid >= 32 && intid < 34)
        line_raise(table_lines[intid - 32], false);
}

static bool line_is_high(u32 line)
{
    bool high;

    pthread_mutex_lock(&bhl);
    high = line_high[line];
    pthread_mutex_unlock(&bhl);
    return high;
}

static u32 taken_anywhere(u32 intid)
{
    u32 n = 0;

    for (int i = 0; i < NR_STARTED; i++)
        n += taken(started[i], intid);
    return n;
}

static void *toggled_thread(void *arg)
{
    int cpu = (long)arg;
    unsigned seed = 99 + cpu;

    cur = cpu;
    while (!atomic_load(&failed)) {
        /* In the guest for a while */
        for (int n = rand_r(&seed) % 60; n; n--)
            serve();
        if (atomic_load(&toggling_over)) {
            if (!is_started(cpu))
                break;
            serve();
            if (atomic_load(&done_threads) == 2)
                return NULL;
            continue;
        }
        /* ...and off, until another core turns it on */
        guest_cpu_off(cpu);
        toggled[cpu].times++;
        while (!atomic_load(&toggled[cpu].on)) {
            if (cpu == boot_cpu_idx && pthread_mutex_trylock(&bhl) == 0) {
                if (mock_mrs(ISR_EL1) & ISR_I)
                    CHECK(hv_vgic_handle_irq());
                pthread_mutex_unlock(&bhl);
            }
            sched_yield();
            if (atomic_load(&failed))
                return NULL;
        }
        /* The caller has the lock: hv_init_secondary() */
        atomic_store(&toggled[cpu].on, 0);
        hv_vgic_init_cpu();
        atomic_store(&toggled[cpu].came, 1);
        while (!is_started(cpu))
            sched_yield();
        /*
         * Its first tick (hv_enter_secondary()), a millisecond away if it is the core that ticks,
         * which the SPIs for any core go to: what has waited for it is delivered
         */
        hv_exit();
    }
    return NULL;
}

static void toggle_on(int cpu)
{
    pthread_mutex_lock(&bhl);
    if (!is_started(cpu)) {
        atomic_store(&toggled[cpu].came, 0);
        atomic_store(&toggled[cpu].on, 1);
        while (!atomic_load(&toggled[cpu].came))
            sched_yield();
        set_started(cpu, true);
    }
    pthread_mutex_unlock(&bhl);
}

static void *sender_thread(void *arg)
{
    int slot = (long)arg; /* 2 or 3 of started[]: cores 7 and 1 */
    unsigned seed = 7 + slot;

    cur = started[slot];
    for (int round = 0; round < rounds && !atomic_load(&failed); round++) {
        u32 intid = 64 + 32 * slot + rand_r(&seed) % 4;
        bool line = rand_r(&seed) % 4 == 0;
        u32 before, spins = 0;
        double start = seconds();

        if (line)
            intid = 32 + (slot - 2); /* a line of its own */
        before = taken_anywhere(intid);
        if (line)
            line_raise(table_lines[intid - 32], true);
        else
            windows_request_spi(intid);
        off_sent[cur]++;
        while (taken_anywhere(intid) == before) {
            serve();
            if (rand_r(&seed) % 64 == 0)
                toggle_on(rand_r(&seed) % 2 ? 6 : 0);
            if (!(++spins & 0xffff) && seconds() - start > 20) {
                printf("FAIL off stress: SPI %u of cpu %d is lost (on: %d %d)\n", intid, cur,
                       is_started(6), is_started(0));
                atomic_store(&failed, 1);
                return NULL;
            }
        }
        /* Whoever has it lets go of the line before it ends it */
        while (line && line_is_high(table_lines[intid - 32]))
            serve();
        for (int n = rand_r(&seed) % 8; n; n--)
            serve();
        if (taken_anywhere(intid) != before + 1) {
            printf("FAIL off stress: SPI %u was taken %u times\n", intid,
                   taken_anywhere(intid) - before);
            atomic_store(&failed, 1);
        }
        if (rand_r(&seed) % 8 == 0)
            toggle_on(rand_r(&seed) % 2 ? 6 : 0);
    }

    /* The two others are on at the end */
    atomic_store(&toggling_over, 1);
    toggle_on(6);
    toggle_on(0);
    atomic_fetch_add(&done_threads, 1);
    while (atomic_load(&done_threads) < 2 && !atomic_load(&failed)) {
        serve();
        toggle_on(6);
        toggle_on(0);
    }
    return NULL;
}

static void off_stress(int lrs, int n)
{
    pthread_t threads[NR_STARTED];
    u64 sent_total = 0, taken_total = 0;

    aic_reset(true, lrs);
    memset(toggled, 0, sizeof(toggled));
    memset(off_sent, 0, sizeof(off_sent));
    atomic_store(&done_threads, 0);
    atomic_store(&toggling_over, 0);
    rounds = n;
    guest_handler = off_handler;
    aic_told = boot_cpu_idx;
    cur = 6;
    for (int slot = 2; slot < NR_STARTED; slot++)
        for (u32 i = 0; i < 4; i++) {
            windows_enable_spi(64 + 32 * slot + i, 0x40 + 0x10 * i, 6, true);
            wr64(GICD(GICD_IROUTER + 8 * (64 + 32 * slot + i)), BIT(31));
        }
    for (u32 i = 32; i < 34; i++) {
        windows_enable_spi(i, 0x80, 6, false);
        wr64(GICD(GICD_IROUTER + 8 * i), BIT(31));
    }
    settle_all();

    pthread_create(&threads[0], NULL, toggled_thread, (void *)6L);
    pthread_create(&threads[1], NULL, toggled_thread, (void *)0L);
    pthread_create(&threads[2], NULL, sender_thread, (void *)2L);
    pthread_create(&threads[3], NULL, sender_thread, (void *)3L);
    for (int i = 0; i < NR_STARTED; i++)
        pthread_join(threads[i], NULL);
    if (atomic_load(&failed))
        exit(1);

    for (int i = 0; i < NR_STARTED; i++) {
        if (is_started(started[i]))
            settle(started[i]);
        sent_total += off_sent[started[i]];
        for (u32 intid = 32; intid < NR_INTIDS; intid++)
            taken_total += taken(started[i], intid);
    }
    if (sent_total != taken_total) {
        printf("FAIL off stress: %lu SPIs were sent, %lu were taken\n", sent_total, taken_total);
        exit(1);
    }
    CHECK(toggled[6].times > 1 && toggled[0].times > 1);
    printf("ok   off stress, %d list registers: %lu SPIs, core 6 was off %lu times, core 0 %lu\n",
           lrs, sent_total, toggled[6].times, toggled[0].times);
    guest_handler = NULL;
    aic_told = -1;
}

/* ---- a timer handler that ends the interrupt before it lowers the line ---- */

struct vgic_counts {
    long injected, maintenance, taken_back;
};

/* What hv_vgic_report() prints for one core; it leaves out a core that has done nothing */
static struct vgic_counts report_counts(int cpu)
{
    struct vgic_counts n = {0, 0, 0};
    char line[512], key[32];
    FILE *f = tmpfile();
    int saved;

    CHECK(f);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0 && dup2(fileno(f), STDOUT_FILENO) >= 0);
    hv_vgic_report();
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    snprintf(key, sizeof(key), "HV: vGIC: CPU %d: ", cpu);
    rewind(f);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, strlen(key)))
            CHECK(
                sscanf(
                    line + strlen(key),
                    "%*d ICC_SGI1R_EL1 writes, %ld injected, %ld maintenance IRQs, %ld taken back",
                    &n.injected, &n.maintenance, &n.taken_back) == 3);
    fclose(f);
    return n;
}

static bool counts_grew(struct vgic_counts from, struct vgic_counts to, long injected,
                        long maintenance, long taken_back)
{
    return to.injected - from.injected == injected &&
           to.maintenance - from.maintenance == maintenance &&
           to.taken_back - from.taken_back == taken_back;
}

/*
 * One tick of the timer of core 6, handled the way ArmPkg/Drivers/TimerDxe does it: EOI first
 * ("Signal end of interrupt early"), then read CNTV_CTL_EL0, then write the next compare value
 * to CNTV_CVAL_EL0. With FEAT_ECV the hypervisor traps both timer registers
 * (CNTHCTL_EL2.EL1TVT), and each trap ends in hv_update_fiq().
 *
 * The line is still high when the interrupt ends, so it is pending again, as with a GIC
 * (IHI 0069H.b 4.1.2: "active and pending" if "the level has not been deasserted"), and it
 * stops being pending when the line goes low (4.1.2, transition B1 or B2). The guest has IRQs
 * masked throughout and must not see it a second time.
 *
 * trap_first: the trap of the read of CNTV_CTL_EL0 is taken before the maintenance interrupt
 * of the EOI. That is the order the M4 showed in R10: at the Nth maintenance interrupt, 2N
 * interrupts had been written to list registers.
 */
static void firmware_tick(bool trap_first)
{
    struct vgic_counts start = report_counts(6), now;

    cur = 6;
    cores[6].masked = true;
    cores[6].timer = true;
    CHECK(physical_interrupt()); /* the timer's FIQ */
    CHECK(guest_ack() == HV_VGIC_INTID_VTIMER);
    guest_eoi(HV_VGIC_INTID_VTIMER);

    if (trap_first) {
        hv_exit(); /* mrs CNTV_CTL_EL0 */
        now = report_counts(6);
        CHECK(counts_grew(start, now, 2, 0, 0));
        CHECK(physical_interrupt()); /* the maintenance interrupt */
        now = report_counts(6);
        CHECK(counts_grew(start, now, 2, 1, 0));
    } else {
        CHECK(physical_interrupt());
        now = report_counts(6);
        CHECK(counts_grew(start, now, 2, 1, 0));
        hv_exit();
    }
    cur = 6;
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, HV_VGIC_INTID_VTIMER)) == ICH_LR_STATE_PENDING);
    CHECK(nr_valid_lrs(6) == 1);

    cores[6].timer = false;
    hv_exit(); /* msr CNTV_CVAL_EL0 */
    CHECK(nr_valid_lrs(6) == 0);
    CHECK(!physical_interrupt());

    cores[6].masked = false;
    CHECK(guest_irq() == 1023);
    now = report_counts(6);
    CHECK(counts_grew(start, now, 2, 1, 1));
}

static void level_timer_tests(void)
{
    struct vgic_counts start, now;

    /* The order of the test guests and of take_timer(): the line is low before the EOI */
    reset(8);
    start = report_counts(6);
    for (int i = 0; i < 100; i++) {
        cur = 6;
        cores[6].timer = true;
        CHECK(physical_interrupt());
        CHECK(guest_ack() == HV_VGIC_INTID_VTIMER);
        cores[6].timer = false;
        hv_exit();
        guest_eoi(HV_VGIC_INTID_VTIMER);
        CHECK(physical_interrupt());
        CHECK(!physical_interrupt() && nr_valid_lrs(6) == 0);
    }
    now = report_counts(6);
    CHECK(counts_grew(start, now, 100, 100, 0));
    printf("ok   timer lowered before the EOI, 100 ticks: 100 injected, 100 maintenance IRQs\n");

    for (int order = 0; order < 2; order++) {
        reset(8);
        start = report_counts(6);
        for (int i = 0; i < 100; i++)
            firmware_tick(order);
        now = report_counts(6);
        CHECK(counts_grew(start, now, 200, 100, 100));
        printf("ok   timer lowered after the EOI, %s first, 100 ticks: 200 injected, 100 "
               "maintenance IRQs, 100 taken back, the guest sees each tick once\n",
               order ? "the trap" : "the maintenance IRQ");
    }

    /* The same with one list register: the second one is not there to take the interrupt */
    reset(1);
    start = report_counts(6);
    cur = 6;
    cores[6].masked = true;
    cores[6].timer = true;
    CHECK(physical_interrupt());
    CHECK(guest_ack() == HV_VGIC_INTID_VTIMER);
    guest_eoi(HV_VGIC_INTID_VTIMER);
    hv_exit(); /* mrs CNTV_CTL_EL0: the only list register still waits for the maintenance IRQ */
    CHECK(nr_valid_lrs(6) == 0);
    CHECK(physical_interrupt());
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(6, HV_VGIC_INTID_VTIMER)) == ICH_LR_STATE_PENDING);
    cores[6].timer = false;
    hv_exit(); /* msr CNTV_CVAL_EL0 */
    CHECK(nr_valid_lrs(6) == 0 && !physical_interrupt());
    cores[6].masked = false;
    CHECK(guest_irq() == 1023);
    now = report_counts(6);
    CHECK(counts_grew(start, now, 2, 1, 1));
    printf("ok   the same with one list register\n");
}

/* ---- WFI ---- */

/* The guest on the current core executes WFI: trap, hv_handle_wfx, hv_exc_exit. Returns the WFEs.
 */
static u64 guest_wfi(void)
{
    u64 before = wfe_count;

    pthread_mutex_lock(&bhl);
    CHECK(hv_handle_wfx(ESR_ISS_WFx_TI_WFI));
    hv_exit();
    pthread_mutex_unlock(&bhl);
    return wfe_count - before;
}

/* Something that happens elsewhere during the wfe_at-th WFE of a wait */
static u64 wfe_at;
static void (*wfe_action)(void);

static void wfe_scripted(void)
{
    /* Nothing is coming any more: the core would wait forever */
    CHECK(wfe_count <= 1000);

    /* The core does not keep the others out of the hypervisor while it waits */
    CHECK(pthread_mutex_trylock(&bhl) == 0);
    pthread_mutex_unlock(&bhl);

    if (wfe_action && wfe_count == wfe_at) {
        void (*action)(void) = wfe_action;
        int self = cur;

        wfe_action = NULL;
        action();
        cur = self;
    }
}

static void during_wfe(u64 n, void (*action)(void))
{
    wfe_at = wfe_count + n;
    wfe_action = action;
}

static void wfi_reset(int lrs)
{
    reset(lrs);
    cntfrq = 1000000000;
    cnthctl = 0x1a400; /* what hv_init() leaves on the M4, findings/0015 section 10 */
    cur = 6;
    hv_wfi_init();
    wfe_count = 0;
    wfe_hook = wfe_scripted;
    wfe_action = NULL;
    isr_read_hook = NULL;
}

static void cpu0_sends_sgi2_to_6(void)
{
    cur = 0;
    guest_send(sgi_to(6, 2));
}

static void cpu0_sends_sgi3_to_6(void)
{
    cur = 0;
    guest_send(sgi_to(6, 3));
}

static void timer_of_6_fires(void)
{
    cores[6].timer = true;
}

static void cpu0_enables_timer_of_6(void)
{
    cur = 0;
    wr32(GICR(6, GICR_ISENABLER0), BIT(HV_VGIC_INTID_VTIMER));
}

static void cpu0_enables_sgi5_of_6(void)
{
    cur = 0;
    wr32(GICR(6, GICR_ISENABLER0), BIT(5));
}

static void cpu0_requests_spi40(void)
{
    cur = 0;
    windows_request_spi(40);
}

/* The guest's handler for the timer: the output goes low before the EOI */
static void take_timer(int cpu)
{
    cur = cpu;
    CHECK(FIELD_GET(ICH_LR_STATE, lr_of(cpu, HV_VGIC_INTID_VTIMER)) == ICH_LR_STATE_PENDING);
    CHECK(guest_ack() == HV_VGIC_INTID_VTIMER);
    cores[cpu].timer = false;
    guest_eoi(HV_VGIC_INTID_VTIMER);
    settle(cpu);
    CHECK(nr_valid_lrs(cpu) == 0);
}

static void wfi_tests(void)
{
    /* The event stream: one event per 65536 ticks at 1 GHz, per 2048 ticks at 24 MHz */
    wfi_reset(8);
    CHECK(cnthctl == (0x1a400 | CNTHCTL_EVNTEN | FIELD_PREP(CNTHCTL_EVNTI, 15)));
    cntfrq = 24000000;
    cnthctl = 0x1a400 | CNTHCTL_EVNTIS | CNTHCTL_EVNTDIR | FIELD_PREP(CNTHCTL_EVNTI, 3);
    hv_wfi_init();
    CHECK(cnthctl == (0x1a400 | CNTHCTL_EVNTEN | FIELD_PREP(CNTHCTL_EVNTI, 10)));
    printf("ok   WFI: event stream, 65.5 us at 1 GHz and 85.3 us at 24 MHz\n");

    /* An interrupt is there already, masked or not: no waiting */
    wfi_reset(8);
    cur = 6;
    cores[6].masked = true;
    guest_send(sgi_to(6, 1));
    CHECK(guest_wfi() == 0);
    CHECK(taken(6, 1) == 0);
    cores[6].masked = false;
    settle(6);
    CHECK(taken(6, 1) == 1);
    printf("ok   WFI: an SGI is pending and IRQs are masked: returns at once\n");

    /* Nothing is there: the core waits, through wakeups for nothing, until another core sends */
    wfi_reset(8);
    cur = 6;
    during_wfe(3, cpu0_sends_sgi2_to_6);
    CHECK(guest_wfi() == 3);
    CHECK(!wfe_action);
    settle(6);
    CHECK(taken(6, 2) == 1 && cores[6].fiqs == 1);
    printf("ok   WFI: waits until an SGI from another core, taken once\n");

    /* The guest's own timer */
    wfi_reset(8);
    cur = 6;
    during_wfe(5, timer_of_6_fires);
    CHECK(guest_wfi() == 5);
    take_timer(6);
    printf("ok   WFI: waits until the timer fires\n");

    /* The timer is high already when the guest executes WFI */
    wfi_reset(8);
    cur = 6;
    cores[6].timer = true;
    CHECK(guest_wfi() == 0);
    take_timer(6);
    printf("ok   WFI: the timer has fired already: returns at once\n");

    /* A source that is high but not enabled is nothing to wake up for, until it is enabled */
    wfi_reset(8);
    cur = 6;
    wr32(GICR(6, GICR_ICENABLER0), BIT(HV_VGIC_INTID_VTIMER));
    cores[6].timer = true;
    settle(6);
    CHECK(nr_valid_lrs(6) == 0);
    during_wfe(4, cpu0_enables_timer_of_6);
    CHECK(guest_wfi() == 4);
    take_timer(6);
    printf(
        "ok   WFI: timer high with its PPI disabled: waits until another core enables the PPI\n");

    wfi_reset(8);
    cur = 6;
    wr32(GICR(6, GICR_ICENABLER0), BIT(5));
    cur = 0;
    guest_send(sgi_to(6, 5));
    settle(6);
    CHECK(taken(6, 5) == 0 && nr_valid_lrs(6) == 0);
    cur = 6;
    during_wfe(3, cpu0_enables_sgi5_of_6);
    CHECK(guest_wfi() == 3);
    settle(6);
    CHECK(taken(6, 5) == 1);
    printf("ok   WFI: SGI pending but disabled: waits until another core enables it\n");

    /* An SPI that another core requests */
    wfi_reset(8);
    cur = 6;
    windows_enable_spi(40, 0x80, 6, true);
    settle(6);
    during_wfe(2, cpu0_requests_spi40);
    CHECK(guest_wfi() == 2);
    settle(6);
    CHECK(taken(6, 40) == 1);
    printf("ok   WFI: waits until another core requests an SPI routed here\n");

    /*
     * The SGI arrives after the core has looked and before it executes WFE. This WFE does not
     * end early for it; the next look, one period of the event stream later, sees it.
     */
    wfi_reset(8);
    cur = 6;
    isr_read_hook = cpu0_sends_sgi3_to_6;
    CHECK(guest_wfi() == 1);
    CHECK(!isr_read_hook);
    settle(6);
    CHECK(taken(6, 3) == 1);
    printf("ok   WFI: SGI between the look at ISR_EL1 and the WFE: noticed after one WFE\n");

    /* The guest ended an interrupt and executes WFI before the maintenance interrupt is taken */
    wfi_reset(8);
    cur = 6;
    guest_send(sgi_to(6, 1));
    CHECK(guest_irq() == 1);
    CHECK(eisr(&cores[6]));
    CHECK(guest_wfi() == 0);
    settle(6);
    CHECK(!eisr(&cores[6]) && cores[6].irqs == 1);
    printf("ok   WFI: a maintenance interrupt is pending: returns at once\n");

    /* The only list register holds an active interrupt and an SGI waits: the guest has to go on */
    wfi_reset(1);
    cur = 6;
    guest_send(sgi_to(6, 1));
    CHECK(guest_ack() == 1);
    guest_send(sgi_to(6, 2));
    CHECK(!lr_of(6, 2));
    CHECK(guest_wfi() == 0);
    guest_eoi(1);
    settle(6);
    CHECK(taken(6, 2) == 1);
    printf("ok   WFI: an SGI waits for a list register: returns at once\n");

    /* The same with the timer: its FIQ is off while its output is high, nothing else would come */
    wfi_reset(1);
    cur = 6;
    guest_send(sgi_to(6, 1));
    CHECK(guest_ack() == 1);
    cores[6].timer = true;
    settle(6);
    CHECK(!lr_of(6, HV_VGIC_INTID_VTIMER) && !(isr(&cores[6]) & ISR_F));
    CHECK(guest_wfi() == 0);
    guest_eoi(1);
    CHECK(physical_interrupt());
    take_timer(6);
    printf("ok   WFI: the timer has fired and waits for a list register: returns at once\n");

    /* WFIT does not wait; WFE and WFET are not this handler's */
    wfi_reset(8);
    cur = 6;
    pthread_mutex_lock(&bhl);
    CHECK(hv_handle_wfx(ESR_ISS_WFx_TI_WFIT));
    CHECK(!hv_handle_wfx(ESR_ISS_WFx_TI_WFE));
    CHECK(!hv_handle_wfx(ESR_ISS_WFx_TI_WFET));
    pthread_mutex_unlock(&bhl);
    CHECK(wfe_count == 0);
    printf("ok   WFI: WFIT returns at once, WFE and WFET are refused\n");

    wfe_hook = NULL;
}

/*
 * Cores that do nothing but idle in WFI pass two SGIs around: whoever takes one sends it on to
 * another core and goes back to WFI. A core that sleeps on an SGI stops it for good.
 */
#define NR_TOKENS 2
#define SGI_STOP  15
static atomic_long hops_left;
static atomic_int ring_stop;
static u64 ring_sent[MAX_CPUS][MAX_CPUS][NR_SGIS]; /* [sender][target][intid] */

static void wfe_yield(void)
{
    sched_yield();
}

static void ring_forward(u32 token, unsigned *seed)
{
    int target;

    if (atomic_fetch_sub(&hops_left, 1) <= 0) {
        /* Done: wake everybody else up for the last time */
        if (!atomic_exchange(&ring_stop, 1)) {
            for (int i = 0; i < NR_STARTED; i++)
                if (started[i] != cur)
                    ring_sent[cur][started[i]][SGI_STOP]++;
            guest_send(BIT(40) | ((u64)SGI_STOP << 24));
        }
        return;
    }

    do
        target = started[rand_r(seed) % NR_STARTED];
    while (target == cur);
    ring_sent[cur][target][token]++;
    guest_send(sgi_to(target, token));
}

static void *ring_thread(void *arg)
{
    int slot = (long)arg;
    unsigned seed = 4321 + slot;
    struct core *c;

    cur = started[slot];
    c = &cores[cur];

    if (slot < NR_TOKENS)
        ring_forward(slot, &seed);

    while (!atomic_load(&ring_stop)) {
        u32 intid;

        /* Windows idles with IRQs masked and unmasks them after the WFI */
        c->masked = rand_r(&seed) & 1;
        guest_wfi();
        c->masked = false;

        for (;;) {
            if (physical_interrupt())
                continue;
            intid = guest_irq();
            if (intid == 1023)
                break;
            if (intid < NR_TOKENS)
                ring_forward(intid, &seed);
        }
    }

    atomic_fetch_add(&done_threads, 1);
    return NULL;
}

static void wfi_stress(int lrs, long hops)
{
    pthread_t threads[NR_STARTED];
    long last = hops;
    double moved;

    wfi_reset(lrs);
    wfe_hook = wfe_yield;
    memset(ring_sent, 0, sizeof(ring_sent));
    atomic_store(&hops_left, hops);
    atomic_store(&ring_stop, 0);
    atomic_store(&done_threads, 0);

    for (long i = 0; i < NR_STARTED; i++)
        pthread_create(&threads[i], NULL, ring_thread, (void *)i);

    moved = seconds();
    while (atomic_load(&done_threads) < NR_STARTED) {
        long now = atomic_load(&hops_left);

        usleep(10000);
        if (now != last) {
            last = now;
            moved = seconds();
        } else if (seconds() - moved > 20) {
            printf("FAIL WFI stress: nothing has moved for 20 s with %ld hops left: a core sleeps "
                   "on an SGI\n",
                   now);
            exit(1);
        }
    }
    for (int i = 0; i < NR_STARTED; i++)
        pthread_join(threads[i], NULL);
    wfe_hook = NULL;

    for (int t = 0; t < NR_STARTED; t++) {
        settle(started[t]);
        for (int i = 0; i < NR_SGIS; i++) {
            u64 want = 0;

            for (int s = 0; s < NR_STARTED; s++)
                want += ring_sent[started[s]][started[t]][i];
            if (taken(started[t], i) != want) {
                printf("FAIL WFI stress: cpu %d took SGI %d %u times, %lu were sent\n", started[t],
                       i, taken(started[t], i), want);
                exit(1);
            }
        }
        CHECK(nr_valid_lrs(started[t]) == 0);
    }
    printf("ok   WFI stress, %d list registers, %ld hops: FIQs %lu %lu %lu %lu\n", lrs, hops,
           cores[6].fiqs, cores[0].fiqs, cores[7].fiqs, cores[1].fiqs);
    cur = 6;
    hv_wfi_report();
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;

    setvbuf(stdout, NULL, _IOLBF, 0);
    /* "aic": only the tests of how AIC lines are forwarded (make vgic-broken) */
    if (argc > 1 && !strcmp(argv[1], "aic")) {
        aic_tests();
        aic_config_tests();
        aic_limit_tests();
        aic_order_tests();
        printf("ALL OK\n");
        return 0;
    }
    /* "off": only those of the cores that are turned off (make vgic-broken) */
    if (argc > 1 && !strcmp(argv[1], "off")) {
        off_tests();
        off_stress(8, 1000);
        off_stress(1, 1000);
        printf("ALL OK\n");
        return 0;
    }
    sgi_tests();
    dist_tests();
    aic_tests();
    aic_config_tests();
    aic_limit_tests();
    aic_order_tests();
    off_tests();
    level_timer_tests();
    wfi_tests();
    stress(8, n, false);
    stress(2, n, false);
    stress(1, n / 4, false);
    stress(8, n / 4, true);
    stress(2, n / 4, true);
    stress(1, n / 8, true);
    off_stress(8, n / 4);
    off_stress(2, n / 8);
    off_stress(1, n / 8);
    wfi_stress(8, n / 4);
    wfi_stress(1, n / 8);
    printf("ALL OK\n");
    return 0;
}
