/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/hv/hv_xtrack.c on several cores at once, with the real
 * src/hv/hv_pmu.c and src/hv/hv_xinsn.c linked in. Threads play the cores; what a core does in
 * EL2 runs with one lock held, as everything of hv_xtrack.c runs with the big hypervisor lock.
 * xtrack_test.c has one core and looks at what execution tracking does to the guest's memory
 * and registers; this one looks at what the cores do to each other (SiliconNT
 * docs/findings/0098, conclusion 15).
 *
 * NR_EXEC cores execute page HOT, and one more core, WRITER, makes it data again from a page of
 * its own. First in step (in_step()): every core faults on the page at once, one other than core
 * 0 scans it, the others find it done, and core 0 is never the one that scans. Then as it comes:
 * the cores execute two pages over and over, page HOT most of the time, with random pauses;
 * WRITER stores into them, reads them with ld1, which EL2 does not take apart, and with an ldr
 * that EL2 carries out. So the cores race each other to scan a page that keeps changing state.
 *
 * The entry of a page in stage 2 is one word here (entry[]), which a core loads whole when it
 * accesses the page. An access goes on with the entry it loaded; the TLBs of all cores have been
 * told of a new invalid entry once no access that loaded the old one is under way any more
 * (users[]), which is what a broadcast TLB invalidation and the DSB after it give.
 *
 * Checks:
 *   - no core is stopped, and each gets its rounds done in time: one that keeps losing the race
 *     is not a core that is stuck (docs/findings/0100, conclusion 6), and the cores do not chase
 *     each other round and round;
 *   - whenever the lock is let go of, no entry is invalid, and the execute-only pages, the
 *     records and the shadow pages in use go one to one, no two pages with the same shadow
 *     page; the other pages point to the guest's own;
 *   - every change of state goes through an invalid entry;
 *   - in step, one core scans the page each round, and core 0, which loses each time, goes on
 *     well beyond XTRACK_MAX_REPEATS: it would have been stopped if only its own scans reset
 *     the count.
 * It does not look at what is in memory, as xtrack_test.c does, nor at the times on real cores:
 * how far the race goes as it comes depends on how many CPUs the build machine gives the test.
 *
 * The Makefile drops the #include lines of hv_xtrack.c; this file stands in for them. With
 * XTRACK_INC defined the Makefile has changed hv_xtrack.c on its way here (make
 * xtrack-smp-broken): the test must fail then.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hv.h"
#include "arm_cpu_regs.h"
#include "hv_xinsn.h"
#include "hv_xpage.h" /* HV_XPAGE_* and HV_XTRACK_* of the real hv.h, see the Makefile */
#include "smp.h"

#define XT_MAX_PAGES 4

#define NR_EXEC  6       /* cores that execute */
#define WRITER   NR_EXEC /* the core that makes the pages data again */
#define NR_CORES (NR_EXEC + 1)

#define ARENA_IPA 0x10100000000UL
#define NR_PAGES  3
#define PAGE      0x1000UL
#define WORDS     1024 /* in a page */
#define HOT       0    /* the page the cores keep coming back to; it has accesses to replace */
#define PLAIN     1    /* ...and another, which has none */
#define OWN       2    /* where the instructions of WRITER are; it is not executed here */

#define MRS_PMCCNTR 0xd53b9d00 /* mrs x0, PMCCNTR_EL0 */
#define NOP         0xd503201f
#define I_STR       0xb9000020 /* str w0, [x1] */
#define I_LDR       0xb9400020 /* ldr w0, [x1] */
#define I_LD1       0x4c407020 /* ld1 {v0.16b}, [x1] */

/* How long a core may take for its rounds */
#define TIME_LIMIT_S 120

/* The registers that hv_xtrack.c reads and writes; those of a core are its thread's */
static __thread u64 test_CNTPCT_EL0, test_SP_EL0, test_SP_EL1;
static const u64 test_CNTFRQ_EL0 = 1000, test_DCZID_EL0 = 4;
static const u64 test_CTR_EL0 = 4UL << 16; /* data cache lines of 64 bytes at least */
#define mrs(reg)      (test_##reg)
#define msr(reg, val) (test_##reg = (val))
#define sysop(op)     ((void)0)
#define min(a, b)     ((a) < (b) ? (a) : (b))

