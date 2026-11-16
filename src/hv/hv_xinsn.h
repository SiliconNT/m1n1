/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HV_XINSN_H
#define HV_XINSN_H

#include "types.h"

/*
 * What an instruction that reads or writes memory does, as far as EL2 has to know to carry it
 * out in the guest's place: see hv_xinsn.c for which instructions, hv_xtrack.c for the why.
 */

enum hv_xinsn_kind {
    XI_LOAD,   // memory to registers
    XI_STORE,  // registers to memory
    XI_ATOMIC, // LD<op> and SWP: the old value to Rt, the old value <op> Rs to memory
    XI_CAS,    // CAS: Rt to memory if the old value is Rs; the old value to Rs
    XI_ZVA,    // DC ZVA: zeroes to the block that the address in Rt is in
};

enum hv_xinsn_mode {
    XI_BASE,    // [Rn, #imm]
    XI_PRE,     // [Rn, #imm]!
    XI_POST,    // [Rn], #imm
    XI_REG,     // [Rn, Rm, <extend> #shift]
    XI_LITERAL, // the address of the instruction + imm
};

/* XI_ATOMIC: what is done with the old value and Rs. The first eight as in the encoding */
enum hv_xinsn_op {
    XI_OP_ADD,
    XI_OP_CLR,
    XI_OP_EOR,
    XI_OP_SET,
    XI_OP_SMAX,
    XI_OP_SMIN,
    XI_OP_UMAX,
    XI_OP_UMIN,
    XI_OP_SWP,
};

struct hv_xinsn {
    u8 kind;
    u8 mode;
    u8 size;  // bytes that one register takes or gives: 1, 2, 4, 8, 16
    u8 count; // registers: 1, or 2 for a pair (Rt at the lower address)
    u8 rt;
    u8 rt2;
    u8 rn;      // the base: 31 is the stack pointer
    u8 rm;      // XI_REG: the offset register, 31 is the zero register
    u8 ext;     // XI_REG: the option field: 2 UXTW, 3 LSL, 6 SXTW, 7 SXTX
    u8 shift;   // XI_REG: the offset is shifted left by this
    u8 rs;      // XI_ATOMIC, XI_CAS
    u8 op;      // XI_ATOMIC
    bool simd;  // Rt and Rt2 are SIMD and floating-point registers
    bool sext;  // XI_LOAD: sign-extended
    bool reg64; // XI_LOAD of general-purpose registers: into all 64 bits (X), not 32 (W)
    s64 imm;
};

bool hv_xinsn_decode(u32 insn, struct hv_xinsn *d);
u64 hv_xinsn_addr(const struct hv_xinsn *d, const u64 *regs, u64 sp, u64 pc);

#endif
