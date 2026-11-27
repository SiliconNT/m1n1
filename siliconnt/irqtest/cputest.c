// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest that looks at every core before Windows does (SiliconNT R50):
 *
 *   - the identification registers of cpuregs.h as a guest sees them. The Windows kernel
 *     insists that some fields are the same on all cores (docs/findings/0097, conclusion 15);
 *     that is checked afterwards, from what this prints
 *   - whether a core that has just started, with its MMU off, sees what the boot core wrote
 *     before that: through a Device-nGnRnE mapping, which is how the kernel writes a mailbox
 *     (0097, conclusion 14), and through a write-back one without cache maintenance, which
 *     nothing depends on
 *   - the counters that a core reads before it has ever been in the hypervisor, next to those
 *     of the boot core: nobody has written CNTVOFF_EL2 by then (0098, conclusion 9)
 *   - how long a core takes from the boot core's PSCI CPU_ON to its first instruction (cpuon.h)
 *   - last, the CTRR registers of every core: an access that may end in the hypervisor
 *
 * The guest never uses the GIC: IRQs stay masked. Only the boot CPU prints, on the virtual
 * UART of the hypervisor; lines start with "HVLOG: " so that the hypervisor copies them to its
 * own console. Nothing here waits with WFI. Entry points, vectors and the header are those of
 * sgitest (sgitest-head.S).
 */

#define GUEST_NAME "cputest"

#include "gic.h"
#include "cpuon.h"
#include "sgitest.h"
#include "guest.h"

/* As in cputest-regs.S */
#define REG_NAME_SIZE 32

#define MAX_REGS     48
#define PAGE_WORDS   512
#define PATTERN_OLD  0x5555555555555555UL
#define PATTERN(cpu) (0xa5a5000000000000UL | ((u64)(cpu) << 32) | 0x12345678UL)

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_nr_cpus;

extern u32 reg_code[], reg_code_late[], reg_code_end[];
extern const char reg_names[], reg_names_late[];

void mmio_write32(u64 addr, u32 val);
void mailbox_write(u64 dev, u64 wb, u64 val, u64 words, u64 ttbr, u64 tcr, u64 mair);

#define isb() asm volatile("isb")

/* Commands for a CPU, see run_on() */
enum {
    OP_NONE,
    OP_LOOK, // look at its mailbox page written through write-back memory again
    OP_LATE, // read the late register number arg
};

/* What a core found in a page: words as they were, words as the boot core wrote them, others */
struct seen {
    u64 old;
    u64 new;
    u64 other;
};

/*
 * All of it is written by the CPU it belongs to, except the cmd_* fields, which the boot CPU
 * writes. The MMU is off, so there are no caches to worry about and no atomics to use.
 */
struct cpu {
    volatile u64 online;
    u64 mpidr;
    u64 vct, pct; // CNTVCT_EL0 and CNTPCT_EL0, the first two things it does
    struct seen dev, wb, wb_clean;

    volatile u64 cmd_seq;
    volatile u64 cmd_op;
    volatile u64 cmd_arg;
    volatile u64 cmd_ack;

    volatile u64 guard; // an undefined instruction is to be skipped
    volatile u64 esr;   // ...its ESR_EL1, with bit 63 set; 0 if there was none

    u64 val[MAX_REGS];
    u64 val_esr[MAX_REGS];
    volatile u64 late;
    volatile u64 late_esr;
};

static struct cpu cpus[MAX_CPUS];
static u64 page_dev[MAX_CPUS][PAGE_WORDS] __attribute__((aligned(4096)));
static u64 page_wb[MAX_CPUS][PAGE_WORDS] __attribute__((aligned(4096)));
static u64 table_l0[PAGE_WORDS] __attribute__((aligned(4096)));
static u64 table_l1[PAGE_WORDS] __attribute__((aligned(4096)));

/* Boot CPU only */
static u64 nr_cpus;
static u64 ticks_per_ms;
static u32 nr_regs, nr_late;
static u32 nr_pass, nr_fail;

static struct cpu *me(void)
{
    return &cpus[read_sysreg(tpidr_el1)];
}

/* ---- output ---- */

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

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
}

u64 sync_handler(u64 esr, u64 elr)
{
    struct cpu *c = me();

    if (!c->guard)
        return 0;
    c->esr = esr | (1UL << 63);
    return elr + 4;
}

/* ---- registers ---- */

/* Run one of the two-instruction functions of cputest-regs.S */
static u64 read_reg(const u32 *code, u64 *esr)
{
    struct cpu *c = me();
    u64 val;

    c->esr = 0;
    c->guard = 1;
    isb();
    val = ((u64(*)(void))code)();
    isb();
    c->guard = 0;
    *esr = c->esr;
    return *esr ? 0 : val;
}

