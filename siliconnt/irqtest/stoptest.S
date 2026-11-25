/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A guest that does one thing the hypervisor stops it for, and nothing else. It has the
 * header of a firmware image (selfboot_check_image() of src/selfboot.c, check_image() of
 * siliconnt/firmware/run_firmware.py), so that it is loaded the way the firmware is:
 *
 *   run_firmware.py --loader m1n1 [--no-host] --disk <any disk with a GPT> build/stoptest.bin
 *
 * What it does is the word at 0x30 of the header, STOP_ARG: 0 in build/stoptest.bin, the
 * number after the dash in build/stoptest-<STOP_ARG>.bin. Bits 7:0:
 *
 *   0  hvc #1, a hypercall of Microsoft's hypervisor, which the EL2 layer does not carry out.
 *      Without a host the machine shows why on its screen and stops there (SiliconNT
 *      docs/findings/0068, R37).
 *   1  PSCI SYSTEM_OFF
 *   2  PSCI SYSTEM_RESET
 *   3  PSCI CPU_ON of a CPU that is not there: without a host the screen also has the CPUs
 *      that are
 *   4  PSCI CPU_OFF: on the only CPU of the guest, or, with bit 8, on every CPU, one after the
 *      other; the last of them is SYSTEM_OFF
 *
 * Bit 8: first turn on, with PSCI CPU_ON, every other CPU that has a Redistributor, and wait
 * until each is there, so that the call is made with all of them in the guest (R51). They spin,
 * or, for 4, turn themselves off once the boot CPU has said so; the boot CPU is the last.
 * Where the Redistributors are is the word at 0x28, GICR_BASE: 0 for where the hypervisor has
 * them (HV_VGIC_REDIST_BASE of src/hv/hv.h).
 *
 * None of them returns. If one does, or a CPU_ON of bit 8 is not answered with 0, or a CPU does
 * not come, the guest says so with hvc #0x5155 (STOP_RETURNED), which nobody carries out
 * either: on the machine it stops the guest a second time, with its own line. x21 is then
 * what went wrong (see below), x22 the MPIDR it is about, x23 the answer of CPU_ON.
 *
 * It touches no device, the UART least of all: without a host the guest has none. MMU off,
 * EL1h, at an address it does not know.
 */

#define PSCI_SYSTEM_OFF   0x84000008
#define PSCI_SYSTEM_RESET 0x84000009
#define PSCI_CPU_OFF      0x84000002
#define PSCI_CPU_ON       0xc4000003
#define NO_MPIDR          0x10104
#define STOP_RETURNED     0x5155

#define ALL_CPUS     0x100
#define REDIST_BASE  0x5100000000 /* HV_VGIC_REDIST_BASE */
#define GICR_STRIDE  0x20000
#define GICR_TYPER   0x0008
#define TYPER_LAST   (1 << 4)
#define MAX_OTHERS   16
#define ARRIVE_TICKS (24000000 / 10) /* 100 ms at the M4's 24 MHz, longer on a faster counter */

/* x21 when it says STOP_RETURNED */
#define WHAT_RETURNED  0 /* the call came back */
#define WHAT_CPU_ON    1 /* CPU_ON of another CPU answered x23 */
#define WHAT_NOT_THERE 2 /* that CPU did not come */

#ifndef STOP_ARG_DEFAULT
#define STOP_ARG_DEFAULT 0
#endif
#ifndef GICR_BASE_DEFAULT
#define GICR_BASE_DEFAULT 0
#endif

    .text
    .globl  _start
_start:
    adr     x1, .                       /* 0x00: the entry code of the firmware has this */
    b       entry                       /* 0x04 */
    .quad   0                           /* 0x08: text offset */
    .quad   image_end - _start          /* 0x10: image size */
    .quad   0                           /* 0x18: flags */
    .quad   0                           /* 0x20 */
gicr_base:
    .quad   GICR_BASE_DEFAULT           /* 0x28: GICR_BASE */
stop_arg:
    .quad   STOP_ARG_DEFAULT            /* 0x30: STOP_ARG */
    .ascii  "ARM\x64"                   /* 0x38: magic */
    .word   0                           /* 0x3c */

