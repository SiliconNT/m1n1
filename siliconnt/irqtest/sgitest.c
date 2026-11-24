// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the SGI path of src/hv/hv_vgic.c: does an SGI that a guest sends by
 * writing ICC_SGI1R_EL1 arrive, exactly once, at exactly the CPUs the register names?
 *
 * It uses the three forms the Windows kernel uses (HalpGic3RequestInterrupt,
 * 10.0.26300.9457): one CPU, a target list within a cluster, and all but self. With one CPU
 * the tests are the ones where the sender is its own target. With more, the boot CPU also
 * directs the others through a command word each of them polls.
 *
 * Only the boot CPU prints, on the hypervisor's virtual UART; lines start with "HVLOG: " so
 * that the hypervisor copies them to its own console. Nothing here waits with WFI.
 */

#include "sgitest.h"
#include "gic.h"

#define NR_SGIS 16

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;
extern u64 hdr_start_reg[MAX_CPUS];
extern u64 hdr_start_val[MAX_CPUS];

void uart_putc(int c);
void mmio_write32(u64 addr, u32 val);
u64 read_sgi1r(void);

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })
#define write_sysreg(r, v) asm volatile("msr " #r ", %0" : : "r"((u64)(v)))
#define isb()              asm volatile("isb")
#define irq_mask()         asm volatile("msr daifset, #2")
/* SError, IRQ and FIQ on. The FIQ must never come: the vector reports it */
#define irq_unmask() asm volatile("msr daifclr, #7")

#define SGI1R_IRM (1UL << 40)

/* Commands for a CPU, see run_on() */
enum {
    OP_NONE,
    OP_SEND,      // write arg to ICC_SGI1R_EL1
    OP_SEND_ALL,  // write arg to ICC_SGI1R_EL1 with each of the 16 INTIDs
    OP_MASK,      // mask IRQs
    OP_UNMASK,    // unmask IRQs
    OP_CROSSFIRE, // send an SGI to a CPU over and over, see crossfire_arg()
};

/* SGIs with a special role in a test */
#define SGI_RESEND  5  // sent again by its own handler, once
#define SGI_CROSS_A 10 // crossfire, the first of the two CPUs to the second
#define SGI_CROSS_B 9  // crossfire, the second to the first
#define SGI_RING    11 // crossfire, every CPU to the next one
#define SGI_PING    14 // bounced between two CPUs

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields, pong_* and resend,
 * which the boot CPU writes while that CPU is not using them. The MMU is off, so there are no
 * caches to worry about and no atomics to use.
 */
struct cpu {
    volatile u64 online;
    u64 mpidr;
    u64 gicr; // its Redistributor, 0 if it has none
    u64 ctlr;
    u64 sctlr;
    u64 current_el;

    volatile u64 cmd_seq;
    volatile u64 cmd_op;
    volatile u64 cmd_arg;
    volatile u64 cmd_ack;
    volatile u64 cmd_failed;

    volatile u32 count[NR_SGIS];
    volatile u32 other;      // interrupts that are not SGIs
    volatile u32 other_last; // ...and the last such INTID
    volatile u32 spurious;   // ICC_IAR1_EL1 read 1023
    volatile u32 resend;     // SGIs the handler sends to this CPU again, once, before the EOI
    volatile u64 pong_to;    // ICC_SGI1R_EL1 value the handler of SGI_PING writes...
    volatile u64 pong_left;  // ...this many more times
    volatile u64 rpr;        // ICC_RPR_EL1 in the handler
    volatile u64 spsr;       // SPSR_EL1 in the handler
    volatile u64 slowest;    // crossfire: longest time from sending an SGI to it being taken
    volatile u64 slow;       // crossfire: how many took longer than SLOW_US

    volatile u64 expect_undef;
    volatile u64 undef_count;
    volatile u64 undef_esr;
};

static struct cpu cpus[MAX_CPUS];

