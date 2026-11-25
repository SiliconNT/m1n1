// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for execution tracking on several cores at once (src/hv/hv_xtrack.c,
 * SiliconNT R51, docs/findings/0100 conclusions 6 and 18): several cores execute the same page
 * of code over and over while another core writes into it and reads it, with a plain load and
 * with LD1, for minutes. Execution tracking has to keep up without stopping the guest, every
 * core that executes has to get on, and every read has to give what is in memory, the word that
 * was replaced in the shadow page as it is in the guest's memory.
 *
 * The page is .hot of xtstress-hot.S. Execution tracking has to be on (run_sgitest.py
 * --xtrack): then the page is execute-only while it is code, because it has an access to a PMUv3
 * register. A store into it from the writing core makes it data again, and so does an LD1;
 * the next fetch of an executing core makes it code again, and all the others that fetch from
 * it meanwhile wait for that and find it done. The hypervisor stops a guest whose core comes
 * back for a page that is code already too often without anything changing in between: that is
 * the count this tests on several cores (0100, conclusion 6).
 *
 * Slot 0 (the boot CPU) keeps time and prints; slot 1 writes and reads; every other slot
 * executes the page. Needs 3 CPUs or more. hdr_arg: bits 15:0 how many seconds it runs (0:
 * 600), bits 23:16 how often it prints and checks that every executing core got on (0: every
 * 30 seconds). Interrupts stay masked. The MMU is off. Entry points, vectors and the header are
 * those of sgitest (sgitest-head.S).
 */

#include "gic.h"
#include "cpuon.h"
#include "sgitest.h"

#define HOT_WORDS 32
/* mrs x9, pmccntr_el0, as xtstress-hot.S has it */
#define PMU_WORD 0xd53b9d09U

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;
extern u64 hdr_arg;

/* xtstress-hot.S */
void hot_loop(volatile u64 *count, volatile u64 *stop);
extern volatile u32 hot_pmu[4];
extern volatile u64 hot_data[HOT_WORDS];
u64 ld1_pair(const volatile void *p, u64 *hi);

void uart_putc(int c);

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })
#define write_sysreg(r, v) asm volatile("msr " #r ", %0" : : "r"((u64)(v)))
#define isb()              asm volatile("isb")

#define WRITER      1
#define MAX_DELAY_US 256

struct cpu {
    volatile u64 online;
    volatile u64 count;    // turns of hot_loop
    volatile u64 returned; // back from hot_loop, or done writing
};

static struct cpu cpus[MAX_CPUS];
static volatile u64 go, stop;

/* Written by the writing core only */
static volatile u64 w_str, w_ldr, w_ld1, w_bad, w_bad_pmu, w_got, w_want;
static u64 expected[HOT_WORDS];

static u64 nr_cpus;
static u64 ticks_per_ms;
static u32 nr_pass, nr_fail;

/* ---- output ---- */

