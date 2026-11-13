// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the Distributor and Redistributor emulation of src/hv/hv_vgic.c: do the
 * registers the Windows kernel reads and writes (ntoskrnl.exe 10.0.26300.9457, see gic.h)
 * behave like those of a GICv3? An interrupt that is not enabled must stay pending and arrive
 * once it is, with the priority the guest gave it; an SPI that the guest requests by writing
 * GICD_ISPENDR must arrive at the CPU GICD_IROUTER names.
 *
 * The same image runs on QEMU's virt machine with an emulated GICv3 (`make qemu-gic`,
 * `make qemu-gic-smp`), which is where the expected results come from.
 *
 * Only the boot CPU prints, on the hypervisor's virtual UART; lines start with "HVLOG: " so
 * that the hypervisor copies them to its own console. Nothing here waits with WFI. Entry
 * points and vectors are those of sgitest (sgitest-head.S).
 */

#include "sgitest.h"
#include "gic.h"

/* Interrupts that are counted one by one: SGIs, PPIs and the first SPIs */
#define NR_COUNTED 64

#define PPI_VTIMER 27
#define SGI_A      1
#define SGI_LOW    2 /* gets a low priority in one test */
#define SGI_HIGH   3
#define SPI_A      40
#define SPI_B      41
#define SPI_REG    44 /* only its registers are used */

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
#define irq_mask()         asm volatile("msr daifset, #2")
/* SError, IRQ and FIQ on. The FIQ must never come: the vector reports it */
#define irq_unmask() asm volatile("msr daifclr, #7")

/* Commands for a CPU, see run_on() */
enum {
    OP_NONE,
    OP_SEND,   // write arg to ICC_SGI1R_EL1
    OP_WR32,   // write the 32 bits of arg2 to the address arg
    OP_MASK,   // mask IRQs
    OP_UNMASK, // unmask IRQs
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
    volatile u64 cmd_arg;
    volatile u64 cmd_arg2;
    volatile u64 cmd_ack;

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
    put("HVLOG: gictest: ");
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

/* ---- CPUs ---- */

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

static void execute(u64 op, u64 arg, u64 arg2)
{
    switch (op) {
        case OP_SEND:
            send_sgi(arg);
            break;
        case OP_WR32:
            wr32(arg, arg2);
            break;
        case OP_MASK:
            irq_mask();
            break;
        case OP_UNMASK:
            irq_unmask();
            break;
    }
}

/* The order and the values of HalpGic3InitializeLocalUnit, then the SGIs the tests use */
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
        gic_enable_local(c->gicr, SGI_LOW, 0x80);
        gic_enable_local(c->gicr, SGI_HIGH, 0x80);
    }

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
            execute(c->cmd_op, c->cmd_arg, c->cmd_arg2);
            c->cmd_ack = seq;
        }
    }
}

/* Have a CPU do something and wait until it has */
static void run_on(int cpu, u64 op, u64 arg, u64 arg2)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 2000 * ticks_per_ms;

    if (cpu == 0) {
        execute(op, arg, arg2);
        return;
    }

    c->cmd_op = op;
    c->cmd_arg = arg;
    c->cmd_arg2 = arg2;
    c->cmd_seq++;
    while (c->cmd_ack != c->cmd_seq) {
        if (now() > end) {
            line("FAIL cpu ");
            put_dec(cpu);
            put(" does not answer\n");
            nr_fail++;
            return;
        }
    }
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
    delay_ms(20);
    return took(cpu, intid);
}

/* How long to wait for an interrupt that must not come */
#define QUIET_MS 30
#define SOON_MS  500

static void request_spi(u32 intid)
{
    wr32(hdr_gicd_base + GICD_ISPENDR + 4 * (intid / 32), 1U << (intid % 32));
}

static int spi_pending(u32 intid)
{
    return (rd32(hdr_gicd_base + GICD_ISPENDR + 4 * (intid / 32)) >> (intid % 32)) & 1;
}

static void arm_timer(void)
{
    write_sysreg(cntv_cval_el0, now());
    write_sysreg(cntv_ctl_el0, 1); /* ENABLE, not masked: the output is high at once */
    isb();
}