/* Boot CPU only */
static u32 seen[MAX_CPUS][NR_SGIS]; // counts already accounted for
static u32 want[MAX_CPUS][NR_SGIS]; // SGIs the running test expects on top of that
static u64 nr_cpus;
static u64 ticks_per_ms;
static u32 nr_pass, nr_fail;
static const char *test_name;
static int test_a, test_b;

static struct cpu *me(void)
{
    return &cpus[read_sysreg(tpidr_el1)];
}

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
    put("HVLOG: sgitest: ");
    put(s);
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

/* ---- the GIC CPU interface ---- */

/* The order and the values of HalpGic3InitializeLocalUnit, as in irqtest.S */
static void gic_cpu_init(u64 gicr)
{
    u64 sre = read_sysreg(ICC_SRE_EL1);

    if (!(sre & 1))
        write_sysreg(ICC_SRE_EL1, sre | 1);
    isb();
    write_sysreg(ICC_IGRPEN1_EL1, 0);
    write_sysreg(ICC_PMR_EL1, 0);
    write_sysreg(ICC_BPR1_EL1, 3);
    if (gicr)
        gic_local_init(gicr);
    write_sysreg(ICC_CTLR_EL1, read_sysreg(ICC_CTLR_EL1) & ~2UL); // EOImode = 0
    write_sysreg(ICC_IGRPEN1_EL1, 1);
    /* HalpGic3SetPriority at the lowest level */
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();

    /* The test uses all 16 SGIs, where Windows registers 0-7 */
    if (gicr)
        for (u32 intid = 0; intid < NR_SGIS; intid++)
            gic_enable_local(gicr, intid, 0x80);
}

static void send_sgi(u64 val)
{
    write_sysreg(ICC_SGI1R_EL1, val);
    isb();
}

/* ICC_SGI1R_EL1 value that names every CPU in the cluster of mpidr whose Aff0 bit is in list */
static u64 sgi_list(u64 mpidr, u32 list, u32 intid)
{
    u64 aff3 = (mpidr >> 32) & 0xff, aff2 = (mpidr >> 16) & 0xff, aff1 = (mpidr >> 8) & 0xff;

    return (aff3 << 48) | (aff2 << 32) | ((u64)intid << 24) | (aff1 << 16) | list;
}

/* ...and the one that names a single CPU */
static u64 sgi_to(int cpu, u32 intid)
{
    return sgi_list(cpus[cpu].mpidr, 1U << (cpus[cpu].mpidr & 0xff), intid);
}

static int same_cluster(int a, int b)
{
    return (cpus[a].mpidr & ~0xffUL) == (cpus[b].mpidr & ~0xffUL);
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
    struct cpu *c = me();
    u64 iar = read_sysreg(ICC_IAR1_EL1);
    u32 intid = iar & 0xffffff;

    if (intid == 1023) {
        /* Nothing to acknowledge, no EOI */
        c->spurious++;
        return;
    }

    if (intid < NR_SGIS) {
        c->rpr = read_sysreg(ICC_RPR_EL1);
        c->spsr = read_sysreg(spsr_el1);
        c->count[intid]++;

        /* While this SGI is still active: it has to come again after the EOI */
        if (c->resend & (1U << intid)) {
            c->resend &= ~(1U << intid);
            send_sgi(sgi_list(c->mpidr, 1U << (c->mpidr & 0xff), intid));
        }

        if (intid == SGI_PING && c->pong_left) {
            c->pong_left--;
            send_sgi(c->pong_to);
        }
    } else {
        c->other++;
        c->other_last = intid;
    }

    write_sysreg(ICC_EOIR1_EL1, iar);
}

