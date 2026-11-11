/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * EL1 test guest for src/hv/hv_vgic.c: does a virtual timer interrupt, put into a list
 * register by the hypervisor, really arrive at EL1 as an IRQ exception?
 *
 * It sets up the GIC CPU interface the way the Windows kernel does on every processor
 * (HalpGic3InitializeLocalUnit, 10.0.26300.9457), arms the EL1 virtual timer, takes
 * NR_IRQS interrupts through ICC_IAR1_EL1 / ICC_EOIR1_EL1 and reports what it saw on the
 * hypervisor's virtual UART. Lines start with "HVLOG: " so that the hypervisor copies them
 * to its own console (src/hv/hv_vuart.c).
 *
 * Raw image, position independent, entered at offset 0 in EL1h with the MMU off. It never
 * executes WFI: M4 loses CPU state there (src/kboot.c).
 *
 * The loader (run_irqtest.py) writes the address of the virtual UART into the header.
 */

#define UTXH 0x20

/* More than twice the largest possible number of list registers (16) */
#define NR_IRQS  40
#define TIMER_HZ 20
/* Give up when this many timer periods have passed */
#define TIMEOUT_PERIODS (NR_IRQS * 4)

#define REC_SHIFT 5 /* 32 bytes per record */

    .text

    .globl  _start
image_start:
    b       _start
    .word   0
    .ascii  "SNTIRQ01"              /* 0x08: magic */
uart_base:
    .quad   0                       /* 0x10: virtual UART base, filled in by the loader */

    .macro  bad_vector num
    .balign 0x80
    mov     x0, #\num
    b       bad_exception
    .endm

    .balign 0x800
vectors:
    bad_vector 0                    /* EL1t */
    bad_vector 1
    bad_vector 2
    bad_vector 3
    bad_vector 4                    /* EL1h sync */
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
    msr     daifset, #0xf
    msr     cntv_ctl_el0, xzr
    msr     cntp_ctl_el0, xzr
    adr     x0, stack_top
    mov     sp, x0
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    adr     x0, msg_start
    bl      puts
    mrs     x0, CurrentEL
    bl      put_hex8
    adr     x0, msg_sre
    bl      puts
    mrs     x0, ICC_SRE_EL1
    bl      put_hex8
    bl      put_nl

    /* The order and the values of HalpGic3InitializeLocalUnit */
    mrs     x8, ICC_SRE_EL1
    tbnz    w8, #0, 1f
    orr     x8, x8, #1
    msr     ICC_SRE_EL1, x8
1:  isb
    msr     ICC_IGRPEN1_EL1, xzr
    msr     ICC_PMR_EL1, xzr
    mov     x8, #3
    msr     ICC_BPR1_EL1, x8
    /* (Windows writes the Redistributor here; there is none yet) */
    mrs     x8, ICC_CTLR_EL1
    bic     x8, x8, #2              /* EOImode = 0 */
    msr     ICC_CTLR_EL1, x8
    mov     x8, #1
    msr     ICC_IGRPEN1_EL1, x8
    /* HalpGic3SetPriority at the lowest level: 0xF0 */
    mov     x8, #0xf0
    msr     ICC_PMR_EL1, x8
    isb

    adr     x0, msg_icc
    bl      puts
    mrs     x0, ICC_CTLR_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_PMR_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_BPR1_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_IGRPEN1_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_RPR_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_HPPIR1_EL1
    bl      put_hex8
    bl      put_nl

    /* Arm the EL1 virtual timer, TIMER_HZ times per second */
    mrs     x9, cntfrq_el0
    mov     x10, #TIMER_HZ
    udiv    x21, x9, x10            /* x21 = period in ticks */
    adr     x10, period
    str     x21, [x10]
    mrs     x22, cntvct_el0         /* x22 = start time */
    add     x11, x22, x21
    msr     cntv_cval_el0, x11
    mov     x8, #1                  /* ENABLE, not masked */
    msr     cntv_ctl_el0, x8
    isb

    adr     x0, msg_armed
    bl      puts
    mov     x0, x21
    bl      put_hex16
    bl      put_nl

    msr     daifclr, #7             /* SError, IRQ and FIQ on */

    mov     x19, #0                 /* x19 = records printed */
