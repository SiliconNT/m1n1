// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for the SGI path of src/hv/hv_vgic.c. The file under test is the real
 * one, compiled against stand-in headers; this file plays the hardware (list registers, the
 * Apple IPI) and the guest (acknowledge, EOI), with one thread per core.
 *
 * What the "hypervisor exit paths" do here mirrors src/hv/hv_exc.c: an IPI is acknowledged
 * first (hv_exc_fiq), a maintenance interrupt goes to hv_vgic_handle_irq() (hv_exc_irq), and
 * every way back to the guest ends in hv_vgic_deliver_sgis() (hv_update_fiq).
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

#define NR_SGIS 16

struct core {
    u64 lr[16];
    u64 mpidr;
    atomic_int ipi; /* the Apple IPI: stays pending until acknowledged */
    bool masked;
    atomic_uint count[NR_SGIS];
    /* IPIs taken, to see the path was used */
    u64 fiqs, irqs;
} ALIGNED(64);

static struct core cores[MAX_CPUS];
static int nr_lr = 8;
static __thread int cur;

struct aic *aic = NULL;
bool hv_started_cpus[MAX_CPUS];

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

u64 mock_mrs(int reg)
{
    struct core *c = &cores[cur];

    if (lr_index(reg) >= 0)
        return c->lr[lr_index(reg)];
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

/* ---- the hypervisor's exit paths, on the current core ---- */

/* Take whatever physical interrupt is pending. Returns false if there was none. */
static bool physical_interrupt(void)
{
    struct core *c = &cores[cur];

    if (atomic_load(&c->ipi)) {
        /* hv_exc_fiq: acknowledge, then hv_exc_exit -> hv_update_fiq */
        atomic_store(&c->ipi, 0);
        c->fiqs++;
        hv_vgic_deliver_sgis();
        return true;
    }
    if (eisr(c)) {
        /* hv_exc_irq: hv_vgic_handle_irq, then hv_exc_exit -> hv_update_fiq */
        c->irqs++;
        CHECK(hv_vgic_handle_irq());
        hv_vgic_deliver_sgis();
        return true;
    }
    return false;
}

/* The guest wrote ICC_SGI1R_EL1: trap, then hv_update_fiq on the way out */
static void guest_send(u64 val)
{
    hv_vgic_send_sgi(val);
    hv_vgic_deliver_sgis();
}

/* ---- the guest's side of the virtual CPU interface ---- */

/* ICC_IAR1_EL1: the pending interrupt in the lowest list register, 1023 if none */
static u32 guest_ack(void)
{
    struct core *c = &cores[cur];

    for (int i = 0; i < nr_lr; i++) {
        if (FIELD_GET(ICH_LR_STATE, c->lr[i]) == ICH_LR_STATE_PENDING) {
            c->lr[i] = (c->lr[i] & ~ICH_LR_STATE) | FIELD_PREP(ICH_LR_STATE, ICH_LR_STATE_ACTIVE);
            return FIELD_GET(ICH_LR_VINTID, c->lr[i]);
        }
    }
    return 1023;
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
    CHECK(intid < NR_SGIS);
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

static u32 total(int cpu)
{
    u32 n = 0;

    for (int i = 0; i < NR_SGIS; i++)
        n += atomic_load(&cores[cpu].count[i]);
    return n;
}

/* The M4's layout: 0-5 in one cluster, 6-9 in another; the guest runs on 6, 0, 7 and 1 */
static const int started[] = {6, 0, 7, 1};
#define NR_STARTED 4

static void reset(int lrs)
{
    nr_lr = lrs;
    memset(cores, 0, sizeof(cores));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    for (int i = 0; i < 10; i++)
        cores[i].mpidr = i < 6 ? (0x80000000UL | i) : (0x80010100UL | (i - 6));
    for (int i = 0; i < NR_STARTED; i++) {
        cur = started[i];
        hv_vgic_init();
        hv_started_cpus[started[i]] = true;
    }
    /* A core the guest is not on, but which has an affinity: must never be a target */
    cur = 8;
    hv_vgic_init();
}

static void expect_counts(const char *what, u32 intid, int c6, int c0, int c7, int c1)
{
    int want[] = {c6, c0, c7, c1};

    for (int i = 0; i < NR_STARTED; i++) {
        settle(started[i]);
        if ((int)atomic_load(&cores[started[i]].count[intid]) != want[i]) {
            printf("FAIL %s: cpu %d sgi %u got %u want %d\n", what, started[i], intid,
                   atomic_load(&cores[started[i]].count[intid]), want[i]);
            exit(1);
        }
    }
    CHECK(total(8) == 0 && !atomic_load(&cores[8].ipi));
    printf("ok   %s\n", what);
}

static void unit_tests(void)
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
        CHECK(atomic_load(&cores[6].count[i]) == 1);
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
        CHECK(atomic_load(&cores[0].count[i]) == 1);
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
    CHECK(atomic_load(&cores[6].count[3]) == 1);
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
    CHECK(guest_ack() == 1023); /* (the model has no priorities: nothing else is pending) */
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
    CHECK(hv_vgic_inject(HV_VGIC_INTID_VTIMER, HV_VGIC_PRIO_DEFAULT));
    CHECK(hv_vgic_inject(HV_VGIC_INTID_VTIMER, HV_VGIC_PRIO_DEFAULT));
    CHECK(nr_valid_lrs(6) == 1);
    guest_send(sgi_to(6, 1));
    guest_send(sgi_to(6, 2));
    CHECK(nr_valid_lrs(6) == 2); /* timer and SGI 1; SGI 2 waits */
    hv_vgic_retract(HV_VGIC_INTID_VTIMER);
    hv_vgic_deliver_sgis();
    CHECK(nr_valid_lrs(6) == 2); /* SGI 2 took the place of the timer */
    settle(6);
    CHECK(atomic_load(&cores[6].count[1]) == 1 && atomic_load(&cores[6].count[2]) == 1);
    printf("ok   timer and SGIs share the list registers\n");
}

/* ---- stress: every core sends to every core, one thread per core ---- */

static int rounds;
static atomic_int failed;
static u64 sent[MAX_CPUS][MAX_CPUS][NR_SGIS]; /* [sender slot][target][intid] */

static double seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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

            before[i] = atomic_load(&cores[target].count[intid]);
            guest_send(sgi_to(target, intid));
            sent[slot][target][intid]++;
        }

        /* Sometimes keep IRQs masked for a while, so that SGIs pile up here */
        c->masked = rand_r(&seed) % 8 == 0;

        for (int i = 0; i < burst; i++) {
            u32 intid = slot * 4 + i;
            u64 spins = 0;

            while (atomic_load(&cores[target].count[intid]) == before[i]) {
                if (!physical_interrupt())
                    guest_irq();
                if (c->masked && ++spins > 200)
                    c->masked = false;
                if (!(++spins & 0xfffff) && seconds() - start > 20) {
                    printf("FAIL stress: cpu %d waited 20 s for cpu %d to take SGI %u\n", cur,
                           target, intid);
                    atomic_store(&failed, 1);
                    return NULL;
                }
            }
        }
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
        if (!physical_interrupt())
            guest_irq();
    return NULL;
}

static void stress(int lrs, int n)
{
    pthread_t threads[NR_STARTED];

    reset(lrs);
    rounds = n;
    memset(sent, 0, sizeof(sent));
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
                if (i / 4 == s && atomic_load(&cores[started[t]].count[i]) != sent[s][started[t]][i]) {
                    printf("FAIL stress: cpu %d took SGI %d %u times, %lu were sent\n", started[t],
                           i, atomic_load(&cores[started[t]].count[i]), sent[s][started[t]][i]);
                    exit(1);
                }
        CHECK(nr_valid_lrs(started[t]) == 0);
    }
    printf("ok   stress, %d list registers, %d rounds per core: FIQs %lu %lu %lu %lu, "
           "maintenance IRQs %lu %lu %lu %lu\n",
           lrs, n, cores[6].fiqs, cores[0].fiqs, cores[7].fiqs, cores[1].fiqs, cores[6].irqs,
           cores[0].irqs, cores[7].irqs, cores[1].irqs);
    cur = 6;
    hv_vgic_report();
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;

    setvbuf(stdout, NULL, _IOLBF, 0);
    unit_tests();
    stress(8, n);
    stress(2, n);
    stress(1, n / 4);
    printf("ALL OK\n");
    return 0;
}
