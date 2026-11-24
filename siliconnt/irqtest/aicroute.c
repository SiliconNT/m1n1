// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest: which core does AIC report a line to, and does every event of a line that
 * software raises arrive as its SPI (src/hv/hv_vgic.c), when some cores run the guest, all of
 * them do, or the others only run m1n1 (SiliconNT R50, docs/findings/0098, conclusion 19)?
 *
 * One line of the table of AIC lines and SPIs is raised with the software trigger of AIC over
 * and over, by one core after the other, with its SPI routed to one core after the other; the
 * handler lowers it. A round in which the SPI has not been taken after ROUND_MS is a miss: the
 * line is lowered then, and the next round begins. Which core the hypervisor took the events
 * on is in its report when the guest is left ("AIC events: N forwarded").
 *
 * hdr_arg: bits 3:0 choose the phases. In the first two the handler lowers the line; in the
 * other two only when it comes the second time, so that AIC gets to choose a core again while
 * the hypervisor, which has IRQs masked, unmasks the line at the end of the first.
 *   bit 0  the cores spin while they wait
 *   bit 1  the cores are in WFI, which the hypervisor turns into a wait in EL2
 *   bit 2  as bit 0, the line held over the first end
 *   bit 3  as bit 1, the line held over the first end
 * Bits 31:8 are the rounds for each pair of a raising and a target core in a phase (0: 20).
 *
 * The loader (run_sgitest.py --aic-line) says which line, as for aictest. The guest never reads
 * AIC: that would take events away from the hypervisor. Only the boot CPU prints, on the
 * virtual UART; lines start with "HVLOG: " so that the hypervisor copies them to its own
 * console. Entry points, vectors and the header are those of sgitest (sgitest-head.S).
 *
 * QEMU has no AIC. There (`make qemu-route-smp`) the line is the pending bit of a level-sensitive
 * SPI, which is set and cleared the way the software trigger is: this checks the guest itself.
 */

#include "gic.h"
#include "sgitest.h"

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;
extern u64 hdr_start_reg[MAX_CPUS];
extern u64 hdr_start_val[MAX_CPUS];
extern u64 hdr_arg;
extern u64 hdr_aic_set;
extern u64 hdr_aic_clr;
extern u64 hdr_aic_bit;
extern u64 hdr_aic_spi;

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

#define SGI_WAKE   1 // gets a core out of WFI to look at its command
#define PPI_VTIMER 27 // ...and the boot CPU out of it, every TURN_MS, to look at the time
#define TURN_MS    1

/*
 * How long a core other than the boot CPU waits before it raises the line: by then every other
 * core waits again, in WFI in the phases that are about that. (When the boot CPU raises it,
 * the boot CPU is awake, and the SPI may well be there before it gets to its WFI.)
 */
#define RAISE_DELAY_US 300

#define ROUND_MS 100
#define LATE_MS  500

/* A phase ends early after so many: each takes as long as the two above together */
#define MISS_LIMIT 30

enum { WAIT_SPIN, WAIT_WFI };

/* Commands for a CPU, see run_on() */
enum {
    OP_NONE,
    OP_RAISE, // raise the line, RAISE_DELAY_US after saying so
};

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields, which the boot CPU
 * writes. The MMU is off, so there are no caches to worry about and no atomics to use.
 */
struct cpu {
    volatile u64 online;
    u64 mpidr;
    u64 gicr; // its Redistributor, 0 if it has none

    volatile u64 cmd_seq;
    volatile u64 cmd_op;
    volatile u64 cmd_ack;

    volatile u64 taken;      // the SPI, taken and ended on this core
    volatile u64 other;      // interrupts other than the SPI and SGI_WAKE
    volatile u64 other_last; // ...and the last such INTID
    volatile u64 spurious;   // ICC_IAR1_EL1 read 1023
};

static struct cpu cpus[MAX_CPUS];
static volatile u64 waiting;     // how the cores wait: WAIT_SPIN or WAIT_WFI
static volatile u64 lower_after; // how often the SPI is taken before the line is lowered
static volatile u64 takes;       // ...and how often it has been in this round
static volatile u64 raised_at;
static volatile u64 taken_at;

/* Boot CPU only */
static u64 nr_cpus;
static u64 ticks_per_ms;
static u32 spi;
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

/* ---- the line ---- */

static void line_raise(void)
{
    raised_at = now();
    mmio_write32(hdr_aic_set, hdr_aic_bit);
    asm volatile("dsb sy");
}

