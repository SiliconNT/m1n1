// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest: which core does AIC report a line to, and does every event of a line that
 * software raises arrive as its SPI (src/hv/hv_vgic.c), when some cores run the guest, all of
 * them do, or the others only run m1n1 (SiliconNT R50, docs/findings/0098, conclusion 19); when
 * two or three lines are raised at once; and when the boot core has turned itself off (R51).
 *
 * One line of the table of AIC lines and SPIs is raised with the software trigger of AIC over
 * and over, by one core after the other, with its SPI routed to one core after the other; the
 * handler lowers it. A round in which the SPI has not been taken after ROUND_MS is a miss: the
 * line is lowered then, and the next round begins. Which core the hypervisor took the events
 * on is in its report when the guest is left ("AIC events: N forwarded").
 *
 * hdr_arg: bits 7:0 choose the phases. In the first two the handler lowers the line; in the
 * next two only when it comes the second time, so that AIC gets to choose a core again while
 * the hypervisor, which has IRQs masked, unmasks the line at the end of the first.
 *   bit 0  the cores spin while they wait
 *   bit 1  the cores are in WFI, which the hypervisor turns into a wait in EL2
 *   bit 2  as bit 0, the line held over the first end
 *   bit 3  as bit 1, the line held over the first end
 *   bit 4  every line the loader gave (two or three) raised at once by one core, each SPI
 *          routed to another core; the cores are in WFI
 *   bit 5  last: the boot core turns itself off (PSCI CPU_OFF) and stays off. The next core
 *          raises the first line from every core but the boot core, its SPI routed to each of
 *          them, the cores in WFI; then that core ends the test. Needs 3 cores or more
 * Bits 31:8 are the rounds for each pair of a raising and a target core in a phase (0: 20).
 *
 * The loader (run_sgitest.py --aic-line) says which lines, as for aictest. The guest never reads
 * AIC: that would take events away from the hypervisor. Only the core that runs the test
 * prints, on the virtual UART; lines start with "HVLOG: " so that the hypervisor copies them to
 * its own console. Entry points, vectors and the header are those of sgitest (sgitest-head.S).
 */

#include "gic.h"
#include "cpuon.h"
#include "sgitest.h"

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;
extern u64 hdr_arg;
extern u64 hdr_aic_set[MAX_AIC_LINES];
extern u64 hdr_aic_clr[MAX_AIC_LINES];
extern u64 hdr_aic_bit[MAX_AIC_LINES];
extern u64 hdr_aic_spi[MAX_AIC_LINES];

void uart_putc(int c);
void mmio_write32(u64 addr, u32 val);

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })
#define write_sysreg(r, v) asm volatile("msr " #r ", %0" : : "r"((u64)(v)))
#define isb()              asm volatile("isb")
#define wfi()              asm volatile("wfi")
#define irq_mask()         asm volatile("msr daifset, #2")
/* SError, IRQ and FIQ on. The FIQ must never come: the vector reports it */
#define irq_unmask() asm volatile("msr daifclr, #7")

#define SGI_WAKE   1  // gets a core out of WFI to look at its command
#define PPI_VTIMER 27 // ...and the core that runs the test out of it, every TURN_MS
#define TURN_MS    1

/*
 * How long a core other than the one that runs the test waits before it raises the line: by
 * then every other core waits again, in WFI in the phases that are about that. (When the core
 * that runs the test raises it, that core is awake, and the SPI may well be there before it
 * gets to its WFI.)
 */
#define RAISE_DELAY_US 300

#define ROUND_MS 100
#define LATE_MS  500

/* A phase ends early after so many: each takes as long as the two above together */
#define MISS_LIMIT 30

/* Phases, bits of hdr_arg */
#define PHASE_LINES    (1 << 4)
#define PHASE_BOOT_OFF (1 << 5)

enum { WAIT_SPIN, WAIT_WFI };

/* Commands for a CPU, see run_on() */
enum {
    OP_NONE,
    OP_RAISE,     // raise the lines of raise_mask, RAISE_DELAY_US after saying so
    OP_TAKE_OVER, // run the rest of the test: the boot core is about to turn itself off
};

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields, which the core that
 * runs the test writes. The MMU is off, so there are no caches to worry about and no atomics to
 * use.
 */
