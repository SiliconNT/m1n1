/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Entry, exception vectors and the guest image of the EL2 that runs a test guest (IMAGE) on
 * QEMU: xotest.bin under xotest-qemu.c, pmutest.bin under pmutest-el2-qemu.c.
 */

    .text
    .globl  _start
_start:
    adr     x0, shim_stack_top
    mov     sp, x0
    adr     x0, shim_vectors
    msr     vbar_el2, x0
    isb
    bl      shim_main               /* returns where the guest starts */
    msr     elr_el2, x0
    mov     x1, #0x3c5              /* EL1h, interrupts masked */
    msr     spsr_el2, x1
    mov     x0, #0
    isb
    eret

    .macro  bad_vector num
    .balign 0x80
    mov     x0, #\num
    b       bad_entry
    .endm

    .balign 0x800
shim_vectors:
    bad_vector 0
    bad_vector 1
    bad_vector 2
    bad_vector 3
    bad_vector 4
    bad_vector 5
    bad_vector 6
    bad_vector 7
    .balign 0x80
    b       lower_sync              /* synchronous, from EL1 or EL0 */
    .balign 0x80
    b       lower_irq               /* an interrupt while EL1 or EL0 ran */
    bad_vector 10
    bad_vector 11
    bad_vector 12
    bad_vector 13
    bad_vector 14
    bad_vector 15
    .balign 0x80

/* struct exc_info of xotest-qemu.c: x0 to x30, then the address to return to */
    .macro  lower_entry handler
    sub     sp, sp, #256
    stp     x0, x1, [sp]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x0, elr_el2
    stp     x30, x0, [sp, #240]
    mov     x0, sp
    bl      \handler
    ldp     x30, x0, [sp, #240]
    msr     elr_el2, x0
    ldp     x0, x1, [sp]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #256
    eret
    .endm

lower_sync:
    lower_entry shim_sync

lower_irq:
    lower_entry shim_irq

bad_entry:
    bl      shim_bad
1:  b       1b

/* u64 shim_smc(u64 x0, u64 x1, u64 x2, u64 x3): QEMU's PSCI, which is behind smc when there is an EL2 */
    .globl  shim_smc
shim_smc:
    smc     #0
    ret

    .bss
    .balign 16
    .fill   0x4000, 1, 0
shim_stack_top:

    .data
    .balign 0x1000
    .globl  guest, guest_end
guest:
    .incbin IMAGE
guest_end:
