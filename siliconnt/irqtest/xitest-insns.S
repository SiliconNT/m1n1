/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The instructions of xitest.c: every form of load and store that src/hv/hv_xinsn.c takes
 * apart, assembled here so that the words are the assembler's and not this test's own idea
 * of the encodings. And a few that it does not take.
 *
 * An entry is struct xi_entry of xitest.c: what the instruction is like (xitest.h), the
 * distance from the base register to the first byte accessed, how many bytes, what x3 holds
 * if it is an offset, five words to run (two, the instruction, two), five more to run on
 * memory that nothing has replaced anything in, and the text of the instruction.
 *
 * The registers: x2 is the base, x0 and x1 (or q0 and q1) the data, x3 an offset register or
 * the second operand of an atomic operation, x4 and x5 the new value of a CASP.
 */

#include "xitest.h"

    .section .rodata.xitest, "a"
    .balign 8
    .globl  xi_entries
xi_entries:

    .macro  head flags, disp, len, x3
    .long   \flags
    .long   \disp
    .long   \len
    .long   0
    .quad   \x3
    .endm

    .macro  name text:vararg
1:  .asciz  "\text"
    .org    1b + XI_NAME
    .endm

    .macro  T flags, disp, len, x3, insn:vararg
    head    \flags, \disp, \len, \x3
    .rept   2
    nop
    nop
    \insn
    nop
    nop
    .endr
    name    \insn
    .endm

    /* With the stack pointer for a base, which the instruction has in place of x2 */
    .macro  TSP flags, disp, len, x3, insn:vararg
    head    \flags, \disp, \len, \x3
    .rept   2
    mov     x9, sp
    mov     sp, x2
    \insn
    mov     x2, sp
    mov     sp, x9
    .endr
    name    \insn
    .endm

    /* The same at EL1t, where the stack pointer is SP_EL0 */
    .macro  TSP0 flags, disp, len, x3, insn:vararg
    head    \flags, \disp, \len, \x3
    .rept   2
    msr     spsel, #0
    mov     sp, x2
    \insn
    mov     x2, sp
    msr     spsel, #1
    .endr
    name    EL1t \insn
    .endm

    /*
     * A load of a literal. xitest.c puts the distance into the instruction; what it is
     * compared with is the same load through x2.
     */
    .macro  TLIT len, mnem, reg
    head    F_LIT, 0, \len, 0
    nop
    nop
7:  \mnem   \reg, 7b
    nop
    nop
    nop
    nop
    \mnem   \reg, [x2]
    nop
    nop
    name    \mnem \reg, literal
    .endm

/* ---- one general-purpose register, an unsigned offset ---- */

    T       0, 5, 1, 0, ldrb w0, [x2, #5]
    T       0, 6, 2, 0, ldrh w0, [x2, #6]
    T       0, 8, 4, 0, ldr w0, [x2, #8]
    T       0, 16, 8, 0, ldr x0, [x2, #16]
    T       0, 0, 8, 0, ldr x0, [x2]
    T       0, 32760, 8, 0, ldr x0, [x2, #32760]
    T       0, 3, 1, 0, ldrsb w0, [x2, #3]
    T       0, 3, 1, 0, ldrsb x0, [x2, #3]
    T       0, 2, 2, 0, ldrsh w0, [x2, #2]
    T       0, 2, 2, 0, ldrsh x0, [x2, #2]
    T       0, 4, 4, 0, ldrsw x0, [x2, #4]
    T       0, 0, 8, 0, ldr xzr, [x2]
    T       F_WRITES, 5, 1, 0, strb w0, [x2, #5]
    T       F_WRITES, 6, 2, 0, strh w0, [x2, #6]
    T       F_WRITES, 8, 4, 0, str w0, [x2, #8]
    T       F_WRITES, 16, 8, 0, str x0, [x2, #16]
    T       F_WRITES, 0, 8, 0, str xzr, [x2]
    T       F_WRITES, 0, 4, 0, str wzr, [x2]

/* ---- an unscaled offset ---- */

    T       0, -1, 1, 0, ldurb w0, [x2, #-1]
    T       0, -3, 2, 0, ldurh w0, [x2, #-3]
    T       0, -7, 4, 0, ldur w0, [x2, #-7]
    T       0, -256, 8, 0, ldur x0, [x2, #-256]
    T       0, 255, 8, 0, ldur x0, [x2, #255]
    T       0, -1, 1, 0, ldursb w0, [x2, #-1]
    T       0, 1, 1, 0, ldursb x0, [x2, #1]
    T       0, -3, 2, 0, ldursh w0, [x2, #-3]
    T       0, 3, 2, 0, ldursh x0, [x2, #3]
    T       0, -5, 4, 0, ldursw x0, [x2, #-5]
    T       F_WRITES, -1, 1, 0, sturb w0, [x2, #-1]
    T       F_WRITES, -3, 2, 0, sturh w0, [x2, #-3]
    T       F_WRITES, -7, 4, 0, stur w0, [x2, #-7]
    T       F_WRITES, -9, 8, 0, stur x0, [x2, #-9]

/* ---- writeback ---- */

    T       0, 1, 1, 0, ldrb w0, [x2, #1]!
    T       0, 0, 1, 0, ldrb w0, [x2], #1
    T       0, -1, 1, 0, ldrsb x0, [x2, #-1]!
    T       0, 0, 1, 0, ldrsb w0, [x2], #-1
    T       0, 2, 2, 0, ldrh w0, [x2, #2]!
    T       0, 0, 2, 0, ldrsh x0, [x2], #-2
    T       0, 4, 4, 0, ldr w0, [x2, #4]!
    T       0, 0, 4, 0, ldr w0, [x2], #-4
    T       0, -4, 4, 0, ldrsw x0, [x2, #-4]!
    T       0, 0, 4, 0, ldrsw x0, [x2], #4
    T       0, 24, 8, 0, ldr x0, [x2, #24]!
    T       0, 0, 8, 0, ldr x0, [x2], #-16
    T       0, -256, 8, 0, ldr x0, [x2, #-256]!
    T       0, 0, 8, 0, ldr x0, [x2], #255
    T       F_WRITES, 1, 1, 0, strb w0, [x2, #1]!
    T       F_WRITES, 0, 1, 0, strb w0, [x2], #-1
    T       F_WRITES, -2, 2, 0, strh w0, [x2, #-2]!
    T       F_WRITES, 0, 2, 0, strh w0, [x2], #2
    T       F_WRITES, 4, 4, 0, str w0, [x2, #4]!
    T       F_WRITES, 0, 4, 0, str w0, [x2], #-4
    T       F_WRITES, -8, 8, 0, str x0, [x2, #-8]!
    T       F_WRITES, 0, 8, 0, str x0, [x2], #8

/* ---- a register offset ---- */

    T       F_X3, 40, 8, 40, ldr x0, [x2, x3]
    T       F_X3, 40, 8, 5, ldr x0, [x2, x3, lsl #3]
    T       F_X3, 0x30, 4, 0xdeadbeef00000030, ldr w0, [x2, w3, uxtw]
    T       F_X3, 0x30, 4, 0xdeadbeef0000000c, ldr w0, [x2, w3, uxtw #2]
    T       F_X3, -32, 8, 0x12345678ffffffe0, ldr x0, [x2, w3, sxtw]
    T       F_X3, -32, 8, 0x12345678fffffffc, ldr x0, [x2, w3, sxtw #3]
    T       F_X3, -24, 8, 0xffffffffffffffe8, ldr x0, [x2, x3, sxtx]
    T       F_X3, 17, 1, 17, ldrb w0, [x2, x3]
    T       F_X3, 17, 1, 17, ldrsb x0, [x2, x3]
    T       F_X3, 18, 2, 9, ldrh w0, [x2, x3, lsl #1]
    T       F_X3, 18, 2, 9, ldrsh x0, [x2, x3, lsl #1]
    T       F_X3, 20, 4, 5, ldrsw x0, [x2, x3, lsl #2]
    T       F_X3|F_WRITES, 40, 8, 40, str x0, [x2, x3]
    T       F_X3|F_WRITES, 48, 8, 6, str x0, [x2, x3, lsl #3]
    T       F_X3|F_WRITES, -16, 4, 0x55555555fffffffc, str w0, [x2, w3, sxtw #2]
    T       F_X3|F_WRITES, 0x21, 1, 0xaaaaaaaa00000021, strb w0, [x2, w3, uxtw]
    T       F_X3|F_WRITES, 18, 2, 9, strh w0, [x2, x3, lsl #1]

/* ---- acquire and release ---- */

    T       F_ALIGNED, 0, 1, 0, ldarb w0, [x2]
    T       F_ALIGNED, 0, 2, 0, ldarh w0, [x2]
    T       F_ALIGNED, 0, 4, 0, ldar w0, [x2]
    T       F_ALIGNED, 0, 8, 0, ldar x0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 1, 0, stlrb w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 2, 0, stlrh w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 4, 0, stlr w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 8, 0, stlr x0, [x2]

/* ---- a pair of general-purpose registers ---- */

    T       0, 8, 8, 0, ldp w0, w1, [x2, #8]
    T       0, -16, 16, 0, ldp x0, x1, [x2, #-16]
    T       0, 0, 16, 0, ldp x0, x1, [x2]
    T       0, 504, 16, 0, ldp x0, x1, [x2, #504]
    T       0, 4, 8, 0, ldpsw x0, x1, [x2, #4]
    T       F_WRITES, 8, 8, 0, stp w0, w1, [x2, #8]
    T       F_WRITES, 16, 16, 0, stp x0, x1, [x2, #16]
    T       F_WRITES, -512, 16, 0, stp x0, x1, [x2, #-512]
    T       F_WRITES, 0, 16, 0, stp xzr, x1, [x2]
    T       0, 32, 16, 0, ldp x0, x1, [x2, #32]!
    T       0, 0, 16, 0, ldp x0, x1, [x2], #-32
    T       0, -8, 8, 0, ldp w0, w1, [x2, #-8]!
    T       0, 0, 8, 0, ldp w0, w1, [x2], #8
    T       0, 8, 8, 0, ldpsw x0, x1, [x2, #8]!
    T       0, 0, 8, 0, ldpsw x0, x1, [x2], #-8
    T       F_WRITES, -16, 16, 0, stp x0, x1, [x2, #-16]!
    T       F_WRITES, 0, 16, 0, stp x0, x1, [x2], #16
    T       F_WRITES, 8, 8, 0, stp w0, w1, [x2, #8]!
    T       F_WRITES, 0, 8, 0, stp w0, w1, [x2], #-8
    T       0, 16, 16, 0, ldnp x0, x1, [x2, #16]
    T       0, -8, 8, 0, ldnp w0, w1, [x2, #-8]
    T       F_WRITES, 16, 16, 0, stnp x0, x1, [x2, #16]
    T       F_WRITES, 8, 8, 0, stnp w0, w1, [x2, #8]

/* ---- one SIMD and floating-point register ---- */

    T       0, 3, 1, 0, ldr b0, [x2, #3]
    T       0, 6, 2, 0, ldr h0, [x2, #6]
    T       0, 12, 4, 0, ldr s0, [x2, #12]
    T       0, 24, 8, 0, ldr d0, [x2, #24]
    T       0, 48, 16, 0, ldr q0, [x2, #48]
    T       F_WRITES, 3, 1, 0, str b0, [x2, #3]
    T       F_WRITES, 6, 2, 0, str h0, [x2, #6]
    T       F_WRITES, 12, 4, 0, str s0, [x2, #12]
    T       F_WRITES, 24, 8, 0, str d0, [x2, #24]
    T       F_WRITES, 48, 16, 0, str q0, [x2, #48]
    T       0, -1, 1, 0, ldur b0, [x2, #-1]
    T       0, -1, 2, 0, ldur h0, [x2, #-1]
    T       0, 1, 4, 0, ldur s0, [x2, #1]
    T       0, -3, 8, 0, ldur d0, [x2, #-3]
    T       0, -9, 16, 0, ldur q0, [x2, #-9]
    T       F_WRITES, -1, 4, 0, stur s0, [x2, #-1]
    T       F_WRITES, -3, 8, 0, stur d0, [x2, #-3]
    T       F_WRITES, -9, 16, 0, stur q0, [x2, #-9]
    T       0, 16, 16, 0, ldr q0, [x2, #16]!
    T       0, 0, 16, 0, ldr q0, [x2], #-16
    T       0, -8, 8, 0, ldr d0, [x2, #-8]!
    T       0, 0, 4, 0, ldr s0, [x2], #4
    T       0, 0, 1, 0, ldr b0, [x2], #1
    T       F_WRITES, -16, 16, 0, str q0, [x2, #-16]!
    T       F_WRITES, 0, 16, 0, str q0, [x2], #16
    T       F_WRITES, 0, 8, 0, str d0, [x2], #8
    T       F_WRITES, 2, 2, 0, str h0, [x2, #2]!
    T       F_X3, 32, 16, 2, ldr q0, [x2, x3, lsl #4]
    T       F_X3, 13, 4, 13, ldr s0, [x2, x3]
    T       F_X3, -24, 8, 0x77777777fffffffd, ldr d0, [x2, w3, sxtw #3]
    T       F_X3|F_WRITES, 24, 8, 0x9999999900000003, str d0, [x2, w3, uxtw #3]
    T       F_X3|F_WRITES, 7, 16, 7, str q0, [x2, x3]
    T       F_X3|F_WRITES, 7, 1, 7, str b0, [x2, x3]

/* ---- a pair of them ---- */

    T       0, 8, 8, 0, ldp s0, s1, [x2, #8]
    T       0, 16, 16, 0, ldp d0, d1, [x2, #16]
    T       0, 32, 32, 0, ldp q0, q1, [x2, #32]
    T       0, -64, 32, 0, ldp q0, q1, [x2, #-64]
    T       F_WRITES, 8, 8, 0, stp s0, s1, [x2, #8]
    T       F_WRITES, 16, 16, 0, stp d0, d1, [x2, #16]
    T       F_WRITES, 32, 32, 0, stp q0, q1, [x2, #32]
    T       0, 32, 32, 0, ldp q0, q1, [x2, #32]!
    T       0, 0, 32, 0, ldp q0, q1, [x2], #-32
    T       0, 0, 16, 0, ldp d0, d1, [x2], #16
    T       0, -8, 8, 0, ldp s0, s1, [x2, #-8]!
    T       F_WRITES, -32, 32, 0, stp q0, q1, [x2, #-32]!
    T       F_WRITES, 0, 32, 0, stp q0, q1, [x2], #32
    T       F_WRITES, 16, 16, 0, stp d0, d1, [x2, #16]!
    T       0, 32, 32, 0, ldnp q0, q1, [x2, #32]
    T       0, 0, 16, 0, ldnp d0, d1, [x2]
    T       F_WRITES, -32, 32, 0, stnp q0, q1, [x2, #-32]
    T       F_WRITES, 4, 8, 0, stnp s0, s1, [x2, #4]

/* ---- atomic operations: x3 is the operand, x0 gets what was in memory ---- */

    .irp    op, ldadd, ldclr, ldeor, ldset, ldsmax, ldsmin, ldumax, ldumin, swp
    T       F_ALIGNED|F_WRITES, 0, 1, 0, \op\()b w3, w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 2, 0, \op\()h w3, w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 4, 0, \op w3, w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 8, 0, \op x3, x0, [x2]
    .endr
    T       F_ALIGNED|F_WRITES, 0, 8, 0, ldaddal x3, x0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 4, 0, swpa w3, w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 8, 0, ldclrl x3, x0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 1, 0, ldsetalb w3, w0, [x2]
    T       F_ALIGNED|F_WRITES, 0, 8, 0, stadd x3, [x2]
    T       F_ALIGNED|F_WRITES, 0, 2, 0, stclrh w3, [x2]
    T       F_ALIGNED|F_WRITES, 0, 8, 0, swp x0, x0, [x2]

/* ---- compare and swap: x3 is what is expected and gets what was there, x0 is the new value ---- */

    .irp    hit, 0, F_CASHIT
    T       F_ALIGNED|F_WRITES|\hit, 0, 1, 0, casb w3, w0, [x2]
    T       F_ALIGNED|F_WRITES|\hit, 0, 2, 0, cash w3, w0, [x2]
    T       F_ALIGNED|F_WRITES|\hit, 0, 4, 0, cas w3, w0, [x2]
    T       F_ALIGNED|F_WRITES|\hit, 0, 8, 0, cas x3, x0, [x2]
    T       F_ALIGNED|F_WRITES|\hit, 0, 8, 0, casal x3, x0, [x2]
    T       F_ALIGNED|F_WRITES|\hit, 0, 1, 0, casab w3, w0, [x2]
    .endr

/* ---- DC ZVA: x2 is 17 bytes into the block ---- */

    T       F_ZVA|F_WRITES, -17, 64, 0, dc zva, x2

/* ---- the stack pointer for a base ---- */

    TSP     0, 16, 8, 0, ldr x0, [sp, #16]
    TSP     0, 0, 16, 0, ldp x0, x1, [sp], #32
    TSP     F_WRITES, -32, 16, 0, stp x0, x1, [sp, #-32]!
    TSP     F_WRITES, 8, 8, 0, str x0, [sp, #8]
    TSP     F_X3, 24, 8, 24, ldr x0, [sp, x3]
    TSP     0, 0, 16, 0, ldr q0, [sp], #16
    TSP     F_WRITES, -16, 16, 0, str q0, [sp, #-16]!
    TSP0    0, 16, 8, 0, ldr x0, [sp, #16]
    TSP0    0, 0, 16, 0, ldp x0, x1, [sp], #32
    TSP0    F_WRITES, -32, 16, 0, stp x0, x1, [sp, #-32]!
    TSP0    F_WRITES, 8, 8, 0, str x0, [sp, #8]
    TSP0    F_X3, 24, 8, 24, ldr x0, [sp, x3]
    TSP0    0, 0, 16, 0, ldr q0, [sp], #16
    TSP0    F_WRITES, -16, 16, 0, str q0, [sp, #-16]!

/* ---- literals ---- */

    TLIT    4, ldr, w0
    TLIT    8, ldr, x0
    TLIT    4, ldrsw, x0
    TLIT    4, ldr, s0
    TLIT    8, ldr, d0
    TLIT    16, ldr, q0

/* ---- what src/hv/hv_xinsn.c does not take: only from another page ---- */

    T       F_OTHER|F_ALIGNED, 0, 8, 0, ldxr x0, [x2]
    T       F_OTHER|F_ALIGNED, 0, 4, 0, ldaxr w0, [x2]
    T       F_OTHER|F_ALIGNED, 0, 8, 0, ldapr x0, [x2]
    T       F_OTHER, 0, 16, 0, ld1 {v0.16b}, [x2]
    T       F_OTHER, 0, 32, 0, ld1 {v0.2d, v1.2d}, [x2]
    T       F_OTHER|F_WRITES, 0, 16, 0, st1 {v0.16b}, [x2]
    T       F_OTHER|F_ALIGNED|F_WRITES, 0, 16, 0, casp x0, x1, x4, x5, [x2]

    .globl  xi_entries_end
xi_entries_end:

/*
 * void xi_call(struct xi_regs *regs, void *code): x0 to x5 and q0 and q1 from regs, call
 * code, and the same registers back into regs.
 */
    .text
    .globl  xi_call
xi_call:
    stp     x29, x30, [sp, #-32]!
    str     x19, [sp, #16]
    mov     x19, x0
    mov     x9, x1
    ldp     q0, q1, [x19, #64]
    ldp     x4, x5, [x19, #32]
    ldp     x2, x3, [x19, #16]
    ldp     x0, x1, [x19]
    blr     x9
    stp     x0, x1, [x19]
    stp     x2, x3, [x19, #16]
    stp     x4, x5, [x19, #32]
    stp     q0, q1, [x19, #64]
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

/* void psci_system_off(void) */
    .globl  psci_system_off
psci_system_off:
    mov     x0, #0x84000000
    orr     x0, x0, #8
    hvc     #0
    ret