static void put(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_hex(u64 v, int digits)
{
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        uart_putc("0123456789abcdef"[(v >> shift) & 0xf]);
}

static void put_dec(u64 v)
{
    char buf[20];
    int n = 0;

    do {
        buf[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}

static void line(const char *s)
{
    put("HVLOG: xtstress: ");
    put(s);
}

static void result(const char *name, int ok)
{
    line(ok ? "PASS " : "FAIL ");
    put(name);
    put("\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;
}

/* ---- time ---- */

static u64 now(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

static void delay_us(u64 us)
{
    u64 end = now() + us * ticks_per_ms / 1000;

    while (now() < end)
        ;
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

void bad_exception(u64 vector)
{
    line("EXCEPTION cpu vector ESR ELR FAR SPSR = ");
    put_hex(read_sysreg(tpidr_el1), 2);
    put(" ");
    put_hex(vector, 2);
    put(" ");
    put_hex(read_sysreg(esr_el1), 8);
    put(" ");
    put_hex(read_sysreg(elr_el1), 16);
    put(" ");
    put_hex(read_sysreg(far_el1), 16);
    put(" ");
    put_hex(read_sysreg(spsr_el1), 8);
    put("\n");
    for (;;)
        ;
}

/* ---- the writing core ---- */

static u64 rng = 0x2545f4914f6cdd1dUL;

static u64 next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* Neither half can be an instruction that execution tracking replaces (those start with 0xd5) */
static u64 value(u64 r)
{
    return 0x1000000010000000UL | (r & 0x0fffffff0fffffffUL);
}

static void check(u64 got, u64 want)
{
    if (got != want) {
        w_bad++;
        w_got = got;
        w_want = want;
    }
}

static void writer(void)
{
    /* FP/SIMD on, for LD1 */
    write_sysreg(cpacr_el1, read_sysreg(cpacr_el1) | (3UL << 20));
    isb();

    for (u32 i = 0; i < HOT_WORDS; i++)
        expected[i] = hot_data[i];

    while (!stop) {
        u64 r = next(), hi;
        u32 i = ((r >> 8) % (HOT_WORDS / 2)) * 2; /* LD1 reads two words, 16-byte aligned */

        switch (r & 3) {
            case 0:
            case 1:
                /* From this page into that one: it becomes data, and the store is retried */
                i += (r >> 16) & 1;
                expected[i] = value(next());
                hot_data[i] = expected[i];
                w_str++;
                break;
            case 2:
                /* While it is code, EL2 carries this out from the guest's page */
                check(hot_data[i], expected[i]);
                if (hot_pmu[0] != PMU_WORD) {
                    w_bad_pmu++;
                    w_got = hot_pmu[0];
                }
                w_ldr++;
                break;
            case 3:
                /* EL2 does not take this apart: the page becomes data, and the load is retried */
                check(ld1_pair(&hot_data[i], &hi), expected[i]);
                check(hi, expected[i + 1]);
                if ((u32)ld1_pair(hot_pmu, &hi) != PMU_WORD) {
                    w_bad_pmu++;
                    w_got = hot_pmu[0];
                }
                w_ld1++;
                break;
        }
        delay_us((r >> 32) % MAX_DELAY_US);
    }
}

/* ---- CPUs ---- */

void secondary_main(int slot)
{
    struct cpu *c = &cpus[slot];

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    c->online = 1;
    while (!go)
        ;
    if (slot == WRITER)
        writer();
    else
        hot_loop(&c->count, &stop);
    c->returned = 1;
    for (;;)
        ;
}

static void report(u64 seconds, const u64 *last, u64 *stuck)
{
    line("at ");
    put_dec(seconds);
    put(" s: turns since the last line:");
    for (u64 cpu = WRITER + 1; cpu < nr_cpus; cpu++) {
        u64 n = cpus[cpu].count;

        put(" ");
        put_dec(n - last[cpu]);
        if (n == last[cpu])
            stuck[cpu]++;
    }
    put("; writer str ldr ld1 = ");
    put_dec(w_str);
    put(" ");
    put_dec(w_ldr);
    put(" ");
    put_dec(w_ld1);
    put(", wrong ");
    put_dec(w_bad);
    put(" ");
    put_dec(w_bad_pmu);
    put("\n");
}

void boot_main(void)
{
    u64 seconds = hdr_arg & 0xffff, period = (hdr_arg >> 16) & 0xff;
    u64 start, last[MAX_CPUS], stuck[MAX_CPUS], ran, returned = 0, end;
    int moved = 1;

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    nr_cpus = hdr_nr_cpus;
    if (nr_cpus < 1 || nr_cpus > MAX_CPUS)
        nr_cpus = 1;
    if (!seconds)
        seconds = 600;
    if (!period)
        period = 30;

    line("start, cpus=");
    put_dec(nr_cpus);
    put(" seconds=");
    put_dec(seconds);
    put(" every ");
    put_dec(period);
    put(" s; the page of code at ");
    put_hex((u64)hot_data & ~0xfffUL, 12);
    put("\n");
    if (nr_cpus < 3) {
        line("FAIL it takes 3 CPUs or more: one keeps time, one writes, the others execute\n");
        nr_fail++;
        goto done;
    }

    for (u64 slot = 1; slot < nr_cpus; slot++) {
        /* PSCI CPU_ON; the CPU then enters secondary_start */
        int ret = cpuon_start(slot, &cpus[slot].online);

        if (ret) {
            line("FAIL cpu ");
            put_dec(slot);
            put(ret == 1 ? " did not come online\n" : " was not turned on: CPU_ON answered ");
            if (ret != 1) {
                put_hex((u32)ret, 8);
                put("\n");
            }
            nr_fail++;
            goto done;
        }
    }
    result("every CPU_ON took at most 10 ms, and CPU_ON of a CPU that is on says ALREADY_ON",
           cpuon_report(nr_cpus, read_sysreg(mpidr_el1) & 0xff00ffffffUL));

    for (u64 cpu = 0; cpu < MAX_CPUS; cpu++)
        last[cpu] = stuck[cpu] = 0;
    asm volatile("dsb sy");
    go = 1;
    asm volatile("dsb sy");
    start = now();

    for (ran = period; ran <= seconds; ran += period) {
        while (now() < start + ran * 1000 * ticks_per_ms)
            ;
        report(ran, last, stuck);
        for (u64 cpu = 0; cpu < nr_cpus; cpu++)
            last[cpu] = cpus[cpu].count;
    }

    stop = 1;
    asm volatile("dsb sy");
    end = now() + 2000 * ticks_per_ms;
    do {
        returned = 0;
        for (u64 cpu = 1; cpu < nr_cpus; cpu++)
            returned += cpus[cpu].returned;
    } while (returned != nr_cpus - 1 && now() < end);

    for (u64 cpu = WRITER + 1; cpu < nr_cpus; cpu++) {
        line("cpu ");
        put_dec(cpu);
        put(": ");
        put_dec(cpus[cpu].count);
        put(" turns, ");
        put_dec(stuck[cpu]);
        put(" periods without one\n");
        if (stuck[cpu])
            moved = 0;
    }
    if (w_bad || w_bad_pmu) {
        line("the last wrong read: got ");
        put_hex(w_got, 16);
        put(", wanted ");
        put_hex(w_bad ? w_want : PMU_WORD, 16);
        put("\n");
    }
    result("every core that executes the page got on in every period", moved);
    result("every read of the page gave what is in memory, the replaced word too",
           !w_bad && !w_bad_pmu);
    result("the writer stored into the page, and read it with LDR and with LD1",
           w_str && w_ldr && w_ld1);
    result("every core came back when it was told to stop", returned == nr_cpus - 1);

done:
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