static u8 arena[NR_PAGES * PAGE] __attribute__((aligned(0x1000)));
static u8 pool[XT_MAX_PAGES * PAGE] __attribute__((aligned(0x1000)));

/* Stage 2 of a page: its state in the low bits, where it points in the others. Atomic. */
static u64 entry[NR_PAGES];
#define ENTRY(st, pa)  ((u64)(pa) | (u64)(st))
#define ENTRY_STATE(e) ((int)((e)&3))
#define ENTRY_PA(e)    ((e) & ~3UL)
/* Accesses under way through an entry that was loaded before. Atomic. */
static int users[NR_PAGES];
/* With the lock held: the page with an invalid entry, -1 if none; how many there have been */
static int switching = -1;
static u64 switches;

/* What a core has when it traps, and its SIMD registers */
static __thread u64 test_esr, test_far, test_spsr = 5;
static __thread u32 test_insn;
static __thread u8 test_simd[32][16];

static __thread int me = -1; /* the core this thread is */
static unsigned long checks, failures;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED);
    if (ok)
        return;
    if (__atomic_add_fetch(&failures, 1, __ATOMIC_SEQ_CST) > 20)
        return;
    pthread_mutex_lock(&print_lock);
    printf("FAIL core %d: ", me);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    pthread_mutex_unlock(&print_lock);
}

static bool failed(void)
{
    return __atomic_load_n(&failures, __ATOMIC_SEQ_CST);
}

/* ---- what hv_xtrack.c uses of the hypervisor ---- */

static bool in_pool(u64 a)
{
    return a >= (u64)pool && a < (u64)pool + sizeof(pool);
}

static void *test_memalign(size_t align, size_t size)
{
    check(align == PAGE && size == sizeof(pool), "memalign(0x%zx, 0x%zx): not the pool", align,
          size);
    return pool;
}
#define memalign(a, s) test_memalign(a, s)

/* The caches are not played here: xtrack_test.c does */
static void dc_cvac_range(void *addr, size_t size)
{
    UNUSED(addr);
    UNUSED(size);
}

static void dc_civac_range(void *addr, size_t size)
{
    UNUSED(addr);
    UNUSED(size);
}

static void ic_ialluis(void)
{
}

static u64 hv_get_esr(void)
{
    return test_esr;
}

static u64 hv_get_far(void)
{
    return test_far;
}

static u64 hv_get_spsr(void)
{
    return test_spsr;
}

static int page_of(u64 ipa)
{
    if (ipa < ARENA_IPA || ipa >= ARENA_IPA + NR_PAGES * PAGE)
        return -1;
    return (ipa - ARENA_IPA) >> 12;
}

static u64 ipa_of(int page, u32 off)
{
    return ARENA_IPA + page * PAGE + off;
}

static u64 guest_page(int n)
{
    return (u64)&arena[n * PAGE];
}

/* Stage 1 of the guest maps every address to itself */
static u64 hv_translate(u64 addr, bool s1, bool w, u64 *par_out)
{
    UNUSED(s1);
    UNUSED(w);
    UNUSED(par_out);
    return addr & MASK(56);
}

/* The instruction a core is executing is whatever it says, not in the arena */
static u64 hv_translate_insn(u64 addr)
{
    UNUSED(addr);
    return (u64)&test_insn;
}

static bool hv_pt_xtrack_on(void)
{
    return true;
}

static int hv_pt_scanned(u64 ipa, u64 *pa)
{
    int n = page_of(ipa);
    u64 e;

    if (n < 0)
        return -1;
    e = __atomic_load_n(&entry[n], __ATOMIC_SEQ_CST);
    if (ENTRY_STATE(e) == HV_XPAGE_INVALID)
        return -1;
    *pa = ENTRY_PA(e);
    return ENTRY_STATE(e);
}

static u64 hv_pt_switch_count(void)
{
    return switches;
}

