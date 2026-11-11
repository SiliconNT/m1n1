/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Runs irqtest.bin on QEMU's "virt" machine with a real (emulated) GICv3, to check the
 * guest itself before it goes anywhere near the hypervisor: `make qemu`.
 *
 * QEMU enters this at EL1. It does what the hypervisor's GIC emulation will have to do on
 * behalf of the guest (Distributor on, Redistributor awake, PPI 27 in Group 1, enabled, with
 * a priority), fills in the UART address and jumps to the image.
 */

#define GICD_BASE  0x08000000
#define GICR_BASE  0x080a0000
#define PL011_BASE 0x09000000

#define GICD_CTLR       0x0000
#define GICR_WAKER      0x0014
#define GICR_SGI_FRAME  0x10000
#define GICR_IGROUPR0   0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_IPRIORITYR 0x0400

#define VTIMER_PPI 27

    .text
    .globl  _start
_start:
    ldr     x0, =GICD_BASE
    mov     w1, #0x12               /* ARE_NS | EnableGrp1NS */
    str     w1, [x0, #GICD_CTLR]

    ldr     x0, =GICR_BASE
    ldr     w1, [x0, #GICR_WAKER]
    bic     w1, w1, #2              /* ProcessorSleep = 0 */
    str     w1, [x0, #GICR_WAKER]
1:  ldr     w1, [x0, #GICR_WAKER]
    tbnz    w1, #2, 1b              /* wait for ChildrenAsleep = 0 */

    add     x0, x0, #GICR_SGI_FRAME
    mov     w1, #0xffffffff
    str     w1, [x0, #GICR_IGROUPR0]
    mov     w1, #0x80
    strb    w1, [x0, #(GICR_IPRIORITYR + VTIMER_PPI)]
    mov     w1, #(1 << VTIMER_PPI)
    str     w1, [x0, #GICR_ISENABLER0]

    /* The guest writes characters to base + 0x20; the PL011 data register is at + 0 */
    adr     x0, guest
    ldr     x1, =(PL011_BASE - 0x20)
    str     x1, [x0, #0x10]
    br      x0

    .ltorg
    .balign 0x1000
guest:
    .incbin "build/irqtest.bin"
