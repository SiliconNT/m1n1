/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* The consoles take no lock here: the tests have one core */
static inline bool mmu_active(void)
{
    return false;
}

#define MAIR_IDX_NORMAL_NC 2
#define PERM_RW            0x40
void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attribute_index, u64 perms);

#endif
