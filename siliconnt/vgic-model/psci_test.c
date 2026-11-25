// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_psci.c. The file under test is the real one, compiled
 * against the stand-in headers of this directory; this file plays the guest (the registers of
 * an hvc), the host of the hypervisor (hv_exc_proxy(), which is where a guest that is not to
 * run again ends up) and the cores of the machine (src/smp.c, and hv_start_secondary() and
 * hv_cpu_off() of src/hv/hv.c).
 *
 * What the answers have to be comes from what the Windows kernel does with them
 * (10.0.26300.9457): it reads PSCI_VERSION and takes six functions for granted on 1.1, asks
 * PSCI_FEATURES about the others and goes without those that answer with a negative number,
 * and stops with a bug check if SYSTEM_RESET, SYSTEM_OFF or CPU_OFF return, or if CPU_ON
 * returns anything but 0, ALREADY_ON or ON_PENDING.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hv.h"
#include "smp.h"

#define NOT_SUPPORTED ((u64)-1)
#define ALREADY_ON    ((u64)-4)

/* ---- the host of the hypervisor ---- */

static jmp_buf left_guest;
static int proxy_calls;
static int proxy_handled; /* how often the host answers "handled" before it leaves the guest */
static u32 proxy_event;
static uartproxy_boot_reason_t proxy_reason;
static struct exc_info proxy_ctx;

/* Returns when the host says the event is handled; does not return when it leaves the guest */
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra)
{
    UNUSED(extra);
    proxy_calls++;
    proxy_reason = reason;
    proxy_event = type;
    proxy_ctx = *ctx;
    if (proxy_calls > proxy_handled)
        longjmp(left_guest, 1);
}

/* ---- the cores: an M4, as src/smp.c has it once hv_init() has started them all ---- */

#define M4_BOOT 6
static const u64 m4_mpidr[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x10100, 0x10101, 0x10102, 0x10103};
#define M4_CPUS ((int)(sizeof(m4_mpidr) / sizeof(m4_mpidr[0])))

int boot_cpu_idx = M4_BOOT;
bool hv_started_cpus[MAX_CPUS];
static bool absent_cpu[MAX_CPUS]; /* a core of the M4 that does not run m1n1 */

static int starts; /* hv_start_secondary() calls, and the last of them */
static bool start_works;
static int start_cpu;
static void *start_entry;
static u64 start_regs[4];

/* The boot core is not one that smp.c has started, so that says nothing of itself */
bool smp_is_alive(int cpu)
{
    return cpu < M4_CPUS && cpu != boot_cpu_idx && !absent_cpu[cpu];
}

/* A core that is not there reads as 0, which is the MPIDR of one that is */
uint64_t smp_get_mpidr(int cpu)
{
    return cpu < M4_CPUS && !absent_cpu[cpu] ? m4_mpidr[cpu] : 0;
}

bool hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    starts++;
    start_cpu = cpu;
    start_entry = entry;
    memcpy(start_regs, regs, sizeof(start_regs));
    if (start_works)
        hv_started_cpus[cpu] = true;
    return start_works;
}

static int offs; /* hv_cpu_off() calls, and what the last of them was given */
static struct exc_info off_ctx;

/* Does not return, but on the last core the guest runs on */
void hv_cpu_off(struct exc_info *ctx)
{
    offs++;
    off_ctx = *ctx;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++)
        if (hv_started_cpus[cpu] && cpu != (int)ctx->cpu_id) {
            hv_started_cpus[ctx->cpu_id] = false;
            longjmp(left_guest, 2);
        }
}

static void reset_cpus(void)
{
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    memset(absent_cpu, 0, sizeof(absent_cpu));
    hv_started_cpus[boot_cpu_idx] = true;
    starts = 0;
    start_works = true;
    offs = 0;
}

/* ---- the guest ---- */

enum outcome {
    RETURNED,  /* the guest goes on behind the hvc */
    STOPPED,   /* the guest was handed to the host and never ran again */
    OFF,       /* the core has left the guest */
    UNHANDLED, /* hv_psci_hvc() said the hvc is not its business */
};

static struct exc_info before, ctx;