u64 sync_handler(u64 esr, u64 elr)
{
    struct cpu *c = me();

    if (!c->expect_undef)
        return 0;

    c->undef_count++;
    c->undef_esr = esr;
    return elr + 4;
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

/* ---- commands ---- */

/*
 * An SGI that only arrives because the target happened to enter the hypervisor for another
 * reason, such as its periodic tick, still arrives. It shows in how long it took.
 */
#define SLOW_US 250

static void crossfire(int target, u32 intid, u64 times)
{
    struct cpu *c = me();
    u64 val = sgi_to(target, intid);

    c->slowest = c->slow = 0;

    for (u64 i = 0; i < times; i++) {
        u32 before = cpus[target].count[intid];
        u64 start = now();
        u64 end = start + 2000 * ticks_per_ms;
        u64 took;

        send_sgi(val);
        while (cpus[target].count[intid] == before) {
            if (now() > end) {
                c->cmd_failed = i + 1;
                return;
            }
        }

        took = now() - start;
        if (took > c->slowest)
            c->slowest = took;
        if (took * 1000 > SLOW_US * ticks_per_ms)
            c->slow++;
    }
}

/* What OP_CROSSFIRE is told: send this SGI so many times, each time waiting for it to arrive */
static u64 crossfire_arg(int target, u32 intid, u64 times)
{
    return times | ((u64)target << 32) | ((u64)intid << 40);
}

static void put_latency(int from, int to)
{
    line("  cpu ");
    put_dec(from);
    put(" to cpu ");
    put_dec(to);
    put(": slowest ");
    put_dec(cpus[from].slowest * 1000 / ticks_per_ms);
    put(" us, slower than 250 us: ");
    put_dec(cpus[from].slow);
    put("\n");
}

static void execute(u64 op, u64 arg)
{
    switch (op) {
        case OP_SEND:
            send_sgi(arg);
            break;
        case OP_SEND_ALL:
            for (u64 intid = 0; intid < NR_SGIS; intid++)
                send_sgi(arg | (intid << 24));
            break;
        case OP_MASK:
            irq_mask();
            break;
        case OP_UNMASK:
            irq_unmask();
            break;
        case OP_CROSSFIRE:
            crossfire((arg >> 32) & 0xff, arg >> 40, arg & 0xffffffff);
            break;
    }
}

static void cpu_init(int slot)
{
    struct cpu *c = &cpus[slot];

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    c->mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    c->sctlr = read_sysreg(sctlr_el1);
    c->current_el = read_sysreg(CurrentEL);
    c->gicr = gic_find_redist(c->mpidr);
    gic_cpu_init(c->gicr);
    c->ctlr = read_sysreg(ICC_CTLR_EL1);
    irq_unmask();
    c->online = 1;
}

void secondary_main(int slot)
{
    struct cpu *c = &cpus[slot];

    cpu_init(slot);

    for (;;) {
        u64 seq = c->cmd_seq;

        if (seq != c->cmd_ack) {
            execute(c->cmd_op, c->cmd_arg);
            c->cmd_ack = seq;
        }
    }
}

static void start_command(int cpu, u64 op, u64 arg)
{
    struct cpu *c = &cpus[cpu];

    c->cmd_op = op;
    c->cmd_arg = arg;
    c->cmd_seq++;
}

static int wait_command(int cpu, u64 ms)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + ms * ticks_per_ms;

    while (c->cmd_ack != c->cmd_seq)
        if (now() > end)
            return 0;
    return 1;
}

/* Have a CPU do something and wait until it has */
static void run_on(int cpu, u64 op, u64 arg)
{
    if (cpu == 0) {
        execute(op, arg);
        return;
    }

    start_command(cpu, op, arg);
    if (!wait_command(cpu, 2000)) {
        line("FAIL cpu ");
        put_dec(cpu);
        put(" does not answer\n");
        nr_fail++;
    }
}

/* ---- checking ---- */

static void expect(int cpu, u32 intid, u32 times)
{
    want[cpu][intid] += times;
}

static void expect_all(int cpu)
{
    for (u32 intid = 0; intid < NR_SGIS; intid++)
        expect(cpu, intid, 1);
}

static int arrived(void)
{
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        for (u32 intid = 0; intid < NR_SGIS; intid++)
            if (cpus[cpu].count[intid] - seen[cpu][intid] < want[cpu][intid])
                return 0;
    return 1;
}

/* The name of a test, with @ and # standing for the two CPUs it is about */
static void put_name(void)
{
    for (const char *s = test_name; *s; s++) {
        if (*s == '@')
            put_dec(test_a);
        else if (*s == '#')
            put_dec(test_b);
        else
            uart_putc(*s);
    }
}

