// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for how the cores get into EL2 and into the guest: src/hv/hv.c with
 * src/smp.c under it, the real files, with the real headers of m1n1 and the accesses to the
 * hardware taken out (cpus/hw.h). Every core is a thread. This file plays the rest:
 *
 *  - the machine: the device tree as far as smp_init() reads it, the registers of the power
 *    manager that start a core, RVBAR, the system registers of each core, the IPIs;
 *  - the guest: hv_enter_guest() runs a function on that thread until hv_exit_guest(). Its
 *    ways into EL2 and out of it are hv_exc_entry(), hv_exc_exit(), hv_exc_fiq() and
 *    hv_exc_proxy() of src/hv/hv_exc.c, the real ones;
 *  - the host, which is told what has stopped the guest and answers (uartproxy_run());
 *  - the rest of the hypervisor, which hv_init() and hv_start() call: nothing, most of it.
 *
 * Every test runs in a process of its own, because a core never ends, and is taken to hang
 * after a while.
 *
 *   build/cpus-model          all of them
 *   build/cpus-model <name>   only that one
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#define UNUSED(x) (void)(x)
#define BIT(x)    (1UL << (x))
#define MAX_CPUS  32 /* src/smp_asm.h */

/* How long a test may take: the longest takes a few seconds. CPUS_TIMEOUT says otherwise */
#define TIMEOUT_S 60

/* ---- what is under test ---- */

typedef struct {
    s64 lock;
    int count;
} __attribute__((aligned(64))) spinlock_t;

extern spinlock_t bhl;
extern bool hv_started_cpus[MAX_CPUS];
extern u64 hv_cpus_in_guest;
extern int boot_cpu_idx;

int smp_init(void);
void smp_start_secondaries(void);
bool smp_start_secondary(int cpu);
void smp_secondary_entry(void);
bool smp_is_alive(int cpu);
bool smp_is_idle(int cpu);
void smp_send_ipi(int cpu);
void smp_call4(int cpu, void *func, u64 arg0, u64 arg1, u64 arg2, u64 arg3);
u64 smp_wait(int cpu);
void hv_init(void);
bool hv_cpus_ready(void);
void hv_start(void *entry, u64 regs[4]);
bool hv_start_secondary(int cpu, void *entry, u64 regs[4]);
void hv_maybe_exit(void);
void hv_exit_cpu(int cpu);
void hv_rendezvous(void);
u64 hv_get_started_cpus(void);
bool hv_switch_cpu(int cpu);
void hv_pin_cpu(int cpu);
int hv_lead_cpu(void);
bool hv_is_leaving(void);
extern int hv_want_cpu, hv_pinned_cpu;
/* An enum hv_sptm_mode of src/hv/hv.h, which the host sets (P_HV_SPTM_SET_MODE) */
extern int hv_sptm_mode;
#define SPTM_NONE        0
#define SPTM_NESTED_VIRT 2

/* src/hv/hv_exc.c, by cpus/exc_head.c and cpus/exc_tail.c */
void model_exc_entry(void);
void model_exc_exit(void);
void model_exc_fiq(void);
void model_exc_proxy(int reason, u32 type);
void model_cpu_off(void);
void model_steal_time(u64 ticks);
u64 model_stolen_time(void);
void model_records_fill(int cpu);
bool model_records_clear(int cpu);

/* src/uartproxy.h and src/hv/hv.h */
#define START_HV          3
#define EXC_RET_HANDLED   2
#define EXC_EXIT_GUEST    3
#define HV_USER_INTERRUPT 3
#define HV_CPU_SWITCH     5
#define HV_GUEST_RESET    8

/* cpus/stubs.c */
void set_sysregs_unlocked(bool unlocked);

/* ---- checks ---- */

static int failures;
static __thread int me = -1; /* the core this thread is */

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            if (__atomic_add_fetch(&failures, 1, __ATOMIC_SEQ_CST) <= 20) {                        \
                fprintf(stderr, "FAIL %s:%d: %s: ", __func__, __LINE__, #cond);                    \
                fprintf(stderr, __VA_ARGS__);                                                      \
                fprintf(stderr, "\n");                                                             \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/* What m1n1 has printed */
static char printed[1 << 20];
static size_t printed_len;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

int debug_printf(const char *fmt, ...)
{
    va_list args;

    pthread_mutex_lock(&print_lock);
    va_start(args, fmt);
    if (printed_len < sizeof(printed) - 1)
        printed_len += vsnprintf(&printed[printed_len], sizeof(printed) - printed_len, fmt, args);
    va_end(args);
    pthread_mutex_unlock(&print_lock);
    return 0;
}

static bool was_printed(const char *text)
{
    return strstr(printed, text);
}

static int lines_with(const char *text)
{
    int n = 0;

    for (const char *p = strstr(printed, text); p; p = strstr(p + 1, text))
        n++;
    return n;
}

/* ---- the machine: an M4 ---- */

#define BOOT      6
#define NR        10
#define EXTRA     11 /* one more, for a device tree with a core too many */
#define CNTFRQ    1000000000UL
#define PMGR      0x300000000UL
#define CPU_START (PMGR + 0x34000) /* T8132, src/smp.c */
#define IMPL(i)   (0x400000000UL + (u64)(i) * 0x100000)

/* Names as they are in the assembly: sr_tkn() of src/utils.h */
#define R_SCTLR_EL12    "s3_5_c1_c0_0"
#define R_IPI_RR_GLOBAL "s3_5_c15_c0_1"
#define R_IPI_SR        "s3_5_c15_c1_1"
#define R_VM_TMR_FIQ    "s3_5_c15_c1_3"
#define R_PMCR0         "s3_1_c15_c0_0"

#define PMCR0_IRQ 0xf00UL /* PMCR0_IACT, PMCR0_IMODE_MASK */

/* CNTP_CTL_EL0 and ISR_EL1 */
#define CTL_ENABLE  BIT(0)
#define CTL_IMASK   BIT(1)
#define CTL_ISTATUS BIT(2)
#define ISR_F       BIT(6)
#define ISR_I       BIT(7)

#define SCTLR_M  BIT(0)
#define SCTLR_C  BIT(2)
#define SCTLR_I  BIT(12)
#define SCTLR_EE BIT(25)

static struct core {
    /* What the device tree says */
    bool listed;
    int id; // cpu-id
    u32 reg;
    bool no_reg; // the node has no "reg"
    /* What it is */
    bool dead; // never comes up
    u64 mpidr;
    u64 rvbar;
    u64 line;   // the smallest data cache line
    int starts; // how often the power manager was told to start it
    pthread_t thread;
    /* Its system registers */
    struct {
        const char *name;
        u64 val;
    } regs[128];
    int nr_regs;
    /* Its interrupts: the IPI, the tick, and one that AIC tells it of */
    int ipi, irq;
    bool serror_on; // DAIF.A is clear: an SError would be taken in EL2
    u64 cval; // when the tick comes
    /* What it did about them */
    int ticks, irqs;
    /* The guest on it */
    int entered;
    bool in_guest; // ...or in EL2, to come back
    bool in_el2;   // in EL2, to come back
    jmp_buf leave;
    struct {
        u64 x[4];
        void *entry;
        u64 spsr;
    } got;
    /* What was called on it since it was last in the guest */
    int vgic_inits, dbg_inits, pmu_inits, xtrack_inits;
    /* ...and ever: turned off by the guest, its MMU is turned on, its stack thrown out of the
     * caches */
    int vgic_offs;
    int mmu_inits, ivacs;
    /* What the one that let it in gave it */
    u64 want_x0;
    void *want_entry;
} cores[EXTRA];

static void (*guest)(int cpu); /* what the guest does on each core */
static int (*host)(int reason, u32 code); /* what the host answers; NULL: leave the guest */
static int host_calls;
static bool host_byte; /* the host wants the guest stopped */
static bool no_host;   /* nobody is there to be told */
static int stopped_on; /* the core that has stopped the guest then */
static bool slow_back; /* a core that has left the guest takes a while */
static u64 alive_at_vgic_init;
static bool wfi_used;
static bool sysregs_unlocked;
static int line_calls;

static u64 *reg_slot(struct core *c, const char *name)
{
    for (int i = 0; i < c->nr_regs; i++)
        if (!strcmp(c->regs[i].name, name))
            return &c->regs[i].val;
    if (c->nr_regs == 128)
        abort();
    c->regs[c->nr_regs].name = name;
    return &c->regs[c->nr_regs++].val;
}

static u64 now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

/* The tick of this core, CNTP_CTL_EL0: is its time come */
static bool tick_due(struct core *c)
{
    return (*reg_slot(c, "CNTP_CTL_EL0") & CTL_ENABLE) && now() >= c->cval;
}

/* ISR_EL1: what is pending on this core, masked or not */
static u64 isr(struct core *c)
{
    u64 val = 0;

    if (__atomic_load_n(&c->ipi, __ATOMIC_SEQ_CST) ||
        (tick_due(c) && !(*reg_slot(c, "CNTP_CTL_EL0") & CTL_IMASK)))
        val |= ISR_F;
    if (__atomic_load_n(&c->irq, __ATOMIC_SEQ_CST))
        val |= ISR_I;
    return val;
}

u64 mock_mrs(const char *name)
{
    if (!strcmp(name, "ISR_EL1"))
        return isr(&cores[me]);
    if (!strcmp(name, "CNTP_CTL_EL0"))
        return *reg_slot(&cores[me], name) | (tick_due(&cores[me]) ? CTL_ISTATUS : 0);
    if (!strcmp(name, "CNTP_TVAL_EL0"))
        return (u32)(cores[me].cval - now());
    if (!strcmp(name, R_IPI_SR))
        return __atomic_load_n(&cores[me].ipi, __ATOMIC_SEQ_CST);
    if (!strcmp(name, "CurrentEL"))
        return 2 << 2;
    if (!strcmp(name, "MPIDR_EL1"))
        return BIT(31) | cores[me].mpidr;
    if (!strcmp(name, "CNTFRQ_EL0"))
        return CNTFRQ;
    if (!strcmp(name, "CNTPCT_EL0"))
        return now();
    if (!strcmp(name, "ID_AA64PFR0_EL1"))
        return 0x0111; /* EL0, EL1, EL2 */
    return *reg_slot(&cores[me], name);
}

void mock_msr(const char *name, u64 val)
{
    if (!strcmp(name, R_IPI_RR_GLOBAL)) {
        u64 mpidr = (val & 0xff) | ((val >> 8) & 0xff00);

        for (int i = 0; i < NR; i++)
            if ((cores[i].mpidr & 0xffff) == mpidr)
                __atomic_store_n(&cores[i].ipi, 1, __ATOMIC_SEQ_CST);
        return;
    }
    if (!strcmp(name, R_IPI_SR)) {
        if (val & 1)
            __atomic_store_n(&cores[me].ipi, 0, __ATOMIC_SEQ_CST);
        return;
    }
    if (!strcmp(name, "CNTP_TVAL_EL0")) {
        cores[me].cval = now() + val;
        return;
    }
    if (!strcmp(name, "CNTP_CTL_EL0"))
        val &= CTL_ENABLE | CTL_IMASK;
    *reg_slot(&cores[me], name) = val;
}

/* Every loop that waits for another core has one of these in it: let that core run */
void mock_sysop(const char *op)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!strcmp(op, "wfe") || !strcmp(op, "dmb sy"))
        sched_yield();
    else if (me >= 0 && !strcmp(op, "msr daifclr, 4"))
        cores[me].serror_on = true;
    else if (me >= 0 && (!strcmp(op, "msr daifset, 4") || !strcmp(op, "msr daifset, #0xf")))
        cores[me].serror_on = false;
}