/* x1 to x30 as the guest left them: no two alike, none that could be mistaken for an answer */
static void fill(u64 x0, u64 x1)
{
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < 31; i++)
        ctx.regs[i] = 0x5a5a000000000000UL | ((u64)i << 32) | (0x1000 + i);
    ctx.regs[0] = x0;
    ctx.regs[1] = x1;
    ctx.cpu_id = 6;
    before = ctx;
}

static enum outcome hvc(u16 imm)
{
    proxy_calls = 0;
    switch (setjmp(left_guest)) {
        case 0:
            return hv_psci_hvc(&ctx, imm) ? RETURNED : UNHANDLED;
        case 1:
            return STOPPED;
        default:
            return OFF;
    }
}

static enum outcome psci(u64 x0, u64 x1)
{
    fill(x0, x1);
    return hvc(0);
}

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* The call came back with this in x0 and with nothing else changed */
#define CHECK_ANSWER(outcome, x0)                                                                  \
    do {                                                                                           \
        CHECK((outcome) == RETURNED);                                                              \
        CHECK(ctx.regs[0] == (u64)(x0));                                                           \
        CHECK(!memcmp(&ctx.regs[1], &before.regs[1], sizeof(ctx.regs) - sizeof(ctx.regs[0])));     \
        CHECK(proxy_calls == 0);                                                                   \
    } while (0)

/* The guest never ran again, and the host got to see the registers of the call untouched */
#define CHECK_STOPPED(outcome, event)                                                              \
    do {                                                                                           \
        CHECK((outcome) == STOPPED);                                                               \
        CHECK(proxy_reason == START_HV);                                                           \
        CHECK(proxy_event == (event));                                                             \
        CHECK(!memcmp(&proxy_ctx, &before, sizeof(before)));                                       \
    } while (0)

/* ---- function IDs: Arm DEN 0022 F.b 5.1, DEN 0028 1.7 H 7.2 to 7.6 ---- */

#define FN_PSCI_VERSION  0x84000000
#define FN_CPU_OFF       0x84000002
#define FN_CPU_ON        0xc4000003
#define FN_SYSTEM_OFF    0x84000008
#define FN_SYSTEM_RESET  0x84000009
#define FN_PSCI_FEATURES 0x8400000a

/* The six the hypervisor says it has */
static const u32 implemented[] = {
    FN_PSCI_VERSION, FN_CPU_OFF, FN_CPU_ON, FN_SYSTEM_OFF, FN_SYSTEM_RESET, FN_PSCI_FEATURES,
};

/*
 * Everything else. The first eleven are what the Windows kernel asks PSCI_FEATURES about on
 * PSCI 1.1 and 1.3 (PsciFunctionCodes, state 1 in PsciPlatformInfo_v1_1 and _v1_3).
 */
static const u32 absent[] = {
    0xc4000001, /* CPU_SUSPEND */
    0x8400000b, /* CPU_FREEZE */
    0xc400000c, /* CPU_DEFAULT_SUSPEND */
    0x8400000f, /* PSCI_SET_SUSPEND_MODE */
    0xc4000010, /* PSCI_STAT_RESIDENCY */
    0xc4000011, /* PSCI_STAT_COUNT */
    0xc4000012, /* SYSTEM_RESET2: the firmware asks about it before a warm reset */
    0x84000013, /* MEM_PROTECT */
    0xc4000014, /* MEM_PROTECT_CHECK_RANGE */
    0x80000000, /* SMCCC_VERSION: "no" ends the kernel's search for firmware mitigations */
    0xc4000015, /* SYSTEM_OFF2 */
    0x84000001, /* CPU_SUSPEND, SMC32 */
    0x84000003, /* CPU_ON, SMC32: Windows calls the SMC64 one */
    0x84000004, /* AFFINITY_INFO */
    0xc4000004,
    0x84000005, /* MIGRATE */
    0x84000006, /* MIGRATE_INFO_TYPE */
    0x84000012, /* SYSTEM_RESET2, SMC32 */
    0x80000001, /* SMCCC_ARCH_FEATURES */
    0x80008000, /* SMCCC_ARCH_WORKAROUND_1 */
    0x80007fff, /* SMCCC_ARCH_WORKAROUND_2 */
    0x86000000, /* a vendor specific hypervisor service */
    0xc4000000, /* PSCI_VERSION has no SMC64 form */
    0x00000000,
    0xffffffff,
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void version_tests(void)
{
    /* Major version in bits 31:16, minor in bits 15:0 (5.3): 1.1 */
    CHECK_ANSWER(psci(FN_PSCI_VERSION, 0), 0x00010001);

    /* The function ID is W0 (DEN 0028 2.6): what is above it does not count */
    CHECK_ANSWER(psci(0xdeadbeef00000000UL | FN_PSCI_VERSION, 0), 0x00010001);

    /* The kernel asks many times, and gets the same answer every time */
    for (int i = 0; i < 100; i++)
        CHECK_ANSWER(psci(FN_PSCI_VERSION, i), 0x00010001);
}

static void features_tests(void)
{
    /* Implemented: bit 31 is 0, and none of the six has feature flags */
    for (size_t i = 0; i < COUNT(implemented); i++)
        CHECK_ANSWER(psci(FN_PSCI_FEATURES, implemented[i]), 0);

    for (size_t i = 0; i < COUNT(absent); i++)
        CHECK_ANSWER(psci(FN_PSCI_FEATURES, absent[i]), NOT_SUPPORTED);

    /* psci_func_id is a uint32, in W1 */
    CHECK_ANSWER(psci(FN_PSCI_FEATURES, 0xdeadbeef00000000UL | FN_SYSTEM_OFF), 0);
    CHECK_ANSWER(psci(FN_PSCI_FEATURES, 0xdeadbeef00000000UL | 0x80000000), NOT_SUPPORTED);
}

/* A function that is not there answers -1 in all of x0 (DEN 0028 5.2) and does nothing else */
static void absent_tests(void)
{
    for (size_t i = 0; i < COUNT(absent); i++)
        CHECK_ANSWER(psci(absent[i], 0x1234), NOT_SUPPORTED);
}

/* CPU_ON as a guest calls it: the answer, and what the core it is about starts with */
#define ENTRY   0x8123456780UL
#define CONTEXT 0xc0ffee00c0ffee00UL

static enum outcome cpu_on(u64 target)
{
    fill(FN_CPU_ON, target);
    ctx.regs[2] = ENTRY;
    ctx.regs[3] = CONTEXT;
    before = ctx;
    starts = 0;
    return hvc(0);
}

/* What was printed while the guest was stopped */
static char printed[16384];

static enum outcome call_printed(enum outcome (*call)(u64), u64 arg)
{
    FILE *f = tmpfile();
    int out = dup(1);
    enum outcome ret;
    size_t len;

    fflush(stdout);
    dup2(fileno(f), 1);
    ret = call(arg);
    fflush(stdout);
    dup2(out, 1);
    close(out);
    rewind(f);
    len = fread(printed, 1, sizeof(printed) - 1, f);
    printed[len] = 0;
    fclose(f);
    return ret;
}

static enum outcome cpu_on_printed(u64 target)
{
    return call_printed(cpu_on, target);
}

static int count_lines(const char *start)
{
    int n = 0;

    for (const char *p = printed; *p; p = strchr(p, '\n') + 1) {
        if (!strncmp(p, start, strlen(start)))
            n++;
        if (!strchr(p, '\n'))
            break;
    }
    return n;
}

static void cpu_on_tests(void)
{
    /* Every core, in the order the kernel starts them (MADT: 6 first, then 7, 8, 9, 0...) */
    reset_cpus();
    for (int i = 1; i < M4_CPUS; i++) {
        int cpu = (M4_BOOT + i) % M4_CPUS;

        CHECK_ANSWER(cpu_on(m4_mpidr[cpu]), 0);
        CHECK(starts == 1 && start_cpu == cpu);
        CHECK(start_entry == (void *)ENTRY);
        CHECK(start_regs[0] == CONTEXT);
        /* Nothing of the caller's in the registers after x0 */
        CHECK(!start_regs[1] && !start_regs[2] && !start_regs[3]);
        /* Those that were not asked for stay where they are */
        for (int j = 0; j < M4_CPUS; j++)
            CHECK(hv_started_cpus[j] == ((j + M4_CPUS - M4_BOOT) % M4_CPUS <= i));
    }

    /* They are all in the guest, the caller too: ALREADY_ON, in all of x0 */
    for (int cpu = 0; cpu < M4_CPUS; cpu++) {
        CHECK_ANSWER(cpu_on(m4_mpidr[cpu]), ALREADY_ON);
        CHECK(!starts);
    }

    /* One that has left the guest starts again like the first time */
    hv_started_cpus[3] = false;
    CHECK_ANSWER(cpu_on(m4_mpidr[3]), 0);
    CHECK(starts == 1 && start_cpu == 3 && start_entry == (void *)ENTRY);
    CHECK_ANSWER(cpu_on(m4_mpidr[3]), ALREADY_ON);

    /* Only the affinity fields count, not what else MPIDR_EL1 has (RES1, U, MT) */
    reset_cpus();
    CHECK_ANSWER(cpu_on(0x81000000UL | m4_mpidr[8]), 0);
    CHECK(starts == 1 && start_cpu == 8);
    CHECK_ANSWER(cpu_on(0xffffff0040000000UL | m4_mpidr[8]), ALREADY_ON);
    CHECK_ANSWER(cpu_on(0x80000000UL | m4_mpidr[M4_BOOT]), ALREADY_ON);
    CHECK(!starts);
}

/* A core that EL2 does not have: the guest stops, and what it asked for and what there is is
 * there to be read */
static void cpu_on_stop_tests(void)
{
    /* The last one is 1.0.0.1, which is not 0.0.0.1 */
    static const u64 unknown[] = {0x6, 0x10104, 0x100, 0x10000, 0xffffff, 0x100000001UL};
    char line[80];

    for (size_t i = 0; i < COUNT(unknown); i++) {
        reset_cpus();
        hv_started_cpus[7] = true;
        CHECK_STOPPED(cpu_on_printed(unknown[i]), HV_GUEST_HVC);
        CHECK(proxy_calls == 1 && !starts);
        snprintf(line, sizeof(line), "HV: MPIDR 0x%lx (x1 = 0x%lx) is none of the CPUs in EL2",
                 unknown[i], unknown[i]);
        CHECK(count_lines(line) == 1);
        CHECK(count_lines("HV: CPU ") == M4_CPUS);
        for (int cpu = 0; cpu < M4_CPUS; cpu++) {
            snprintf(line, sizeof(line), "HV: CPU %d: MPIDR 0x%lx, %s\n", cpu, m4_mpidr[cpu],
                     cpu == M4_BOOT || cpu == 7 ? "in the guest" : "waiting in EL2");
            CHECK(count_lines(line) == 1);
        }
        /* A host that says "handled" does not get the guest going again either */
        proxy_handled = 3;
        CHECK_STOPPED(cpu_on_printed(unknown[i]), HV_GUEST_HVC);
        CHECK(proxy_calls == 4 && !starts);
        proxy_handled = 0;
    }

    /* A core that does not run m1n1 is not one of them, nor are the indices that have none */
    reset_cpus();
    absent_cpu[0] = true;
    CHECK_STOPPED(cpu_on_printed(m4_mpidr[0]), HV_GUEST_HVC);
    CHECK(proxy_calls == 1 && !starts);
    CHECK(count_lines("HV: CPU ") == M4_CPUS - 1 && !count_lines("HV: CPU 0:"));

    /* One that EL2 has and cannot let in, say because the watchdog runs on it */
    reset_cpus();
    start_works = false;
    CHECK_STOPPED(cpu_on_printed(m4_mpidr[2]), HV_GUEST_HVC);
    CHECK(proxy_calls == 1 && starts == 1 && start_cpu == 2);
    CHECK(count_lines("HV: MPIDR 0x2 (x1 = 0x2) is a CPU that cannot be started") == 1);
    CHECK(count_lines("HV: CPU ") == M4_CPUS);
}

static void stop_tests(void)
{
    static const struct {
        u32 fn;
        u32 event;
    } calls[] = {
        {FN_SYSTEM_RESET, HV_GUEST_RESET},
        {FN_SYSTEM_OFF, HV_GUEST_OFF},
    };

    for (size_t i = 0; i < COUNT(calls); i++) {
        proxy_handled = 0;
        CHECK_STOPPED(psci(calls[i].fn, 0x101), calls[i].event);
        CHECK(proxy_calls == 1);

        /* A host that says "handled" does not get the guest going again either */
        proxy_handled = 3;
        CHECK_STOPPED(psci(calls[i].fn, 0x101), calls[i].event);
        CHECK(proxy_calls == 4);
        proxy_handled = 0;
    }
}

static enum outcome report(u64 unused)
{
    UNUSED(unused);
    hv_psci_report();
    return RETURNED;
}

/* CPU_OFF of a core. It has no parameters: what is in x1 does not count */
static enum outcome cpu_off(u64 cpu)
{
    fill(FN_CPU_OFF, 0xdeadbeef);
    ctx.cpu_id = cpu;
    before = ctx;
    offs = 0;
    return hvc(0);
}

static void cpu_off_tests(void)
{
    char line[80];

    /* Every core can turn itself off, the boot core too, while the guest runs on another */
    for (int cpu = 0; cpu < M4_CPUS; cpu++) {
        reset_cpus();
        for (int i = 0; i < M4_CPUS; i++)
            hv_started_cpus[i] = true;
        CHECK(call_printed(cpu_off, cpu) == OFF);
        CHECK(offs == 1 && !memcmp(&off_ctx, &before, sizeof(before)));
        CHECK(!proxy_calls && !starts);
        CHECK(!hv_started_cpus[cpu]);
        /* Nothing is said of it: the guest may do this over and over */
        CHECK(!printed[0]);

        /* ...and is turned on again, by another one */
        CHECK_ANSWER(cpu_on(m4_mpidr[cpu]), 0);
        CHECK(starts == 1 && start_cpu == cpu);
    }

    /* One after the other, until there is one left, whichever that is: it is SYSTEM_OFF there */
    for (int last = 0; last < M4_CPUS; last++) {
        reset_cpus();
        for (int i = 0; i < M4_CPUS; i++)
            hv_started_cpus[i] = true;
        for (int i = 1; i < M4_CPUS; i++)
            CHECK(cpu_off((last + i) % M4_CPUS) == OFF);
        CHECK(!proxy_calls);
        CHECK_STOPPED(call_printed(cpu_off, last), HV_GUEST_OFF);
        CHECK(proxy_calls == 1 && offs == 1);
        CHECK(hv_started_cpus[last]);
        snprintf(line, sizeof(line), "HV: PSCI: CPU_OFF from the guest on CPU %d, its last one",
                 last);
        CHECK(count_lines(line) == 1);

        /* A host that says "handled" does not get the guest going again either */
        proxy_handled = 3;
        CHECK_STOPPED(cpu_off(last), HV_GUEST_OFF);
        CHECK(proxy_calls == 4 && offs == 1);
        proxy_handled = 0;
    }
    reset_cpus();

    /* The report has the number of calls */
    CHECK(call_printed(report, 0) == RETURNED);
    snprintf(line, sizeof(line), ", %d CPU_OFF", M4_CPUS + M4_CPUS * (M4_CPUS + 1));
    CHECK(strstr(printed, line));
}

/*
 * hvc with another immediate is not PSCI (5.2.1: "the immediate value used with an SMC (or
 * HVC) instruction must be 0"). #1 is a Microsoft hypercall and #4 is how the Windows loader
 * starts Hyper-V; returning from either would be taken for an answer.
 */
static void immediate_tests(void)
{
    static const u16 imms[] = {1, 2, 4, 0x4242, 0xffff};

    for (size_t i = 0; i < COUNT(imms); i++) {
        fill(FN_PSCI_VERSION, 0);
        CHECK_STOPPED(hvc(imms[i]), HV_GUEST_HVC);
        CHECK(proxy_calls == 1);

        fill(0xffffffff, 0);
        proxy_handled = 2;
        CHECK_STOPPED(hvc(imms[i]), HV_GUEST_HVC);
        CHECK(proxy_calls == 3);
        proxy_handled = 0;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* The host side knows these by number: HV_EVENT in proxyclient/m1n1/hv/types.py */
    CHECK(HV_GUEST_RESET == 8);
    CHECK(HV_GUEST_OFF == 9);
    CHECK(HV_GUEST_HVC == 10);

    version_tests();
    features_tests();
    absent_tests();
    cpu_on_tests();
    cpu_on_stop_tests();
    stop_tests();
    cpu_off_tests();
    immediate_tests();
    hv_psci_report();

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
