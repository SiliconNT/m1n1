// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the WFI trap of src/hv/hv_wfi.c: a CPU that idles in WFI has to come out
 * of it for an interrupt that was pending already, for its timer and for an SGI from another
 * CPU, and must then get that interrupt exactly once.
 *
 * It idles the way an operating system does: with IRQs masked, so that no interrupt can slip
 * in between the look and the WFI, in a loop, because a WFI may return for no reason at all
 * ("The PE is permitted to leave the low-power state for any reason", Arm DDI 0487 M.d
 * D1.7.2), and it unmasks IRQs afterwards. How long a wakeup took and how many WFIs were
 * executed is printed. Under the hypervisor the boot CPU leaves WFI at every tick of the
 * hypervisor, a thousand times a second, so on it a wakeup that was missed looks like one that
 * was a little late. The other CPUs have such a tick once a second: there a wakeup that takes
 * longer than LATE_MS fails the test.
 *
 * Only the boot CPU prints, on the hypervisor's virtual UART; lines start with "HVLOG: " so
 * that the hypervisor copies them to its own console. Entry points and vectors are those of
 * sgitest (sgitest-head.S).
 */

#include "sgitest.h"
#include "gic.h"

/* Interrupts that are counted one by one: SGIs and PPIs */
#define NR_COUNTED 32

#define PPI_VTIMER 27
#define SGI_A      1

#define ISR_I (1 << 7)

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;
extern u64 hdr_start_reg[MAX_CPUS];
extern u64 hdr_start_val[MAX_CPUS];

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

/* Commands for a CPU other than the boot CPU, see start_on() */
enum {
    OP_NONE,
    OP_WFI_PENDING, // make an SGI pending for itself with IRQs masked, then one WFI
    OP_IDLE,        // idle in WFI until an interrupt is pending
    OP_IDLE_TIMER,  // the same after setting its timer to arg ms from now
    OP_SEND_LATER,  // wait arg2 ms, then write arg to ICC_SGI1R_EL1
};

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields other than the two
 * answers, which the boot CPU writes. The MMU is off, so there are no caches to worry about
 * and no atomics to use.
 */
struct cpu {
    volatile u64 online;
    u64 mpidr;
    u64 gicr; // its Redistributor, 0 if it has none

    volatile u64 cmd_seq;
    volatile u64 cmd_op;
    volatile u64 cmd_arg;
    volatile u64 cmd_arg2;
    volatile u64 cmd_begun; // the command with this number has been started
    volatile u64 cmd_ack;   // ...and is done

    volatile u64 stamp; // when it came out of WFI, when it sent its SGI, or how long one WFI took
    volatile u64 late;  // the latest its timer got it out of WFI after it had fired, in ticks
    volatile u64 early; // it came out of idle before its timer had fired
    volatile u64 wfis;  // WFI instructions it has executed

    volatile u32 count[NR_COUNTED];
    volatile u32 other;      // interrupts with another INTID
    volatile u32 other_last; // ...and the last such INTID
    volatile u32 spurious;   // ICC_IAR1_EL1 read 1023
};

static struct cpu cpus[MAX_CPUS];

/* Boot CPU only */
static u32 seen[MAX_CPUS][NR_COUNTED]; // counts already accounted for
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
    put("HVLOG: wfitest: ");
    put(s);
}