/* ---- tests on the boot CPU ---- */

static void tests_registers(void)
{
    u64 gicd = hdr_gicd_base, gicr = cpus[0].gicr;
    u32 typer = rd32(gicd + GICD_TYPER);
    u32 lines = ((typer & 0x1f) + 1) * 32;
    int ok;

    line("GICD CTLR TYPER PIDR2, GICR TYPER = ");
    put_hex(rd32(gicd + GICD_CTLR), 8);
    put(" ");
    put_hex(typer, 8);
    put(" ");
    put_hex(rd32(gicd + GICD_PIDR2), 8);
    put(", ");
    put_hex(rd64(gicr + GICR_TYPER), 16);
    put("\n");

    /* ARE, DS and EnableGrp1 */
    result("Distributor: affinity routing, one Security state, Group 1 enabled", 0,
           (rd32(gicd + GICD_CTLR) & 0x52) == 0x52);

    ok = 1;
    for (u32 n = 1; n < lines / 32; n++)
        if (rd32(gicd + GICD_ISENABLER + 4 * n))
            ok = 0;
    result("Distributor: every SPI is disabled after the initialisation", 0, ok);

    /* SGIs 8-15 are whatever the firmware left */
    result("Redistributor: only the three SGIs of this test are enabled", 0,
           (rd32(gicr + GICR_ISENABLER0) & ~0xff00U) ==
               ((1 << SGI_A) | (1 << SGI_LOW) | (1 << SGI_HIGH)));

    wr32(gicr + GICR_IPRIORITYR + 4, 0x70605040);
    wr8(gicr + GICR_IPRIORITYR + 5, 0xa0);
    result("GICR_IPRIORITYR: one byte written, the other three keep their values", 0,
           rd32(gicr + GICR_IPRIORITYR + 4) == 0x7060a040 && rd8(gicr + GICR_IPRIORITYR + 6) == 0x60);

    wr32(gicd + GICD_IPRIORITYR + SPI_REG, 0x70605040);
    wr8(gicd + GICD_IPRIORITYR + SPI_REG + 1, 0xa0);
    result("GICD_IPRIORITYR: one byte written, the other three keep their values", 0,
           rd32(gicd + GICD_IPRIORITYR + SPI_REG) == 0x7060a040 &&
               rd8(gicd + GICD_IPRIORITYR + SPI_REG + 2) == 0x60);

    wr64(gicd + GICD_IROUTER + 8 * SPI_REG, gic_affinity(cpus[0].mpidr));
    result("GICD_IROUTER: written and read as 64 bits and as two halves", 0,
           rd64(gicd + GICD_IROUTER + 8 * SPI_REG) == gic_affinity(cpus[0].mpidr) &&
               rd32(gicd + GICD_IROUTER + 8 * SPI_REG) == (u32)gic_affinity(cpus[0].mpidr) &&
               rd32(gicd + GICD_IROUTER + 8 * SPI_REG + 4) == 0);

    {
        u64 cfg = gicd + GICD_ICFGR + 4 * (SPI_REG / 16);
        u32 bit = 2U << (2 * (SPI_REG % 16));
        u32 before = rd32(cfg);

        wr32(cfg, before | bit);
        ok = (rd32(cfg) & bit) != 0;
        wr32(cfg, before & ~bit);
        result("GICD_ICFGR: the trigger of one SPI set to edge and back to level", 0,
               ok && !(rd32(cfg) & bit));
    }

    /*
     * What a crash dump reads (HalpGicdReadableRanges, HalpGicrReadableRanges). An access that
     * the machine does not answer ends in bad_exception().
     */
    {
        static const u32 gicd_ranges[][2] = {{0x0, 0x10},      {0x80, 0xf00},    {0xf10, 0xf30},
                                             {0x1000, 0x3700}, {0x6100, 0xa000}, {0xffd0, 0x10000}};
        static const u32 gicr_ranges[][2] = {{0x0, 0x20},       {0x70, 0x80},
                                             {0xc0, 0xc8},      {0xffd0, 0x10000},
                                             {0x10080, 0x10e00}};
        u32 sum = 0;

        for (u32 i = 0; i < sizeof(gicd_ranges) / sizeof(gicd_ranges[0]); i++)
            for (u32 off = gicd_ranges[i][0]; off < gicd_ranges[i][1]; off += 4)
                sum += rd32(gicd + off);
        for (u32 i = 0; i < sizeof(gicr_ranges) / sizeof(gicr_ranges[0]); i++)
            for (u32 off = gicr_ranges[i][0]; off < gicr_ranges[i][1]; off += 4)
                sum += rd32(gicr + off);
        (void)sum;
        result("every register a crash dump reads can be read", 0, 1);
    }
}

