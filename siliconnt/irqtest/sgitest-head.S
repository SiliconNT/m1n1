/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Entry points, exception vectors and the few things of sgitest that have to be exact
 * instructions. The test itself is sgitest.c.
 *
 * Raw image, position independent, MMU off, EL1h. The boot CPU enters at offset 0. Every
 * other CPU enters at the offset stored at SECONDARY_OFF, with its slot number in x0.
 * It never executes WFI: M4 loses CPU state there (src/kboot.c).
 */

#include "sgitest.h"

#define UTXH 0x20

    .section .head, "ax"

    .globl  image_start
image_start:
    b       _start
    .word   0
    .ascii  "SNTSGI01"              /* 0x08: magic */
    .globl  hdr_uart_base
hdr_uart_base:
    .quad   0                       /* 0x10: virtual UART base, from the loader */
    .globl  hdr_nr_cpus
hdr_nr_cpus:
    .quad   0                       /* 0x18: CPUs the guest runs on, from the loader */
    .quad   secondary_start - image_start /* 0x20: where the other CPUs enter */
    .globl  hdr_start_reg
hdr_start_reg:
    .fill   MAX_CPUS, 8, 0          /* 0x28: per slot, where to write to get that CPU started */
    .globl  hdr_start_val
hdr_start_val:
    .fill   MAX_CPUS, 8, 0          /*       ...and what; register 0 = it starts by itself */

    .macro  bad_vector num
    .balign 0x80
    mov     x0, #\num
    b       bad_entry
    .endm

    .balign 0x800
vectors:
    bad_vector 0                    /* EL1t */
    bad_vector 1
    bad_vector 2
    bad_vector 3
    .balign 0x80
    b       sync_entry              /* EL1h sync */
    .balign 0x80
    b       irq_entry               /* EL1h IRQ */
    bad_vector 6                    /* EL1h FIQ: the hypervisor must never send one */
    bad_vector 7                    /* EL1h SError */
    bad_vector 8                    /* lower EL */
    bad_vector 9
    bad_vector 10
    bad_vector 11
    bad_vector 12
    bad_vector 13
    bad_vector 14
    bad_vector 15
    .balign 0x80

_start:
    mov     x0, #0
secondary_start:
    msr     daifset, #0xf
    mov     x19, x0                 /* x19 = slot */
    /* Whatever ran here before may have left a timer armed */
    msr     cntv_ctl_el0, xzr
    msr     cntp_ctl_el0, xzr
    adr     x1, stack_top
    sub     x1, x1, x19, lsl #STACK_SHIFT
    mov     sp, x1
    adr     x1, vectors
    msr     vbar_el1, x1
    msr     tpidr_el1, x19
    isb
    mov     x29, #0
    mov     x0, x19
    cbnz    x19, 1f
    bl      boot_main
    b       halt
1:  bl      secondary_main
halt:
    b       halt

    .macro  save_regs
    sub     sp, sp, #176
    stp     x0, x1, [sp]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x29, [sp, #144]
    str     x30, [sp, #160]
    .endm

    .macro  restore_regs
    ldp     x0, x1, [sp]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x29, [sp, #144]
    ldr     x30, [sp, #160]
    add     sp, sp, #176
    .endm

irq_entry:
    save_regs
    bl      irq_handler
    restore_regs
    eret

/*
 * The only synchronous exception the test expects is the one for reading the write-only
 * ICC_SGI1R_EL1. sync_handler() returns where to continue, or 0 if it was not that.
 */
sync_entry:
    save_regs
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      sync_handler
    cbz     x0, 1f
    msr     elr_el1, x0
    restore_regs
    eret
1:  mov     x0, #4
bad_entry:
    bl      bad_exception
    b       halt

/* void uart_putc(int c): the store has to be one the hypervisor's MMIO emulation decodes */
    .globl  uart_putc
uart_putc:
    adr     x9, hdr_uart_base
    ldr     x9, [x9]
    str     w0, [x9, #UTXH]
    ret

/* void mmio_write32(u64 addr, u32 val) */
    .globl  mmio_write32
mmio_write32:
    str     w1, [x0]
    ret

/*
 * u64 read_sgi1r(void): MRS of the write-only ICC_SGI1R_EL1, which the assembler would refuse
 * by name. UNDEFINED; sync_handler() skips it.
 */
    .globl  read_sgi1r
read_sgi1r:
    mov     x0, #0
    mrs     x0, s3_0_c12_c11_5
    ret

    .bss
    .balign 16
    .fill   MAX_CPUS << STACK_SHIFT, 1, 0
stack_top:
