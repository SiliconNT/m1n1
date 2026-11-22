/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name: the heap is the test's (model.h) */

#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

void *model_malloc(size_t size);
void *model_calloc(size_t n, size_t size);
void *model_memalign(size_t align, size_t size);
void model_free(void *p);
#define malloc(s)      model_malloc(s)
#define calloc(n, s)   model_calloc(n, s)
#define memalign(a, s) model_memalign(a, s)
#define free(p)        model_free(p)

#endif