u64 mock_read64(u64 addr)
{
    for (int i = 0; i < EXTRA; i++)
        if (addr == IMPL(i))
            return cores[i].rvbar;
    CHECK(0, "read64(0x%lx)", addr);
    return 0;
}

void mock_write64(u64 addr, u64 val)
{
    for (int i = 0; i < EXTRA; i++)
        if (addr == IMPL(i)) {
            CHECK(sysregs_unlocked, "core %d: its RVBAR is written", i);
            cores[i].rvbar = val;
            return;
        }
    CHECK(0, "write64(0x%lx)", addr);
}

extern u8 _vectors_start[];

static void *core_thread(void *arg)
{
    me = (struct core *)arg - cores;
    smp_secondary_entry();
    return NULL;
}

void mock_write32(u64 addr, u32 val)
{
    if (addr == CPU_START + 4) /* "some kind of system level startup/status bit" */
        return;
    for (int i = 0; i < EXTRA; i++) {
        struct core *c = &cores[i];

        if (addr != CPU_START + 8 + 4 * (c->reg >> 8) || val != BIT(c->reg & 0xff))
            continue;
        CHECK(!c->starts++, "core %d is started again", i);
        /* Where it starts. One that starts elsewhere is not seen again */
        if (c->dead || (c->rvbar & ~0xfffUL) != (u64)_vectors_start)
            return;
        pthread_create(&c->thread, NULL, core_thread, c);
        return;
    }
    CHECK(0, "write32(0x%lx, 0x%x)", addr, val);
}

void udelay(u32 us)
{
    u64 until = now() + us * 1000UL;

    if (us >= 1000)
        usleep(1000);
    while (us < 1000 && now() < until)
        sched_yield();
}

/* The WFI of the M4, which loses the state of the core: no core is ever to get there */
void deep_wfi(void)
{
    wfi_used = true;
    sched_yield();
}

/* ---- the device tree ---- */

#define NODE_ARM_IO 1
#define NODE_CPUS   2
#define NODE_CPU(i) (100 + (i))

int adt_path_offset(const void *adt, const char *path)
{
    UNUSED(adt);
    if (!strcmp(path, "/arm-io"))
        return NODE_ARM_IO;
    if (!strcmp(path, "/cpus"))
        return NODE_CPUS;
    return -1;
}

const char *pmgr_name = "/arm-io/pmgr";

int pmgr_adt_path_offset_trace(const void *adt, int *path)
{
    UNUSED(adt);
    UNUSED(path);
    return 0;
}

int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    UNUSED(adt);
    UNUSED(path);
    UNUSED(prop);
    UNUSED(idx);
    UNUSED(size);
    *addr = PMGR;
    return 0;
}

static int next_core(int i)
{
    while (i < EXTRA && !cores[i].listed)
        i++;
    return i;
}

int adt_get_child_count(const void *adt, int node)
{
    int n = 0;

    UNUSED(adt);
    CHECK(node == NODE_CPUS, "%d", node);
    for (int i = next_core(0); i < EXTRA; i = next_core(i + 1))
        n++;
    return n;
}

int adt_first_child_offset(const void *adt, int node)
{
    UNUSED(adt);
    UNUSED(node);
    return NODE_CPU(next_core(0));
}

int adt_next_sibling_offset(const void *adt, int node)
{
    UNUSED(adt);
    return NODE_CPU(next_core(node - NODE_CPU(0) + 1));
}

int adt_getprop_copy(const void *adt, int node, const char *name, void *out, size_t len)
{
    struct core *c = &cores[node - NODE_CPU(0)];
    u64 impl[2] = {IMPL(node - NODE_CPU(0)), 0x1000};
    u32 val;

    UNUSED(adt);
    if (!strcmp(name, "cpu-impl-reg")) {
        memcpy(out, impl, len);
        return len;
    }
    if (!strcmp(name, "cpu-id"))
        val = c->id;
    else if (!strcmp(name, "reg") && !c->no_reg)
        val = c->reg;
    else
        return -1;
    memcpy(out, &val, len);
    return len;
}

const void *adt_getprop(const void *adt, int node, const char *name, u32 *len)
{
    UNUSED(adt);
    UNUSED(len);
    if (strcmp(name, "state"))
        return NULL;
    return node == NODE_CPU(BOOT) ? "running" : "waiting";
}

/* ---- the rest of m1n1 and of the hypervisor ---- */

void *adt;
u32 chip_id = 0x8132;
u8 _vectors_start[0x1000] __attribute__((aligned(0x1000)));
u8 _stack_top[16], _stack_bot[16];
char _hv_vectors_start[16];
bool display_is_external;
int uartproxy_iodev;

/*
 * The core has taken an exception from the guest, which masked SErrors, and has not taken a lock
 * since. The first lock it waits for, it waits for with them masked: it may wait for good, behind
 * a core that has stopped the guest (hv_exc_entry()).
 */
static __thread bool exception_taken;

