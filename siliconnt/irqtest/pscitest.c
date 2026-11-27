// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for PSCI CPU_ON and CPU_OFF of src/hv/hv_psci.c, on every core: each other CPU
 * is turned on and turns itself off, over and over, and then the boot CPU turns itself off and
 * is turned on by another one.
 *
 * Bits 31:16 of the argument the loader leaves in the image header are how often each other
 * CPU, bits 15:9 how often the boot CPU (0: as often as the others). 0 in bits 31:16: nothing is
 * tested. The other CPUs are those that have a Redistributor, and are off when this starts; one
 * of them is on at a time. The first time each other CPU is on, CPU_ON of it again has to answer
 * ALREADY_ON. Before every CPU_ON the caller writes a page anew that the CPU reads when it comes,
 * as the parking page of the Windows kernel (SiliconNT R51).
 *
 * It ends with "DONE pass fail = " and spins; the loader stops it. It runs with interrupts
 * masked, and of the GIC and the timer it only reads GICR_TYPER and the counter. Entry points
 * and vectors are those of sgitest (sgitest-head.S).
 */

#define GUEST_NAME "pscitest"

#include "sgitest.h"
#include "gic.h" /* for u64 and u32 */
#include "guest.h"

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_arg;

#define ARG_BOOT_ROUNDS_SHIFT 9
#define ARG_BOOT_ROUNDS_MASK  0x7f
#define ARG_ROUNDS_SHIFT      16
#define ARG_ROUNDS_MASK       0xffff

/* sgitest-head.S: where the image is, and the word of its header that says where a CPU enters */
extern char image_start[];
#define HDR_SECONDARY_OFF 0x20

/* Arm DEN 0022 F.b 5.1, 5.2.2, and the fields of MPIDR_EL1 that the target_cpu of CPU_ON has */
#define FN_CPU_OFF     0x84000002UL
#define FN_CPU_ON      0xc4000003UL
#define ALREADY_ON     (-4)
#define ON_PENDING     (-5)
#define MPIDR_AFFINITY 0xff00ffffffUL

static u32 nr_pass, nr_fail;

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

/* ---- exception handlers, called from sgitest-head.S ---- */

