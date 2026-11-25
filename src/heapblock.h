/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HEAPBLOCK_H
#define HEAPBLOCK_H

#include "types.h"

void heapblock_init(void);

void *heapblock_alloc(size_t size);
void *heapblock_alloc_aligned(size_t size, size_t align);
void heapblock_set_limit(void *limit);
/* How many bytes are left below the limit; false, and nothing in *left, if there is none */
bool heapblock_left(size_t *left);

#endif