void spin_lock(spinlock_t *lock)
{
    s64 free;

    if (exception_taken) {
        CHECK(!cores[me].serror_on, "core %d waits for the lock with SErrors on", me);
        exception_taken = false;
    }
    if (__atomic_load_n(&lock->lock, __ATOMIC_ACQUIRE) == me) {
        lock->count++;
        return;
    }
    for (;;) {
        free = -1;
        if (__atomic_compare_exchange_n(&lock->lock, &free, me, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        sched_yield();
    }
    lock->count++;
}

void spin_unlock(spinlock_t *lock)
{
    CHECK(lock->lock == me && lock->count > 0, "core %d unlocks what it does not have", me);
    if (!--lock->count)
        __atomic_store_n(&lock->lock, -1, __ATOMIC_RELEASE);
}

void __attribute__((noreturn)) flush_and_reboot(void)
{
    CHECK(0, "core %d: m1n1 restarts the machine", me);
    fprintf(stderr, "%s", printed_len > 2000 ? &printed[printed_len - 2000] : printed);
    _exit(1);
}

void hv_do_panic(void)
{
    CHECK(0, "core %d: hv_panic()", me);
}

#define STACK_SIZE 0x10000 /* src/smp_asm.h */
extern u8 secondary_stacks[MAX_CPUS][STACK_SIZE];

void test_mmu_configured(void)
{
    cores[me].mmu_inits++;
}

/*
 * The stack of this core, with what is on it, is thrown out of the caches. That is right once,
 * while nothing of it is in them (R50, SiliconNT docs/findings/0098)
 */
void test_ivac(void *addr, size_t len)
{
    CHECK(addr == secondary_stacks[me] && len == STACK_SIZE, "core %d: %p, 0x%lx", me, addr, len);
    cores[me].ivacs++;
}

u64 hv_xtrack_line(void)
{
    __atomic_add_fetch(&line_calls, 1, __ATOMIC_SEQ_CST);
    return cores[me].line;
}

/* On the boot core from hv_init(), and only there: the Redistributors are laid out */
static int vgic_global_inits;

void hv_vgic_init(void)
{
    CHECK(me == BOOT, "core %d", me);
    vgic_global_inits++;
    alive_at_vgic_init = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (smp_is_alive(i))
            alive_at_vgic_init |= BIT(i);
}

void hv_vgic_init_cpu(void)
{
    cores[me].vgic_inits++;
}

/* Of the core that is turned off, with the lock held. The IPI is for the tick, as in hv_vgic.c */
void hv_vgic_cpu_off(void)
{
    CHECK(bhl.lock == me && bhl.count == 1, "core %d: the lock is %ld's", me, (long)bhl.lock);
    CHECK(!hv_started_cpus[me], "core %d is still one the guest runs on", me);
    CHECK(hv_lead_cpu() >= 0 && hv_lead_cpu() != me, "core %d", hv_lead_cpu());
    cores[me].vgic_offs++;
    smp_send_ipi(hv_lead_cpu());
}

/* An IRQ of this core, with the lock held */
bool hv_vgic_handle_irq(void)
{
    CHECK(bhl.lock == me && bhl.count == 1, "core %d: the lock is %ld's", me, (long)bhl.lock);
    __atomic_store_n(&cores[me].irq, 0, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cores[me].irqs, 1, __ATOMIC_SEQ_CST);
    return true;
}

/* The tick, by which the watchdog of m1n1 knows that the hypervisor is alive */
void hv_wdt_pet(void)
{
    __atomic_add_fetch(&cores[me].ticks, 1, __ATOMIC_SEQ_CST);
}

/*
 * src/hv/hv_stop.c: without a host, the core that has stopped the guest stays where it is, with the
 * lock. What it does before that has siliconnt/nohost-model
 */
void model_stop(int reason, u32 code)
{
    if (!no_host)
        return;
    CHECK(reason == START_HV && code == HV_GUEST_RESET, "stopped for %d, %d", reason, code);
    __atomic_store_n(&stopped_on, me, __ATOMIC_SEQ_CST);
    for (;;)
        usleep(1000);
}

int model_host(int reason, u32 code)
{
    CHECK(bhl.lock == me && bhl.count == 1, "core %d: the lock is %ld's", me, (long)bhl.lock);
    __atomic_add_fetch(&host_calls, 1, __ATOMIC_SEQ_CST);
    return host ? host(reason, code) : EXC_EXIT_GUEST;
}

void hv_dbg_init_cpu(int cpu)
{
    CHECK(cpu == me, "core %d: the debug registers of core %d", me, cpu);
    cores[cpu].dbg_inits++;
}

void hv_pmu_init_cpu(int cpu)
{
    CHECK(cpu == me, "core %d: the PMU registers of core %d", me, cpu);
    cores[cpu].pmu_inits++;
}

void hv_xtrack_init_cpu(int cpu)
{
    CHECK(cpu == me, "core %d: the aborts of core %d", me, cpu);
    cores[cpu].xtrack_inits++;
}

/* clang-format off */
void pcie_shutdown(void) {}
int display_start_dcp(void) { return -1; }
void display_shutdown(int mode) { UNUSED(mode); }
void usb_hpm_restore_irqs(bool force) { UNUSED(force); }
bool gxf_enabled(void) { return false; }
bool in_gl12(void) { return false; }
void gxf_init(void) {}
u64 gl2_call(void *f, u64 a, u64 b, u64 c, u64 d) { UNUSED(f); UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); abort(); }
bool heapblock_left(size_t *left) { *left = 1 << 20; return true; }
/* clang-format on */

/* What has been hooked: the state register of a core, hv_cpu_state_setup_hook() of hv.c */
static u64 hooked[2 * EXTRA];
static int nr_hooked;

int hv_map_hook(u64 from, void *hook, u64 size)
{
    UNUSED(hook);
    CHECK(size == 8 && nr_hooked < 2 * EXTRA, "a hook at 0x%lx, 0x%lx bytes", from, size);
    if (nr_hooked < 2 * EXTRA)
        hooked[nr_hooked++] = from;
    return 0;
}

/* clang-format off */
bool hv_pa_rw(void *ctx, u64 addr, u64 *val, bool write, int width) { UNUSED(ctx); UNUSED(addr); UNUSED(val); UNUSED(write); UNUSED(width); return false; }
void hv_pt_init(void) {}
int hv_pt_check_ctrr(void) { return 0; }
void hv_aic_init(void) {}
bool hv_amcc_init(void) { return true; }
bool hv_cpu_power_init(void) { return true; }
bool hv_vgic_ranges_free(void) { return true; }
bool hv_vgic_has_aicmap(void) { return true; }
void hv_vgic_start(void) {}
void hv_vgic_stop(void) {}
void hv_vgic_report(void) {}
void hv_sprr_init(void) {}
void hv_sprr_init_secondary(int cpu) { UNUSED(cpu); }
void hv_sprr_timer_arm(void) {}
void hv_wdt_init(void) {}
void hv_wdt_stop(void) {}
void hv_wfi_init(void) {}
void hv_wfi_report(void) {}
void hv_psci_init(void) {}
void hv_psci_report(void) {}
void hv_pmu_init(void) {}
void hv_pmu_report(void) {}
void hv_dbg_init(void) {}
void hv_dbg_report(void) {}
void hv_xtrack_init(void) {}
void hv_xtrack_report(void) {}
void hv_vuart_poll(void) {}
bool hv_has_host(void) { return !no_host; }
void iodev_console_flush(void) {}
void iodev_handle_events(int id) { UNUSED(id); }
s64 iodev_can_read(int id) { UNUSED(id); return __atomic_load_n(&host_byte, __ATOMIC_SEQ_CST); }
void aic_write(u32 reg, u32 val) { UNUSED(reg); UNUSED(val); abort(); }
u32 aic_ack(void) { abort(); }
void cpu_sleep(bool deep) { UNUSED(deep); abort(); }
/* clang-format on */

/* ---- the guest ---- */

void __attribute__((noreturn)) hv_exit_guest(void)
{
    longjmp(cores[me].leave, 1);
}

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry, u64 spsr)
{
    struct core *c = &cores[me];

    c->got.x[0] = x0;
    c->got.x[1] = x1;
    c->got.x[2] = x2;
    c->got.x[3] = x3;
    c->got.entry = entry;
    c->got.spsr = spsr;
    if (!setjmp(c->leave)) {
        __atomic_store_n(&c->in_el2, false, __ATOMIC_SEQ_CST);
        __atomic_store_n(&c->in_guest, true, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&c->entered, 1, __ATOMIC_SEQ_CST);
        guest(me);
        CHECK(0, "core %d: the guest came to its end", me);
    }
    __atomic_store_n(&cores[me].in_guest, false, __ATOMIC_SEQ_CST);

    /* Whichever way it has left: nobody is kept waiting for it, and nothing is to interrupt it */
    CHECK(bhl.lock != me, "core %d has the lock", me);
    CHECK(!(__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_SEQ_CST) & BIT(me)) || hv_started_cpus[me],
          "core %d is still waited for", me);
    CHECK(!*reg_slot(&cores[me], "CNTP_CTL_EL0"), "core %d has a tick", me);
    CHECK(!*reg_slot(&cores[me], R_VM_TMR_FIQ), "core %d: the timers of the guest", me);
    /* The others: longer than hv_start() waits by itself before it looks for them (200 ms) */
    if (slow_back && me != BOOT)
        usleep(300000);
}

static u32 rnd(u32 n)
{
    static __thread u64 state;

    if (!state)
        state = 0x9e3779b97f4a7c15UL * (me + 1);
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (state >> 20) % n;
}

/* On the way out of EL2, where the lock has just been let go of */
void model_window(void)
{
    for (u32 n = rnd(4); n; n--)
        sched_yield();
}

/* An exception of the guest, with the lock held between the two */
/* Taking an exception masks SErrors, as the others (DAIF) */
static void take_exception(void)
{
    __atomic_store_n(&cores[me].in_el2, true, __ATOMIC_SEQ_CST);
    cores[me].serror_on = false;
    exception_taken = true;
}

static void exc_entry(void)
{
    take_exception();
    model_exc_entry();
}

static void exc_exit(void)
{
    model_exc_exit();
    __atomic_store_n(&cores[me].in_el2, false, __ATOMIC_SEQ_CST);
}

/* By whoever has the lock: no core runs the guest afterwards, or gets to before it is let go of */
static void rendezvous(void)
{
    hv_rendezvous();
    for (int look = 0; look < 3; look++) {
        for (int i = 0; i < NR; i++)
            CHECK(i == me || !__atomic_load_n(&cores[i].in_guest, __ATOMIC_SEQ_CST) ||
                      __atomic_load_n(&cores[i].in_el2, __ATOMIC_SEQ_CST),
                  "core %d runs the guest after a rendezvous of core %d", i, me);
        sched_yield();
    }
}

/* The guest goes on for a bit; on some core it would do something else in the meantime */
static void run_a_bit(void)
{
    u64 pending = isr(&cores[me]);

    if (pending & ISR_F) {
        /* The IPI or the tick: hv_exc_fiq() */
        take_exception();
        model_exc_fiq();
        __atomic_store_n(&cores[me].in_el2, false, __ATOMIC_SEQ_CST);
    } else if (pending & ISR_I) {
        exc_entry();
        hv_vgic_handle_irq();
        exc_exit();
    }
    sched_yield();
}

static void spin_in_guest(int cpu)
{
    UNUSED(cpu);
    for (;;)
        run_a_bit();
}

/* Something stops the guest on this core, and the host is told. Returns if it says "handled" */
static void stop_guest(u32 code)
{
    exc_entry();
    model_exc_proxy(START_HV, code);
    exc_exit();
}

/* ...and the host leaves the guest */
static void __attribute__((noreturn)) host_leaves(void)
{
    stop_guest(HV_USER_INTERRUPT);
    CHECK(0, "core %d is in the guest, which the host has left", me);
    abort();
}

/* The guest executes WFI: hv_handle_wfx() of hv_wfi.c */
static void wait_in_wfi(void)
{
    exc_entry();
    spin_unlock(&bhl);
    while (!(isr(&cores[me]) & (ISR_F | ISR_I)))
        sched_yield();
    spin_lock(&bhl);
    exc_exit();
}

/* PSCI CPU_OFF, as hv_psci.c has it. Returns on the last core, which is in the guest still */
static void cpu_off(void)
{
    exc_entry();
    model_cpu_off();
    CHECK(hv_get_started_cpus() == BIT(me), "core %d is not the last one: 0x%lx", me,
          hv_get_started_cpus());
    exc_exit();
}

