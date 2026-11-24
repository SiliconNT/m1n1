/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Code that run_cpuregs.py puts into the memory of a target that runs m1n1, to read system
 * registers at EL2 on a core that m1n1 has started (its MMU off) and on the boot core (its MMU
 * on). Position independent; nothing of m1n1 is called.
 *
 *   +0x00  u64 probe(void *out)
 *          reads every register of cpuregs.h, in its order: two 64-bit words each, the value,
 *          and 0 or, if the read ended in an exception, ESR_EL2 with bit 63 set. Returns
 *          PROBE_MAGIC
 *   +0x08  u64 wake_wait(u64 *flag, u64 *out)
 *          waits as the idle loop of m1n1 does (WFE) until the flag is 1, then writes the
 *          physical count to *out
 *   +0x10  u64 wake_kick(u64 *flag)
 *          sets the flag, cleans it to memory, SEV; returns the physical count before that
 *   +0x18  u64 isr(void)           ISR_EL1
 *   +0x20  u64 load32(u32 *addr)   one 32-bit load, such as of the event register of AIC
 */

#include "cpuregs.h"

#define PROBE_MAGIC 0x50525a30

    .text
    .globl  _start
_start:
    b       probe
    nop
    b       wake_wait
    nop
    b       wake_kick
    nop
    b       isr
    nop
    b       load32
    nop

probe:
    stp     x29, x30, [sp, #-48]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    mov     x19, x0
    mrs     x22, daif
    msr     daifset, #3             /* no IRQ, no FIQ while the vectors are these */
    mrs     x21, vbar_el1           /* VBAR_EL2, as HCR_EL2.E2H is 1 */
    adr     x1, vectors
    msr     vbar_el1, x1
    isb

#define ENC(op0, op1, crn, crm, op2) S##op0##_##op1##_C##crn##_C##crm##_##op2
#define REG(name, where, op0, op1, crn, crm, op2)                                                  \
    mov x9, #0 ; mov x0, #0 ; isb ; mrs x0, ENC(op0, op1, crn, crm, op2) ; isb ;                   \
    stp x0, x9, [x19], #16 ;
    CPU_REGS
#undef REG

    dsb     sy
    msr     vbar_el1, x21
    isb
    msr     daif, x22
    ldr     w0, =PROBE_MAGIC
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #48
    ret

wake_wait:
1:  ldr     x2, [x0]
    cmp     x2, #1
    b.eq    2f
    wfe
    isb
    b       1b
2:  isb
    mrs     x2, cntpct_el0
    str     x2, [x1]
    dsb     sy
    mov     x0, #0
    ret

wake_kick:
    mov     x1, #1
    isb
    mrs     x2, cntpct_el0
    str     x1, [x0]
    dc      civac, x0
    dsb     sy
    sev
    mov     x0, x2
    ret

isr:
    isb
    mrs     x0, isr_el1
    ret

load32:
    ldr     w0, [x0]
    dsb     sy
    ret

    .ltorg

/* Skip the instruction that took the exception, and say which one it was in x9 */
    .macro  skip
    .balign 0x80
    mrs     x9, elr_el1
    add     x9, x9, #4
    msr     elr_el1, x9
    mrs     x9, esr_el1
    orr     x9, x9, #(1 << 63)
    mov     x0, #0
    eret
    .endm

    .macro  hang
    .balign 0x80
    b       .
    .endm

    .balign 0x800
vectors:
    hang                            /* EL2t */
    hang
    hang
    hang
    skip                            /* EL2h, synchronous */
    hang
    hang
    hang                            /* SError */
    hang                            /* lower ELs */
    hang
    hang
    hang
    hang
    hang
    hang
    hang
    .balign 0x80
