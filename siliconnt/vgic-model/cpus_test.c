// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for how the cores get into EL2 and into the guest: src/hv/hv.c with
 * src/smp.c under it, the real files, with the real headers of m1n1 and the accesses to the
 * hardware taken out (cpus/hw.h). Every core is a thread. This file plays the rest:
 *
 *  - the machine: the device tree as far as smp_init() reads it, the registers of the power
 *    manager that start a core, RVBAR, the system registers of each core, the IPIs;
 *  - the guest: hv_enter_guest() runs a function on that thread until hv_exit_guest(). Its
 *    way into EL2 and out of it is hv_exc_entry() and hv_exc_exit() of src/hv/hv_exc.c, the real
 *    ones;
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

/* src/hv/hv_exc.c, by cpus/exc_head.c and cpus/exc_tail.c */
void model_exc_entry(void);
void model_exc_exit(void);
void model_steal_time(u64 ticks);

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
    /* The guest on it */
    int ipi;
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
    /* ...and ever: its MMU is turned on, its stack thrown out of the caches */
    int mmu_inits, ivacs;
    /* What the one that let it in gave it */
    u64 want_x0;
    void *want_entry;
} cores[EXTRA];

static void (*guest)(int cpu); /* what the guest does on each core */
static u64 stolen;             /* what the host took of the time of the guest */
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

u64 mock_mrs(const char *name)
{
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
    *reg_slot(&cores[me], name) = val;
}

/* Every loop that waits for another core has one of these in it: let that core run */
void mock_sysop(const char *op)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!strcmp(op, "wfe") || !strcmp(op, "dmb sy"))
        sched_yield();
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
u8 _stack_bot[16];
char _hv_vectors_start[16];
bool display_is_external;
int uartproxy_iodev;

void spin_lock(spinlock_t *lock)
{
    s64 free;

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

/* On the boot core from hv_init(): the Redistributors are laid out */
void hv_vgic_init(void)
{
    cores[me].vgic_inits++;
    if (me != BOOT)
        return;
    alive_at_vgic_init = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (smp_is_alive(i))
            alive_at_vgic_init |= BIT(i);
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
int hv_map_hook(u64 from, void *hook, u64 size) { UNUSED(from); UNUSED(hook); UNUSED(size); return 0; }
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
void hv_wdt_pet(void) {}
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
bool hv_has_host(void) { return false; }
void hv_exc_proxy(void *ctx, int reason, u32 type, void *extra) { UNUSED(ctx); UNUSED(reason); UNUSED(type); UNUSED(extra); abort(); }
void iodev_console_flush(void) {}
void iodev_handle_events(int id) { UNUSED(id); }
s64 iodev_can_read(int id) { UNUSED(id); return 0; }
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
static void exc_entry(void)
{
    __atomic_store_n(&cores[me].in_el2, true, __ATOMIC_SEQ_CST);
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

/* hv_exc_fiq(), for an IPI */
static void take_ipi(void)
{
    hv_maybe_exit();
    exc_entry();
    __atomic_store_n(&cores[me].ipi, 0, __ATOMIC_SEQ_CST);
    exc_exit();
}

/* The guest goes on for a bit; on some core it would do something else in the meantime */
static void run_a_bit(void)
{
    if (__atomic_load_n(&cores[me].ipi, __ATOMIC_SEQ_CST))
        take_ipi();
    sched_yield();
}

static void spin_in_guest(int cpu)
{
    UNUSED(cpu);
    for (;;) {
        run_a_bit();
        /* The tick of hv_exc_fiq(): once a second on the machine */
        hv_maybe_exit();
    }
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
    /* What exception_initialize() leaves on the boot core: nothing masked */
    *reg_slot(&cores[BOOT], "DAIF") = 0;
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
    hv_exit_guest();
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
    /* ...and these once for all times it was in the guest, since it came up */
    CHECK(c->mmu_inits == 1 && c->ivacs == 1, "core %d: its MMU %d times, its stack %d times", cpu,
          c->mmu_inits, c->ivacs);
    CHECK(*reg_slot(c, "SCTLR_EL1") & SCTLR_M, "core %d: the MMU of EL2 is off", cpu);
    CHECK(__atomic_load_n(&hv_started_cpus[cpu], __ATOMIC_SEQ_CST), "core %d is not started", cpu);
    CHECK(*reg_slot(c, "CNTVOFF_EL2") == stolen, "core %d: CNTVOFF_EL2 0x%lx, not 0x%lx", cpu,
          *reg_slot(c, "CNTVOFF_EL2"), stolen);
    /* The guest turns things on, as Windows would have left them when the core stops */
    *reg_slot(c, R_SCTLR_EL12) |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_EE;
    *reg_slot(c, "VBAR_EL12") = 0xfffff80000000000;
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
        stolen += 1000;
        exc_exit();
        CHECK(*reg_slot(&cores[BOOT], "CNTVOFF_EL2") == stolen, "CNTVOFF_EL2 is 0x%lx",
              *reg_slot(&cores[BOOT], "CNTVOFF_EL2"));

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
    hv_exit_guest();
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
    /* The boot core, if it were not in the guest; a core that is not there */
    hv_started_cpus[BOOT] = false;
    CHECK(cpu_on(BOOT, (void *)0x8000400000, 1) == -1, "CPU_ON of the boot core");
    hv_started_cpus[BOOT] = true;
    CHECK(cpu_on(20, (void *)0x8000400000, 1) == -1, "CPU_ON of a core that is not there");
    exc_exit();
    hv_exit_guest();
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

    hv_exit_guest();
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
        hv_exit_guest();
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
        hv_maybe_exit();
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

/* ---- */

static const struct {
    const char *name;
    void (*run)(void);
} tests[] = {
    {"all-up", test_all_up},
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