/* CPU_ON as hv_psci.c carries it out. The caller's stack is gone once the call is over. */
static int cpu_on(int cpu, void *entry, u64 x0)
{
    u64 regs[4] = {x0};
    bool ok;

    if (hv_started_cpus[cpu])
        return -4;
    cores[cpu].want_x0 = x0;
    cores[cpu].want_entry = entry;
    ok = hv_start_secondary(cpu, entry, regs);
    memset(regs, 0xee, sizeof(regs));
    __asm__ volatile("" : : "r"(regs) : "memory");
    return ok ? 0 : -1;
}

static void reset(void)
{
    for (int i = 0; i < EXTRA; i++) {
        struct core *c = &cores[i];

        memset(c, 0, sizeof(*c));
        c->listed = i < NR;
        c->id = i;
        c->reg = i < 6 ? i : i < NR ? 0x100 | (i - 6) : 0x200;
        c->mpidr = i < 6 ? i : 0x10100 | (i - 6);
        c->rvbar = (u64)_vectors_start | 1;
        c->line = 64;
    }
    me = BOOT;
    sysregs_unlocked = false;
    set_sysregs_unlocked(false);
    guest = spin_in_guest;
    /* What exception_initialize() leaves on the boot core: nothing masked; and its MMU is on */
    *reg_slot(&cores[BOOT], "DAIF") = 0;
    *reg_slot(&cores[BOOT], "SCTLR_EL1") = SCTLR_M;
    /* FEAT_ECV: the cores that do not tick for all have one tick a second */
    *reg_slot(&cores[BOOT], "ID_AA64MMFR0_EL1") = 1UL << 60;
    host = NULL;
    host_calls = 0;
    host_byte = false;
    no_host = false;
    stopped_on = -1;
    slow_back = false;
    vgic_global_inits = 0;
    nr_hooked = 0;
    /* UNKNOWN after a reset (Arm DDI 0487) */
    for (int i = 0; i < NR; i++)
        *reg_slot(&cores[i], "CNTVOFF_EL2") = 0xbad0000 + i;
}

/* ---- every core comes into EL2 ---- */

#define ALL_BUT_BOOT ((BIT(NR) - 1) & ~BIT(BOOT))

static void boot_leaves(int cpu)
{
    CHECK(cpu == BOOT, "core %d is in the guest", cpu);
    CHECK(*reg_slot(&cores[cpu], "CNTVOFF_EL2") == 0, "CNTVOFF_EL2 is 0x%lx",
          *reg_slot(&cores[cpu], "CNTVOFF_EL2"));
    host_leaves();
}

static void check_all_up(void)
{
    u64 regs[4] = {0x1111, 0x2222, 0x3333, 0x4444};

    CHECK(hv_cpus_ready(), "not every core is in EL2");
    /* Before there is a vGIC, which has a Redistributor for those that are alive then */
    CHECK(alive_at_vgic_init == ALL_BUT_BOOT, "0x%lx", alive_at_vgic_init);
    CHECK(line_calls == NR, "%d cores were asked for their cache line", line_calls);
    CHECK(!wfi_used, "a core waited in WFI");
    for (int i = 0; i < NR; i++)
        CHECK(cores[i].starts == (i != BOOT), "core %d was started %d times", i, cores[i].starts);

    /* And then the guest is entered, on the boot core, which keeps its masks */
    guest = boot_leaves;
    *reg_slot(&cores[BOOT], "DAIF") = 0x40;
    hv_start((void *)0x8000001000, regs);
    CHECK(cores[BOOT].entered == 1, "%d", cores[BOOT].entered);
    CHECK(cores[BOOT].got.entry == (void *)0x8000001000 && cores[BOOT].got.spsr == 0x45,
          "entry %p, SPSR 0x%lx", cores[BOOT].got.entry, cores[BOOT].got.spsr);
    for (int i = 0; i < 4; i++)
        CHECK(cores[BOOT].got.x[i] == regs[i], "x%d = 0x%lx", i, cores[BOOT].got.x[i]);
    for (int i = 0; i < NR; i++)
        CHECK(i == BOOT || !cores[i].entered, "core %d was in the guest", i);
}

static void test_all_up(void)
{
    smp_init();
    hv_init();
    check_all_up();

    /* A second time, as a script that runs two guests does: they are there already */
    line_calls = 0;
    cores[BOOT].entered = 0;
    hv_init();
    check_all_up();
}

/*
 * The register of each core at cpu-impl-reg + 0x100 is hooked for macOS under SPTM, and for no
 * other guest: Windows reads it as it is (SiliconNT docs/DESIGN.md, "设计核心" 1)
 */
static void test_state_hook(void)
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    guest = boot_leaves;
    hv_start((void *)0x8000001000, regs);
    CHECK(cores[BOOT].entered == 1 && !nr_hooked, "%d hooks, without SPTM", nr_hooked);

    hv_sptm_mode = SPTM_NESTED_VIRT;
    cores[BOOT].entered = 0;
    hv_init();
    hv_start((void *)0x8000001000, regs);
    CHECK(cores[BOOT].entered == 1 && nr_hooked == NR, "%d hooks, with SPTM", nr_hooked);
    for (int i = 0; i < NR; i++) {
        bool found = false;

        for (int k = 0; k < nr_hooked; k++)
            found |= hooked[k] == IMPL(i) + 0x100;
        CHECK(found, "no hook for core %d", i);
    }
    hv_sptm_mode = SPTM_NONE;
}

/* RVBAR is not locked: m1n1 writes it, whatever is there */
static void test_all_up_unlocked(void)
{
    sysregs_unlocked = true;
    set_sysregs_unlocked(true);
    cores[3].rvbar = 0x10004724000;
    cores[BOOT].rvbar = 0x10004724000;
    smp_init();
    hv_init();
    check_all_up();
    CHECK(cores[BOOT].rvbar == (u64)_vectors_start, "0x%lx", cores[BOOT].rvbar);
}

/* The host has started some of them before. They wait with WFE all the same. */
static void test_started_before(void)
{
    smp_init();
    CHECK(smp_start_secondary(0) && smp_start_secondary(9), "cores 0 and 9 do not start");
    wfi_used = false; /* until hv_init() */
    hv_init();
    /* Those two may be on their way out of the WFI they were in: hv_init() has sent the IPIs */
    wfi_used = false;
    check_all_up();
}

/* ---- one that does not come into EL2: the guest is not entered, and it says which and why ---- */

static void check_refused(const char *line)
{
    u64 regs[4] = {0};
    char text[200];

    smp_init();
    hv_init();
    CHECK(was_printed(line), "hv_init() does not say: %s", line);
    CHECK(!hv_cpus_ready(), "every core is in EL2");
    guest = boot_leaves;
    hv_start((void *)0x8000001000, regs);
    CHECK(!cores[BOOT].entered, "the guest was entered");
    snprintf(text, sizeof(text), "%s\n", line);
    /* By hv_init(), by the question above, and by hv_start() */
    CHECK(lines_with(text) == 3, "%d lines: %s", lines_with(text), line);
    CHECK(was_printed("HV: the guest would not have all its CPUs; can't start the hypervisor\n"),
          "hv_start() does not say that it does not start");
    if (failures)
        fprintf(stderr, "%s", printed);
}

static void test_locked_elsewhere(void)
{
    cores[3].rvbar = 0x10004724001;
    check_refused("HV: CPU 3 is not in EL2: its reset vector is locked to another image of m1n1");
    CHECK(!cores[3].starts, "core 3 was started");
    CHECK(was_printed("RVBAR (=0x10004724000) is locked"), "the address is not there");
    CHECK(lines_with(" is not in EL2") == 3, "another core is said not to be in EL2");
}

static void test_dead(void)
{
    cores[9].dead = true;
    check_refused("HV: CPU 9 is not in EL2: it did not come up after it was started");
    CHECK(lines_with(" is not in EL2") == 3, "another core is said not to be in EL2");
}

static void test_two_missing(void)
{
    cores[0].dead = true;
    cores[7].dead = true;
    check_refused("HV: CPU 0 is not in EL2: it did not come up after it was started");
    CHECK(lines_with("HV: CPU 7 is not in EL2: it did not come up after it was started\n") == 3,
          "core 7 is not named");
}

static void test_small_lines(void)
{
    cores[2].line = 32;
    check_refused("HV: CPU 2 is not in EL2: its data cache lines are too small");
    CHECK(was_printed("CPU 2: the smallest data cache line is 32 bytes"), "the size is not there");
}

static void test_small_lines_boot(void)
{
    cores[BOOT].line = 32;
    check_refused("HV: CPU 6 is not in EL2: its data cache lines are too small");
}

static void test_larger_lines(void)
{
    cores[2].line = 128;
    smp_init();
    hv_init();
    check_all_up();
}

static void test_no_reg(void)
{
    cores[5].no_reg = true;
    check_refused("HV: CPU 5 is not in EL2: the device tree does not say how to start it");
}

/* A core whose index is beyond what m1n1 has room for */
static void test_beyond(void)
{
    cores[NR].listed = true;
    cores[NR].id = 40;
    check_refused("HV: 1 CPUs are not in EL2: m1n1 has room for CPUs 0 to 31 only");
}

static void busy_for_good(void)
{
    for (;;)
        sched_yield();
}

static void test_busy(void)
{
    smp_init();
    hv_init();
    smp_call4(4, busy_for_good, 0, 0, 0, 0);
    check_refused("HV: CPU 4 is not in EL2: it is busy with something else of m1n1");
}

/* ---- CPU_ON ---- */

static void wait_entered(int cpu, int times)
{
    while (__atomic_load_n(&cores[cpu].entered, __ATOMIC_SEQ_CST) < times)
        sched_yield();
}