static void tests_delivery(void)
{
    u64 gicd = hdr_gicd_base, gicr = cpus[0].gicr;
    u32 n, m, ctlr;
    int pending;

    /* An SGI and its enable bit */
    wr32(gicr + GICR_ICENABLER0, 1 << SGI_A);
    send_sgi(sgi_to(0, SGI_A));
    n = wait_took(0, SGI_A, QUIET_MS);
    pending = (rd32(gicr + GICR_ISPENDR0) >> SGI_A) & 1;
    result("SGI not enabled: not delivered, reads as pending", 0, n == 0 && pending);
    wr32(gicr + GICR_ISENABLER0, 1 << SGI_A);
    n = wait_took(0, SGI_A, SOON_MS);
    pending = (rd32(gicr + GICR_ISPENDR0) >> SGI_A) & 1;
    result("SGI enabled: delivered once, no longer pending", 0, n == 1 && !pending);

    /* Two priorities and the priority mask of the CPU interface */
    wr8(gicr + GICR_IPRIORITYR + SGI_LOW, 0xa0);
    wr8(gicr + GICR_IPRIORITYR + SGI_HIGH, 0x40);
    write_sysreg(ICC_PMR_EL1, 0x80);
    isb();
    send_sgi(sgi_to(0, SGI_LOW));
    send_sgi(sgi_to(0, SGI_HIGH));
    n = wait_took(0, SGI_HIGH, SOON_MS);
    m = took(0, SGI_LOW);
    result("priority mask 0x80: the SGI with priority 0x40 is taken, the one with 0xa0 waits", 0,
           n == 1 && m == 0);
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();
    m = wait_took(0, SGI_LOW, SOON_MS);
    result("priority mask 0xf0: the SGI with priority 0xa0 is taken", 0, m == 1);
    wr8(gicr + GICR_IPRIORITYR + SGI_LOW, 0x80);
    wr8(gicr + GICR_IPRIORITYR + SGI_HIGH, 0x80);

    /* The Group 1 enable of the Distributor */
    ctlr = rd32(gicd + GICD_CTLR);
    wr32(gicd + GICD_CTLR, ctlr & ~2U);
    send_sgi(sgi_to(0, SGI_A));
    n = wait_took(0, SGI_A, QUIET_MS);
    result("GICD_CTLR.EnableGrp1 clear: SGI not delivered", 0, n == 0);
    wr32(gicd + GICD_CTLR, ctlr);
    n = wait_took(0, SGI_A, SOON_MS);
    result("GICD_CTLR.EnableGrp1 set again: delivered once", 0, n == 1);

    /* An SPI that software requests, as HalpGic3RequestInterrupt does above INTID 15 */
    gic_enable_spi(SPI_A, 0x80, cpus[0].mpidr, 1);
    request_spi(SPI_A);
    n = wait_took(0, SPI_A, SOON_MS);
    result("SPI requested through GICD_ISPENDR: delivered once, no longer pending", 0,
           n == 1 && !spi_pending(SPI_A));

    wr32(gicd + GICD_ICENABLER + 4 * (SPI_A / 32), 1U << (SPI_A % 32));
    request_spi(SPI_A);
    n = wait_took(0, SPI_A, QUIET_MS);
    result("SPI not enabled: not delivered, reads as pending", 0, n == 0 && spi_pending(SPI_A));
    wr32(gicd + GICD_ISENABLER + 4 * (SPI_A / 32), 1U << (SPI_A % 32));
    n = wait_took(0, SPI_A, SOON_MS);
    result("SPI enabled: delivered once", 0, n == 1);

    irq_mask();
    request_spi(SPI_A);
    request_spi(SPI_A);
    pending = spi_pending(SPI_A);
    irq_unmask();
    n = wait_took(0, SPI_A, SOON_MS);
    result("SPI requested twice while IRQs are masked: reads as pending, delivered once", 0,
           pending && n == 1);

    /* The virtual timer, a level-sensitive PPI; the handler turns the timer off */
    arm_timer();
    n = wait_took(0, PPI_VTIMER, QUIET_MS);
    pending = (rd32(gicr + GICR_ISPENDR0) >> PPI_VTIMER) & 1;
    result("timer PPI not enabled: not delivered, reads as pending", 0, n == 0 && pending);
    gic_enable_local(gicr, PPI_VTIMER, 0x80);
    n = wait_took(0, PPI_VTIMER, SOON_MS);
    result("timer PPI enabled: delivered once", 0, n == 1);
    m = 0;
    for (int i = 0; i < 20; i++) {
        arm_timer();
        m += wait_took(0, PPI_VTIMER, SOON_MS);
    }
    result("timer armed 20 more times: 20 interrupts", 0, m == 20);
    wr32(gicr + GICR_ICENABLER0, 1U << PPI_VTIMER);
    arm_timer();
    delay_ms(QUIET_MS);
    write_sysreg(cntv_ctl_el0, 0);
    isb();
    wr32(gicr + GICR_ISENABLER0, 1U << PPI_VTIMER);
    n = wait_took(0, PPI_VTIMER, QUIET_MS);
    pending = (rd32(gicr + GICR_ISPENDR0) >> PPI_VTIMER) & 1;
    result("timer went high and low again while its PPI was disabled: nothing delivered", 0,
           n == 0 && !pending);
}