/* It has reached AIC when this returns: the next raise, on another core, must not overtake it */
static void line_lower(void)
{
    mmio_write32(hdr_aic_clr, hdr_aic_bit);
    asm volatile("dsb sy");
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
    struct cpu *c = &cpus[read_sysreg(tpidr_el1)];
    u64 iar = read_sysreg(ICC_IAR1_EL1);
    u32 intid = iar & 0xffffff;

    if (intid == 1023) {
        /* Nothing to acknowledge, no EOI */
        c->spurious++;
        return;
    }

    if (intid == hdr_aic_spi) {
        if (++takes >= lower_after)
            line_lower();
        taken_at = now();
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

    if (c->gicr) {
        gic_enable_local(c->gicr, SGI_WAKE, 0x80);
        if (!slot)
            gic_enable_local(c->gicr, PPI_VTIMER, 0x80);
    }

    irq_unmask();
    c->online = 1;
}

/*
 * One turn of a wait for something that another core or a handler writes to memory. What gets a
 * core out of WFI is an interrupt: the SPI, SGI_WAKE, or the timer of the boot CPU
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
            line_raise();
        }
    }
}

static u64 sgi_to(int cpu, u32 intid)
{
    u64 mpidr = cpus[cpu].mpidr;
    u64 aff3 = (mpidr >> 32) & 0xff, aff2 = (mpidr >> 16) & 0xff, aff1 = (mpidr >> 8) & 0xff;

    return (aff3 << 48) | (aff2 << 32) | ((u64)intid << 24) | (aff1 << 16) | (1UL << (mpidr & 0xff));
}

/* Have a core raise the line; false if it does not answer */
static int raise_on(int cpu)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 2000 * ticks_per_ms;

    if (cpu == 0) {
        line_raise();
        return 1;
    }
    c->cmd_op = OP_RAISE;
    c->cmd_seq++;
    if (waiting == WAIT_WFI) {
        write_sysreg(ICC_SGI1R_EL1, sgi_to(cpu, SGI_WAKE));
        isb();
    }
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

/* Wait until the SPI has been taken so often on all cores; false if it has not been by then */
static int wait_taken(int target, u64 until, u64 ms)
{
    u64 end = now() + ms * ticks_per_ms;

    for (;;) {
        u64 seen = cpus[target].taken;

        if (taken_on_all() >= until)
            return 1;
        if (now() > end)
            return 0;
        if (waiting == WAIT_WFI) {
            write_sysreg(cntv_cval_el0, now() + TURN_MS * ticks_per_ms);
            write_sysreg(cntv_ctl_el0, 1);
            isb();
        }
        wait_turn(&cpus[target].taken, seen);
    }
}

/* ---- the test ---- */

static void phase(int how, u64 times, u64 rounds)
{
    u64 taken = 0, missed = 0, late = 0, elsewhere = 0, slowest = 0, sum = 0;
    u64 missed_by[MAX_CPUS], missed_to[MAX_CPUS];
    u64 silent = 0, at_start = taken_on_all(), total;

    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        missed_by[cpu] = missed_to[cpu] = 0;
    waiting = how;
    lower_after = times;
    asm volatile("dsb sy");
    /* Those that spin see it by themselves; those in WFI have to be told */
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        write_sysreg(ICC_SGI1R_EL1, sgi_to(cpu, SGI_WAKE));
    isb();
    delay_ms(20);

    for (u64 target = 0; target < nr_cpus; target++) {
        wr64(hdr_gicd_base + GICD_IROUTER + 8 * spi, gic_affinity(cpus[target].mpidr));
        for (u64 raiser = 0; raiser < nr_cpus; raiser++) {
            for (u64 round = 0; round < rounds && missed + silent < MISS_LIMIT; round++) {
                u64 before = taken_on_all(), target_before = cpus[target].taken;

                takes = 0;
                asm volatile("dsb sy");
                if (!raise_on(raiser)) {
                    silent++;
                    line_lower();
                    continue;
                }
                if (wait_taken(target, before + times, ROUND_MS)) {
                    u64 took = taken_at - raised_at;

                    taken++;
                    sum += took;
                    if (took > slowest)
                        slowest = took;
                    if (cpus[target].taken != target_before + times)
                        elsewhere++;
                } else {
                    missed++;
                    missed_by[raiser]++;
                    missed_to[target]++;
                    if (missed <= 10) {
                        line("  missed: raised by cpu ");
                        put_dec(raiser);
                        put(", routed to cpu ");
                        put_dec(target);
                        put(", taken ");
                        put_dec(taken_on_all() - before);
                        put("\n");
                    }
                    /* Does it come later, with nothing done to it? */
                    if (wait_taken(target, before + times, LATE_MS))
                        late++;
                    line_lower();
                    delay_ms(2);
                }
                /* One too many would be counted as the next round's */
                if (round == 0)
                    delay_ms(1);
            }
        }
    }

    /* One more than was asked for shows here, if not in the round it came in */
    delay_ms(20);
    total = taken_on_all() - at_start;

    if (missed + silent >= MISS_LIMIT)
        line("  too many missed, this phase ends here\n");
    line(how == WAIT_WFI ? "waiting in WFI" : "spinning");
    put(times > 1 ? ", the line held over the first end: taken " : ": taken ");
    put_dec(taken);
    put(", missed ");
    put_dec(missed);
    put(" (");
    put_dec(late);
    put(" of them came later), on another core ");
    put_dec(elsewhere);
    put(", no answer ");
    put_dec(silent);
    put(", SPIs taken in all ");
    put_dec(total);
    put("; from the raise to the handler, us: slowest ");
    put_dec(slowest * 1000 / ticks_per_ms);
    put(" average ");
    put_dec(taken ? sum * 1000 / ticks_per_ms / taken : 0);
    put("\n");
    if (missed) {
        for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
            line("  cpu ");
            put_dec(cpu);
            put(": missed when it raised ");
            put_dec(missed_by[cpu]);
            put(", when it was the target ");
            put_dec(missed_to[cpu]);
            put("\n");
        }
    }
    line("  taken on each core:");
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        put(" ");
        put_dec(cpus[cpu].taken);
    }
    put("\n");
    result("every event arrived on the core its SPI is routed to", !missed && !elsewhere && !silent && total == taken * times);
}

