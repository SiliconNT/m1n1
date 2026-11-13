/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Runs sgitest.bin on QEMU's "virt" machine with a real (emulated) GICv3, on NR_CPUS CPUs, to
 * check the guest itself before it goes anywhere near the hypervisor: `make qemu-sgi`,
 * `make qemu-sgi-smp`.
 *
 * QEMU enters this at EL1 on CPU 0 with the other CPUs off. It does what the hypervisor's GIC
 * emulation will have to do on behalf of the guest (Distributor on; per CPU: Redistributor
 * awake, the SGIs in Group 1, enabled, with a priority), fills in the header of the image,
 * starts the other CPUs through QEMU's PSCI and jumps to the image. On the hypervisor there
 * is no PSCI yet; the loader starts the other CPUs there (run_sgitest.py).
 */

#define GICD_BASE   0x08000000
#define GICR_BASE   0x080a0000
#define GICR_STRIDE 0x20000
#define PL011_BASE  0x09000000

#define GICD_CTLR       0x0000
#define GICR_WAKER      0x0014
#define GICR_SGI_FRAME  0x10000
#define GICR_IGROUPR0   0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_IPRIORITYR 0x0400

#define PSCI_CPU_ON 0xc4000003

/* Offsets in the image, see sgitest-head.S */
#define HDR_UART_BASE     0x10
#define HDR_NR_CPUS       0x18
#define HDR_SECONDARY_OFF 0x20

    .text
    .globl  _start
_start:
    ldr     x0, =GICD_BASE
    mov     w1, #0x12               /* ARE_NS | EnableGrp1NS */
    str     w1, [x0, #GICD_CTLR]

    mov     x0, #0
    bl      redist_setup

    /* The guest writes characters to base + 0x20; the PL011 data register is at + 0 */
    adr     x20, guest
    ldr     x1, =(PL011_BASE - 0x20)
    str     x1, [x20, #HDR_UART_BASE]
    mov     x1, #NR_CPUS
    str     x1, [x20, #HDR_NR_CPUS]

    /* On this machine CPU n has MPIDR_EL1.Aff0 == n, and the rest of the affinity is 0 */
    mov     x21, #1
1:  cmp     x21, #NR_CPUS
    b.hs    2f
    ldr     x0, =PSCI_CPU_ON
    mov     x1, x21                 /* target MPIDR */
    adr     x2, secondary           /* entry point */
    mov     x3, x21                 /* context: arrives in x0 */
    hvc     #0
    add     x21, x21, #1
    b       1b

2:  br      x20

/* x0 = slot, which is also the CPU number */
secondary:
    mov     x19, x0
    bl      redist_setup
    adr     x20, guest
    ldr     x1, [x20, #HDR_SECONDARY_OFF]
    add     x1, x20, x1
    mov     x0, x19
    br      x1

/* x0 = CPU number. Clobbers x0-x2 */
redist_setup:
    ldr     x1, =GICR_BASE
    mov     x2, #GICR_STRIDE
    madd    x0, x0, x2, x1
    ldr     w1, [x0, #GICR_WAKER]
    bic     w1, w1, #2              /* ProcessorSleep = 0 */
    str     w1, [x0, #GICR_WAKER]
1:  ldr     w1, [x0, #GICR_WAKER]
    tbnz    w1, #2, 1b              /* wait for ChildrenAsleep = 0 */

    add     x0, x0, #GICR_SGI_FRAME
    mov     w1, #0xffffffff
    str     w1, [x0, #GICR_IGROUPR0]
    mov     w1, #0x80808080         /* priority 0x80 for SGIs 0-15 */
    str     w1, [x0, #GICR_IPRIORITYR]
    str     w1, [x0, #(GICR_IPRIORITYR + 4)]
    str     w1, [x0, #(GICR_IPRIORITYR + 8)]
    str     w1, [x0, #(GICR_IPRIORITYR + 12)]
    mov     w1, #0xffff
    str     w1, [x0, #GICR_ISENABLER0]
    ret

    .ltorg
    .balign 0x1000
guest:
    .incbin "build/sgitest.bin"
