/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, for src/aic.c: see aic_test.c */

#ifndef ASSERT_H
#define ASSERT_H
#include <stdlib.h>

#define assert(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("CHECK FAILED: assert(%s) in aic.c\n", #cond);                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#endif
