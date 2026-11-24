/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name: that of nohost-model, and what is missing */

#ifndef DCP_MODEL_UTILS_H
#define DCP_MODEL_UTILS_H

#include "types.h"

#include "../nohost-model/utils.h"

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define dprintf(...)                                                                               \
    do {                                                                                           \
    } while (0)

void hexdump(const void *d, size_t len);
u64 get_ticks(void);
u64 ticks_to_msecs(u64 ticks);
u64 top_of_memory_alloc(size_t size);

extern u32 board_id;
extern bool is_mac;

#endif
