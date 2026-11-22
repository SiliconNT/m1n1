/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name: a failed assertion makes m1n1 restart */

#ifndef ASSERT_H
#define ASSERT_H

#include "utils.h"

#define assert(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            debug_printf("Assertion failed: '%s' on %s:%d\n", #cond, __FILE__, __LINE__);          \
            flush_and_reboot();                                                                    \
        }                                                                                          \
    } while (0)

#endif
