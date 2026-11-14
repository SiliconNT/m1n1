/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

/* Host-side stand-in for m1n1's types.h, for the vGIC model test */
#ifndef TYPES_H
#define TYPES_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define ALIGNED(x)             __attribute__((aligned(x)))
#define UNUSED(x)              (void)(x)
#define BIT(x)                 (1UL << (x))
#define MASK(x)                (BIT(x) - 1)
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~((field)-1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))
/* a system register is just a number here */
#define sys_reg(op0, op1, CRn, CRm, op2) (((op0) << 14) | ((op1) << 11) | ((CRn) << 7) | ((CRm) << 3) | (op2))
#endif