main_loop:
    adr     x0, irq_count
    ldr     x20, [x0]
    cmp     x19, x20
    b.hs    2f

    /* One line per interrupt taken */
    adr     x0, msg_irq
    bl      puts
    mov     x0, x19
    bl      put_hex8
    adr     x24, records
    add     x24, x24, x19, lsl #REC_SHIFT
    bl      put_sp
    ldr     x0, [x24]               /* ICC_IAR1_EL1 */
    bl      put_hex8
    bl      put_sp
    ldr     x0, [x24, #8]           /* ICC_RPR_EL1 while active */
    bl      put_hex8
    bl      put_sp
    ldr     x0, [x24, #24]          /* SPSR_EL1 */
    bl      put_hex8
    bl      put_sp
    ldr     x0, [x24, #16]          /* CNTVCT_EL0 */
    sub     x0, x0, x22
    bl      put_hex16
    bl      put_nl
    add     x19, x19, #1
    b       main_loop

2:  cmp     x19, #NR_IRQS
    b.hs    done

    mrs     x0, cntvct_el0
    sub     x0, x0, x22
    mov     x1, #TIMEOUT_PERIODS
    mul     x1, x1, x21
    cmp     x0, x1
    b.lo    main_loop

    /* Timed out: say what the CPU interface and the timer look like */
    msr     daifset, #0xf
    adr     x0, msg_timeout
    bl      puts
    mov     x0, x19
    bl      put_hex8
    bl      put_sp
    mrs     x0, isr_el1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_HPPIR1_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_RPR_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, cntv_ctl_el0
    bl      put_hex8
    bl      put_nl
    b       halt

done:
    msr     daifset, #0xf
    adr     x0, msg_done
    bl      puts
    mov     x0, x19
    bl      put_hex8
    bl      put_sp
    adr     x0, spurious_count
    ldr     x0, [x0]
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_RPR_EL1
    bl      put_hex8
    bl      put_sp
    mrs     x0, ICC_HPPIR1_EL1
    bl      put_hex8
    bl      put_nl

halt:
    b       halt

/*
 * IRQ from EL1h: what HalpGic3AcceptAndGetSource and HalpGic3WriteEndOfInterrupt do, with
 * the timer moved on in between so that its output is low again before the EOI.
 */
irq_entry:
    sub     sp, sp, #48
    stp     x0, x1, [sp]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]

    mrs     x0, ICC_IAR1_EL1
    mov     w1, #1023
    cmp     w0, w1
    b.eq    3f

    mrs     x2, ICC_RPR_EL1
    adr     x3, irq_count
    ldr     x4, [x3]
    cmp     x4, #NR_IRQS
    b.hs    1f
    adr     x5, records
    add     x5, x5, x4, lsl #REC_SHIFT
    str     x0, [x5]
    str     x2, [x5, #8]
    mrs     x1, cntvct_el0
    str     x1, [x5, #16]
    mrs     x1, spsr_el1
    str     x1, [x5, #24]
1:  add     x4, x4, #1
    str     x4, [x3]

    cmp     x4, #NR_IRQS
    b.hs    2f
    mrs     x1, cntv_cval_el0
    adr     x2, period
    ldr     x2, [x2]
    add     x1, x1, x2
    msr     cntv_cval_el0, x1
    b       4f
2:  msr     cntv_ctl_el0, xzr       /* last one: timer off */
4:  isb
    msr     ICC_EOIR1_EL1, x0
    b       5f

3:  adr     x3, spurious_count      /* 1023: nothing to acknowledge, no EOI */
    ldr     x4, [x3]
    add     x4, x4, #1
    str     x4, [x3]

5:  ldp     x0, x1, [sp]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    add     sp, sp, #48
    eret

/* Any other exception: report it and stop. x0 = vector number */
bad_exception:
    mov     x19, x0
    adr     x0, msg_exc
    bl      puts
    mov     x0, x19
    bl      put_hex8
    bl      put_sp
    mrs     x0, esr_el1
    bl      put_hex8
    bl      put_sp
    mrs     x0, elr_el1
    bl      put_hex16
    bl      put_sp
    mrs     x0, far_el1
    bl      put_hex16
    bl      put_sp
    mrs     x0, spsr_el1
    bl      put_hex8
    bl      put_nl
    b       halt

/* w0 = character. Clobbers x9 */
putc:
    adr     x9, uart_base
    ldr     x9, [x9]
    str     w0, [x9, #UTXH]
    ret

put_sp:
    mov     w0, #0x20
    b       putc

put_nl:
    mov     w0, #0x0a
    b       putc

/* x0 = NUL-terminated string. Clobbers x0, x9, x10, x11 */
puts:
    mov     x10, x30
    mov     x11, x0
1:  ldrb    w0, [x11], #1
    cbz     w0, 2f
    bl      putc
    b       1b
2:  ret     x10

/* x0 = value, printed as 8 or 16 hex digits. Clobbers x0, x9-x14 */
put_hex8:
    mov     x12, #32
    b       put_hex
put_hex16:
    mov     x12, #64
put_hex:
    mov     x10, x30
    mov     x11, x0
1:  sub     x12, x12, #4
    lsr     x0, x11, x12
    and     x0, x0, #0xf
    add     x13, x0, #0x30          /* '0' */
    add     x14, x0, #0x57          /* 'a' - 10 */
    cmp     x0, #10
    csel    x0, x13, x14, lo
    bl      putc
    cbnz    x12, 1b
    ret     x10

msg_start:
    .asciz  "HVLOG: irqtest: start, CurrentEL="
msg_sre:
    .asciz  " ICC_SRE_EL1="
msg_icc:
    .asciz  "HVLOG: irqtest: CTLR PMR BPR1 IGRPEN1 RPR HPPIR1 = "
msg_armed:
    .asciz  "HVLOG: irqtest: timer armed, period="
msg_irq:
    .asciz  "HVLOG: irqtest: irq n IAR1 RPR SPSR time = "
msg_done:
    .asciz  "HVLOG: irqtest: DONE irqs spurious RPR HPPIR1 = "
msg_timeout:
    .asciz  "HVLOG: irqtest: TIMEOUT irqs ISR_EL1 HPPIR1 RPR CNTV_CTL = "
msg_exc:
    .asciz  "HVLOG: irqtest: EXCEPTION vector ESR ELR FAR SPSR = "

    .balign 16
period:
    .quad   0
irq_count:
    .quad   0
spurious_count:
    .quad   0
    .balign 32
records:
    .fill   NR_IRQS << REC_SHIFT, 1, 0

    .balign 16
    .fill   0x2000, 1, 0
stack_top:
