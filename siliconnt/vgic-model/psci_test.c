// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_psci.c. The file under test is the real one, compiled
 * against the stand-in headers of this directory; this file plays the guest (the registers of
 * an hvc) and the host of the hypervisor (hv_exc_proxy(), which is where a guest that is not
 * to run again ends up).
 *
 * What the answers have to be comes from what the Windows kernel does with them
 * (10.0.26300.9457): it reads PSCI_VERSION and takes six functions for granted on 1.1, asks
 * PSCI_FEATURES about the others and goes without those that answer with a negative number,
 * and stops with a bug check if SYSTEM_RESET, SYSTEM_OFF or CPU_OFF return.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "hv.h"

#define NOT_SUPPORTED ((u64)-1)

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

/* ---- the guest ---- */

enum outcome {
    RETURNED,  /* the guest goes on behind the hvc */
    STOPPED,   /* the guest was handed to the host and never ran again */
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
    if (setjmp(left_guest))
        return STOPPED;
    return hv_psci_hvc(&ctx, imm) ? RETURNED : UNHANDLED;
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

static void stop_tests(void)
{
    static const struct {
        u32 fn;
        u32 event;
    } calls[] = {
        {FN_SYSTEM_RESET, HV_GUEST_RESET},
        {FN_SYSTEM_OFF, HV_GUEST_OFF},
        /* Until the hypervisor can start and stop cores: no answer is better than a wrong one */
        {FN_CPU_ON, HV_GUEST_HVC},
        {FN_CPU_OFF, HV_GUEST_HVC},
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
    stop_tests();
    immediate_tests();
    hv_psci_report();

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