static int hv_pt_set_scanned(u64 ipa, int st, u64 pa)
{
    int n = page_of(ipa);
    u64 e;

    if (n < 0)
        return -1;
    e = __atomic_load_n(&entry[n], __ATOMIC_SEQ_CST);

    if (st == HV_XPAGE_INVALID) {
        check(switching < 0 && ENTRY_STATE(e) != HV_XPAGE_INVALID,
              "page %d is made invalid while page %d is", n, switching);
        __atomic_store_n(&entry[n], ENTRY(HV_XPAGE_INVALID, 0), __ATOMIC_SEQ_CST);
        /* Every core has been told: no access through the entry from before is under way */
        while (__atomic_load_n(&users[n], __ATOMIC_SEQ_CST))
            sched_yield();
        switching = n;
        switches++;
        return 0;
    }

    /* Break-before-make: from a valid entry to another only through an invalid one */
    check(switching == n && ENTRY_STATE(e) == HV_XPAGE_INVALID,
          "page %d goes from state %d to %d without an invalid entry in between", n,
          ENTRY_STATE(e), st);
    if (st == HV_XPAGE_XONLY)
        check(in_pool(pa) && !(pa & (PAGE - 1)), "page %d executes 0x%lx, not a shadow page", n,
              pa);
    else
        check(pa == guest_page(n), "page %d in state %d points to 0x%lx", n, st, pa);
    __atomic_store_n(&entry[n], ENTRY(st, pa), __ATOMIC_SEQ_CST);
    switching = -1;
    return 0;
}

static void get_simd_state(void *st)
{
    memcpy(st, test_simd, sizeof(test_simd));
}

static void put_simd_state_masked(const void *st, u32 mask)
{
    for (int i = 0; i < 32; i++)
        if (mask & BIT(i))
            memcpy(test_simd[i], (const u8 *)st + 16 * i, 16);
}

#ifndef XTRACK_INC
#define XTRACK_INC "hv_xtrack.inc"
#endif
#include XTRACK_INC

/* ---- the cores ---- */

/* The big hypervisor lock */
static pthread_mutex_t el2 = PTHREAD_MUTEX_INITIALIZER;

static unsigned long rounds;
static int executing; /* cores that have rounds left. Atomic. */

/* What each core did; only its own thread writes it */
static struct core_stats {
    u64 rounds;  // executions, for WRITER stores and reads, done
    u64 faults;  // instruction aborts
    u64 scans;   // ...that scanned the page
    u64 lost;    // ...that found another core had been quicker
    u64 streak;  // ...of those, in a row on the same page with no scan of its own in between
    u64 longest; // the longest streak
    u64 retries; // aborts after which the access was tried again
    int last;    // the page of the last instruction abort
} stats[NR_CORES];

static double seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A while, which the others may get in between */
static void pause_a_bit(unsigned *seed)
{
    unsigned n = rand_r(seed) % 64;

    if (!n)
        sched_yield();
    for (volatile unsigned i = 0; i < 16 * n; i++)
        ;
}

enum access { EXEC, READ, WRITE };

static bool allowed(int st, enum access a)
{
    switch (a) {
        case EXEC:
            return st == HV_XPAGE_SCANNED || st == HV_XPAGE_XONLY;
        case READ:
            return st == HV_XPAGE_UNSCANNED || st == HV_XPAGE_SCANNED;
        default:
            return st == HV_XPAGE_UNSCANNED;
    }
}

/*
 * The core loads the entry and, if it allows the access, goes on with it until access_end().
 * Otherwise false, with the state it found: an abort.
 */
static bool access_begin(int n, enum access a, int *st)
{
    __atomic_add_fetch(&users[n], 1, __ATOMIC_SEQ_CST);
    *st = ENTRY_STATE(__atomic_load_n(&entry[n], __ATOMIC_SEQ_CST));
    if (allowed(*st, a))
        return true;
    __atomic_sub_fetch(&users[n], 1, __ATOMIC_SEQ_CST);
    return false;
}

static void access_end(int n)
{
    __atomic_sub_fetch(&users[n], 1, __ATOMIC_SEQ_CST);
}