/*
 * Wait until every SGI the test expects has arrived, give stray ones a moment to show up,
 * then compare: each CPU must have taken exactly what was expected and nothing else.
 */
static void check(const char *name, u64 timeout_ms)
{
    u64 end = now() + timeout_ms * ticks_per_ms;
    int ok = 1;

    test_name = name;

    while (!arrived() && now() < end)
        ;
    delay_ms(20);

    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        struct cpu *c = &cpus[cpu];

        for (u32 intid = 0; intid < NR_SGIS; intid++) {
            u32 got = c->count[intid] - seen[cpu][intid];

            if (got != want[cpu][intid]) {
                line("  ");
                put_name();
                put(": cpu ");
                put_dec(cpu);
                put(" sgi ");
                put_dec(intid);
                put(" got ");
                put_dec(got);
                put(" want ");
                put_dec(want[cpu][intid]);
                put("\n");
                ok = 0;
            }
            seen[cpu][intid] = c->count[intid];
            want[cpu][intid] = 0;
        }

        if (c->other || c->spurious || c->cmd_failed) {
            line("  ");
            put_name();
            put(": cpu ");
            put_dec(cpu);
            put(" other spurious last-other cmd-failed = ");
            put_dec(c->other);
            put(" ");
            put_dec(c->spurious);
            put(" ");
            put_dec(c->other_last);
            put(" ");
            put_dec(c->cmd_failed);
            put("\n");
            c->other = c->spurious = 0;
            c->cmd_failed = 0;
            ok = 0;
        }
    }

    line(ok ? "PASS " : "FAIL ");
    put_name();
    put("\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;
}

/* An Aff0 below 16 that no CPU in the cluster of the boot CPU has, or -1 */
static int unused_aff0(void)
{
    for (int aff0 = 15; aff0 >= 0; aff0--) {
        int used = 0;

        for (u64 cpu = 0; cpu < nr_cpus; cpu++)
            if (same_cluster(0, cpu) && (cpus[cpu].mpidr & 0xff) == (u64)aff0)
                used = 1;
        if (!used)
            return aff0;
    }
    return -1;
}

/* ---- tests ---- */

