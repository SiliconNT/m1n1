/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see run_test.c */

#include "types.h"
void *heapblock_alloc(size_t size);
void heapblock_set_limit(void *limit);
