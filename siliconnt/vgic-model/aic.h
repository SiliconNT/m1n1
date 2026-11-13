/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stand-in for the m1n1 header of the same name, see test.c */

#include "types.h"
struct aic;
extern struct aic *aic;
uint32_t aic_ack(void);