/* Interrupts stay masked and nothing enables one */
void irq_handler(void)
{
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

/*
 * ---- CPUs on and off ----
 *
 * The MMU is off: every CPU sees what another has written. A CPU that is turned on gets the
 * context of the call in x0, which sgitest-head.S takes for its slot, that is for which stack it
 * runs on.
 *
 * Before each CPU_ON the caller writes the parking page anew, as the Windows kernel writes the
 * parking page of a processor before it starts it (HalpInterruptSetProcessorStartContext,
 * docs/findings/0097, conclusion 11): through a mapping that is not cached, which with the MMU
 * off is every access. The CPU that comes reads all of it, first thing, before it says it came.
 */

#define SLOT_OTHER 1 /* the one other CPU that is on */
#define SLOT_BOOT  2 /* the boot CPU, after it was off */

#define PARK_WORDS 512

static u32 rounds;      /* how often each other CPU */
static u32 boot_target; /* ...and the boot CPU */
static u64 boot_mpidr;  /* the CPU this started on */
static u64 other_mpidr, nr_others;
static u32 boot_rounds;
static int boot_bad;
static u32 boot_parking_bad;
static u32 parking_bad, already_bad;

/* What the other CPU found when it came: written by it, and the count last */
static volatile u32 came;
static volatile u64 came_mpidr;
static volatile u32 came_parking_bad;
/* What it is to do before it turns itself off again */
static volatile u32 wake_boot;
/* While this is 1, it does not turn itself off */
static volatile u32 hold;

static volatile u64 parking[PARK_WORDS] __attribute__((aligned(4096)));
static volatile u64 parking_tag;

static void barrier(void)
{
    asm volatile("dsb sy; isb" : : : "memory");
}

static int psci(u64 fn, u64 arg1, u64 arg2, u64 arg3)
{
    register u64 x0 asm("x0") = fn;
    register u64 x1 asm("x1") = arg1;
    register u64 x2 asm("x2") = arg2;
    register u64 x3 asm("x3") = arg3;

    asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return (int)x0;
}

static u64 ms_since(u64 start)
{
    barrier();
    return (read_sysreg(cntvct_el0) - start) / (read_sysreg(cntfrq_el0) / 1000);
}

static u64 parking_word(u64 tag, u32 i)
{
    return (tag << 20) ^ (0x9e3779b97f4a7c15UL * (i + 1));
}

/* The page anew, then the tag that says what it holds */
static void parking_fill(void)
{
    u64 tag = parking_tag + 1;

    for (u32 i = 0; i < PARK_WORDS; i++)
        parking[i] = parking_word(tag, i);
    barrier();
    parking_tag = tag;
    barrier();
}

/* How many words of the page are not what the tag says */
static u32 parking_check(void)
{
    u64 tag;
    u32 bad = 0;

    barrier();
    tag = parking_tag;
    for (u32 i = 0; i < PARK_WORDS; i++)
        if (parking[i] != parking_word(tag, i))
            bad++;
    return bad;
}

static u64 entry_point(void)
{
    return (u64)image_start + *(u64 *)(image_start + HDR_SECONDARY_OFF);
}

/* CPU_ON, over again while the CPU is still on its way out. The answer, as w0 has it */
static int turn_on(u64 mpidr, u64 slot)
{
    u64 start = read_sysreg(cntvct_el0);
    int ret;

    parking_fill();
    do {
        ret = psci(FN_CPU_ON, mpidr, entry_point(), slot);
    } while ((ret == ALREADY_ON || ret == ON_PENDING) && ms_since(start) < 1000);
    return ret;
}

static int came_again(u32 before)
{
    u64 start = read_sysreg(cntvct_el0);

    while (came == before)
        if (ms_since(start) >= 1000)
            return 0;
    barrier();
    return 1;
}

static void on_off(u64 mpidr)
{
    u32 n, done = 0, wrong_page = 0;
    int ret = 0, bad = 0, again = 0;

    wake_boot = 0;
    for (n = 0; n < rounds; n++) {
        u32 before = came;

        /* The first time it stays until it has been asked to turn on again */
        hold = n == 0;
        barrier();
        ret = turn_on(mpidr, SLOT_OTHER);
        if (ret || !came_again(before)) {
            hold = 0;
            break;
        }
        if (n == 0) {
            again = psci(FN_CPU_ON, mpidr, entry_point(), SLOT_OTHER);
            if (again != ALREADY_ON)
                already_bad++;
            barrier();
            hold = 0;
            barrier();
        }
        if (came != before + 1 || came_mpidr != mpidr)
            bad++;
        if (came_parking_bad)
            wrong_page++;
        done++;
    }
    parking_bad += wrong_page;

    result("a CPU was turned on, and turned itself off", done == rounds && !bad);
    line("     MPIDR ");
    put_hex(mpidr, 10);
    put(": ");
    put_dec(done);
    put(" of ");
    put_dec(rounds);
    put(" times, ");
    put_dec(bad);
    put(" wrong, the parking page wrong ");
    put_dec(wrong_page);
    put(" times; the last answer ");
    put_hex((u32)ret, 8);
    put(", CPU_ON while it was on ");
    put_hex((u32)again, 8);
    put("\n");
}

/* Each CPU that has a Redistributor, but this one. Afterwards none of them is on */
static void test_on_off(void)
{
    u64 gicr = hdr_gicr_base, typer;

    do {
        u64 mpidr;

        typer = rd64(gicr + GICR_TYPER);
        mpidr = ((typer >> 56) << 32) | ((typer >> 32) & 0xffffff);
        if (mpidr != boot_mpidr) {
            on_off(mpidr);
            other_mpidr = mpidr;
            nr_others++;
        }
        gicr += GICR_STRIDE;
    } while (!(typer & (1 << 4))); /* Last */

    if (nr_others) {
        result("every CPU that was turned on read the parking page as it had been written",
               !parking_bad);
        result("CPU_ON of another CPU that is on answers ALREADY_ON", !already_bad);
    }
}

static void finish(void)
{
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
}

static void boot_off_result(int ok)
{
    result("the boot CPU turned itself off, and was turned on", ok);
    line("     ");
    put_dec(boot_rounds);
    put(" of ");
    put_dec(boot_target);
    put(" times, ");
    put_dec(boot_bad);
    put(" wrong, the parking page wrong ");
    put_dec(boot_parking_bad);
    put(" times\n");
    finish();
}

/*
 * The boot CPU turns itself off, and the other one turns it on. It goes on in secondary_main(),
 * on the stack of another slot.
 */
static void boot_off(void)
{
    u32 before = came;

    wake_boot = 1;
    hold = 0;
    if (turn_on(other_mpidr, SLOT_OTHER) || !came_again(before)) {
        boot_off_result(0);
        return;
    }
    psci(FN_CPU_OFF, 0, 0, 0);
    line("FAIL CPU_OFF of the boot CPU came back\n");
    nr_fail++;
    finish();
}

static void boot_back(void)
{
    u32 wrong = parking_check();

    boot_rounds++;
    if (wrong)
        boot_parking_bad++;
    if ((read_sysreg(mpidr_el1) & MPIDR_AFFINITY) != boot_mpidr || wrong)
        boot_bad++;
    if (boot_rounds < boot_target)
        boot_off();
    else
        boot_off_result(!boot_bad);
}

void secondary_main(int slot)
{
    int ret;
    u32 wrong;

    if (slot == SLOT_BOOT) {
        boot_back();
        return;
    }

    wrong = parking_check();
    came_mpidr = read_sysreg(mpidr_el1) & MPIDR_AFFINITY;
    came_parking_bad = wrong;
    barrier();
    came = came + 1;
    barrier();

    while (hold)
        barrier();

    if (wake_boot) {
        ret = turn_on(boot_mpidr, SLOT_BOOT);
        if (ret) {
            line("FAIL the boot CPU could not be turned on: ");
            put_hex((u32)ret, 8);
            put("\n");
            return;
        }
    }

    psci(FN_CPU_OFF, 0, 0, 0);
    line("FAIL CPU_OFF came back\n");
}

void boot_main(void)
{
    rounds = (hdr_arg >> ARG_ROUNDS_SHIFT) & ARG_ROUNDS_MASK;
    boot_target = (hdr_arg >> ARG_BOOT_ROUNDS_SHIFT) & ARG_BOOT_ROUNDS_MASK;
    if (!boot_target)
        boot_target = rounds;
    boot_mpidr = read_sysreg(mpidr_el1) & MPIDR_AFFINITY;

    line("start, arg=");
    put_hex(hdr_arg, 8);
    put("\n");

    if (rounds)
        test_on_off();

    if (nr_others && boot_target)
        boot_off(); /* goes on elsewhere */
    else
        finish();
}
