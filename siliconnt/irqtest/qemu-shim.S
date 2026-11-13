/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Runs irqtest.bin on QEMU's "virt" machine with a real (emulated) GICv3, to check the
 * guest itself before it goes anywhere near the hypervisor: `make qemu`.
 *
 * QEMU enters this at EL1. It does what firmware does before Windows starts: it wakes the
 * Redistributor, which Windows never does. Then it fills in the addresses of the UART and of
 * the GIC and jumps to the image.
 */

#define GICD_BASE  0x08000000
#define GICR_BASE  0x080a0000
#define PL011_BASE 0x09000000

#define GICR_WAKER 0x0014

    .text
    .globl  _start
_start:
    ldr     x0, =GICR_BASE
    ldr     w1, [x0, #GICR_WAKER]
    bic     w1, w1, #2              /* ProcessorSleep = 0 */
    str     w1, [x0, #GICR_WAKER]
1:  ldr     w1, [x0, #GICR_WAKER]
    tbnz    w1, #2, 1b              /* wait for ChildrenAsleep = 0 */

    /* The guest writes characters to base + 0x20; the PL011 data register is at + 0 */
    adr     x0, guest
    ldr     x1, =(PL011_BASE - 0x20)
    str     x1, [x0, #0x10]
    ldr     x1, =GICD_BASE
    str     x1, [x0, #0x18]
    ldr     x1, =GICR_BASE
    str     x1, [x0, #0x20]
    br      x0

    .ltorg
    .balign 0x1000
guest:
    .incbin "build/irqtest.bin"
