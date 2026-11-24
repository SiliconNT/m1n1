/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * For cputest.c: a function of two instructions for each register of cpuregs.h that the guest
 * reads, and its name; and the write that a core makes with its MMU on, mailbox_write().
 */

#include "cpuregs.h"

/* ---- the registers: the ones it reads first, then the late ones ---- */

/* As in cputest.c */
#define REG_NAME_SIZE 32

#define ENC(op0, op1, crn, crm, op2) S##op0##_##op1##_C##crn##_C##crm##_##op2

    .text

    .balign 8
    .globl  reg_code
reg_code:
#define CODE_BOTH(reg) mrs x0, reg ; ret ;
#define CODE_LATE(reg)
#define CODE_EL2(reg)
#define REG(name, where, op0, op1, crn, crm, op2) CODE_##where(ENC(op0, op1, crn, crm, op2))
    CPU_REGS
#undef REG
#undef CODE_BOTH
#undef CODE_LATE

    .globl  reg_code_late
reg_code_late:
#define CODE_BOTH(reg)
#define CODE_LATE(reg) mrs x0, reg ; ret ;
#define REG(name, where, op0, op1, crn, crm, op2) CODE_##where(ENC(op0, op1, crn, crm, op2))
    CPU_REGS
#undef REG

    .globl  reg_code_end
reg_code_end:

/*
 * void mailbox_write(u64 dev, u64 wb, u64 val, u64 words, u64 ttbr, u64 tcr, u64 mair)
 *
 * Turns the MMU and the caches of this core on, writes val to every 64-bit word of the page at
 * dev, which the tables of the caller map as Device-nGnRnE, and of the one at wb, which they
 * map as write-back memory, the way the Windows kernel writes a mailbox: DSB SY and no cache
 * maintenance (SiliconNT docs/findings/0097, conclusion 14). Then turns them off again. Uses
 * no stack: what it wrote there with the caches on would be missing once they are off.
 */
    .globl  mailbox_write
mailbox_write:
    mrs     x7, sctlr_el1
    msr     mair_el1, x6
    msr     tcr_el1, x5
    msr     ttbr0_el1, x4
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    mov     x8, #0x1005             /* I, C and M */
    orr     x8, x7, x8
    msr     sctlr_el1, x8
    isb
1:  str     x2, [x0], #8
    str     x2, [x1], #8
    subs    x3, x3, #1
    b.ne    1b
    dsb     sy
    msr     sctlr_el1, x7
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ret

    .section .rodata
    .balign REG_NAME_SIZE
    .globl  reg_names
reg_names:
#define NAME_BOTH(name) .asciz #name ; .balign REG_NAME_SIZE ;
#define NAME_LATE(name)
#define NAME_EL2(name)
#define REG(name, where, op0, op1, crn, crm, op2) NAME_##where(name)
    CPU_REGS
#undef REG
#undef NAME_BOTH
#undef NAME_LATE

    .globl  reg_names_late
reg_names_late:
#define NAME_BOTH(name)
#define NAME_LATE(name) .asciz #name ; .balign REG_NAME_SIZE ;
#define REG(name, where, op0, op1, crn, crm, op2) NAME_##where(name)
    CPU_REGS
#undef REG