void boot_main(void)
{
    u64 rounds = (hdr_arg >> 8) & 0xffffff;
    u64 other = 0, spurious = 0;
    int dist;

    nr_cpus = hdr_nr_cpus;
    if (nr_cpus < 1 || nr_cpus > MAX_CPUS)
        nr_cpus = 1;
    if (!rounds)
        rounds = 20;
    spi = hdr_aic_spi;

    /* The Distributor first, as HalpGic3InitializeIoUnit comes before the first local unit */
    dist = gic_dist_init();
    cpu_init(0);

    line("start, cpus=");
    put_dec(nr_cpus);
    put(" SPI ");
    put_dec(spi);
    put(" arg 0x");
    put_hex(hdr_arg, 8);
    put("\n");
    if (dist) {
        line("FAIL Distributor not usable, check ");
        put_dec(dist);
        put("\n");
        nr_fail++;
        goto done;
    }
    if (!hdr_aic_set || !hdr_aic_clr || !hdr_aic_bit || spi < 32 || spi > 1019) {
        line("FAIL the loader did not say which line (run_sgitest.py --aic-line)\n");
        nr_fail++;
        goto done;
    }

    for (u64 slot = 1; slot < nr_cpus; slot++) {
        u64 end;

        /* Where the platform needs to be asked, ask; the CPU then enters secondary_start */
        if (hdr_start_reg[slot])
            mmio_write32(hdr_start_reg[slot], hdr_start_val[slot]);

        end = now() + 20000 * ticks_per_ms;
        while (!cpus[slot].online && now() < end)
            ;
        if (!cpus[slot].online) {
            line("FAIL cpu ");
            put_dec(slot);
            put(" did not come online\n");
            nr_fail++;
            nr_cpus = slot;
            break;
        }
    }
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        line("cpu ");
        put_dec(cpu);
        put(": MPIDR = ");
        put_hex(cpus[cpu].mpidr, 10);
        put("\n");
        if (!cpus[cpu].gicr) {
            line("FAIL a CPU has no Redistributor\n");
            nr_fail++;
            goto done;
        }
    }

    line_lower();
    gic_enable_spi(spi, 0x80, cpus[0].mpidr, 0);
    for (u32 i = 0; i < 4; i++)
        if (hdr_arg & (1UL << i))
            phase(i & 1 ? WAIT_WFI : WAIT_SPIN, i & 2 ? 2 : 1, rounds);

    waiting = WAIT_SPIN;
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        write_sysreg(ICC_SGI1R_EL1, sgi_to(cpu, SGI_WAKE));
    isb();
    line_lower();
    delay_ms(50);
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        other += cpus[cpu].other;
        spurious += cpus[cpu].spurious;
    }
    result("no interrupt arrived that no test asked for", other == 0);
    line("spurious (ICC_IAR1_EL1 read 1023) = ");
    put_dec(spurious);
    put("\n");

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