/* The name of a test, with # standing for the other CPU it is about */
static void result(const char *name, int cpu, int ok)
{
    line(ok ? "PASS " : "FAIL ");
    for (const char *s = name; *s; s++) {
        if (*s == '#')
            put_dec(cpu);
        else
            uart_putc(*s);
    }
    put("\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;
}

/* A measurement that goes with the test above it: "name = value us" */
static void measured(const char *name, int cpu, u64 ticks)
{
    line("     ");
    for (const char *s = name; *s; s++) {
        if (*s == '#')
            put_dec(cpu);
        else
            uart_putc(*s);
    }
    put(" = ");
    put_dec(ticks * 1000 / ticks_per_ms);
    put(" us\n");
}

static void counted(const char *name, int cpu, u64 n)
{
    line("     ");
    for (const char *s = name; *s; s++) {
        if (*s == '#')
            put_dec(cpu);
        else
            uart_putc(*s);
    }
    put(" = ");
    put_dec(n);
    put("\n");
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

    if (intid == PPI_VTIMER) {
        /* Level-sensitive: the source has to go low before the EOI */
        write_sysreg(cntv_ctl_el0, 0);
        isb();
    }

    if (intid < NR_COUNTED) {
        c->count[intid]++;
    } else {
        c->other++;
        c->other_last = intid;
    }

    write_sysreg(ICC_EOIR1_EL1, iar);
}

/* No synchronous exception is expected: sgitest-head.S reports it and stops */
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

/* ---- what a CPU does with WFI ---- */

static void send_sgi(u64 val)
{
    write_sysreg(ICC_SGI1R_EL1, val);
    isb();
}

/* ICC_SGI1R_EL1 value that names one CPU */
static u64 sgi_to(int cpu, u32 intid)
{
    u64 mpidr = cpus[cpu].mpidr;
    u64 aff3 = (mpidr >> 32) & 0xff, aff2 = (mpidr >> 16) & 0xff, aff1 = (mpidr >> 8) & 0xff;

    return (aff3 << 48) | (aff2 << 32) | ((u64)intid << 24) | (aff1 << 16) | (1UL << (mpidr & 0xff));
}

static void set_timer(u64 cval)
{
    write_sysreg(cntv_cval_el0, cval);
    write_sysreg(cntv_ctl_el0, 1); /* ENABLE, not masked */
    isb();
}

static int irq_pending(void)
{
    return (read_sysreg(isr_el1) & ISR_I) != 0;
}

/*
 * Idle until an interrupt is pending. Called with IRQs masked: a pending interrupt ends a WFI
 * whether it is masked or not. Returns when that was.
 */
static u64 idle(struct cpu *c)
{
    while (!irq_pending()) {
        wfi();
        c->wfis++;
    }
    return now();
}

/* The same without WFI, to compare */
static u64 spin(void)
{
    while (!irq_pending())
        ;
    return now();
}

/*
 * With IRQs masked and an interrupt on its way: see that it is pending, then execute one WFI.
 * Returns how long the WFI took, or all ones if nothing showed up as pending.
 */
static u64 wfi_once_pending(struct cpu *c)
{
    u64 start, end = now() + 100 * ticks_per_ms;

    while (!irq_pending())
        if (now() > end)
            return ~0UL;
    start = now();
    wfi();
    end = now();
    c->wfis++;
    return end - start;
}

/* ...the interrupt being an SGI this CPU sends to itself. IRQs come back on. */
static u64 wfi_with_sgi_pending(int slot)
{
    u64 ticks;

    irq_mask();
    send_sgi(sgi_to(slot, SGI_A));
    ticks = wfi_once_pending(&cpus[slot]);
    irq_unmask();
    isb();
    return ticks;
}

/* Idle until the timer, set to ms from now, has made its interrupt pending. IRQs come back on. */
static void idle_until_timer(struct cpu *c, u64 ms, int use_wfi)
{
    u64 cval, woke;

    irq_mask();
    cval = now() + ms * ticks_per_ms;
    set_timer(cval);
    woke = use_wfi ? idle(c) : spin();
    if (woke < cval)
        c->early++;
    else if (woke - cval > c->late)
        c->late = woke - cval;
    irq_unmask();
    isb();
}

static void execute(struct cpu *c, u64 op, u64 arg, u64 arg2)
{
    switch (op) {
        case OP_WFI_PENDING:
            c->stamp = wfi_with_sgi_pending(c - cpus);
            break;
        case OP_IDLE:
            irq_mask();
            c->stamp = idle(c);
            irq_unmask();
            isb();
            break;
        case OP_IDLE_TIMER:
            idle_until_timer(c, arg, 1);
            break;
        case OP_SEND_LATER:
            delay_ms(arg2);
            c->stamp = now();
            send_sgi(arg);
            break;
    }
}

/* ---- CPUs ---- */

/* The order and the values of HalpGic3InitializeLocalUnit, then the interrupts the tests use */
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
    /* HalpGic3SetPriority at the lowest level */
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();

    if (c->gicr) {
        gic_enable_local(c->gicr, SGI_A, 0x80);
        gic_enable_local(c->gicr, PPI_VTIMER, 0x80);
    }

    irq_unmask();
    c->online = 1;
}