/* With the lock held, on the way out of EL2 */
static void check_tables(const char *after)
{
    int xonly = 0;

    check(switching < 0, "after %s, page %d is left with an invalid entry", after, switching);
    for (int n = 0; n < NR_PAGES; n++) {
        u64 e = __atomic_load_n(&entry[n], __ATOMIC_SEQ_CST);

        check(ENTRY_STATE(e) != HV_XPAGE_INVALID, "after %s, page %d is invalid", after, n);
        if (ENTRY_STATE(e) != HV_XPAGE_XONLY) {
            check(ENTRY_PA(e) == guest_page(n), "after %s, page %d points to 0x%lx", after, n,
                  ENTRY_PA(e));
            continue;
        }
        xonly++;
        check(in_pool(ENTRY_PA(e)), "after %s, page %d executes 0x%lx", after, n, ENTRY_PA(e));
        for (int m = 0; m < n; m++)
            check(__atomic_load_n(&entry[m], __ATOMIC_SEQ_CST) != e,
                  "after %s, pages %d and %d execute the same shadow page", after, m, n);
    }
    check(xonly == (int)xt_nr_recs && xt_nr_recs + xt_nr_free == XT_MAX_PAGES,
          "after %s: %d execute-only pages, %u records, %u shadow pages free of %d", after, xonly,
          xt_nr_recs, xt_nr_free, XT_MAX_PAGES);
}

static void el2_enter(void)
{
    pthread_mutex_lock(&el2);
}

static void el2_leave(const char *what)
{
    check_tables(what);
    pthread_mutex_unlock(&el2);
}

/*
 * The core could not execute page n, whose entry it found in state st: an instruction abort.
 * Returns false if the guest is stopped.
 */
static bool instruction_abort(int n, int st, unsigned *seed)
{
    struct core_stats *s = &stats[me];
    struct exc_info ctx = {.cpu_id = me, .elr = ipa_of(n, 4 * (rand_r(seed) % WORDS))};
    u64 scans;
    bool ok;

    el2_enter();
    /* A permission fault, or a translation fault on the entry that was invalid */
    test_esr = st == HV_XPAGE_INVALID ? 0x82000007 : 0x8200000f;
    test_far = ctx.elr;
    test_spsr = 5;
    scans = xt_stats.scans;
    ok = hv_xtrack_iabort(&ctx);
    if (xt_stats.scans != scans) {
        s->scans++;
        s->streak = 0;
    } else {
        s->lost++;
        s->streak = s->last == n ? s->streak + 1 : 1;
        if (s->streak > s->longest)
            s->longest = s->streak;
    }
    s->faults++;
    s->last = n;
    el2_leave("an instruction abort");

    check(ok, "stopped on page %d, after %lu instruction aborts in a row that another core was "
              "quicker for",
          n, s->streak);
    return ok;
}

/* The core executes something in page n: once it may, it has. Returns false if stopped. */
static bool execute(int n, unsigned *seed)
{
    int st;

    while (!access_begin(n, EXEC, &st)) {
        if (!instruction_abort(n, st, seed))
            return false;
        pause_a_bit(seed);
    }
    access_end(n);
    return true;
}