static void read_regs(struct cpu *c)
{
    for (u32 i = 0; i < nr_regs; i++)
        c->val[i] = read_reg(&reg_code[2 * i], &c->val_esr[i]);
}

static void put_value(u64 val, u64 esr)
{
    if (esr) {
        put(" !esr=");
        put_hex(esr & 0xffffffff, 8);
    } else {
        put(" ");
        put_hex(val, 16);
    }
}

/* ---- the mailbox pages ---- */

static void look(const u64 *page, u64 expect, struct seen *s)
{
    s->old = s->new = s->other = 0;
    for (u32 i = 0; i < PAGE_WORDS; i++) {
        u64 v = ((const volatile u64 *)page)[i];

        if (v == expect)
            s->new++;
        else if (v == PATTERN_OLD)
            s->old++;
        else
            s->other++;
    }
}

/* Where the Device-nGnRnE mapping is, above the identity mapping of the image */
#define ALIAS_GB 64

/*
 * The image mapped to itself as write-back memory with blocks of 1 GB, and once more as
 * Device-nGnRnE, ALIAS_GB further up. Returns what TCR_EL1 is to be.
 */
static u64 build_tables(void)
{
    u64 base = (u64)page_dev;
    u64 gb = base >> 30;
    u64 parange = read_sysreg(id_aa64mmfr0_el1) & 0xf;

    if (parange > 5)
        parange = 5;
    for (u32 i = 0; i < PAGE_WORDS; i++)
        table_l0[i] = table_l1[i] = 0;
    for (u64 k = gb; k < gb + 2; k++) {
        /* Attribute 0, inner shareable, accessed; EL1 may do everything */
        table_l1[k & 511] = (k << 30) | (1UL << 10) | (3UL << 8) | 1;
        /* Attribute 1, accessed, never executable */
        table_l1[(k + ALIAS_GB) & 511] =
            (k << 30) | (3UL << 53) | (1UL << 10) | (1UL << 2) | 1;
    }
    table_l0[(base >> 39) & 511] = (u64)table_l1 | 3;
    /* T0SZ 16, write-back walks, inner shareable, no walks through TTBR1, IPS */
    return 16 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 23) | (parange << 32);
}

static void clean(const void *start, u64 size)
{
    for (u64 at = (u64)start & ~63UL; at < (u64)start + size; at += 64)
        asm volatile("dc civac, %0" : : "r"(at));
    asm volatile("dsb sy");
}

/* ---- CPUs ---- */

static void execute(struct cpu *c, u64 op, u64 arg)
{
    u64 esr;

    switch (op) {
        case OP_LOOK:
            look(page_wb[c - cpus], PATTERN(c - cpus), &c->wb_clean);
            break;
        case OP_LATE:
            c->late = read_reg(&reg_code_late[2 * arg], &esr);
            c->late_esr = esr;
            break;
    }
}

void secondary_main(int slot)
{
    struct cpu *c = &cpus[slot];

    isb();
    c->vct = read_sysreg(cntvct_el0);
    c->pct = read_sysreg(cntpct_el0);
    look(page_dev[slot], PATTERN(slot), &c->dev);
    look(page_wb[slot], PATTERN(slot), &c->wb);
    c->mpidr = read_sysreg(mpidr_el1);
    read_regs(c);
    c->online = 1;

    for (;;) {
        u64 seq = c->cmd_seq;

        if (seq != c->cmd_ack) {
            execute(c, c->cmd_op, c->cmd_arg);
            c->cmd_ack = seq;
        }
    }
}

/* Have a CPU do something and wait until it has; false if it does not answer */
static int run_on(int cpu, u64 op, u64 arg)
{
    struct cpu *c = &cpus[cpu];
    u64 end = now() + 10000 * ticks_per_ms;

    if (cpu == 0) {
        execute(c, op, arg);
        return 1;
    }
    c->cmd_op = op;
    c->cmd_arg = arg;
    c->cmd_seq++;
    while (c->cmd_ack != c->cmd_seq)
        if (now() > end)
            return 0;
    return 1;
}

static void put_seen(const char *name, const struct seen *s)
{
    put(name);
    put(" new old other = ");
    put_dec(s->new);
    put(" ");
    put_dec(s->old);
    put(" ");
    put_dec(s->other);
}

void boot_main(void)
{
    u64 tcr;
    int dev_ok = 1, clean_ok = 1;

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    nr_regs = (reg_code_late - reg_code) / 2;
    nr_late = (reg_code_end - reg_code_late) / 2;
    nr_cpus = hdr_nr_cpus;
    if (nr_cpus < 1 || nr_cpus > MAX_CPUS)
        nr_cpus = 1;

    line("start, cpus=");
    put_dec(nr_cpus);
    put(" registers=");
    put_dec(nr_regs);
    put("+");
    put_dec(nr_late);
    put("\n");
    if (nr_regs > MAX_REGS) {
        line("FAIL more registers than there is room for\n");
        nr_fail++;
        goto done;
    }

    cpus[0].vct = now();
    cpus[0].pct = read_sysreg(cntpct_el0);
    cpus[0].mpidr = read_sysreg(mpidr_el1);
    read_regs(&cpus[0]);
    cpus[0].online = 1;

    /*
     * Nothing that the loader wrote to these pages may come out of a cache later, over what is
     * written to memory here; and nothing of them may be left in the caches after that
     */
    clean(page_dev, sizeof(page_dev));
    clean(page_wb, sizeof(page_wb));
    clean(table_l0, sizeof(table_l0));
    clean(table_l1, sizeof(table_l1));
    for (u64 slot = 0; slot < MAX_CPUS; slot++)
        for (u32 i = 0; i < PAGE_WORDS; i++)
            page_dev[slot][i] = page_wb[slot][i] = PATTERN_OLD;
    tcr = build_tables();
    asm volatile("dsb sy");
    clean(page_dev, sizeof(page_dev));
    clean(page_wb, sizeof(page_wb));
    clean(table_l0, sizeof(table_l0));
    clean(table_l1, sizeof(table_l1));

    for (u64 slot = 1; slot < nr_cpus; slot++) {
        int ret;

        mailbox_write((u64)page_dev[slot] + ((u64)ALIAS_GB << 30), (u64)page_wb[slot],
                      PATTERN(slot), PAGE_WORDS, (u64)table_l0, tcr, 0x00ff);

        /* PSCI CPU_ON; the CPU then enters secondary_start */
        ret = cpuon_start(slot, &cpus[slot].online);
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
        clean(page_wb[slot], sizeof(page_wb[slot]));
        if (!run_on(slot, OP_LOOK, 0)) {
            line("FAIL cpu ");
            put_dec(slot);
            put(" does not answer\n");
            nr_fail++;
        }
    }

    for (u64 slot = 0; slot < nr_cpus; slot++) {
        struct cpu *c = &cpus[slot];

        line("cpu ");
        put_dec(slot);
        put(": MPIDR_EL1 = ");
        put_hex(c->mpidr, 16);
        put("\n");
        if (!slot)
            continue;

        line("cpu ");
        put_dec(slot);
        put(": ");
        put_seen("device", &c->dev);
        put(", ");
        put_seen("write-back", &c->wb);
        put(", ");
        put_seen("cleaned", &c->wb_clean);
        put("\n");
        if (c->dev.new != PAGE_WORDS)
            dev_ok = 0;
        if (c->wb_clean.new != PAGE_WORDS)
            clean_ok = 0;

        /* The difference is CNTVOFF_EL2 and a few ticks */
        line("cpu ");
        put_dec(slot);
        put(": at its entry CNTPCT - CNTVCT = ");
        put_hex(c->pct - c->vct, 16);
        put("\n");
    }
    line("boot cpu: CNTPCT - CNTVCT = ");
    put_hex(cpus[0].pct - cpus[0].vct, 16);
    put("\n");
    if (nr_cpus > 1) {
        result("every CPU_ON took at most 10 ms, and CPU_ON of a CPU that is on says ALREADY_ON",
               cpuon_report(nr_cpus, cpus[0].mpidr & 0xff00ffffffUL));
        result("every core sees what was written through a Device-nGnRnE mapping before its start",
               dev_ok);
        result("...and what was written through write-back memory, once it has been cleaned",
               clean_ok);
    }

    for (u32 i = 0; i < nr_regs; i++) {
        line("reg ");
        put(&reg_names[REG_NAME_SIZE * i]);
        put(" =");
        for (u64 slot = 0; slot < nr_cpus; slot++)
            put_value(cpus[slot].val[i], cpus[slot].val_esr[i]);
        put("\n");
    }

    for (u32 i = 0; i < nr_late; i++) {
        for (u64 slot = 0; slot < nr_cpus; slot++) {
            line("late ");
            put(&reg_names_late[REG_NAME_SIZE * i]);
            put(" cpu ");
            put_dec(slot);
            put(" ...\n");
            if (!run_on(slot, OP_LATE, i)) {
                line("FAIL it does not answer\n");
                nr_fail++;
                continue;
            }
            line("late ");
            put(&reg_names_late[REG_NAME_SIZE * i]);
            put(" cpu ");
            put_dec(slot);
            put(" =");
            put_value(cpus[slot].late, cpus[slot].late_esr);
            put("\n");
        }
    }

done:
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}