/* Between commands the other CPUs spin: when they execute WFI is up to the boot CPU */
void secondary_main(int slot)
{
    struct cpu *c = &cpus[slot];

    cpu_init(slot);

    for (;;) {
        u64 seq = c->cmd_seq;

        if (seq != c->cmd_ack) {
            c->cmd_begun = seq;
            execute(c, c->cmd_op, c->cmd_arg, c->cmd_arg2);
            c->cmd_ack = seq;
        }
    }
}

/* Have another CPU begin something; finish_on() waits until it is done with it */
static void start_on(int cpu, u64 op, u64 arg, u64 arg2)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 2000 * ticks_per_ms;

    c->cmd_op = op;
    c->cmd_arg = arg;
    c->cmd_arg2 = arg2;
    c->cmd_seq++;
    while (c->cmd_begun != c->cmd_seq && now() < end)
        ;
}

static int finish_on(int cpu)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 2000 * ticks_per_ms;

    while (c->cmd_ack != c->cmd_seq)
        if (now() > end)
            return 0;
    return 1;
}

/* ---- checking ---- */

/* How often a CPU took an interrupt since this was last asked */
static u32 took(int cpu, u32 intid)
{
    u32 n = cpus[cpu].count[intid] - seen[cpu][intid];

    seen[cpu][intid] += n;
    return n;
}

/* The same after waiting for it to arrive, and then a little longer for one too many */
static u32 wait_took(int cpu, u32 intid, u64 timeout_ms)
{
    u64 end = now() + timeout_ms * ticks_per_ms;

    while (cpus[cpu].count[intid] == seen[cpu][intid] && now() < end)
        ;
    delay_ms(5);
    return took(cpu, intid);
}

#define SOON_MS  500
#define TIMER_MS 20
#define LATE_MS  100

/* ---- tests on the boot CPU ---- */

static void tests_boot(void)
{
    struct cpu *c = &cpus[0];
    u64 took_ticks, wfis;
    u32 n, m;

    took_ticks = wfi_with_sgi_pending(0);
    n = wait_took(0, SGI_A, SOON_MS);
    result("WFI with an SGI pending and IRQs masked: returns, the SGI is taken once", 0,
           took_ticks < 100 * ticks_per_ms && n == 1);
    if (took_ticks != ~0UL)
        measured("that WFI took", 0, took_ticks);

    /* The timer has fired by the time of the WFI */
    irq_mask();
    set_timer(now());
    took_ticks = wfi_once_pending(c);
    irq_unmask();
    isb();
    n = wait_took(0, PPI_VTIMER, SOON_MS);
    result("WFI with the timer interrupt pending and IRQs masked: returns, taken once", 0,
           took_ticks < 100 * ticks_per_ms && n == 1);
    if (took_ticks != ~0UL)
        measured("that WFI took", 0, took_ticks);

    /* Nothing is pending: the CPU stays in WFI until its timer fires */
    c->late = c->early = 0;
    wfis = c->wfis;
    m = 0;
    for (int i = 0; i < 20; i++) {
        idle_until_timer(c, TIMER_MS, 1);
        m += wait_took(0, PPI_VTIMER, SOON_MS);
    }
    result("idle in WFI until the timer fires, 20 times: 20 interrupts, never early", 0,
           m == 20 && !c->early);
    measured("latest out of WFI after the timer had fired", 0, c->late);
    counted("WFI instructions executed for these 20", 0, c->wfis - wfis);

    /* The same wait without WFI, for comparison */
    c->late = c->early = 0;
    m = 0;
    for (int i = 0; i < 20; i++) {
        idle_until_timer(c, TIMER_MS, 0);
        m += wait_took(0, PPI_VTIMER, SOON_MS);
    }
    result("the same spinning instead of WFI: 20 interrupts, never early", 0, m == 20 && !c->early);
    measured("latest notice after the timer had fired", 0, c->late);
}

/* ---- tests with a second CPU ---- */

