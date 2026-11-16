/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The loads and stores that execution tracking carries out in the guest's place (hv_xtrack.c),
 * taken apart: which registers, how many bytes, at which address.
 *
 * ESR_EL2 says that much itself for a load or store of one general-purpose register without
 * writeback (Arm DDI 0487 M.d, D24.2.45, ISV). This is for everything else, and for those too
 * when the address that the core reports cannot be relied on. What is taken:
 *
 *  - one general-purpose register: LDR, STR, the byte, halfword and sign-extending forms, with
 *    an unsigned or unscaled offset, with pre- or post-index writeback, with a register
 *    offset, and LDR and LDRSW of a literal; LDAR and STLR;
 *  - a pair of them: LDP, STP, LDNP, STNP of 32 or 64 bits and LDPSW, with or without writeback;
 *  - one SIMD and floating-point register or a pair, in the same forms;
 *  - the atomic operations LD<op> and SWP, and CAS, of 1, 2, 4 and 8 bytes;
 *  - DC ZVA.
 *
 * And what is not: the exclusives (LDXR, STXR and their kin: an exclusive pair that EL2 comes
 * between is no longer one), CASP, LDAPR and the other acquire and release forms, the
 * unprivileged LDTR and STTR, the loads with pointer authentication, the tagged stores, the
 * structure loads and stores LD1 to LD4 and ST1 to ST4, the memory copy and set instructions,
 * and all of SVE and SME. A prefetch is not a load. Nor is anything taken that the
 * architecture calls CONSTRAINED UNPREDICTABLE.
 *
 * The encodings are those of Arm DDI 0487 M.d, C4.1 "A64 instruction set encoding", the group
 * "Loads and Stores", bits 27 and 25 of the word being 1 and 0. siliconnt/vgic-model/test_xinsn.py
 * compares this decoder with the disassembler of binutils, word by word: whatever is taken here is
 * what that says it is.
 *
 * Nothing here touches the guest or the hardware.
 */

#include "hv_xinsn.h"

static s64 sext(u64 val, int bits)
{
    return (s64)(val << (64 - bits)) >> (64 - bits);
}

/* The size and the direction of a load or store of one register: opc is bits 23:22 */
static bool xinsn_single(u32 insn, struct hv_xinsn *d)
{
    u32 size = insn >> 30;
    u32 opc = (insn >> 22) & 3;

    d->count = 1;
    d->size = 1 << size;

    if (insn & (1 << 26)) {
        d->simd = true;
        /* B, H, S, D by size; Q has size 0 and the upper bit of opc */
        if (opc & 2) {
            if (size)
                return false;
            d->size = 16;
        }
        d->kind = opc & 1 ? XI_LOAD : XI_STORE;
        return true;
    }

    switch (opc) {
        case 0:
            d->kind = XI_STORE;
            return true;
        case 1:
            d->kind = XI_LOAD;
            d->reg64 = size == 3;
            return true;
        case 2:
            /* Sign-extended into 64 bits. With size 3 this is a prefetch */
            if (size == 3)
                return false;
            d->kind = XI_LOAD;
            d->sext = true;
            d->reg64 = true;
            return true;
        default:
            /* Sign-extended into 32 bits: a byte or a halfword */
            if (size >= 2)
                return false;
            d->kind = XI_LOAD;
            d->sext = true;
            return true;
    }
}

static int log2_size(u32 size)
{
    return size == 16 ? 4 : size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
}

static bool xinsn_decode(u32 insn, struct hv_xinsn *d);

bool hv_xinsn_decode(u32 insn, struct hv_xinsn *d)
{
    if (!xinsn_decode(insn, d))
        return false;
    if (d->kind != XI_LOAD && d->kind != XI_STORE)
        return true;

    /*
     * What the architecture leaves open (CONSTRAINED UNPREDICTABLE) is not carried out for the
     * guest: a pair loaded into one register twice, and writeback to a base that is also
     * loaded or stored.
     */
    if (d->kind == XI_LOAD && d->count == 2 && d->rt == d->rt2)
        return false;
    if ((d->mode == XI_PRE || d->mode == XI_POST) && !d->simd && d->rn != 31 &&
        (d->rn == d->rt || (d->count == 2 && d->rn == d->rt2)))
        return false;
    return true;
}