struct cpu {
    volatile u64 online;
    u64 mpidr;
    u64 gicr; // its Redistributor, 0 if it has none

    volatile u64 cmd_seq;
    volatile u64 cmd_op;
    volatile u64 cmd_ack;

    volatile u64 taken;      // SPIs of the lines, taken and ended on this core
    volatile u64 other;      // interrupts other than those and SGI_WAKE
    volatile u64 other_last; // ...and the last such INTID
    volatile u64 spurious;   // ICC_IAR1_EL1 read 1023
};

/*
 * Per line. Its SPI is taken on one core at a time, so only that core writes takes, line_taken
 * and taken_at; the core that runs the test writes the rest before it raises the line
 */
struct line {
    u32 spi;
    volatile u64 lower_after; // how often the SPI is taken before the line is lowered
    volatile u64 takes;       // ...and how often it has been in this round
    volatile u64 line_taken;  // in all
    volatile u64 raised_at;
    volatile u64 taken_at;
};

static struct cpu cpus[MAX_CPUS];
static struct line lines[MAX_AIC_LINES];
static u64 nr_lines;
static volatile u64 waiting;    // how the cores wait: WAIT_SPIN or WAIT_WFI
static volatile u64 raise_mask; // which lines OP_RAISE raises
static volatile u64 driver;     // the core that runs the test: 0, or 1 once the boot core is off

static u64 nr_cpus;
static u64 ticks_per_ms;
static u64 rounds;
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
    put("HVLOG: aicroute: ");
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

static void delay_ms(u64 ms)
{
    u64 end = now() + ms * ticks_per_ms;

    while (now() < end)
        ;
}

/* ---- the lines ---- */

/* All the lines of the mask, one write after the other, as close together as they can be */
static void lines_raise(u64 mask)
{
    u64 at = now();

    for (u64 l = 0; l < nr_lines; l++)
        if (mask & (1UL << l))
            lines[l].raised_at = at;
    for (u64 l = 0; l < nr_lines; l++)
        if (mask & (1UL << l))
            mmio_write32(hdr_aic_set[l], hdr_aic_bit[l]);
    asm volatile("dsb sy");
}

/* It has reached AIC when this returns: the next raise, on another core, must not overtake it */
static void line_lower(u64 l)
{
    mmio_write32(hdr_aic_clr[l], hdr_aic_bit[l]);
    asm volatile("dsb sy");
}