static void *executor(void *arg)
{
    unsigned seed = 1234 + (long)arg;
    double start = seconds();
    struct core_stats *s;

    me = (long)arg;
    s = &stats[me];
    s->last = -1;
    while (s->rounds < rounds && !failed()) {
        if (!execute(rand_r(&seed) % 16 ? HOT : PLAIN, &seed))
            break;
        s->rounds++;
        if (!(s->rounds & 0xff) && seconds() - start > TIME_LIMIT_S) {
            check(false, "%lu of its %lu rounds in %d s", s->rounds, rounds, TIME_LIMIT_S);
            break;
        }
        pause_a_bit(&seed);
    }
    __atomic_sub_fetch(&executing, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/*
 * WRITER accesses page n from its own page with insn, at off. A data abort goes to EL2 with esr,
 * and the access is tried again unless EL2 carried it out. Returns false if it does not get done.
 */
static bool data_access(int n, u32 off, enum access a, u32 insn, u64 esr)
{
    struct core_stats *s = &stats[me];
    u64 ipa = ipa_of(n, off);
    int st;

    for (int tries = 0;; tries++) {
        struct exc_info ctx = {.cpu_id = me, .elr = ipa_of(OWN, 4 * (tries % WORDS))};
        int ret = HV_XTRACK_NONE;

        if (access_begin(n, a, &st)) {
            if (a == WRITE)
                *(volatile u32 *)&arena[n * PAGE + off] = NOP;
            else
                check(*(volatile u32 *)&arena[n * PAGE + off] == NOP, "page %d reads 0x%x at 0x%x",
                      n, *(volatile u32 *)&arena[n * PAGE + off], off);
            access_end(n);
            return true;
        }
        if (failed() || !__atomic_load_n(&executing, __ATOMIC_SEQ_CST))
            return false;

        ctx.regs[0] = NOP;
        ctx.regs[1] = ipa;
        el2_enter();
        /* A translation fault on the invalid entry: hv_vm.c has the guest try again */
        if (st != HV_XPAGE_INVALID) {
            test_esr = esr;
            test_far = ipa;
            test_insn = insn;
            test_spsr = 5;
            ctx.sp[0] = test_SP_EL0;
            ctx.sp[1] = test_SP_EL1;
            ret = hv_xtrack_dabort(&ctx, ipa, a == WRITE);
        }
        el2_leave("a data abort");

        if (ret == HV_XTRACK_SKIP) {
            check(a == READ && (u32)ctx.regs[0] == NOP, "a read of page %d at 0x%x: x0 0x%lx", n,
                  off, ctx.regs[0]);
            return true;
        }
        check(st == HV_XPAGE_INVALID || ret == HV_XTRACK_RETRY,
              "insn 0x%08x on page %d in state %d: answer %d", insn, n, st, ret);
        if (st != HV_XPAGE_INVALID && ret != HV_XTRACK_RETRY)
            return false;
        s->retries++;
    }
}

/* ---- in step ---- */

/*
 * Every core faults on page HOT at the same time. Another than core 0 gets to EL2 first and scans
 * the page, then the others come in and find it done; then a store from elsewhere makes the page
 * data again. Core 0 is never first: it goes on only because the page changing state resets its
 * count (docs/findings/0100, conclusion 6). That is the same on any number of CPUs, which the
 * race further down is not.
 */
#define STEP_ROUNDS (4 * XTRACK_MAX_REPEATS)
static pthread_barrier_t step;
static int step_first; /* the core that gets to EL2 first in this round */

static void *step_executor(void *arg)
{
    unsigned seed = 99 + (long)arg;
    bool refused;
    int st;

    me = (long)arg;
    stats[me].last = -1;
    for (int r = 0; r < STEP_ROUNDS; r++) {
        pthread_barrier_wait(&step); /* page HOT is data */
        refused = !access_begin(HOT, EXEC, &st);
        check(refused && st == HV_XPAGE_UNSCANNED, "round %d: page HOT runs, or is in state %d", r,
              st);
        if (!refused)
            access_end(HOT);
        pthread_barrier_wait(&step); /* every core has faulted on it */
        if (me == step_first)
            instruction_abort(HOT, st, &seed);
        pthread_barrier_wait(&step); /* one has scanned it */
        if (me != step_first)
            instruction_abort(HOT, st, &seed);
        pthread_barrier_wait(&step);
    }
    return NULL;
}

static void in_step(void)
{
    pthread_t threads[NR_EXEC];
    unsigned seed = 555;
    u64 scans = 0;

    pthread_barrier_init(&step, NULL, NR_EXEC + 1);
    executing = NR_EXEC;
    for (long i = 0; i < NR_EXEC; i++)
        pthread_create(&threads[i], NULL, step_executor, (void *)i);
    me = WRITER;
    for (int r = 0; r < STEP_ROUNDS; r++) {
        check(data_access(HOT, 4 * (WORDS - 1), WRITE, I_STR, 0x9200004f | BIT(24) | (2UL << 22)),
              "round %d: page HOT does not become data", r);
        step_first = 1 + rand_r(&seed) % (NR_EXEC - 1);
        for (int b = 0; b < 4; b++)
            pthread_barrier_wait(&step);
    }
    for (int i = 0; i < NR_EXEC; i++)
        pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&step);

    for (int i = 0; i < NR_EXEC; i++)
        scans += stats[i].scans;
    check(scans == STEP_ROUNDS && !stats[0].scans && stats[0].longest == STEP_ROUNDS,
          "in step: %lu scans in %d rounds; core 0 scanned %lu times, and found another core "
          "quicker %lu times in a row",
          scans, STEP_ROUNDS, stats[0].scans, stats[0].longest);
    printf("xtrack-smp-model: in step, %d rounds: core 0 found another core quicker each time, "
           "and goes on\n",
           STEP_ROUNDS);
    memset(stats, 0, sizeof(stats));
}

/* ---- as it comes ---- */

static void *writer(void *arg)
{
    unsigned seed = 4321;
    struct core_stats *s;

    UNUSED(arg);
    me = WRITER;
    s = &stats[me];
    while (__atomic_load_n(&executing, __ATOMIC_SEQ_CST) && !failed()) {
        int n = rand_r(&seed) % 8 ? HOT : PLAIN;
        /* In the upper half of the page: where no access to replace is */
        u32 off = 4 * (WORDS / 2 + rand_r(&seed) % (WORDS / 2));
        /* A data abort on a word, from a lower EL, permission fault, level 3 */
        u64 esr = 0x9200000f;
        u64 syndrome = BIT(24) | (2UL << 22); /* ISV, a word, x0 */
        bool done;

        switch (rand_r(&seed) % 4) {
            case 0:
            case 1:
                done = data_access(n, off, WRITE, I_STR, esr | BIT(6) | syndrome);
                break;
            case 2:
                done = data_access(n, off, READ, I_LDR, esr | syndrome);
                break;
            default:
                done = data_access(n, off, READ, I_LD1, esr);
                break;
        }
        if (done)
            s->rounds++;
        pause_a_bit(&seed);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[NR_CORES];
    u64 longest = 0;

    rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 50000;
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Page HOT has accesses to replace, in its lower half; the rest is NOPs */
    for (int n = 0; n < NR_PAGES; n++) {
        u32 *words = (u32 *)&arena[n * PAGE];

        for (int i = 0; i < (int)WORDS; i++)
            words[i] = NOP;
        if (n == HOT)
            words[0] = words[100] = words[WORDS / 2 - 1] = MRS_PMCCNTR;
        entry[n] = ENTRY(HV_XPAGE_UNSCANNED, guest_page(n));
    }
    hv_pmu_init();
    hv_xtrack_init();
    check(hv_xtrack_start() == 0, "hv_xtrack_start() failed");

    in_step();

    executing = NR_EXEC;
    for (long i = 0; i < NR_EXEC; i++)
        pthread_create(&threads[i], NULL, executor, (void *)i);
    pthread_create(&threads[WRITER], NULL, writer, NULL);
    for (int i = 0; i < NR_CORES; i++)
        pthread_join(threads[i], NULL);

    me = -1;
    el2_enter();
    el2_leave("the end");
    for (int i = 0; i < NR_EXEC; i++) {
        check(stats[i].rounds == rounds, "core %d did %lu of its %lu rounds", i, stats[i].rounds,
              rounds);
        if (stats[i].longest > longest)
            longest = stats[i].longest;
        printf("xtrack-smp-model: core %d: %lu rounds, %lu instruction aborts: %lu scanned, %lu "
               "found another core quicker, at most %lu times in a row on a page\n",
               i, stats[i].rounds, stats[i].faults, stats[i].scans, stats[i].lost,
               stats[i].longest);
    }
    printf("xtrack-smp-model: core %d: %lu stores and reads, %lu data aborts tried again; %lu "
           "changes of state in all\n",
           WRITER, stats[WRITER].rounds, stats[WRITER].retries, switches);
    check(xt_stats.stops == 0, "execution tracking stopped the guest %lu times", xt_stats.stops);
    check(stats[WRITER].rounds && xt_stats.to_data_read && xt_stats.reads_syn,
          "the writing core left something out: %lu stores and reads, %lu by ld1, %lu by ldr",
          stats[WRITER].rounds, xt_stats.to_data_read, xt_stats.reads_syn);
    /* How far the race went by itself; with one CPU it may not happen at all */
    printf("xtrack-smp-model: as it comes, a core found another quicker %lu times in a row at "
           "most (%d stopped it before)\n",
           longest, XTRACK_MAX_REPEATS + 1);
    hv_xtrack_report();

    printf("xtrack-smp-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
