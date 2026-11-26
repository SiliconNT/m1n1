/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The debug registers of the guest.
 *
 * Windows reads and writes the breakpoint and watchpoint registers of the CPU, and reads
 * OSLSR_EL1, whether or not anybody debugs it. With MDCR_EL2.TDA and MDCR_EL2.TDOSA set
 * (Arm DDI 0487 M.d, D24.3.17) those accesses trap to EL2, and they are answered here.
 *
 * DBGBVR<n>_EL1, DBGBCR<n>_EL1, DBGWVR<n>_EL1, DBGWCR<n>_EL1: what the guest writes is kept
 * for the core it wrote it on, and that is what it reads on that core. It is never put into
 * the register of the CPU: a breakpoint or a watchpoint the guest sets up does not fire. All
 * sixteen that the encodings have (D24.3.2, D24.3.3, D24.3.11, D24.3.12) are kept, however
 * many the CPU has.
 *
 * OSLSR_EL1 (D24.3.28): a read gets what the register of the CPU says.
 *
 * Every other access is left to whoever else handles trapped accesses.
 *
 * A core only ever touches its own registers here, so none of this needs the hypervisor lock.
 */

#include "hv.h"
#include "cpu_regs.h"
#include "smp.h"
#include "string.h"
#include "types.h"
#include "utils.h"

/* DBGBVR<n>_EL1 and the other three: op0 2, op1 0, CRn 0, CRm n, and op2 says which */
#define DBG_COUNT 16
#define DBG_OP2   4
#define DBG_KINDS 4

struct hv_dbg_cpu {
    u64 regs[DBG_KINDS][DBG_COUNT];
    u64 reads;
    u64 writes;
} ALIGNED(64);

static struct hv_dbg_cpu hv_dbg[MAX_CPUS];

void hv_dbg_init(void)
{
    memset(hv_dbg, 0, sizeof(hv_dbg));
}

/* A core is about to start the guest again: its debug registers are as after a reset */
void hv_dbg_init_cpu(int cpu)
{
    memset(hv_dbg[cpu].regs, 0, sizeof(hv_dbg[cpu].regs));
}

/*
 * iss: the syndrome of the trapped MRS or MSR (D24.2.45, exception class 0x18). Returns
 * whether the access was answered; the caller moves the guest past the instruction.
 */
bool hv_dbg_handle_msr(struct exc_info *ctx, u64 iss)
{
    struct hv_dbg_cpu *cpu = &hv_dbg[smp_id()];
    u64 crn = FIELD_GET(ESR_ISS_MSR_CRn, iss);
    u64 crm = FIELD_GET(ESR_ISS_MSR_CRm, iss);
    u64 op2 = FIELD_GET(ESR_ISS_MSR_OP2, iss);
    u64 rt = FIELD_GET(ESR_ISS_MSR_Rt, iss);
    bool is_read = iss & ESR_ISS_MSR_DIR;
    u64 val;

    if (FIELD_GET(ESR_ISS_MSR_OP0, iss) != 2 || FIELD_GET(ESR_ISS_MSR_OP1, iss) != 0)
        return false;

    if (crn == 1 && crm == 1 && op2 == 4 && is_read) {
        val = mrs(SYS_OSLSR_EL1);
    } else if (crn == 0 && op2 >= DBG_OP2) {
        u64 *reg = &cpu->regs[op2 - DBG_OP2][crm];

        if (!is_read) {
            /* Rt 31 is the zero register */
            *reg = rt == 31 ? 0 : ctx->regs[rt];
            cpu->writes++;
            return true;
        }
        val = *reg;
    } else {
        return false;
    }

    if (rt != 31)
        ctx->regs[rt] = val;
    cpu->reads++;
    return true;
}

void hv_dbg_report(void)
{
    u64 reads = 0, writes = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        reads += hv_dbg[i].reads;
        writes += hv_dbg[i].writes;
    }
    printf("HV: debug registers: %ld reads and %ld writes of the guest answered\n", reads, writes);
}