static void lines_lower(void)
{
    for (u64 l = 0; l < nr_lines; l++)
        line_lower(l);
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
    struct cpu *c = &cpus[read_sysreg(tpidr_el1)];
    u64 iar = read_sysreg(ICC_IAR1_EL1);
    u32 intid = iar & 0xffffff;
    u64 l;

    if (intid == 1023) {
        /* Nothing to acknowledge, no EOI */
        c->spurious++;
        return;
    }

    for (l = 0; l < nr_lines; l++)
        if (intid == lines[l].spi)
            break;
    if (l < nr_lines) {
        struct line *ln = &lines[l];

        if (++ln->takes >= ln->lower_after)
            line_lower(l);
        ln->taken_at = now();
        ln->line_taken++;
        c->taken++;
    } else if (intid == PPI_VTIMER) {
        /* Level-sensitive: the source has to go low before the EOI */
        write_sysreg(cntv_ctl_el0, 0);
        isb();
    } else if (intid != SGI_WAKE) {
        c->other++;
        c->other_last = intid;
    }

    write_sysreg(ICC_EOIR1_EL1, iar);
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

void bad_exception(u64 vector)
{
    irq_mask();
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

/* ---- CPUs ---- */

/* The order and the values of HalpGic3InitializeLocalUnit, as in the other guests */
static void cpu_init(int slot)
{
    struct cpu *c = &cpus[slot];
    u64 sre;

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    c->mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    c->gicr = gic_find_redist(c->mpidr);

    sre = read_sysreg(ICC_SRE_EL1);
    if (!(sre & 1))
        write_sysreg(ICC_SRE_EL1, sre | 1);
    isb();
    write_sysreg(ICC_IGRPEN1_EL1, 0);
    write_sysreg(ICC_PMR_EL1, 0);
    write_sysreg(ICC_BPR1_EL1, 3);
    if (c->gicr)
        gic_local_init(c->gicr);
    write_sysreg(ICC_CTLR_EL1, read_sysreg(ICC_CTLR_EL1) & ~2UL); // EOImode = 0
    write_sysreg(ICC_IGRPEN1_EL1, 1);
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();

    /* Every core may come to run the test, and then needs its timer to look at the time */
    if (c->gicr) {
        gic_enable_local(c->gicr, SGI_WAKE, 0x80);
        gic_enable_local(c->gicr, PPI_VTIMER, 0x80);
    }

    irq_unmask();
    c->online = 1;
}

/*
 * One turn of a wait for something that another core or a handler writes to memory. What gets a
 * core out of WFI is an interrupt: an SPI, SGI_WAKE, or the timer of the core that runs the test
 */
static void wait_turn(const volatile u64 *word, u64 before)
{
    if (waiting != WAIT_WFI)
        return;
    irq_mask();
    isb();
    if (*word == before)
        wfi();
    irq_unmask();
    isb();
}

static void finish(void);
static void boot_off_phase(void);

void secondary_main(int slot)
{
    struct cpu *c = &cpus[slot];

    cpu_init(slot);

    for (;;) {
        u64 seq = c->cmd_seq;

        if (seq == c->cmd_ack) {
            wait_turn(&c->cmd_seq, seq);
            continue;
        }
        c->cmd_ack = seq;
        asm volatile("dsb sy");
        if (c->cmd_op == OP_RAISE) {
            u64 at = now() + RAISE_DELAY_US * ticks_per_ms / 1000;

            while (now() < at)
                ;
            lines_raise(raise_mask);
        } else if (c->cmd_op == OP_TAKE_OVER) {
            driver = slot;
            boot_off_phase();
            finish();
            irq_mask();
            for (;;)
                ;
        }
    }
}

static u64 sgi_to(int cpu, u32 intid)
{
    u64 mpidr = cpus[cpu].mpidr;
    u64 aff3 = (mpidr >> 32) & 0xff, aff2 = (mpidr >> 16) & 0xff, aff1 = (mpidr >> 8) & 0xff;

    return (aff3 << 48) | (aff2 << 32) | ((u64)intid << 24) | (aff1 << 16) | (1UL << (mpidr & 0xff));
}

static void wake(int cpu)
{
    write_sysreg(ICC_SGI1R_EL1, sgi_to(cpu, SGI_WAKE));
    isb();
}

/* Tell a core to do something; it is not waited for */
static void tell(int cpu, u64 op)
{
    struct cpu *c = &cpus[cpu];

    c->cmd_op = op;
    asm volatile("dsb sy");
    c->cmd_seq++;
    asm volatile("dsb sy");
    if (waiting == WAIT_WFI)
        wake(cpu);
}

/* Have a core raise the lines of the mask; false if it does not answer */
static int raise_on(int cpu, u64 mask)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 2000 * ticks_per_ms;

    raise_mask = mask;
    if ((u64)cpu == driver) {
        lines_raise(mask);
        return 1;
    }
    tell(cpu, OP_RAISE);
    while (c->cmd_ack != c->cmd_seq)
        if (now() > end)
            return 0;
    return 1;
}

static u64 taken_on_all(void)
{
    u64 n = 0;

    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        n += cpus[cpu].taken;
    return n;
}

/*
 * Wait until each line of the mask has been taken so often since `from`; false if not by then.
 * In WFI the core that runs the test has its timer get it out every TURN_MS
 */
static int wait_lines(u64 mask, const u64 *from, u64 times, u64 ms)
{
    u64 end = now() + ms * ticks_per_ms;
    struct cpu *self = &cpus[driver];

    for (;;) {
        u64 seen = self->taken;
        int all = 1;

        for (u64 l = 0; l < nr_lines; l++)
            if (mask & (1UL << l) && lines[l].line_taken < from[l] + times)
                all = 0;
        if (all)
            return 1;
        if (now() > end)
            return 0;
        if (waiting == WAIT_WFI) {
            write_sysreg(cntv_cval_el0, now() + TURN_MS * ticks_per_ms);
            write_sysreg(cntv_ctl_el0, 1);
            isb();
        }
        wait_turn(&self->taken, seen);
    }
}

static void route(u64 l, u64 target)
{
    wr64(hdr_gicd_base + GICD_IROUTER + 8 * lines[l].spi, gic_affinity(cpus[target].mpidr));
}

/* Every core but `skip` (MAX_CPUS for none) waits as `how` says from now on */
static void set_waiting(int how, u64 skip)
{
    waiting = how;
    asm volatile("dsb sy");
    /* Those that spin see it by themselves; those in WFI have to be told */
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        if (cpu != driver && cpu != skip)
            wake(cpu);
    delay_ms(20);
}

struct tally {
    u64 taken, missed, late, elsewhere, silent, slowest, sum;
    u64 missed_by[MAX_CPUS], missed_to[MAX_CPUS];
};

static void tally_start(struct tally *t)
{
    t->taken = t->missed = t->late = t->elsewhere = t->silent = t->slowest = t->sum = 0;
    for (u64 cpu = 0; cpu < MAX_CPUS; cpu++)
        t->missed_by[cpu] = t->missed_to[cpu] = 0;
}

/*
 * One round: `raiser` raises the lines of the mask, line l routed to targets[l]. Each has to be
 * taken `times` times, on its target.
 */
static void round_of(struct tally *t, u64 raiser, u64 mask, const u64 *targets, u64 times)
{
    u64 from[MAX_AIC_LINES], target_before[MAX_AIC_LINES];

    for (u64 l = 0; l < nr_lines; l++) {
        from[l] = lines[l].line_taken;
        target_before[l] = cpus[targets[l]].taken;
        lines[l].takes = 0;
        lines[l].lower_after = times;
    }
    asm volatile("dsb sy");
    if (!raise_on(raiser, mask)) {
        t->silent++;
        lines_lower();
        return;
    }
    if (wait_lines(mask, from, times, ROUND_MS)) {
        for (u64 l = 0; l < nr_lines; l++) {
            u64 took;

            if (!(mask & (1UL << l)))
                continue;
            took = lines[l].taken_at - lines[l].raised_at;
            t->taken++;
            t->sum += took;
            if (took > t->slowest)
                t->slowest = took;
        }
        /* Only meaningful for one line, or for lines with targets of their own */
        for (u64 l = 0; l < nr_lines; l++)
            if (mask & (1UL << l) && mask == (1UL << l) &&
                cpus[targets[l]].taken != target_before[l] + times)
                t->elsewhere++;
        return;
    }

    t->missed++;
    t->missed_by[raiser]++;
    for (u64 l = 0; l < nr_lines; l++)
        if (mask & (1UL << l) && lines[l].line_taken < from[l] + times)
            t->missed_to[targets[l]]++;
    if (t->missed <= 10) {
        line("  missed: raised by cpu ");
        put_dec(raiser);
        put(", lines taken / routed to:");
        for (u64 l = 0; l < nr_lines; l++) {
            if (!(mask & (1UL << l)))
                continue;
            put(" ");
            put_dec(lines[l].line_taken - from[l]);
            put("/cpu ");
            put_dec(targets[l]);
        }
        put("\n");
    }
    /* Does it come later, with nothing done to it? */
    if (wait_lines(mask, from, times, LATE_MS))
        t->late++;
    lines_lower();
    delay_ms(2);
}

static void tally_print(const char *what, struct tally *t, u64 total, u64 expected)
{
    if (t->missed + t->silent >= MISS_LIMIT)
        line("  too many missed, this phase ends here\n");
    line(what);
    put(": taken ");
    put_dec(t->taken);
    put(", missed ");
    put_dec(t->missed);
    put(" (");
    put_dec(t->late);
    put(" of them came later), on another core ");
    put_dec(t->elsewhere);
    put(", no answer ");
    put_dec(t->silent);
    put(", SPIs taken in all ");
    put_dec(total);
    put("; from the raise to the handler, us: slowest ");
    put_dec(t->slowest * 1000 / ticks_per_ms);
    put(" average ");
    put_dec(t->taken ? t->sum * 1000 / ticks_per_ms / t->taken : 0);
    put("\n");
    if (t->missed) {
        for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
            line("  cpu ");
            put_dec(cpu);
            put(": missed when it raised ");
            put_dec(t->missed_by[cpu]);
            put(", when it was a target ");
            put_dec(t->missed_to[cpu]);
            put("\n");
        }
    }
    line("  taken on each core:");
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        put(" ");
        put_dec(cpus[cpu].taken);
    }
    put("\n");
    result("every event arrived on the core its SPI is routed to",
           !t->missed && !t->elsewhere && !t->silent && total == expected);
}

