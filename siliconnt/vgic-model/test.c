// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_vgic.c. The file under test is the real one, compiled
 * against stand-in headers; this file plays the hardware (list registers, the Apple IPI), the
 * rest of the hypervisor (the MMIO hooks, the exit paths) and the guest (the Distributor and
 * Redistributor accesses of the Windows kernel, acknowledge, EOI), with one thread per core.
 *
 * What the "hypervisor exit paths" do here mirrors src/hv/hv_exc.c: an IPI is acknowledged
 * first (hv_exc_fiq), a maintenance interrupt goes to hv_vgic_handle_irq() (hv_exc_irq), a
 * trapped MMIO access runs its hook with the big hypervisor lock held (hv_exc_sync), and
 * every way back to the guest ends in hv_update_fiq(), which looks at the virtual timer and
 * calls hv_vgic_deliver().
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aic.h"
#include "cpu_regs.h"
#include "hv.h"
#include "smp.h"
#include "utils.h"

#define NR_SGIS   16
#define NR_INTIDS 1020

struct core {
    u64 lr[16];
    u64 mpidr;
    atomic_int ipi; /* the Apple IPI: stays pending until acknowledged */
    bool masked;
    u32 pmr;    /* ICC_PMR_EL1: only priorities below this are taken */
    bool timer; /* the output of the guest's virtual timer */
    atomic_uint count[NR_INTIDS];
    /* IPIs taken, to see the path was used */
    u64 fiqs, irqs;
} ALIGNED(64);

static struct core cores[MAX_CPUS];
static int nr_lr = 8;
static u32 alive = 0x3ff; /* the cores that run m1n1 */
static __thread int cur;

struct aic *aic = NULL;
bool hv_started_cpus[MAX_CPUS];
int boot_cpu_idx = 6;

uint32_t aic_ack(void)
{
    return 0;
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
        if (!FIELD_GET(ICH_LR_STATE, c->lr[i]) && !(c->lr[i] & ICH_LR_HW) && (c->lr[i] & ICH_LR_EOI))
            v |= BIT(i);
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
    }
    return 0;
}

void mock_msr(int reg, u64 val)
{
    if (lr_index(reg) >= 0)
        cores[cur].lr[lr_index(reg)] = val;
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

/* The big hypervisor lock: held around the MMIO hooks, not on the other paths used here */
static pthread_mutex_t bhl = PTHREAD_MUTEX_INITIALIZER;

/* hv_update_fiq(), on the current core */
static void hv_exit(void)
{
    hv_vgic_ppi_level(HV_VGIC_INTID_VTIMER, cores[cur].timer);
    hv_vgic_deliver();
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
    if (eisr(c)) {
        /* hv_exc_irq: hv_vgic_handle_irq, then hv_exc_exit -> hv_update_fiq */
        c->irqs++;
        CHECK(hv_vgic_handle_irq());
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
#define GICR(cpu, off) (HV_VGIC_REDIST_BASE + (u64)(cpu)*HV_VGIC_REDIST_SIZE + (off))

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

/* Take one virtual interrupt if the guest can. Returns the INTID or 1023. */
static u32 guest_irq(void)
{
    struct core *c = &cores[cur];
    u32 intid;

    if (c->masked)
        return 1023;
    intid = guest_ack();
    if (intid == 1023)
        return 1023;
    CHECK(intid < NR_INTIDS);
    atomic_fetch_add(&c->count[intid], 1);
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
    for (int i = 0; i < MAX_CPUS; i++)
        cores[i].pmr = 0x100;
    for (int i = 0; i < NR_STARTED; i++) {
        cur = started[i];
        hv_vgic_init();
        hv_started_cpus[started[i]] = true;
    }
    /* A core the guest is not on, but which has an affinity: must never be a target */
    cur = 8;
    hv_vgic_init();
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
    guest_send(sgi_list(cores[6].mpidr, 0xffff, 8, 0)); /* CPU 8 exists but the guest is not on it */
    expect_counts("target list, all 16 bits, P cluster", 8, 1, 0, 1, 0);

    cur = 6;
    guest_send(sgi_list(cores[6].mpidr, 0, 9, 0));
    guest_send(sgi_list(cores[6].mpidr, 0x3, 9, 1));           /* RS = 1: Aff0 16 and 17 */
    guest_send(sgi_list(cores[6].mpidr ^ 0x4000, 0x3, 9, 0));  /* no such Aff1 */
    guest_send(sgi_list(cores[6].mpidr ^ 0x400000, 0x3, 9, 0)); /* no such Aff2 */
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
    CHECK(rd32(GICD(GICD_IROUTER + 8 * 40)) == aff_of(6) && rd32(GICD(GICD_IROUTER + 8 * 40 + 4)) == 0);
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
    CHECK(rd32(GICD(GICD_IPRIORITYR + 60)) == 0xbbaa && rd(GICD(GICD_IPRIORITYR + 60), 1) == 0xbbaa);
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
    static const u32 gicd_ranges[][2] = {{0x0, 0x10},       {0x80, 0xf00},     {0xf10, 0xf30},
                                         {0x1000, 0x3700},  {0x6100, 0xa000},  {0xffd0, 0x10000}};
    static const u32 gicr_ranges[][2] = {{0x0, 0x20},       {0x70, 0x80},      {0xc0, 0xc8},
                                         {0xffd0, 0x10000}, {0x10080, 0x10e00}};
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

/* ---- stress: every core sends to every core, one thread per core ---- */

static int rounds;
static atomic_int failed;
static u64 sent[MAX_CPUS][MAX_CPUS][NR_SGIS]; /* [sender slot][target][intid] */
static u64 spis_sent[MAX_CPUS];                /* [sender], all SPIs */
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

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;

    setvbuf(stdout, NULL, _IOLBF, 0);
    sgi_tests();
    dist_tests();
    stress(8, n, false);
    stress(2, n, false);
    stress(1, n / 4, false);
    stress(8, n / 4, true);
    stress(2, n / 4, true);
    stress(1, n / 8, true);
    printf("ALL OK\n");
    return 0;
}
