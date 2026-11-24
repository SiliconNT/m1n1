/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in: the files under test are linked whole, and nothing here calls these */

#ifndef LIBFDT_H
#define LIBFDT_H

#include <stdint.h>

typedef uint32_t fdt32_t;
typedef uint64_t fdt64_t;
#define cpu_to_fdt32(x) __builtin_bswap32(x)

int fdt_setprop(void *fdt, int node, const char *name, const void *val, int len);
int fdt_node_offset_by_phandle(const void *fdt, uint32_t phandle);
int fdt_node_check_compatible(const void *fdt, int node, const char *compatible);
const char *fdt_get_name(const void *fdt, int node, int *lenp);

#endif