/* ---- the phases ---- */

/*
 * The first line, raised by each core but `skip` with its SPI routed to each core but `skip`
 * in turn; taken `times` times before it is lowered
 */
static void phase(const char *what, int how, u64 times, u64 skip)
{
    struct tally t;
    u64 at_start = taken_on_all(), targets[MAX_AIC_LINES];

    tally_start(&t);
    set_waiting(how, skip);

    for (u64 target = 0; target < nr_cpus; target++) {
        if (target == skip)
            continue;
        for (u64 l = 0; l < nr_lines; l++)
            targets[l] = target;
        route(0, target);
        for (u64 raiser = 0; raiser < nr_cpus; raiser++) {
            if (raiser == skip)
                continue;
            for (u64 r = 0; r < rounds && t.missed + t.silent < MISS_LIMIT; r++) {
                round_of(&t, raiser, 1, targets, times);
                /* One too many would be counted as the next round's */
                if (r == 0)
                    delay_ms(1);
            }
        }
    }

    /* One more than was asked for shows here, if not in the round it came in */
    delay_ms(20);
    tally_print(what, &t, taken_on_all() - at_start, t.taken * times);
}

/* All the lines at once, each routed to another core, by each core in turn; the cores in WFI */
static void phase_lines(void)
{
    struct tally t;
    u64 at_start = taken_on_all(), targets[MAX_AIC_LINES], all = (1UL << nr_lines) - 1;

    tally_start(&t);
    set_waiting(WAIT_WFI, MAX_CPUS);

    for (u64 base = 0; base < nr_cpus; base++) {
        for (u64 l = 0; l < nr_lines; l++) {
            targets[l] = (base + l) % nr_cpus;
            route(l, targets[l]);
        }
        for (u64 raiser = 0; raiser < nr_cpus; raiser++)
            for (u64 r = 0; r < rounds && t.missed + t.silent < MISS_LIMIT; r++) {
                round_of(&t, raiser, all, targets, 1);
                if (r == 0)
                    delay_ms(1);
            }
    }

    delay_ms(20);
    line("lines at once:");
    for (u64 l = 0; l < nr_lines; l++) {
        put(" SPI ");
        put_dec(lines[l].spi);
    }
    put("\n");
    tally_print("  waiting in WFI, the lines raised together", &t, taken_on_all() - at_start,
                t.taken);
}

