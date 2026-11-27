// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the SGI path of src/hv/hv_vgic.c: does an SGI that a guest sends by
 * writing ICC_SGI1R_EL1 arrive, exactly once, at exactly the CPUs the register names?
 *
 * It uses two of the three forms the Windows kernel uses (HalpGic3RequestInterrupt,
 * 10.0.26300.9457): one CPU, and all but self. With one CPU the tests are the ones where the
 * sender is its own target. With more, the boot CPU also directs the others through a command
 * word each of them polls, and at the end every CPU sends to the next one at the same time.
 *
 * Only the boot CPU prints, on the hypervisor's virtual UART; lines start with "HVLOG: " so
 * that the hypervisor copies them to its own console. Nothing here waits with WFI.
 */

#define GUEST_NAME "sgitest"

#include "sgitest.h"
#include "gic.h"
#include "cpuon.h"
#include "guest.h"

#define NR_SGIS 16

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;

void mmio_write32(u64 addr, u32 val);

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

#define SGI_RING 11 // crossfire, every CPU to the next one

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields, which the boot CPU
 * writes while that CPU is not using them. The MMU is off, so there are no caches to worry
 * about and no atomics to use.
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
        c->count[intid]++;
    } else {
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

/* ---- commands ---- */

/* Send an SGI to a CPU so many times, each time waiting until it has been taken */
static void crossfire(int target, u32 intid, u64 times)
{
    struct cpu *c = me();
    u64 val = sgi_to(target, intid);

    for (u64 i = 0; i < times; i++) {
        u32 before = cpus[target].count[intid];
        u64 end = now() + 2000 * ticks_per_ms;

        send_sgi(val);
        while (cpus[target].count[intid] == before) {
            if (now() > end) {
                c->cmd_failed = i + 1;
                return;
            }
        }
    }
}

/* What OP_CROSSFIRE is told */
static u64 crossfire_arg(int target, u32 intid, u64 times)
{
    return times | ((u64)target << 32) | ((u64)intid << 40);
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

/* ---- tests ---- */

/* The sender is its own target. These are all there is with a single CPU. */
static void tests_self(void)
{
    struct cpu *c = &cpus[0];

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

    /* More SGIs pending at once than there can be list registers, none of them lost */
    irq_mask();
    execute(OP_SEND_ALL, sgi_to(0, 0));
    delay_ms(5);
    irq_unmask();
    expect_all(0);
    check("self, 16 SGIs pending at once", 1000);
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

#define RING_ROUNDS 1000

/* Every CPU sends to the next one, all of them at the same time */
static void tests_ring(void)
{
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        start_command(cpu, OP_CROSSFIRE,
                      crossfire_arg((cpu + 1) % nr_cpus, SGI_RING, RING_ROUNDS));
    crossfire(1, SGI_RING, RING_ROUNDS);
    for (u64 cpu = 1; cpu < nr_cpus; cpu++)
        if (!wait_command(cpu, 20000))
            cpus[cpu].cmd_failed = ~0UL;
    for (u64 cpu = 0; cpu < nr_cpus; cpu++)
        expect(cpu, SGI_RING, RING_ROUNDS);
    check("every cpu sending to the next one, all at the same time", 1000);
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
        describe(slot);
    }
    if (nr_cpus > 1) {
        int ok = cpuon_report(nr_cpus, cpus[0].mpidr);

        line(ok ? "PASS " : "FAIL ");
        put("every CPU_ON took at most 10 ms, and CPU_ON of a CPU that is on says ALREADY_ON\n");
        if (ok)
            nr_pass++;
        else
            nr_fail++;
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

    if (nr_cpus > 2)
        tests_ring();

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