/* The sender is its own target. These are all there is with a single CPU. */
static void tests_self(void)
{
    struct cpu *c = &cpus[0];
    int spare = unused_aff0();

    /* Every INTID, one at a time, with the interrupt taken before the next is sent */
    for (u32 intid = 0; intid < NR_SGIS; intid++) {
        u32 before = c->count[intid];
        u64 end = now() + 500 * ticks_per_ms;

        send_sgi(sgi_to(0, intid));
        while (c->count[intid] == before && now() < end)
            ;
        expect(0, intid, 1);
    }
    check("self, each of the 16 SGIs", 500);

    line("  in the handler: RPR SPSR = ");
    put_hex(c->rpr, 8);
    put(" ");
    put_hex(c->spsr, 8);
    put("\n");

    /* More SGIs pending at once than there can be list registers, none of them lost */
    irq_mask();
    execute(OP_SEND_ALL, sgi_to(0, 0));
    delay_ms(5);
    line("  16 pending, masked: taken HPPIR1 ISR_EL1 = ");
    put_dec(c->count[0] - seen[0][0]);
    put(" ");
    put_hex(read_sysreg(ICC_HPPIR1_EL1), 8);
    put(" ");
    put_hex(read_sysreg(isr_el1), 8);
    put("\n");
    irq_unmask();
    expect_all(0);
    check("self, 16 SGIs pending at once", 1000);

    /* An SGI that is already pending does not become two */
    irq_mask();
    send_sgi(sgi_to(0, 3));
    send_sgi(sgi_to(0, 3));
    send_sgi(sgi_to(0, 3));
    delay_ms(5);
    irq_unmask();
    expect(0, 3, 1);
    check("self, sent three times while pending", 500);

    /* An SGI sent while it is being handled is taken again */
    c->resend = 1U << SGI_RESEND;
    send_sgi(sgi_to(0, SGI_RESEND));
    expect(0, SGI_RESEND, 2);
    check("self, sent again from its own handler", 500);

    /* All but self: nobody when alone, the others otherwise (checked again further down) */
    send_sgi(SGI1R_IRM | (7UL << 24));
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        expect(cpu, 7, 1);
    check("all but self, from the boot CPU", 500);

    /* Targets that do not exist are ignored */
    send_sgi(sgi_list(c->mpidr, 0, 1));
    send_sgi(sgi_list(c->mpidr ^ 0x4000, 1U << (c->mpidr & 0xff), 1));
    send_sgi(sgi_list(c->mpidr ^ 0x400000, 1U << (c->mpidr & 0xff), 1));
    if (spare >= 0)
        send_sgi(sgi_list(c->mpidr, 1U << spare, 1));
    check("targets that do not exist", 100);

    /* ...also when they share the target list with one that does */
    if (spare >= 0) {
        send_sgi(sgi_list(c->mpidr, (1U << spare) | (1U << (c->mpidr & 0xff)), 2));
        expect(0, 2, 1);
        check("target list with self and a CPU that does not exist", 500);
    }

    /* ICC_SGI1R_EL1 is write-only: reading it is UNDEFINED */
    c->expect_undef = 1;
    read_sgi1r();
    c->expect_undef = 0;
    line("  read of ICC_SGI1R_EL1: exceptions ESR_EL1 = ");
    put_dec(c->undef_count);
    put(" ");
    put_hex(c->undef_esr, 8);
    put("\n");
    if (c->undef_count == 1 && (c->undef_esr >> 26) == 0) {
        line("PASS read of ICC_SGI1R_EL1 is UNDEFINED\n");
        nr_pass++;
    } else {
        line("FAIL read of ICC_SGI1R_EL1 is UNDEFINED\n");
        nr_fail++;
    }
}

/* From one CPU to another */
static void tests_pair(int a, int b)
{
    test_a = a;
    test_b = b;

    run_on(a, OP_SEND, sgi_to(b, 6));
    expect(b, 6, 1);
    check("cpu @ to cpu #, one SGI", 1000);

    /* More than the target has list registers, while it cannot take any */
    run_on(b, OP_MASK, 0);
    run_on(a, OP_SEND_ALL, sgi_to(b, 0));
    delay_ms(5);
    run_on(b, OP_UNMASK, 0);
    expect_all(b);
    check("cpu @ to cpu #, 16 SGIs while it has IRQs masked", 1000);

    run_on(a, OP_SEND, SGI1R_IRM | (8UL << 24));
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        if (cpu != (u64)a)
            expect(cpu, 8, 1);
    check("cpu @, all but self", 1000);
}

#define PING_ROUNDS  1000
#define CROSS_ROUNDS 1000

/* The boot CPU and one other, sending to each other a lot */
static void tests_stress(int b)
{
    test_a = 0;
    test_b = b;

    /*
     * Every arrival sends the SGI back, from the handler. The first one goes to b, which
     * therefore takes one more than the boot CPU.
     */
    cpus[0].pong_to = sgi_to(b, SGI_PING);
    cpus[b].pong_to = sgi_to(0, SGI_PING);
    cpus[0].pong_left = cpus[b].pong_left = PING_ROUNDS;
    send_sgi(sgi_to(b, SGI_PING));
    expect(0, SGI_PING, PING_ROUNDS);
    expect(b, SGI_PING, PING_ROUNDS + 1);
    check("cpu @ and cpu #, SGI bounced back and forth", 20000);

    /*
     * Both write ICC_SGI1R_EL1 for each other at the same time, each waiting for its SGI to
     * be taken before it sends the next, so that none may be merged or lost.
     */
    start_command(b, OP_CROSSFIRE, crossfire_arg(0, SGI_CROSS_B, CROSS_ROUNDS));
    crossfire(b, SGI_CROSS_A, CROSS_ROUNDS);
    if (!wait_command(b, 20000))
        cpus[b].cmd_failed = ~0UL;
    expect(0, SGI_CROSS_B, CROSS_ROUNDS);
    expect(b, SGI_CROSS_A, CROSS_ROUNDS);
    check("cpu @ and cpu #, both sending at the same time", 1000);
    put_latency(0, b);
    put_latency(b, 0);
}