/* What a core has to find, checked on that core as the first thing in the guest */
static void check_state(int cpu)
{
    struct core *c = &cores[cpu];

    CHECK(c->got.x[0] == c->want_x0 && !c->got.x[1] && !c->got.x[2] && !c->got.x[3],
          "core %d: x0 to x3 = 0x%lx 0x%lx 0x%lx 0x%lx, not 0x%lx 0 0 0", cpu, c->got.x[0],
          c->got.x[1], c->got.x[2], c->got.x[3], c->want_x0);
    CHECK(c->got.entry == c->want_entry, "core %d: entry %p", cpu, c->got.entry);
    CHECK(c->got.spsr == 0x3c5, "core %d: SPSR 0x%lx", cpu, c->got.spsr);
    CHECK(*reg_slot(c, "VMPIDR_EL2") == (BIT(31) | c->mpidr), "core %d: VMPIDR_EL2 0x%lx", cpu,
          *reg_slot(c, "VMPIDR_EL2"));
    CHECK(*reg_slot(c, "HCR_EL2") == *reg_slot(&cores[BOOT], "HCR_EL2"), "core %d: HCR_EL2", cpu);
    CHECK(*reg_slot(c, R_SCTLR_EL12) == 0x30d50980, "core %d: SCTLR_EL12 0x%lx", cpu,
          *reg_slot(c, R_SCTLR_EL12));
    CHECK(*reg_slot(c, "VBAR_EL12") == 0, "core %d: VBAR_EL12 0x%lx", cpu,
          *reg_slot(c, "VBAR_EL12"));
    CHECK(c->vgic_inits == 1 && c->dbg_inits == 1 && c->pmu_inits == 1 && c->xtrack_inits == 1,
          "core %d: set up %d %d %d %d times", cpu, c->vgic_inits, c->dbg_inits, c->pmu_inits,
          c->xtrack_inits);
    c->vgic_inits = c->dbg_inits = c->pmu_inits = c->xtrack_inits = 0;
    /* ...and these once for all times it was in the guest, since it came up; not the boot core */
    CHECK(c->mmu_inits == (cpu != BOOT) && c->ivacs == (cpu != BOOT),
          "core %d: its MMU %d times, its stack %d times", cpu, c->mmu_inits, c->ivacs);
    CHECK(*reg_slot(c, "SCTLR_EL1") & SCTLR_M, "core %d: the MMU of EL2 is off", cpu);
    CHECK(__atomic_load_n(&hv_started_cpus[cpu], __ATOMIC_SEQ_CST), "core %d is not started", cpu);
    CHECK(*reg_slot(c, "CNTVOFF_EL2") == model_stolen_time(),
          "core %d: CNTVOFF_EL2 0x%lx, not 0x%lx", cpu, *reg_slot(c, "CNTVOFF_EL2"),
          model_stolen_time());
    /* What hv_exc.c keeps of it, and what is behind that */
    CHECK(model_records_clear(cpu), "core %d: something is left of the last time", cpu);
    CHECK(!(*reg_slot(c, R_PMCR0) & PMCR0_IRQ), "core %d: PMCR0 0x%lx", cpu,
          *reg_slot(c, R_PMCR0));
    CHECK(*reg_slot(c, "CNTP_CTL_EL0") == CTL_ENABLE, "core %d: no tick", cpu);
    /* The guest turns things on, as Windows would have left them when the core stops */
    *reg_slot(c, R_SCTLR_EL12) |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_EE;
    *reg_slot(c, "VBAR_EL12") = 0xfffff80000000000;
    *reg_slot(c, R_PMCR0) |= PMCR0_IRQ;
    model_records_fill(cpu);
}

static void guest_cpu_on(int cpu)
{
    u64 started = BIT(BOOT);

    if (cpu != BOOT) {
        check_state(cpu);
        spin_in_guest(cpu);
    }

    for (int round = 1; round <= 3; round++) {
        /* The host takes some time: the cores that come in have the same offset as this one */
        exc_entry();
        model_steal_time(1000);
        exc_exit();
        CHECK(*reg_slot(&cores[BOOT], "CNTVOFF_EL2") == model_stolen_time() &&
                  model_stolen_time() == 1000UL * round,
              "CNTVOFF_EL2 is 0x%lx", *reg_slot(&cores[BOOT], "CNTVOFF_EL2"));

        for (int n = 1; n < NR; n++) {
            int target = (BOOT + n) % NR;

            exc_entry();
            CHECK(cpu_on(target, (void *)(0x8000400000 + 0x1000 * target), 0xc0de0000 + target) ==
                      0,
                  "CPU_ON of core %d", target);
            /* It is in the guest for everybody by now: the answer to a second call */
            CHECK(hv_started_cpus[target], "core %d is not started", target);
            CHECK(cpu_on(target, NULL, 0) == -4, "a second CPU_ON of core %d", target);
            started |= BIT(target);
            CHECK(hv_get_started_cpus() == started, "the host is told 0x%lx, not 0x%lx",
                  hv_get_started_cpus(), started);
            /* Had the host something to say now, it would be in EL2 with the others */
            rendezvous();
            CHECK(cores[target].entered == round, "core %d has not been in the guest", target);
            exc_exit();
            wait_entered(target, round);
        }
        /* Everybody into EL2, as the proxy has it, and on. They all stay in the guest. */
        exc_entry();
        rendezvous();
        exc_exit();
        for (int i = 0; i < 100; i++) {
            usleep(20);
            run_a_bit();
        }
        for (int target = 0; target < NR; target++)
            CHECK(hv_started_cpus[target] &&
                      __atomic_load_n(&cores[target].in_guest, __ATOMIC_SEQ_CST),
                  "core %d has left the guest", target);
        /*
         * The host takes them out of the guest; they can come back. They leave on the IPI of the
         * rendezvous, while this core has the lock.
         */
        for (int target = 0; target < NR; target++) {
            if (target == BOOT)
                continue;
            exc_entry();
            hv_exit_cpu(target);
            rendezvous();
            exc_exit();
            while (__atomic_load_n(&hv_started_cpus[target], __ATOMIC_SEQ_CST))
                run_a_bit();
            started &= ~BIT(target);
            CHECK(hv_get_started_cpus() == started, "the host is told 0x%lx, not 0x%lx",
                  hv_get_started_cpus(), started);
        }
    }

    /* Nothing of it is left: they are all where they were */
    exc_entry();
    CHECK(cpu_on(0, (void *)0x8000400000, 0xc0de0000) == 0, "CPU_ON of core 0 at the end");
    exc_exit();
    wait_entered(0, 4);
    host_leaves();
}

static void test_cpu_on(void)
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    guest = guest_cpu_on;
    for (int i = 0; i < NR; i++) {
        /* What a core has after a reset (R7b), and m1n1's own vectors (src/exception.c) */
        *reg_slot(&cores[i], R_SCTLR_EL12) = 0x30d50980 | SCTLR_EE;
        *reg_slot(&cores[i], "VBAR_EL12") = 0x10004730000;
        /* Nothing of the guest that ran on it before */
        cores[i].vgic_inits = 0;
    }
    hv_start((void *)0x8000001000, regs);
    for (int i = 0; i < NR; i++) {
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
        CHECK(cores[i].entered == (i == BOOT ? 1
                                   : i       ? 3
                                             : 4),
              "core %d: %d times", i, cores[i].entered);
    }
    CHECK(!hv_cpus_in_guest, "0x%lx", hv_cpus_in_guest);
    /* One line for each power of two, not for each time */
    CHECK(lines_with("HV: CPU 0 (MPIDR 0x0) enters the guest") == 3, "%d lines about core 0",
          lines_with("HV: CPU 0 (MPIDR 0x0) enters the guest"));
    CHECK(lines_with("HV: CPU 9 (MPIDR 0x10103) enters the guest") == 2, "%d lines about core 9",
          lines_with("HV: CPU 9 (MPIDR 0x10103) enters the guest"));
}

/* One that cannot be let in: the caller hears of it, and nothing has happened */
static void guest_refused(int cpu)
{
    u64 regs[4] = {0};

    if (cpu == 3)
        spin_in_guest(cpu);
    CHECK(cpu == BOOT, "core %d is in the guest", cpu);
    exc_entry();
    /* The host, about a core that is in the guest: hv_psci.c does not come here with one */
    CHECK(cpu_on(3, (void *)0x8000400000, 1) == 0, "CPU_ON of core 3");
    CHECK(!hv_start_secondary(3, (void *)0x8000400000, regs), "core 3 is let in twice");
    CHECK(cpu_on(4, (void *)0x8000400000, 1) == -1, "CPU_ON of a core that is busy");
    CHECK(!hv_started_cpus[4], "core 4 is started");
    /* The boot core, if it were on its way to where it waits for good; one that is not there */
    hv_started_cpus[BOOT] = false;
    CHECK(cpu_on(BOOT, (void *)0x8000400000, 1) == -1, "CPU_ON of the boot core");
    hv_started_cpus[BOOT] = true;
    CHECK(cpu_on(20, (void *)0x8000400000, 1) == -1, "CPU_ON of a core that is not there");
    exc_exit();
    host_leaves();
}

static void test_refused(void)
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    smp_call4(4, busy_for_good, 0, 0, 0, 0);
    guest = guest_refused;
    hv_start((void *)0x8000001000, regs);
    CHECK(was_printed("HV: CPU 4 is busy with something else of m1n1\n"), "nothing is said");
    CHECK(was_printed("HV: CPU 3 is in the guest already\n"), "nothing is said of core 3");
    for (int i = 0; i < NR; i++)
        CHECK(cores[i].entered == (i == BOOT || i == 3), "core %d: %d times", i, cores[i].entered);
}

