/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

void mmu_map_framebuffer(u64 addr, size_t size);

#endif