/* The same for two CPUs neither of which is the boot CPU, which only looks on */
static void tests_cross(int a, int b)
{
    test_a = a;
    test_b = b;

    start_command(a, OP_CROSSFIRE, crossfire_arg(b, SGI_CROSS_A, CROSS_ROUNDS));
    start_command(b, OP_CROSSFIRE, crossfire_arg(a, SGI_CROSS_B, CROSS_ROUNDS));
    if (!wait_command(a, 20000))
        cpus[a].cmd_failed = ~0UL;
    if (!wait_command(b, 20000))
        cpus[b].cmd_failed = ~0UL;
    expect(a, SGI_CROSS_B, CROSS_ROUNDS);
    expect(b, SGI_CROSS_A, CROSS_ROUNDS);
    check("cpu @ and cpu #, both sending at the same time", 1000);
    put_latency(a, b);
    put_latency(b, a);
}

/* Every CPU sends to the next one, all of them at the same time */
static void tests_ring(void)
{
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        start_command(cpu, OP_CROSSFIRE,
                      crossfire_arg((cpu + 1) % nr_cpus, SGI_RING, CROSS_ROUNDS));
    crossfire(1, SGI_RING, CROSS_ROUNDS);
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        if (!wait_command(cpu, 20000))
            cpus[cpu].cmd_failed = ~0UL;
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        expect(cpu, SGI_RING, CROSS_ROUNDS);
    check("every cpu sending to the next one, all at the same time", 1000);
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        put_latency(cpu, (cpu + 1) % nr_cpus);
}

/* A target list that names several CPUs of one cluster, the sender among them */
static void tests_list(void)
{
    u32 list = 0;
    int n = 0;

    for (u64 cpu = 0; cpu < nr_cpus; cpu++) {
        if (same_cluster(0, cpu)) {
            list |= 1U << (cpus[cpu].mpidr & 0xff);
            expect(cpu, 4, 1);
            n++;
        }
    }

    if (n < 2) {
        for (u64 cpu = 0; cpu < nr_cpus; cpu++)
            want[cpu][4] = 0;
        line("SKIP target list with several CPUs: no other CPU in the cluster of the boot CPU\n");
        return;
    }

    send_sgi(sgi_list(cpus[0].mpidr, list, 4));
    check("target list with every CPU of the boot CPU's cluster", 1000);
}

static void describe(int slot)
{
    struct cpu *c = &cpus[slot];

    line("cpu ");
    put_dec(slot);
    put(": MPIDR GICR CurrentEL SCTLR_EL1 ICC_CTLR_EL1 = ");
    put_hex(c->mpidr, 10);
    put(" ");
    put_hex(c->gicr, 10);
    put(" ");
    put_hex(c->current_el, 1);
    put(" ");
    put_hex(c->sctlr, 16);
    put(" ");
    put_hex(c->ctlr, 8);
    put("\n");
}

void boot_main(void)
{
    int dist;

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
        if ((cpus[cpu].mpidr & 0xff) >= 16) {
            line("FAIL a CPU has Aff0 >= 16, which needs the range selector\n");
            nr_fail++;
            goto done;
        }
    }

    tests_self();

    for (u64 a = 0; a < nr_cpus; a++)
        for (u64 b = 0; b < nr_cpus; b++)
            if (a != b)
                tests_pair(a, b);

    if (nr_cpus > 1) {
        tests_list();
        for (u64 b = 1; b < nr_cpus; b++)
            tests_stress(b);
    }
    if (nr_cpus > 2) {
        for (u64 a = 1; a < nr_cpus; a++)
            for (u64 b = a + 1; b < nr_cpus; b++)
                tests_cross(a, b);
        tests_ring();
    }

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