/* ---- in and out of the guest, on every core at once ---- */

static int rounds_done;
static u64 next_x0 = 0x100;
#define ROUNDS 20000

static void guest_on_off(int cpu)
{
    if (cpu != BOOT)
        check_state(cpu);

    while (cpu != BOOT || __atomic_load_n(&rounds_done, __ATOMIC_SEQ_CST) < ROUNDS) {
        switch (rnd(8)) {
            case 0:
            case 1:
            case 2: {
                /* Any core turns any other one on */
                int target = rnd(NR);
                int ret;

                exc_entry();
                ret = cpu_on(target, (void *)(0x8000400000 + target), next_x0++);
                CHECK(ret == 0 || ret == -4, "CPU_ON of core %d by core %d: %d", target, cpu, ret);
                exc_exit();
                if (!ret)
                    __atomic_add_fetch(&rounds_done, 1, __ATOMIC_SEQ_CST);
                break;
            }
            case 3:
                /* ...and leaves by itself: what the host does with hv.p.hv_exit_cpu() */
                if (cpu == BOOT)
                    break;
                exc_entry();
                hv_exit_cpu(-1);
                exc_exit();
                CHECK(0, "core %d does not leave the guest", cpu);
                break;
            case 4:
                if (rnd(4))
                    break;
                exc_entry();
                rendezvous();
                exc_exit();
                break;
            default:
                run_a_bit();
                break;
        }
    }

    host_leaves();
}

static void test_on_off(void)
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    guest = guest_on_off;
    for (int i = 0; i < NR; i++) {
        *reg_slot(&cores[i], R_SCTLR_EL12) = 0x30d50980;
        cores[i].vgic_inits = 0;
    }
    /* Printing is not what this is about */
    hv_start((void *)0x8000001000, regs);
    for (int i = 0; i < NR; i++)
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
    CHECK(!hv_cpus_in_guest, "0x%lx", hv_cpus_in_guest);
}

/*
 * ---- rendezvous: every core goes in and out of EL2 all the time, and any of them may want all
 * of them there; then they leave the guest one by one, the boot core in the middle of it ----
 */

static int rendezvous_done;
#define RENDEZVOUS 200 /* for each guest... */
#define GUESTS     40  /* ...of which the boot core leaves each once */

static void guest_rendezvous(int cpu)
{
    bool last = false;

    if (cpu == BOOT) {
        for (int i = 0; i < NR; i++) {
            if (i == BOOT)
                continue;
            exc_entry();
            CHECK(cpu_on(i, (void *)0x8000400000, 0) == 0, "CPU_ON of core %d", i);
            exc_exit();
        }
    }

    for (;;) {
        exc_entry();
        if (!rnd(4)) {
            rendezvous();
            last = __atomic_add_fetch(&rendezvous_done, 1, __ATOMIC_SEQ_CST) > RENDEZVOUS;
        }
        /* The way out of hv_exc_exit(), with everybody else in a rendezvous still */
        if (cpu == BOOT && last)
            hv_exit_cpu(-1);
        exc_exit();
        run_a_bit();
    }
}

static void test_rendezvous(void)
{
    u64 regs[4] = {0};

    smp_init();
    guest = guest_rendezvous;
    for (int n = 0; n < GUESTS; n++) {
        hv_init();
        rendezvous_done = 0;
        hv_start((void *)0x8000001000, regs);
        for (int i = 0; i < NR; i++)
            CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
        CHECK(!hv_cpus_in_guest, "0x%lx", hv_cpus_in_guest);
        CHECK(!hv_get_started_cpus(), "0x%lx", hv_get_started_cpus());
    }
}

/* ---- the host leaves the guest while the others go on with CPU_ON ---- */

/*
 * Cores 8 and 9 are the last that hv_start() makes leave. Until then they turn every other core
 * on that is not, over and over; those leave by themselves while the boot core is there, and
 * stay once it is not.
 */
static bool boot_has_left;

static void guest_leaving(int cpu)
{
    if (cpu == BOOT) {
        exc_entry();
        cpu_on(8, (void *)0x8000400000, 0);
        cpu_on(9, (void *)0x8000400000, 0);
        exc_exit();
        for (int i = 0; i < NR; i++)
            wait_entered(i, 1);
        __atomic_store_n(&boot_has_left, true, __ATOMIC_SEQ_CST);
        host_leaves();
    }

    for (;;) {
        if (cpu >= 8) {
            for (int target = 0; target < 8; target++) {
                int ret;

                exc_entry();
                ret = cpu_on(target, (void *)0x8000400000, 0);
                CHECK(ret == 0 || ret == -4, "CPU_ON of core %d: %d", target, ret);
                exc_exit();
            }
        } else if (!__atomic_load_n(&boot_has_left, __ATOMIC_SEQ_CST)) {
            exc_entry();
            hv_exit_cpu(-1);
            exc_exit();
        }
        run_a_bit();
    }
}

static void test_leaving(void)
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    guest = guest_leaving;
    hv_start((void *)0x8000001000, regs);
    for (int i = 0; i < NR; i++)
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
    usleep(100000);
    for (int i = 0; i < NR; i++)
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d came back into the guest", i);

    /* And the next guest starts like the first */
    hv_init();
    boot_has_left = false;
    for (int i = 0; i < NR; i++)
        cores[i].entered = 0;
    hv_start((void *)0x8000001000, regs);
    for (int i = 0; i < NR; i++)
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
}

/* ---- the guest turns cores off and on, and somebody leaves it ---- */

static void check_nobody_left(void)
{
    for (int i = 0; i < NR; i++) {
        CHECK(!hv_started_cpus[i] && !cores[i].in_guest, "core %d is still in the guest", i);
        /* It is where it can be called: nobody is to lose a call to it from here on */
        CHECK(i == BOOT ? !smp_is_alive(i) : smp_is_idle(i), "core %d is not back", i);
    }
    CHECK(!hv_cpus_in_guest, "0x%lx", hv_cpus_in_guest);
    CHECK(!hv_get_started_cpus(), "0x%lx", hv_get_started_cpus());
    CHECK(bhl.lock == -1, "core %ld has the lock", (long)bhl.lock);
}

/* A guest, as after a reset of the machine: what check_state() wants to find */
static void run_guest(void (*what)(int))
{
    u64 regs[4] = {0};

    smp_init();
    hv_init();
    for (int i = 0; i < NR; i++) {
        *reg_slot(&cores[i], R_SCTLR_EL12) = 0x30d50980;
        cores[i].vgic_inits = cores[i].dbg_inits = cores[i].pmu_inits = cores[i].xtrack_inits = 0;
        cores[i].entered = cores[i].ticks = cores[i].irqs = cores[i].vgic_offs = 0;
        __atomic_store_n(&cores[i].irq, 0, __ATOMIC_SEQ_CST);
    }
    vgic_global_inits = 0;
    host_calls = 0;
    guest = what;
    hv_start((void *)0x8000001000, regs);
    check_nobody_left();
    /* The state of the vGIC is the guest's for as long as it runs */
    CHECK(!vgic_global_inits, "the vGIC was set up %d times more", vgic_global_inits);
}

static void turn_on(int cpu)
{
    int times = __atomic_load_n(&cores[cpu].entered, __ATOMIC_SEQ_CST) + 1;

    exc_entry();
    CHECK(cpu_on(cpu, (void *)(0x8000400000 + 0x1000 * cpu), 0xc0de0000 + cpu) == 0,
          "CPU_ON of core %d by core %d", cpu, me);
    exc_exit();
    wait_entered(cpu, times);
}

static void wait_off(int cpu)
{
    while (__atomic_load_n(&hv_started_cpus[cpu], __ATOMIC_SEQ_CST))
        run_a_bit();
}

static void run_for(int ms)
{
    u64 until = now() + ms * 1000000UL;

    while (now() < until)
        run_a_bit();
}

#define TICKS_MS 100 /* so long... */
#define SOME     5   /* ...and at least so many ticks, of one each millisecond */

static int ticks_of(int cpu)
{
    return __atomic_load_n(&cores[cpu].ticks, __ATOMIC_SEQ_CST);
}

/* This core goes on for a while: in that time `cpu`, this one or another, ticks, and no other */
static void check_ticks(int cpu, const char *when)
{
    int before[NR];

    for (int i = 0; i < NR; i++)
        before[i] = ticks_of(i);
    run_for(TICKS_MS);
    for (int i = 0; i < NR; i++) {
        int n = ticks_of(i) - before[i];

        CHECK(i == cpu ? n >= SOME : n <= 1, "%s: core %d has ticked %d times in %d ms", when, i, n,
              TICKS_MS);
    }
}

/*
 * Every core turns others on and itself off, the boot core too, while AIC tells the boot core of
 * lines, in the guest or not. In the end several have something for the host at once, which
 * leaves the guest on whichever comes first, and hears of nothing more.
 */
static u64 off_rounds, off_raised;
static bool last_was_off;
#define OFF_ROUNDS 2000 /* for each guest */
#define OFF_GUESTS 10

