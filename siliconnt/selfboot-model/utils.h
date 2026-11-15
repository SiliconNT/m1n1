/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>

#include "types.h"

/* As in src/utils.h */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#endif
