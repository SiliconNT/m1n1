/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The page of code that xtstress.c has several cores execute while another one writes it and
 * reads it (SiliconNT R51), and the one load that C cannot write here.
 *
 * .hot is a page of its own (xtstress.ld). It holds hot_loop, which the executing cores never
 * leave until they are told to stop; one access to a PMUv3 register that is never executed,
 * so that execution tracking replaces it in a shadow page and the page is execute-only while it
 * is code (src/hv/hv_xtrack.c); and the words that the writing core stores to and reads.
 */

#define HOT_WORDS 32

    .section .hot, "ax"
    .balign 64

/* void hot_loop(volatile u64 *count, volatile u64 *stop): counts until *stop is not 0 */
    .globl  hot_loop
hot_loop:
1:  ldr     x2, [x0]
    add     x2, x2, #1
    str     x2, [x0]
    ldr     x3, [x1]
    cbz     x3, 1b
    ret

    .balign 64
/* Never executed; read by the writing core, which has to get this word and not what replaced it */
    .globl  hot_pmu
hot_pmu:
    mrs     x9, pmccntr_el0
    ret
    .fill   2, 4, 0

    .balign 64
    .globl  hot_data
hot_data:
    .fill   HOT_WORDS, 8, 0

    .text

/*
 * u64 ld1_pair(const volatile void *p, u64 *hi): one LD1 of the 16 bytes at p, which execution
 * tracking does not take apart: on a page of code it has the page become data first. Returns
 * the low 8 bytes, the high 8 go to *hi. FP/SIMD has to be on (CPACR_EL1.FPEN).
 */
    .globl  ld1_pair
ld1_pair:
    ld1     {v0.16b}, [x0]
    mov     x2, v0.d[1]
    str     x2, [x1]
    mov     x0, v0.d[0]
    ret
