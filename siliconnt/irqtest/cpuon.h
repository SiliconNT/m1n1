/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * How the guests of sgitest-head.S get onto their other CPUs: PSCI CPU_ON, as the Windows kernel
 * does it (HalpInterruptStartProcessor, SiliconNT docs/findings/0097, conclusion 11), of the
 * hypervisor (src/hv/hv_psci.c).
 *
 * The loader says for each slot which Redistributor frame is that CPU's (hdr_cpu_frame); the
 * MPIDR that CPU_ON is given is the affinity in that frame's GICR_TYPER, which is how the kernel
 * pairs a Redistributor with a processor. The CPU enters at hdr_cpu_entry of its slot, or, where
 * that is 0, at secondary_start of the image, with its slot in x0 (the context of the call).
 * secondary_start reads the virtual count first thing (cpu_first_insn): from the call to that
 * is how long CPU_ON took, as the CPU itself saw it. That count is the same on every CPU while
 * nothing has changed CNTVOFF_EL2 in between, which is the case without the host's part.
 *
 * Every function here prints, if at all, through put() of guest.h with lines that start with
 * "HVLOG: cpuon: ", so that each guest can use them with its own output.
 */

#ifndef CPUON_H
#define CPUON_H

#include "gic.h"
#include "guest.h"
#include "sgitest.h"

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_cpu_frame[MAX_CPUS];
extern u64 hdr_cpu_entry[MAX_CPUS];
extern volatile u64 cpu_first_insn[MAX_CPUS];
extern char image_start[];
extern char secondary_start[];

/* Arm DEN 0022 F.b 5.1, 5.2.2 */
#define CPUON_FN_CPU_ON  0xc4000003UL
#define CPUON_ALREADY_ON (-4)

/* The longest a CPU_ON may take, from the call to the first instruction (R51) */
#define CPUON_SLOW_MS 10
/* How long to wait for a CPU to say that it is there */
#define CPUON_WAIT_MS 20000

struct cpuon {
    u64 mpidr;
    int answer;     // of the CPU_ON that started it
    int again;      // of a second CPU_ON, once it was there
    u64 took;       // ticks from the call to its first instruction
    u64 arrived_in; // ticks from the call until it said it is there
};

static struct cpuon cpuon[MAX_CPUS];

static inline u64 cpuon_now(void)
{
    u64 v;

    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline u64 cpuon_ticks_per_ms(void)
{
    u64 v;

    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v / 1000;
}

static inline int cpuon_call(u64 mpidr, u64 entry, u64 context)
{
    register u64 x0 asm("x0") = CPUON_FN_CPU_ON;
    register u64 x1 asm("x1") = mpidr;
    register u64 x2 asm("x2") = entry;
    register u64 x3 asm("x3") = context;

    asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (int)x0;
}

/* The MPIDR fields of the affinity in GICR_TYPER of the slot's frame */
static inline u64 cpuon_mpidr(u64 slot)
{
    u64 typer = rd64(hdr_gicr_base + hdr_cpu_frame[slot] * GICR_STRIDE + GICR_TYPER);

    return ((typer >> 56) << 32) | ((typer >> 32) & 0xffffff);
}

static inline u64 cpuon_entry(u64 slot)
{
    return hdr_cpu_entry[slot] ? hdr_cpu_entry[slot] : (u64)secondary_start;
}

/*
 * CPU_ON of the CPU in a slot; then wait until *online is not 0. Returns the answer of the
 * call; 1 if it was 0 but the CPU did not say it was there in CPUON_WAIT_MS.
 */
static inline int cpuon_start(u64 slot, volatile u64 *online)
{
    struct cpuon *c = &cpuon[slot];
    u64 start, end;

    c->mpidr = cpuon_mpidr(slot);
    cpu_first_insn[slot] = 0;
    asm volatile("dsb sy" : : : "memory");
    start = cpuon_now();
    c->answer = cpuon_call(c->mpidr, cpuon_entry(slot), slot);
    if (c->answer)
        return c->answer;
    end = start + CPUON_WAIT_MS * cpuon_ticks_per_ms();
    while (!*online)
        if (cpuon_now() > end)
            return 1;
    c->arrived_in = cpuon_now() - start;
    asm volatile("dsb sy" : : : "memory");
    c->took = cpu_first_insn[slot] - start;
    return 0;
}

/*
 * Once the CPUs of slots 1 to nr_cpus - 1 are there: CPU_ON of each again, and of the caller,
 * must say ALREADY_ON. Prints one line per slot and the slowest. Returns whether every CPU
 * took at most CPUON_SLOW_MS and every second call said ALREADY_ON.
 */
static inline int cpuon_report(u64 nr_cpus, u64 self_mpidr)
{
    u64 per_ms = cpuon_ticks_per_ms(), slowest = 0, slowest_slot = 0;
    int ok = 1, self;

    for (u64 slot = 1; slot < nr_cpus; slot++) {
        struct cpuon *c = &cpuon[slot];

        c->again = cpuon_call(c->mpidr, cpuon_entry(slot), slot);
        if (c->took > slowest) {
            slowest = c->took;
            slowest_slot = slot;
        }
        if (c->took > CPUON_SLOW_MS * per_ms || c->again != CPUON_ALREADY_ON)
            ok = 0;
        put("HVLOG: cpuon: cpu ");
        put_dec(slot);
        put(" MPIDR ");
        put_hex(c->mpidr, 10);
        put(": CPU_ON ");
        put_dec(c->answer);
        put(", first instruction after ");
        put_dec(c->took * 1000 / per_ms);
        put(" us, there after ");
        put_dec(c->arrived_in * 1000 / per_ms);
        put(" us; CPU_ON again ");
        put_hex((u32)c->again, 8);
        put("\n");
    }
    self = cpuon_call(self_mpidr, cpuon_entry(0), 0);
    if (self != CPUON_ALREADY_ON)
        ok = 0;
    put("HVLOG: cpuon: slowest ");
    put_dec(slowest * 1000 / per_ms);
    put(" us (cpu ");
    put_dec(slowest_slot);
    put("), limit ");
    put_dec(CPUON_SLOW_MS * 1000);
    put(" us; CPU_ON of the caller ");
    put_hex((u32)self, 8);
    put("\n");
    return ok;
}

#endif
