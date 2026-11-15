/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the two model tests of this directory share: the checks, the device tree functions of
 * src/adt.h over a device tree that a test makes (model.c), and the M4 as it was in R21.
 */

#ifndef MODEL_H
#define MODEL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "types.h"
#include "xnuboot.h"

extern int checks, failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failed++;                                                                              \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        u64 _a = (a), _b = (b);                                                                    \
        checks++;                                                                                  \
        if (_a != _b) {                                                                            \
            failed++;                                                                              \
            printf("FAILED %s:%d: %s is 0x%lx, not 0x%lx\n", __FILE__, __LINE__, #a, _a, _b);      \
        }                                                                                          \
    } while (0)

/*
 * Making a device tree in the format of Apple's: a node is two counts, its properties, its
 * children; a property is a name of 32 bytes, a length, the value padded to 4 bytes. Each
 * returns where the next thing goes.
 */
u8 *put_node(u8 *p, u32 properties, u32 children);
u8 *put_prop(u8 *p, const char *name, const void *value, u32 len);
u8 *put_name(u8 *p, const char *name);
u8 *put_range(u8 *p, const char *name, u64 base, u64 size);

/* What iBoot lists that says nothing: neither a base nor a size */
#define NOTHING 0xffffffffffffffffUL

/* The M4 of R21: what run_firmware.py printed there, and what the firmware printed it found */
#define R21_PHYS_BASE 0x100189a8000UL /* the top of the heap the host kept */
#define R21_MEM_TOP   0x103db610000UL
#define R21_IMAGE     0x1e00000UL
#define R21_DISK      0x28200000UL

/* The boot arguments m1n1 had there */
void r21_ba(struct boot_args *out);

#endif
