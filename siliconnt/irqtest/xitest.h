/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef XITEST_H
#define XITEST_H

/* What an entry of the table in xitest-insns.S says of its instruction */
#define F_X3      1  /* x3 is an offset: the entry says what it holds */
#define F_ALIGNED 2  /* the address has to be a multiple of the size */
#define F_WRITES  4  /* it writes to memory */
#define F_CASHIT  8  /* a CAS that is to find what it expects */
#define F_LIT     16 /* a load of a literal: the address is a distance from the instruction */
#define F_OTHER   32 /* not from the page that it accesses: EL2 does not take it apart */
#define F_ZVA     64 /* DC ZVA: the address is anywhere in a block of 64 bytes */

/* Words of an entry: two before the instruction, the instruction, two behind it */
#define XI_WORDS 5
#define XI_INSN  2
#define XI_NAME  40

#endif
