/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A guest that does one thing the hypervisor stops it for, and nothing else. It has the
 * header of a firmware image (selfboot_check_image() of src/selfboot.c, check_image() of
 * siliconnt/firmware/run_firmware.py), so that it is loaded the way the firmware is:
 *
 *   run_firmware.py --loader m1n1 [--no-host] --disk <any disk with a GPT> build/stoptest.bin
 *
 * What it does is the word at 0x30 of the header, STOP_ARG, 0 in the file:
 *
 *   0  hvc #1, a hypercall of Microsoft's hypervisor, which the EL2 layer does not carry out.
 *      Without a host the machine shows why on its screen and stops there (SiliconNT
 *      docs/findings/0068, R37).
 *   1  PSCI SYSTEM_OFF
 *   2  PSCI SYSTEM_RESET
 *   3  PSCI CPU_ON of a CPU that is not there: without a host the screen also has the CPUs
 *      that are
 *   4  PSCI CPU_OFF: the guest runs on no other CPU, so this is SYSTEM_OFF
 *
 * None of them returns. If one does, the guest says so with hvc #0x5155 (STOP_RETURNED),
 * which nobody carries out either: the QEMU shell of src/hv/hv_stop.c (nohost-qemu.c) fails
 * on it, and on the machine it stops the guest a second time, with its own line.
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

    .text
    .globl  _start
_start:
    adr     x1, .                       /* 0x00: the entry code of the firmware has this */
    b       entry                       /* 0x04 */
    .quad   0                           /* 0x08: text offset */
    .quad   image_end - _start          /* 0x10: image size */
    .quad   0                           /* 0x18: flags */
    .quad   0                           /* 0x20 */
    .quad   0                           /* 0x28 */
stop_arg:
    .quad   0                           /* 0x30: STOP_ARG */
    .ascii  "ARM\x64"                   /* 0x38: magic */
    .word   0                           /* 0x3c */

entry:
    /* Registers that are easy to find on the screen */
    ldr     x19, =0x1919191919191919
    ldr     x20, =0x2020202020202020
    ldr     x2, stop_arg
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

6:  ldr     x0, =PSCI_CPU_OFF
    hvc     #0
    b       4b

    .ltorg
    /* The image is a whole number of 16 KB pages */
    .balign 0x4000
image_end:
