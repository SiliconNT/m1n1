/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, for the build machine: as src/types.h has it */

#ifndef TYPES_H
#define TYPES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define __always_unused __attribute__((__unused__))
#define __maybe_unused  __attribute__((__unused__))
#define UNUSED(x)       (void)(x)
#define ALIGNED(x)      __attribute__((aligned(x)))
#define PACKED          __attribute__((packed))

#define SZ_2K  (1 << 11)
#define SZ_4K  (1 << 12)
#define SZ_16K (1 << 14)
#define SZ_1M  (1 << 20)
#define SZ_32M (1 << 25)

#define BIT(x)                 (1UL << (x))
#define MASK(x)                (BIT(x) - 1)
#define GENMASK(msb, lsb)      ((BIT((msb + 1) - (lsb)) - 1) << (lsb))
#define _FIELD_LSB(field)      ((field) & ~(field - 1))
#define FIELD_PREP(field, val) (((val) * (_FIELD_LSB(field))) & (field))
#define FIELD_GET(field, val)  (((val) & (field)) / _FIELD_LSB(field))

#endif