static void guest_off(int cpu)
{
    if (cpu != BOOT || __atomic_load_n(&cores[cpu].entered, __ATOMIC_SEQ_CST) > 1)
        check_state(cpu);

    while (__atomic_load_n(&off_rounds, __ATOMIC_SEQ_CST) < OFF_ROUNDS) {
        switch (rnd(10)) {
            case 0:
            case 1:
            case 2:
            case 3: {
                int target = rnd(NR);
                int ret;

                exc_entry();
                ret = cpu_on(target, (void *)(0x8000400000 + target), next_x0++);
                CHECK(ret == 0 || ret == -4, "CPU_ON of core %d by core %d: %d", target, cpu, ret);
                exc_exit();
                if (!ret)
                    __atomic_add_fetch(&off_rounds, 1, __ATOMIC_SEQ_CST);
                break;
            }
            case 4:
                cpu_off();
                __atomic_store_n(&last_was_off, true, __ATOMIC_SEQ_CST);
                break;
            case 5:
                if (rnd(4))
                    break;
                exc_entry();
                rendezvous();
                exc_exit();
                break;
            case 6:
                /* A line, for the boot core: it is heard of, wherever that core is */
                if (__atomic_exchange_n(&cores[BOOT].irq, 1, __ATOMIC_SEQ_CST))
                    break;
                __atomic_add_fetch(&off_raised, 1, __ATOMIC_SEQ_CST);
                if (cpu != BOOT)
                    while (__atomic_load_n(&cores[BOOT].irq, __ATOMIC_SEQ_CST))
                        run_a_bit();
                break;
            default:
                run_a_bit();
                break;
        }
    }

    if (rnd(3))
        run_for(1);
    host_leaves();
}

static void test_cpu_off(void)
{
    int boot_was_off = 0, boot_irqs = 0;

    for (int n = 0; n < OFF_GUESTS; n++) {
        off_rounds = off_raised = 0;
        run_guest(guest_off);
        CHECK(host_calls == 1, "the host was told of %d things", host_calls);
        for (int i = 0; i < NR; i++)
            CHECK(i == BOOT || cores[i].vgic_offs <= cores[i].entered, "core %d: %d, %d", i,
                  cores[i].vgic_offs, cores[i].entered);
        boot_was_off += cores[BOOT].vgic_offs;
        boot_irqs += cores[BOOT].irqs;
        CHECK(!cores[0].irqs && !cores[9].irqs, "another core has taken an IRQ");
    }
    CHECK(boot_was_off > OFF_GUESTS, "the boot core was off %d times", boot_was_off);
    CHECK(boot_irqs > OFF_GUESTS, "%d IRQs", boot_irqs);
    /* Not a line for each time a core leaves: at most one for each power of two, and the end */
    CHECK(lines_with("HV: Exiting from CPU 9\n") <= 13 * OFF_GUESTS, "%d lines",
          lines_with("HV: Exiting from CPU 9\n"));
}

/* The host leaves the guest on a core that is not the boot core: all of them leave */
static void guest_exit_secondary(int cpu)
{
    if (cpu == BOOT) {
        turn_on(0);
        turn_on(9);
        turn_on(3);
    }
    if (cpu == 3)
        host_leaves();
    spin_in_guest(cpu);
}

static void test_exit_secondary(void)
{
    slow_back = true;
    run_guest(guest_exit_secondary);
    CHECK(host_calls == 1, "%d", host_calls);
    /* And again: nothing of it is left */
    run_guest(guest_exit_secondary);
}

/* ...while the guest has the boot core turned off */
static void guest_exit_boot_off(int cpu)
{
    if (cpu == BOOT) {
        turn_on(0);
        turn_on(3);
        cpu_off();
        CHECK(0, "the boot core is in the guest");
    }
    if (cpu == 3) {
        wait_off(BOOT);
        host_leaves();
    }
    spin_in_guest(cpu);
}

static void test_exit_boot_off(void)
{
    run_guest(guest_exit_boot_off);
    CHECK(host_calls == 1 && cores[BOOT].vgic_offs == 1, "%d %d", host_calls,
          cores[BOOT].vgic_offs);
    run_guest(guest_exit_boot_off);
}

/* ...which the host does with hv_exit_cpu() as well, of the boot core */
static int host_exits_boot(int reason, u32 code)
{
    UNUSED(reason);
    UNUSED(code);
    hv_exit_cpu(BOOT);
    return EXC_RET_HANDLED;
}

static void test_exit_cpu_boot(void)
{
    host = host_exits_boot;
    run_guest(guest_exit_boot_off);
    run_guest(guest_exit_secondary);
}

/* The host takes the last core out of the guest: there is nothing left to wait for */
static void guest_exit_last(int cpu)
{
    if (cpu == BOOT) {
        turn_on(3);
        cpu_off();
        CHECK(0, "the boot core is in the guest");
    }
    wait_off(BOOT);
    host_leaves();
}

static int host_exits_this(int reason, u32 code)
{
    UNUSED(reason);
    UNUSED(code);
    hv_exit_cpu(-1);
    return EXC_RET_HANDLED;
}

static void test_exit_last(void)
{
    host = host_exits_this;
    run_guest(guest_exit_last);
}

/* Core 0 waits out a WFI of the guest in EL2 (hv_wfi.c), and no core is in the guest */
static void guest_exit_wfi(int cpu)
{
    if (cpu == BOOT) {
        turn_on(0);
        while (!__atomic_load_n(&cores[0].in_el2, __ATOMIC_SEQ_CST))
            run_a_bit();
        run_for(5);
        host_leaves();
    }
    wait_in_wfi();
    CHECK(0, "core 0 is in the guest");
}

static void test_exit_wfi(void)
{
    u64 start = now();

    run_guest(guest_exit_wfi);
    /* The tick of core 0 would have it look a second after it came */
    CHECK(now() - start < 500000000UL, "it took %ld ms", (now() - start) / 1000000);
}

/*
 * Two cores have something for the host at once. It leaves the guest with the first; the second,
 * which waits for the lock meanwhile, does not tell it anything.
 */
static void guest_two_stops(int cpu)
{
    static int there;

    if (cpu == BOOT) {
        turn_on(3);
        turn_on(4);
        spin_in_guest(cpu);
    }
    __atomic_add_fetch(&there, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&there, __ATOMIC_SEQ_CST) < 2)
        run_a_bit();
    stop_guest(HV_GUEST_RESET);
    /* A stop for good: whatever the host says, the guest does not go on */
    for (;;)
        stop_guest(HV_GUEST_RESET);
}

static int host_leaves_late(int reason, u32 code)
{
    UNUSED(reason);
    CHECK(code == HV_GUEST_RESET, "%d", code);
    usleep(20000);
    return EXC_EXIT_GUEST;
}

static void test_two_stops(void)
{
    host = host_leaves_late;
    run_guest(guest_two_stops);
    CHECK(host_calls == 1, "the host was told of %d things", host_calls);
}

/*
 * Without a host nothing of this happens: a guest that is stopped is not left, wherever it is
 * stopped, and the boot core takes its IRQs, as with a host, but is not heard of otherwise
 */
static void guest_no_host(int cpu)
{
    if (cpu == BOOT) {
        turn_on(0);
        turn_on(3);
        cpu_off();
        CHECK(0, "the boot core is in the guest");
    }
    if (cpu == 3) {
        wait_off(BOOT);
        for (int n = 0; n < 100; n++) {
            __atomic_store_n(&cores[BOOT].irq, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&cores[BOOT].irq, __ATOMIC_SEQ_CST))
                run_a_bit();
        }
        CHECK(cores[BOOT].irqs == 100, "%d IRQs", cores[BOOT].irqs);
        /* Something comes in where a host would be: core 0, which ticks, does not mind */
        __atomic_store_n(&host_byte, true, __ATOMIC_SEQ_CST);
        check_ticks(0, "without a host");
        stop_guest(HV_GUEST_RESET);
        CHECK(0, "the guest goes on");
    }
    spin_in_guest(cpu);
}

static void *watch_stopped(void *arg)
{
    UNUSED(arg);
    while (__atomic_load_n(&stopped_on, __ATOMIC_SEQ_CST) < 0)
        usleep(1000);
    /* An IRQ that nobody can take any more: who has the lock keeps it */
    __atomic_store_n(&cores[BOOT].irq, 1, __ATOMIC_SEQ_CST);
    usleep(300000);
    CHECK(stopped_on == 3, "it stopped on core %d", stopped_on);
    CHECK(!host_calls, "the host was told of %d things", host_calls);
    CHECK(!hv_is_leaving(), "the guest is being left");
    CHECK(hv_get_started_cpus() == (BIT(0) | BIT(3)), "0x%lx", hv_get_started_cpus());
    /* They are all in EL2, and stay there */
    CHECK(!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_SEQ_CST), "0x%lx", hv_cpus_in_guest);
    CHECK(bhl.lock == 3, "core %ld has the lock", (long)bhl.lock);
    CHECK(cores[BOOT].irqs == 100, "%d IRQs", cores[BOOT].irqs);
    CHECK(!was_printed("HV: Exiting hypervisor"), "the hypervisor ends");
    _exit(failures ? 1 : 0);
}

static void test_no_host(void)
{
    pthread_t thread;

    no_host = true;
    pthread_create(&thread, NULL, watch_stopped, NULL);
    run_guest(guest_no_host);
    CHECK(0, "the hypervisor has ended");
}

/*
 * CPU_ON while the guest is being left: core 3 has come into EL2 for it while the boot core had
 * the lock, to tell the host something, and gets the lock once the host has left the guest. It is
 * answered, and nobody goes in.
 */
static bool leaving_go;

static void guest_leaving_cpu_on(int cpu)
{
    if (cpu == BOOT) {
        turn_on(3);
        exc_entry();
        __atomic_store_n(&leaving_go, true, __ATOMIC_SEQ_CST);
        /* It waits for the lock, out of the guest: the rendezvous does not wait for it */
        while (__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_SEQ_CST) & BIT(3))
            sched_yield();
        model_exc_proxy(START_HV, HV_USER_INTERRUPT);
        CHECK(0, "the boot core is in the guest, which the host has left");
        abort();
    }
    while (!__atomic_load_n(&leaving_go, __ATOMIC_SEQ_CST))
        sched_yield();
    exc_entry();
    CHECK(hv_is_leaving(), "core 3 has the lock before the host has left");
    CHECK(cpu_on(5, (void *)0x8000400000, 0) == 0, "CPU_ON of core 5 while leaving");
    CHECK(!hv_started_cpus[5], "core 5 is let into the guest that is being left");
    exc_exit();
    CHECK(0, "core 3 is in the guest");
    abort();
}

