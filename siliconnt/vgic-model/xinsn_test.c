/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side test for src/hv/hv_xinsn.c, the instruction decoder of execution tracking.
 *
 * With no argument: checks of hv_xinsn_addr(), which computes where an instruction reads or
 * writes.
 *
 * With the argument "dump": reads instruction words, one in hex on a line, and writes for
 * each what hv_xinsn_decode() makes of it, or "-" if it is not an instruction the decoder
 * takes. test_xinsn.py compares that with what the disassembler of binutils says of the
 * same words.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "hv_xinsn.h"

static unsigned long checks, failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    failures++;
    printf("FAIL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int dump(void)
{
    static const char *kinds[] = {"load", "store", "atomic", "cas", "zva"};
    static const char *modes[] = {"base", "pre", "post", "reg", "lit"};
    char line[64];

    while (fgets(line, sizeof(line), stdin)) {
        u32 insn = strtoul(line, NULL, 16);
        struct hv_xinsn d;

        memset(&d, 0xa5, sizeof(d));
        if (!hv_xinsn_decode(insn, &d)) {
            printf("%08x -\n", insn);
            continue;
        }
        printf("%08x %s size=%d count=%d simd=%d sext=%d reg64=%d rt=%d rt2=%d rn=%d mode=%s "
               "imm=%ld rm=%d ext=%d shift=%d rs=%d op=%d\n",
               insn, kinds[d.kind], d.size, d.count, d.simd, d.sext, d.reg64, d.rt, d.rt2, d.rn,
               modes[d.mode], (long)d.imm, d.rm, d.ext, d.shift, d.rs, d.op);
    }
    return 0;
}

/* The address that this instruction reads or writes, with x<n> = 0x1000 * (n + 1) */
static u64 addr(u32 insn, u64 sp, u64 pc)
{
    struct hv_xinsn d;
    u64 regs[32];

    for (int i = 0; i < 32; i++)
        regs[i] = 0x1000 * (i + 1);
    if (!hv_xinsn_decode(insn, &d))
        return ~0UL;
    return hv_xinsn_addr(&d, regs, sp, pc);
}

int main(int argc, char **argv)
{
    u64 regs[32];
    struct hv_xinsn d;

    if (argc > 1 && !strcmp(argv[1], "dump"))
        return dump();

    /* x1 is 0x2000, x2 is 0x3000, sp and pc as given */
    check(addr(0xf9400420, 0, 0) == 0x2008, "ldr x0, [x1, #8]: 0x%lx", addr(0xf9400420, 0, 0));
    check(addr(0xf85f8020, 0, 0) == 0x1ff8, "ldur x0, [x1, #-8]: 0x%lx", addr(0xf85f8020, 0, 0));
    check(addr(0xf8408c20, 0, 0) == 0x2008, "ldr x0, [x1, #8]!: 0x%lx", addr(0xf8408c20, 0, 0));
    check(addr(0xf8408420, 0, 0) == 0x2000, "ldr x0, [x1], #8: 0x%lx", addr(0xf8408420, 0, 0));
    check(addr(0xf84087e0, 0x7770, 0) == 0x7770, "ldr x0, [sp], #8: 0x%lx",
          addr(0xf84087e0, 0x7770, 0));
    check(addr(0xf9400be0, 0x7770, 0) == 0x7780, "ldr x0, [sp, #16]: 0x%lx",
          addr(0xf9400be0, 0x7770, 0));
    check(addr(0xa9bf7bfd, 0x7770, 0) == 0x7760, "stp x29, x30, [sp, #-16]!: 0x%lx",
          addr(0xa9bf7bfd, 0x7770, 0));
    check(addr(0x58000100, 0, 0x40000) == 0x40020, "ldr x0, pc + 0x20: 0x%lx",
          addr(0x58000100, 0, 0x40000));
    check(addr(0x18ffffe0, 0, 0x40000) == 0x3fffc, "ldr w0, pc - 4: 0x%lx",
          addr(0x18ffffe0, 0, 0x40000));
    check(addr(0xf8626820, 0, 0) == 0x5000, "ldr x0, [x1, x2]: 0x%lx", addr(0xf8626820, 0, 0));
    check(addr(0xf8627820, 0, 0) == 0x1a000, "ldr x0, [x1, x2, lsl #3]: 0x%lx",
          addr(0xf8627820, 0, 0));
    check(addr(0xf87f6820, 0, 0) == 0x2000, "ldr x0, [x1, xzr]: 0x%lx", addr(0xf87f6820, 0, 0));
    check(addr(0xd50b7421, 0, 0) == 0x2000, "dc zva, x1: 0x%lx", addr(0xd50b7421, 0, 0));
    check(addr(0xf8200020, 0, 0) == 0x2000, "ldadd x0, x0, [x1]: 0x%lx", addr(0xf8200020, 0, 0));
    check(addr(0xc8a07c20, 0, 0) == 0x2000, "cas x0, x0, [x1]: 0x%lx", addr(0xc8a07c20, 0, 0));

    /* The extends of an offset register: x2 has a negative 32-bit value in its lower half */
    for (int i = 0; i < 32; i++)
        regs[i] = 0;
    regs[1] = 0x100000;
    regs[2] = 0xdeadbeeffffffff0UL;
    check(hv_xinsn_decode(0xf8624820, &d) && hv_xinsn_addr(&d, regs, 0, 0) == 0x1000ffff0UL,
          "ldr x0, [x1, w2, uxtw]");
    check(hv_xinsn_decode(0xf862c820, &d) && hv_xinsn_addr(&d, regs, 0, 0) == 0xffff0UL,
          "ldr x0, [x1, w2, sxtw]");
    check(hv_xinsn_decode(0xf862d820, &d) && hv_xinsn_addr(&d, regs, 0, 0) == 0xfff80UL,
          "ldr x0, [x1, w2, sxtw #3]");
    check(hv_xinsn_decode(0xf862e820, &d) &&
              hv_xinsn_addr(&d, regs, 0, 0) == 0xdeadbeeffffffff0UL + 0x100000,
          "ldr x0, [x1, x2, sxtx]");

    printf("xinsn-model: %lu checks, %lu failed\n", checks, failures);
    if (failures)
        return 1;
    printf("ALL OK\n");
    return 0;
}
