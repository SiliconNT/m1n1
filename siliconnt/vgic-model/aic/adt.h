/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, for src/aic.c: see aic_test.c */

#ifndef ADT_H
#define ADT_H
#include <stddef.h>

#include "types.h"

extern void *adt;
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
int adt_getprop_copy(const void *adt, int nodeoffset, const char *name, void *out, size_t len);
#define ADT_GETPROP(adt, nodeoffset, name, val)                                                    \
    adt_getprop_copy(adt, nodeoffset, name, (val), sizeof(*(val)))
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
#endif