static void test_leaving_cpu_on(void)
{
    run_guest(guest_leaving_cpu_on);
    CHECK(!cores[5].entered, "core 5 was in the guest");
    CHECK(host_calls == 1, "the host was told of %d things", host_calls);
}

/* ---- who ticks ---- */

static bool tick_boot_off, tick_boot_on, tick_boot_done;

static void guest_tick(int cpu)
{
    switch (cpu) {
        case BOOT:
            if (__atomic_load_n(&cores[BOOT].entered, __ATOMIC_SEQ_CST) == 1) {
                turn_on(3);
                turn_on(0);
                turn_on(9);
                /* The boot core, though there are others with a lower index */
                CHECK(hv_lead_cpu() == BOOT, "%d", hv_lead_cpu());
                check_ticks(BOOT, "all on");
                cpu_off();
            }
            /* Core 9 turned it on again */
            check_ticks(BOOT, "the boot core on again");
            __atomic_store_n(&tick_boot_on, true, __ATOMIC_SEQ_CST);
            cpu_off();
            break;
        case 0:
            wait_off(BOOT);
            /* The one that is left with the lowest index */
            CHECK(hv_lead_cpu() == 0, "%d", hv_lead_cpu());
            run_for(5);
            check_ticks(0, "the boot core off");
            __atomic_store_n(&tick_boot_off, true, __ATOMIC_SEQ_CST);
            while (!__atomic_load_n(&tick_boot_on, __ATOMIC_SEQ_CST))
                run_a_bit();
            wait_off(BOOT);
            run_for(5);
            check_ticks(0, "the boot core off once more");
            /* The host calls: it is this core that hears it */
            __atomic_store_n(&tick_boot_done, true, __ATOMIC_SEQ_CST);
            cpu_off();
            break;
        case 9:
            while (!__atomic_load_n(&tick_boot_off, __ATOMIC_SEQ_CST))
                run_a_bit();
            turn_on(BOOT);
            while (!__atomic_load_n(&tick_boot_done, __ATOMIC_SEQ_CST))
                run_a_bit();
            wait_off(0);
            CHECK(hv_lead_cpu() == 3, "%d", hv_lead_cpu());
            run_for(5);
            __atomic_store_n(&host_byte, true, __ATOMIC_SEQ_CST);
            break;
    }
    spin_in_guest(cpu);
}

static int host_called_on = -1;

static int host_hears(int reason, u32 code)
{
    CHECK(reason == START_HV && code == HV_USER_INTERRUPT, "%d %d", reason, code);
    host_called_on = me;
    return EXC_EXIT_GUEST;
}

static void test_tick(void)
{
    host = host_hears;
    run_guest(guest_tick);
    CHECK(host_calls == 1 && host_called_on == 3, "%d, on core %d", host_calls, host_called_on);
}

/*
 * The boot core turns itself off while core 0, the next one to tick, is in hv_exc_fiq() already and
 * waits for the lock: the IPI that would tell it is acknowledged there
 */
static bool tick_race_wait;

static void guest_tick_race(int cpu)
{
    if (cpu == BOOT) {
        turn_on(0);
        for (int i = 0; i < 3; i++)
            run_for(1);
        exc_entry();
        smp_send_ipi(0);
        __atomic_store_n(&tick_race_wait, true, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_SEQ_CST) & BIT(0))
            sched_yield();
        usleep(2000); /* it has looked by now who ticks */
        model_cpu_off();
        CHECK(0, "the boot core is in the guest");
    }
    while (!__atomic_load_n(&tick_race_wait, __ATOMIC_SEQ_CST))
        sched_yield();
    wait_off(BOOT);
    check_ticks(0, "after the race");
    host_leaves();
}

static void test_tick_race(void)
{
    run_guest(guest_tick_race);
}

/* ---- a core that the host is waiting for turns itself off ---- */

static int switch_step;
static bool switch_by_exit;

/*
 * The host, on core BOOT: go to core 3. On core 3, in its CPU_OFF: here it is, go on. Then, on
 * core BOOT again: leave. Told to leave first, core 3 never shows up.
 */
static int host_switches(int reason, u32 code)
{
    UNUSED(reason);
    switch (switch_step++) {
        case 0:
            CHECK(me == BOOT && code == HV_USER_INTERRUPT, "core %d, %d", me, code);
            if (switch_by_exit)
                hv_exit_cpu(3);
            CHECK(hv_switch_cpu(3), "core 3 cannot be switched to");
            return EXC_RET_HANDLED;
        case 1:
            if (switch_by_exit) {
                CHECK(me == BOOT && code == HV_USER_INTERRUPT, "core %d, %d", me, code);
                return EXC_EXIT_GUEST;
            }
            CHECK(me == 3 && code == HV_CPU_SWITCH, "core %d, %d", me, code);
            CHECK(hv_started_cpus[3], "core 3 is off");
            return EXC_RET_HANDLED;
        default:
            return EXC_EXIT_GUEST;
    }
}

static void guest_switch(int cpu)
{
    if (cpu == BOOT) {
        turn_on(3);
        stop_guest(HV_USER_INTERRUPT);
        /* The host is not kept waiting, nor is anybody else */
        wait_off(3);
        CHECK(hv_want_cpu == -1, "%d", hv_want_cpu);
        host_leaves();
    }
    if (switch_by_exit) {
        /* It leaves from here, and does not look who wants what */
        wait_in_wfi();
    } else {
        /* The host has stopped the guest, and the next thing the guest does here is this */
        while (!__atomic_load_n(&cores[me].ipi, __ATOMIC_SEQ_CST))
            sched_yield();
        cpu_off();
    }
    CHECK(0, "core 3 is in the guest");
}

static void test_switch(void)
{
    host = host_switches;
    switch_step = 0;
    run_guest(guest_switch);
    CHECK(switch_step == 3, "%d", switch_step);
}

/* The same for one that the host has told to leave the guest: it does not show up any more */
static void test_switch_exit(void)
{
    host = host_switches;
    switch_by_exit = true;
    switch_step = 0;
    run_guest(guest_switch);
    CHECK(switch_step == 2, "%d", switch_step);
}

/* The host has pinned core 3, as for a single step, and it turns itself off with that step */
static int host_pins(int reason, u32 code)
{
    UNUSED(reason);
    UNUSED(code);
    if (me != 3)
        return EXC_EXIT_GUEST;
    hv_pin_cpu(3);
    return EXC_RET_HANDLED;
}

static void guest_pinned(int cpu)
{
    if (cpu == BOOT) {
        turn_on(3);
        wait_off(3);
        CHECK(hv_pinned_cpu == -1, "%d", hv_pinned_cpu);
        host_leaves();
    }
    stop_guest(HV_USER_INTERRUPT);
    cpu_off();
    CHECK(0, "core 3 is in the guest");
}

static void test_pinned(void)
{
    host = host_pins;
    run_guest(guest_pinned);
    CHECK(host_calls == 2, "%d", host_calls);
}

/* ---- */

static const struct {
    const char *name;
    void (*run)(void);
} tests[] = {
    {"all-up", test_all_up},
    {"state-hook", test_state_hook},
    {"all-up-unlocked", test_all_up_unlocked},
    {"started-before", test_started_before},
    {"locked-elsewhere", test_locked_elsewhere},
    {"dead", test_dead},
    {"two-missing", test_two_missing},
    {"small-lines", test_small_lines},
    {"small-lines-boot", test_small_lines_boot},
    {"larger-lines", test_larger_lines},
    {"no-reg", test_no_reg},
    {"beyond", test_beyond},
    {"busy", test_busy},
    {"cpu-on", test_cpu_on},
    {"refused", test_refused},
    {"on-off", test_on_off},
    {"rendezvous", test_rendezvous},
    {"leaving", test_leaving},
    {"cpu-off", test_cpu_off},
    {"exit-secondary", test_exit_secondary},
    {"exit-boot-off", test_exit_boot_off},
    {"exit-cpu-boot", test_exit_cpu_boot},
    {"exit-last", test_exit_last},
    {"exit-wfi", test_exit_wfi},
    {"two-stops", test_two_stops},
    {"leaving-cpu-on", test_leaving_cpu_on},
    {"no-host", test_no_host},
    {"tick", test_tick},
    {"tick-race", test_tick_race},
    {"switch", test_switch},
    {"switch-exit", test_switch_exit},
    {"pinned", test_pinned},
};

int main(int argc, char **argv)
{
    int bad = 0, run = 0;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int status;
        pid_t pid;

        if (argc > 1 && strcmp(argv[1], tests[i].name))
            continue;
        run++;
        fflush(NULL);
        pid = fork();
        if (!pid) {
            alarm(getenv("CPUS_TIMEOUT") ? atoi(getenv("CPUS_TIMEOUT")) : TIMEOUT_S);
            reset();
            tests[i].run();
            _exit(failures ? 1 : 0);
        }
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("FAIL %s: %s\n", tests[i].name,
                   WTERMSIG(status) == SIGALRM ? "it hangs" : strsignal(WTERMSIG(status)));
            bad++;
        } else if (WEXITSTATUS(status)) {
            printf("FAIL %s\n", tests[i].name);
            bad++;
        } else {
            printf("ok   %s\n", tests[i].name);
        }
        fflush(stdout);
        if (bad && getenv("CPUS_TIMEOUT")) /* the broken ones: the first is enough */
            break;
    }

    if (bad || !run) {
        printf("%d FAILED\n", bad);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