/* ---- tests with a second CPU ---- */

static void tests_pair(int b)
{
    u64 gicd = hdr_gicd_base;
    u32 n, m;

    gic_enable_spi(SPI_B, 0x80, cpus[b].mpidr, 1);
    request_spi(SPI_B);
    n = wait_took(b, SPI_B, SOON_MS);
    m = took(0, SPI_B);
    result("SPI routed to cpu #, requested on cpu 0: delivered on cpu # only", b, n == 1 && m == 0);

    run_on(b, OP_WR32, gicd + GICD_ISPENDR + 4 * (SPI_A / 32), 1U << (SPI_A % 32));
    n = wait_took(0, SPI_A, SOON_MS);
    m = took(b, SPI_A);
    result("SPI routed to cpu 0, requested on cpu #: delivered on cpu 0 only", b, n == 1 && m == 0);

    /* One CPU writes the Redistributor of another */
    wr32(cpus[b].gicr + GICR_ICENABLER0, 1 << SGI_A);
    send_sgi(sgi_to(b, SGI_A));
    n = wait_took(b, SGI_A, QUIET_MS);
    wr32(cpus[b].gicr + GICR_ISENABLER0, 1 << SGI_A);
    m = wait_took(b, SGI_A, SOON_MS);
    result("SGI of cpu # disabled and enabled by cpu 0: delivered once it is enabled", b,
           n == 0 && m == 1);

    /* A pending SPI follows its route */
    run_on(b, OP_MASK, 0, 0);
    request_spi(SPI_B);
    delay_ms(5);
    wr64(gicd + GICD_IROUTER + 8 * SPI_B, gic_affinity(cpus[0].mpidr));
    n = wait_took(0, SPI_B, SOON_MS);
    run_on(b, OP_UNMASK, 0, 0);
    m = wait_took(b, SPI_B, QUIET_MS);
    result("SPI rerouted to cpu 0 while cpu # had IRQs masked: delivered on cpu 0 only", b,
           n == 1 && m == 0);
    wr32(gicd + GICD_ICENABLER + 4 * (SPI_B / 32), 1U << (SPI_B % 32));
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

    tests_registers();
    tests_delivery();
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

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