static bool xinsn_decode(u32 insn, struct hv_xinsn *d)
{
    u32 opc = insn >> 30;
    bool v = insn & (1 << 26);

    d->kind = XI_LOAD;
    d->mode = XI_BASE;
    d->size = 0;
    d->count = 1;
    d->rt = insn & 31;
    d->rt2 = 0;
    d->rn = (insn >> 5) & 31;
    d->rm = 0;
    d->ext = 0;
    d->shift = 0;
    d->rs = 0;
    d->op = 0;
    d->simd = false;
    d->sext = false;
    d->reg64 = false;
    d->imm = 0;

    /* DC ZVA, Xt: SYS #3, C7, C4, #1 */
    if ((insn & 0xffffffe0) == 0xd50b7420) {
        d->kind = XI_ZVA;
        d->rn = 0;
        return true;
    }

    if ((insn & 0x0a000000) != 0x08000000)
        return false;

    switch ((insn >> 28) & 3) {
        case 0:
            /* With V, the structure loads and stores of SIMD. Bit 24 set: nothing allocated */
            if (v || (insn & (1 << 24)))
                return false;

            /* LDAR and STLR: o2 = 1, o1 = 0, o0 = 1, Rs and Rt2 all ones */
            if ((insn & 0x3fbffc00) == 0x089ffc00) {
                d->size = 1 << opc;
                if (insn & (1 << 22)) {
                    d->kind = XI_LOAD;
                    d->reg64 = opc == 3;
                } else {
                    d->kind = XI_STORE;
                }
                return true;
            }

            /* CAS: o2 = 1, o1 = 1, Rt2 all ones */
            if ((insn & 0x3fa07c00) == 0x08a07c00) {
                d->kind = XI_CAS;
                d->size = 1 << opc;
                d->rs = (insn >> 16) & 31;
                return true;
            }
            return false;

        case 1:
            /* A literal. Bit 24 set: LDAPUR and STLUR, memory copy and set */
            if (insn & (1 << 24))
                return false;

            d->kind = XI_LOAD;
            d->mode = XI_LITERAL;
            d->rn = 0;
            d->imm = sext((insn >> 5) & 0x7ffff, 19) * 4;
            if (v) {
                if (opc == 3)
                    return false;
                d->simd = true;
                d->size = 4 << opc;
                return true;
            }
            switch (opc) {
                case 0:
                    d->size = 4;
                    return true;
                case 1:
                    d->size = 8;
                    d->reg64 = true;
                    return true;
                case 2:
                    /* LDRSW */
                    d->size = 4;
                    d->sext = true;
                    d->reg64 = true;
                    return true;
                default:
                    /* PRFM */
                    return false;
            }

        case 2: {
            /* A pair: bits 24:23 are 0 no-allocate, 1 post-index, 2 offset, 3 pre-index */
            u32 how = (insn >> 23) & 3;
            bool load = insn & (1 << 22);

            d->kind = load ? XI_LOAD : XI_STORE;
            d->mode = how == 1 ? XI_POST : how == 3 ? XI_PRE : XI_BASE;
            d->count = 2;
            d->rt2 = (insn >> 10) & 31;

            if (opc == 3)
                return false;
            if (v) {
                d->simd = true;
                d->size = 4 << opc;
            } else if (opc == 1) {
                /* LDPSW. As a store this is STGP, and no-allocate it is nothing */
                if (!load || how == 0)
                    return false;
                d->size = 4;
                d->sext = true;
                d->reg64 = true;
            } else {
                d->size = opc ? 8 : 4;
                d->reg64 = load && opc;
            }
            d->imm = sext((insn >> 15) & 0x7f, 7) * d->size;
            return true;
        }

        default:
            break;
    }

    /* One register */
    if (insn & (1 << 24)) {
        /* Unsigned offset, in units of the size */
        if (!xinsn_single(insn, d))
            return false;
        d->imm = (u64)((insn >> 10) & 0xfff) << log2_size(d->size);
        return true;
    }

    if (!(insn & (1 << 21))) {
        /* A 9-bit offset: bits 11:10 are 0 unscaled, 1 post-index, 2 unprivileged, 3 pre-index */
        u32 how = (insn >> 10) & 3;

        if (how == 2)
            return false;
        if (!xinsn_single(insn, d))
            return false;
        d->mode = how == 1 ? XI_POST : how == 3 ? XI_PRE : XI_BASE;
        d->imm = sext((insn >> 12) & 0x1ff, 9);
        return true;
    }

    switch ((insn >> 10) & 3) {
        case 0:
            /* Atomic memory operations: bit 15 is o3, bits 14:12 the operation */
            if (v)
                return false;
            d->kind = XI_ATOMIC;
            d->size = 1 << opc;
            d->rs = (insn >> 16) & 31;
            d->op = (insn >> 12) & 7;
            if (insn & (1 << 15)) {
                /* SWP. The others with o3 are LDAPR and the 64-byte loads and stores */
                if (d->op)
                    return false;
                d->op = XI_OP_SWP;
            }
            return true;

        case 2:
            /* A register offset: bits 15:13 say how it is extended, bit 12 whether it is scaled */
            if (!xinsn_single(insn, d))
                return false;
            d->mode = XI_REG;
            d->rm = (insn >> 16) & 31;
            d->ext = (insn >> 13) & 7;
            if (!(d->ext & 2))
                return false;
            d->shift = insn & (1 << 12) ? log2_size(d->size) : 0;
            return true;

        default:
            /* LDRAA and LDRAB */
            return false;
    }
}

/*
 * The address that the instruction reads or writes. regs are x0 to x30 of the guest, sp its
 * stack pointer, pc where the instruction is. For XI_ZVA, the address in the register: the
 * block is the caller's to find. Writeback, where there is any (XI_PRE, XI_POST), adds
 * d->imm to the base.
 */
u64 hv_xinsn_addr(const struct hv_xinsn *d, const u64 *regs, u64 sp, u64 pc)
{
    if (d->kind == XI_ZVA)
        return d->rt == 31 ? 0 : regs[d->rt];
    if (d->mode == XI_LITERAL)
        return pc + d->imm;

    u64 base = d->rn == 31 ? sp : regs[d->rn];

    switch (d->mode) {
        case XI_POST:
            return base;
        case XI_REG: {
            u64 off = d->rm == 31 ? 0 : regs[d->rm];

            if (d->ext == 2)
                off = (u32)off;
            else if (d->ext == 6)
                off = sext(off, 32);
            return base + (off << d->shift);
        }
        default:
            return base + d->imm;
    }
}