static void tests_pair(int b)
{
    struct cpu *c = &cpus[0], *o = &cpus[b];
    u64 sent, woke, worst, wfis;
    u32 n, m;
    int ok;

    start_on(b, OP_WFI_PENDING, 0, 0);
    ok = finish_on(b);
    n = wait_took(b, SGI_A, SOON_MS);
    result("cpu #: WFI with an SGI pending and IRQs masked: returns, the SGI is taken once", b,
           ok && o->stamp < 100 * ticks_per_ms && n == 1);
    if (ok && o->stamp != ~0UL)
        measured("that WFI took", b, o->stamp);

    /* The other CPU idles; an SGI from this one gets it out */
    wfis = o->wfis;
    worst = 0;
    m = 0;
    ok = 1;
    for (int i = 0; i < 50; i++) {
        start_on(b, OP_IDLE, 0, 0);
        delay_ms(2);
        n = took(b, SGI_A); /* nothing must have got it out before */
        if (n || o->cmd_ack == o->cmd_seq)
            ok = 0;
        sent = now();
        send_sgi(sgi_to(b, SGI_A));
        if (!finish_on(b))
            ok = 0;
        else if (o->stamp - sent > worst)
            worst = o->stamp - sent;
        m += wait_took(b, SGI_A, SOON_MS);
    }
    result("cpu # idle in WFI, SGI from cpu 0, 50 times: out of WFI in time each time, 50 interrupts",
           b, ok && m == 50 && worst < LATE_MS * ticks_per_ms);
    measured("latest out of WFI after the SGI was sent", b, worst);
    counted("WFI instructions executed for these 50", b, o->wfis - wfis);

    /* This CPU idles; an SGI from the other one gets it out */
    wfis = c->wfis;
    worst = 0;
    m = 0;
    ok = 1;
    for (int i = 0; i < 20; i++) {
        irq_mask();
        start_on(b, OP_SEND_LATER, sgi_to(0, SGI_A), 3);
        woke = idle(c);
        irq_unmask();
        isb();
        if (!finish_on(b))
            ok = 0;
        else if (woke - o->stamp > worst)
            worst = woke - o->stamp;
        m += wait_took(0, SGI_A, SOON_MS);
    }
    result("cpu 0 idle in WFI, SGI from cpu #, 20 times: out of WFI each time, 20 interrupts", b,
           ok && m == 20);
    measured("latest out of WFI after the SGI was sent", b, worst);
    counted("WFI instructions executed for these 20", 0, c->wfis - wfis);

    /* The other CPU idles until its own timer fires */
    o->late = o->early = 0;
    wfis = o->wfis;
    m = 0;
    ok = 1;
    for (int i = 0; i < 10; i++) {
        start_on(b, OP_IDLE_TIMER, TIMER_MS, 0);
        if (!finish_on(b))
            ok = 0;
        m += wait_took(b, PPI_VTIMER, SOON_MS);
    }
    result("cpu # idle in WFI until its timer fires, 10 times: 10 interrupts, never early or late",
           b, ok && m == 10 && !o->early && o->late < LATE_MS * ticks_per_ms);
    measured("latest out of WFI after the timer had fired", b, o->late);
    counted("WFI instructions executed for these 10", b, o->wfis - wfis);
}

static void describe(int slot)
{
    struct cpu *c = &cpus[slot];

    line("cpu ");
    put_dec(slot);
    put(": MPIDR GICR = ");
    put_hex(c->mpidr, 10);
    put(" ");
    put_hex(c->gicr, 10);
    put("\n");
}

void boot_main(void)
{
    int dist;
    u32 other = 0, spurious = 0;

    nr_cpus = hdr_nr_cpus;
    if (nr_cpus < 1 || nr_cpus > MAX_CPUS)
        nr_cpus = 1;

    /* The Distributor first, as HalpGic3InitializeIoUnit comes before the first local unit */
    dist = gic_dist_init();
    cpu_init(0);

    line("start, cpus=");
    put_dec(nr_cpus);
    put("\n");
    if (dist) {
        line("FAIL Distributor not usable, check ");
        put_dec(dist);
        put("\n");
        nr_fail++;
        goto done;
    }
    describe(0);

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
        describe(slot);
    }

    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        if (!cpus[cpu].gicr) {
            line("FAIL a CPU has no Redistributor\n");
            nr_fail++;
            goto done;
        }
    }

    tests_boot();
    for (u64 b = 1; b < nr_cpus; b++)
        tests_pair(b);

    /* Everything that arrived has been asked for by a test */
    delay_ms(50);
    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        other += cpus[cpu].other;
        spurious += cpus[cpu].spurious;
        for (u32 intid = 0; intid < NR_COUNTED; intid++)
            other += took(cpu, intid);
    }
    result("no interrupt arrived that no test asked for", 0, other == 0);
    line("spurious (ICC_IAR1_EL1 read 1023) = ");
    put_dec(spurious);
    put("\n");
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        counted("WFI instructions executed in all by cpu #", cpu, cpus[cpu].wfis);

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