/* On the core that took over: the boot core is off, or on its way there */
static void boot_off_phase(void)
{
    /* Its CPU_OFF has taken it out of the guest long before this */
    delay_ms(50);
    line("the boot core has turned itself off; cpu ");
    put_dec(driver);
    put(" runs the test now\n");
    phase("the boot core off, waiting in WFI", WAIT_WFI, 1, 0);
}

static void finish(void)
{
    u64 other = 0, spurious = 0;

    set_waiting(WAIT_SPIN, driver == 0 ? MAX_CPUS : 0);
    lines_lower();
    delay_ms(50);
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        other += cpus[cpu].other;
        spurious += cpus[cpu].spurious;
    }
    result("no interrupt arrived that no test asked for", other == 0);
    line("spurious (ICC_IAR1_EL1 read 1023) = ");
    put_dec(spurious);
    put("\n");

    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}

static void done_early(void)
{
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}

void boot_main(void)
{
    int dist;

    rounds = (hdr_arg >> 8) & 0xffffff;
    nr_cpus = hdr_nr_cpus;
    if (nr_cpus < 1 || nr_cpus > MAX_CPUS)
        nr_cpus = 1;
    if (!rounds)
        rounds = 20;
    for (nr_lines = 0; nr_lines < MAX_AIC_LINES && hdr_aic_spi[nr_lines]; nr_lines++)
        lines[nr_lines].spi = hdr_aic_spi[nr_lines];

    /* The Distributor first, as HalpGic3InitializeIoUnit comes before the first local unit */
    dist = gic_dist_init();
    cpu_init(0);

    line("start, cpus=");
    put_dec(nr_cpus);
    put(" SPIs");
    for (u64 l = 0; l < nr_lines; l++) {
        put(" ");
        put_dec(lines[l].spi);
    }
    put(" arg 0x");
    put_hex(hdr_arg, 8);
    put("\n");
    if (dist) {
        line("FAIL Distributor not usable, check ");
        put_dec(dist);
        put("\n");
        nr_fail++;
        done_early();
        return;
    }
    if (!nr_lines) {
        line("FAIL the loader did not say which line (run_sgitest.py --aic-line)\n");
        nr_fail++;
        done_early();
        return;
    }
    for (u64 l = 0; l < nr_lines; l++) {
        if (!hdr_aic_set[l] || !hdr_aic_clr[l] || !hdr_aic_bit[l] || lines[l].spi < 32 ||
            lines[l].spi > 1019) {
            line("FAIL the loader said too little about a line\n");
            nr_fail++;
            done_early();
            return;
        }
    }
    if (hdr_arg & PHASE_LINES && nr_lines < 2) {
        line("FAIL the phase of lines at once needs two lines or three\n");
        nr_fail++;
        done_early();
        return;
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
            nr_cpus = slot;
            break;
        }
    }
    if (nr_cpus > 1)
        result("every CPU_ON took at most 10 ms, and CPU_ON of a CPU that is on says ALREADY_ON",
               cpuon_report(nr_cpus, cpus[0].mpidr));
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        line("cpu ");
        put_dec(cpu);
        put(": MPIDR = ");
        put_hex(cpus[cpu].mpidr, 10);
        put("\n");
        if (!cpus[cpu].gicr) {
            line("FAIL a CPU has no Redistributor\n");
            nr_fail++;
            done_early();
            return;
        }
    }
    if (hdr_arg & PHASE_BOOT_OFF && nr_cpus < 3) {
        line("FAIL the phase with the boot core off needs 3 cores or more\n");
        nr_fail++;
        done_early();
        return;
    }

    lines_lower();
    for (u64 l = 0; l < nr_lines; l++)
        gic_enable_spi(lines[l].spi, 0x80, cpus[0].mpidr, 0);
    for (u32 i = 0; i < 4; i++)
        if (hdr_arg & (1UL << i))
            phase(i & 2 ? (i & 1 ? "waiting in WFI, the line held over the first end"
                                 : "spinning, the line held over the first end")
                        : (i & 1 ? "waiting in WFI" : "spinning"),
                  i & 1 ? WAIT_WFI : WAIT_SPIN, i & 2 ? 2 : 1, MAX_CPUS);
    if (hdr_arg & PHASE_LINES)
        phase_lines();

    if (hdr_arg & PHASE_BOOT_OFF) {
        /* Nothing is routed to the boot core from here on */
        for (u64 l = 0; l < nr_lines; l++)
            route(l, 1);
        set_waiting(WAIT_WFI, MAX_CPUS);
        tell(1, OP_TAKE_OVER);
        irq_mask();
        {
            register u64 x0 asm("x0") = 0x84000002UL; /* CPU_OFF */

            asm volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
        }
        line("FAIL CPU_OFF of the boot core came back\n");
        for (;;)
            ;
    }

    finish();
}
