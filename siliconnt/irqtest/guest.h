/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * What every guest of sgitest-head.S has: reading a system register, output on the
 * hypervisor's virtual UART, and what an exception that no handler of the guest takes prints.
 *
 * Lines start with "HVLOG: " so that the hypervisor copies them to its own console, then the
 * name of the guest: GUEST_NAME, which the guest defines before it includes this.
 */

#ifndef GUEST_H
#define GUEST_H

#include "gic.h" /* for u64 */

#ifndef GUEST_NAME
#error "define GUEST_NAME before including guest.h"
#endif

void uart_putc(int c);

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })

static inline void put(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static inline void put_hex(u64 v, int digits)
{
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        uart_putc("0123456789abcdef"[(v >> shift) & 0xf]);
}

static inline void put_dec(u64 v)
{
    char buf[20];
    int n = 0;

    do {
        buf[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}

static inline void line(const char *s)
{
    put("HVLOG: " GUEST_NAME ": ");
    put(s);
}

/*
 * sgitest-head.S calls this for an exception the guest does not handle. IRQs are masked: taking
 * the exception masked them. tpidr_el1 is the slot of the CPU.
 */
void bad_exception(u64 vector)
{
    line("EXCEPTION cpu vector ESR ELR FAR SPSR = ");
    put_hex(read_sysreg(tpidr_el1), 2);
    put(" ");
    put_hex(vector, 2);
    put(" ");
    put_hex(read_sysreg(esr_el1), 8);
    put(" ");
    put_hex(read_sysreg(elr_el1), 16);
    put(" ");
    put_hex(read_sysreg(far_el1), 16);
    put(" ");
    put_hex(read_sysreg(spsr_el1), 8);
    put("\n");
    for (;;)
        ;
}

#endif
