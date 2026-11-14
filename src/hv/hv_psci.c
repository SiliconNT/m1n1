/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * PSCI for the guest (Arm DEN 0022 F.b), over the HVC conduit of the SMC calling convention
 * (Arm DEN 0028 1.7 H): the guest puts a function ID in w0 and executes hvc #0.
 *
 * The guest is Windows, with a UEFI firmware in front of it whose FADT says PSCI_COMPLIANT
 * and PSCI_USE_HVC. What the Windows kernel does with the answers decides what they are:
 *
 *  - It reads PSCI_VERSION and, for 1.1, takes PSCI_VERSION, PSCI_FEATURES, CPU_ON, CPU_OFF,
 *    SYSTEM_RESET and SYSTEM_OFF for granted. So the version is 1.1 and those six are what
 *    PSCI_FEATURES says is there.
 *  - It asks PSCI_FEATURES about every other function before it uses it, and goes without
 *    those that answer with a negative number. All of them do. That includes CPU_SUSPEND,
 *    which the specification makes mandatory, and SMCCC_VERSION, where "no" ends the
 *    kernel's search for firmware mitigations of speculation attacks.
 *  - It stops with a bug check if SYSTEM_RESET, SYSTEM_OFF or CPU_OFF return, or if CPU_ON
 *    returns anything but 0, ALREADY_ON or ON_PENDING.
 *
 * The firmware calls SYSTEM_RESET and SYSTEM_OFF from its ResetSystem service and asks
 * PSCI_FEATURES about SYSTEM_RESET2 before a warm reset; nothing else.
 *
 * A call changes x0 and nothing else. Functions with 32-bit parameters take them from w1 to
 * w3 (DEN 0028 2.6); an error is -1 in all of x0, which is what both a 32-bit and a 64-bit
 * caller expect (5.1, 5.2).
 *
 * SYSTEM_RESET and SYSTEM_OFF do not reset or power off the machine: the guest is stopped for
 * good and the host of the hypervisor is told, which is where a machine that is run from
 * another one over USB wants to end up. CPU_ON and CPU_OFF are not written yet. A guest that
 * calls one of them is stopped the same way, because no answer it could be given is true.
 *
 * An hvc with another immediate is not PSCI ("the immediate value used with an SMC (or HVC)
 * instruction must be 0", DEN 0022 5.2.1). Windows has two: hvc #1 is a hypercall to the
 * Microsoft hypervisor and hvc #4 is how its loader starts that hypervisor. Coming back from
 * either would be taken for an answer, so the guest is stopped there too.
 *
 * Everything here runs with the hypervisor lock held, on whichever core the guest called from.
 */

#include "hv.h"
#include "utils.h"

/* DEN 0022 5.1. Bit 30 of a function ID says its parameters are 64 bits wide */
#define PSCI_FN_VERSION      0x84000000
#define PSCI_FN_CPU_OFF      0x84000002
#define PSCI_FN_CPU_ON       0xc4000003
#define PSCI_FN_SYSTEM_OFF   0x84000008
#define PSCI_FN_SYSTEM_RESET 0x84000009
#define PSCI_FN_FEATURES     0x8400000a

/* Major version in bits 31:16, minor version in bits 15:0 (5.3) */
#define PSCI_VERSION_1_1 0x00010001L

/* Also the "Unknown Function Identifier" of DEN 0028 5.2 */
#define PSCI_NOT_SUPPORTED (-1L)

/* How many calls of functions that are not there get a line of their own */
#define PSCI_LOG_ABSENT 8

static struct {
    u64 version;
    u64 features;        // PSCI_FEATURES calls...
    u64 features_absent; // ...of which were about a function that is not there
    u64 absent;          // calls of functions that are not there
    u32 last_absent;     // ...and the function ID of the last one
} psci_stats;

void hv_psci_init(void)
{
    psci_stats.version = 0;
    psci_stats.features = 0;
    psci_stats.features_absent = 0;
    psci_stats.absent = 0;
    psci_stats.last_absent = 0;
}

static bool hv_psci_implemented(u32 fn)
{
    switch (fn) {
        case PSCI_FN_VERSION:
        case PSCI_FN_CPU_OFF:
        case PSCI_FN_CPU_ON:
        case PSCI_FN_SYSTEM_OFF:
        case PSCI_FN_SYSTEM_RESET:
        case PSCI_FN_FEATURES:
            return true;
    }
    return false;
}

/*
 * The guest does not run again. The host is told why and sees the registers of the call; it
 * can look around and leave the guest, and if it says to go on, it is told again.
 */
static void __attribute__((noreturn)) hv_psci_stop(struct exc_info *ctx, u32 event)
{
    for (;;)
        hv_exc_proxy(ctx, START_HV, event, NULL);
}

/*
 * The guest executed hvc with this immediate; ctx->elr is behind the instruction already.
 * Returns true if the guest is to go on there, with the answer in ctx->regs[0].
 */
bool hv_psci_hvc(struct exc_info *ctx, u16 imm)
{
    u32 fn = ctx->regs[0];
    s64 ret;

    if (imm != 0) {
        printf("HV: hvc #0x%x from the guest on CPU %ld at 0x%lx, x0 = 0x%lx: only hvc #0 is "
               "answered, stopping the guest\n",
               imm, ctx->cpu_id, ctx->elr - 4, ctx->regs[0]);
        hv_psci_stop(ctx, HV_GUEST_HVC);
    }

    switch (fn) {
        case PSCI_FN_VERSION:
            psci_stats.version++;
            ret = PSCI_VERSION_1_1;
            break;
        case PSCI_FN_FEATURES:
            psci_stats.features++;
            /* None of the six has feature flags (5.16.2, table 11) */
            ret = 0;
            if (!hv_psci_implemented(ctx->regs[1])) {
                psci_stats.features_absent++;
                ret = PSCI_NOT_SUPPORTED;
            }
            break;
        case PSCI_FN_SYSTEM_RESET:
            printf("HV: PSCI: SYSTEM_RESET from the guest on CPU %ld, stopping the guest\n",
                   ctx->cpu_id);
            hv_psci_stop(ctx, HV_GUEST_RESET);
        case PSCI_FN_SYSTEM_OFF:
            printf("HV: PSCI: SYSTEM_OFF from the guest on CPU %ld, stopping the guest\n",
                   ctx->cpu_id);
            hv_psci_stop(ctx, HV_GUEST_OFF);
        case PSCI_FN_CPU_ON:
        case PSCI_FN_CPU_OFF:
            printf("HV: PSCI: %s from the guest on CPU %ld, x1 = 0x%lx x2 = 0x%lx x3 = 0x%lx: "
                   "not written yet, stopping the guest\n",
                   fn == PSCI_FN_CPU_ON ? "CPU_ON" : "CPU_OFF", ctx->cpu_id, ctx->regs[1],
                   ctx->regs[2], ctx->regs[3]);
            hv_psci_stop(ctx, HV_GUEST_HVC);
        default:
            if (psci_stats.absent++ < PSCI_LOG_ABSENT)
                printf("HV: PSCI: function 0x%x called from the guest on CPU %ld at 0x%lx, "
                       "x1 = 0x%lx: not supported\n",
                       fn, ctx->cpu_id, ctx->elr - 4, ctx->regs[1]);
            psci_stats.last_absent = fn;
            ret = PSCI_NOT_SUPPORTED;
            break;
    }

    ctx->regs[0] = ret;
    return true;
}

void hv_psci_report(void)
{
    printf("HV: PSCI: %ld PSCI_VERSION, %ld PSCI_FEATURES (%ld said not supported), %ld calls of "
           "functions that are not there",
           psci_stats.version, psci_stats.features, psci_stats.features_absent, psci_stats.absent);
    if (psci_stats.absent)
        printf(" (the last one 0x%x)", psci_stats.last_absent);
    printf("\n");
}