entry:
    /* Registers that are easy to find on the screen */
    ldr     x19, =0x1919191919191919
    ldr     x20, =0x2020202020202020
    mov     x21, #WHAT_RETURNED
    mov     x22, #0
    mov     x23, #0
    ldr     x2, stop_arg
    tbz     x2, #8, 7f
    bl      all_on
    ldr     x2, stop_arg
7:  and     x2, x2, #0xff
    cbz     x2, 1f
    cmp     x2, #1
    b.eq    2f
    cmp     x2, #2
    b.eq    3f
    cmp     x2, #3
    b.eq    5f
    cmp     x2, #4
    b.eq    6f
    b       .

1:  ldr     x0, =0x0001000100010001    /* what a hypercall input value could look like */
    hvc     #1
    b       4f

2:  ldr     x0, =PSCI_SYSTEM_OFF
    hvc     #0
    b       4f

3:  ldr     x0, =PSCI_SYSTEM_RESET
    hvc     #0

4:  hvc     #STOP_RETURNED
    b       .

5:  ldr     x0, =PSCI_CPU_ON
    ldr     x1, =NO_MPIDR
    adr     x2, _start
    mov     x3, #0
    hvc     #0
    b       4b

    /* The others first, if they are on: the boot CPU is the last to go */
6:  adr     x1, go_off
    mov     x0, #1
    str     x0, [x1]
    dsb     sy
    ldr     x0, =PSCI_CPU_OFF
    hvc     #0
    b       4b

/*
 * Every CPU with a Redistributor but this one: CPU_ON, with its number in the order of the
 * frames as the context, then wait until it has written its byte of `arrived`. Clobbers x0-x18.
 */
all_on:
    mov     x18, x30
    ldr     x10, gicr_base
    cbnz    x10, 1f
    ldr     x10, =REDIST_BASE
1:  mrs     x11, mpidr_el1
    ldr     x9, =0xff00ffffff
    and     x11, x11, x9
    mov     x12, #0                     /* number of the next one */
2:  ldr     x13, [x10, #GICR_TYPER]
    lsr     x14, x13, #32
    and     x14, x14, #0xffffff         /* Aff2-Aff0 */
    lsr     x15, x13, #56
    orr     x14, x14, x15, lsl #32      /* Aff3 */
    cmp     x14, x11
    b.eq    3f
    cmp     x12, #MAX_OTHERS
    b.hs    3f
    ldr     x0, =PSCI_CPU_ON
    mov     x1, x14
    adr     x2, other_entry
    mov     x3, x12
    hvc     #0
    mov     x23, x0
    mov     x22, x14
    mov     x21, #WHAT_CPU_ON
    cbnz    x0, 9f
    /* Wait until it is there */
    mrs     x16, cntvct_el0
    ldr     x9, =ARRIVE_TICKS
    add     x16, x16, x9
    adr     x15, arrived
4:  ldrb    w9, [x15, x12]
    cbnz    w9, 5f
    mrs     x9, cntvct_el0
    cmp     x9, x16
    b.lo    4b
    mov     x21, #WHAT_NOT_THERE
    b       9f
5:  add     x12, x12, #1
3:  tbnz    x13, #4, 6f                 /* GICR_TYPER.Last */
    ldr     x9, =GICR_STRIDE
    add     x10, x10, x9
    b       2b
6:  mov     x21, #WHAT_RETURNED
    mov     x22, #0
    mov     x23, #0
    mov     x30, x18
    ret
9:  hvc     #STOP_RETURNED
    b       .

/* Another CPU, x0 = its number. It spins, or turns itself off once go_off is 1 */
other_entry:
    msr     daifset, #0xf
    adr     x1, arrived
    mov     w2, #1
    strb    w2, [x1, x0]
    dsb     sy
    adr     x1, stop_arg
    ldr     x2, [x1]
    and     x2, x2, #0xff
    cmp     x2, #4
    b.ne    2f
    adr     x1, go_off
1:  ldr     x2, [x1]
    cbz     x2, 1b
    ldr     x0, =PSCI_CPU_OFF
    hvc     #0
    hvc     #STOP_RETURNED
2:  b       .

    .ltorg
    .balign 8
go_off:
    .quad   0
arrived:
    .fill   MAX_OTHERS, 1, 0
    /* The image is a whole number of 16 KB pages */
    .balign 0x4000
image_end:
