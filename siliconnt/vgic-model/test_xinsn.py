#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Compares the instruction decoder of execution tracking, src/hv/hv_xinsn.c, with the
disassembler of binutils, word by word.

    test_xinsn.py build/xinsn-model [number of random words]

The decoder decides what EL2 does in the guest's place when the guest reads or writes a page
that it may only execute. An instruction that it takes for another one is memory of the guest
changed behind its back, so for every word it must either say what the disassembler says or
refuse; and it may refuse only what described() below does not describe, and what the
architecture leaves open.

The words: all of them at random within the encoding group of loads and stores (bit 27 set,
bit 25 clear), plus a share from anywhere, plus DC ZVA with every register. The seed is fixed.
"""

import os
import random
import re
import struct
import subprocess
import sys
import tempfile

OBJDUMP = os.environ.get("OBJDUMP", "aarch64-linux-gnu-objdump")

GP1 = {"b": 1, "h": 2}
SIMD = {"b": 1, "h": 2, "s": 4, "d": 8, "q": 16}
EXT = {"uxtw": 2, "lsl": 3, "sxtw": 6, "sxtx": 7}
ATOMIC = ["add", "clr", "eor", "set", "smax", "smin", "umax", "umin"]


def reg(name):
    """(class, number): class is w, x, or one of b h s d q; sp and the zero registers are 31"""
    if name in ("sp", "xzr"):
        return "x", 31
    if name in ("wsp", "wzr"):
        return "w", 31
    m = re.fullmatch(r"([wxbhsdq])(\d+)", name)
    if not m:
        raise ValueError(name)
    return m.group(1), int(m.group(2))


def address(text, pc, want):
    """The addressing mode of an operand string that starts at the bracket"""
    m = re.fullmatch(r"\[(\w+)\]", text)
    if m:
        return dict(rn=reg(m.group(1))[1], mode="base", imm=0)
    m = re.fullmatch(r"\[(\w+), #(-?\d+)\](!?)", text)
    if m:
        return dict(rn=reg(m.group(1))[1], mode="pre" if m.group(3) else "base",
                    imm=int(m.group(2)))
    m = re.fullmatch(r"\[(\w+)\], #(-?\d+)", text)
    if m:
        return dict(rn=reg(m.group(1))[1], mode="post", imm=int(m.group(2)))
    m = re.fullmatch(r"\[(\w+), (\w+)(?:, (\w+))?(?: #(\d+))?\]", text)
    if m:
        ext = m.group(3) or "lsl"
        return dict(rn=reg(m.group(1))[1], mode="reg", rm=reg(m.group(2))[1], ext=EXT[ext],
                    shift=int(m.group(4) or 0))
    m = re.fullmatch(r"(?:0x)?([0-9a-f]+)(?: <.*>)?", text)
    if m:
        imm = (int(m.group(1), 16) - pc) & (2**64 - 1)
        return dict(mode="lit", imm=imm - 2**64 if imm >= 2**63 else imm)
    raise ValueError(text)


def expected(word, mnemonic, operands, pc):
    """What the decoder must say of this instruction, as a dict, or None if it must refuse"""
    e = described(word, mnemonic, operands, pc)
    if e is None or e["kind"] not in ("load", "store"):
        return e

    # What the architecture leaves open (CONSTRAINED UNPREDICTABLE) is not carried out for the
    # guest: a pair loaded into one register twice, and writeback to a base that is also
    # loaded or stored. SIMD registers are never the base.
    regs = [e["rt"]] + ([e["rt2"]] if e["count"] == 2 else [])
    if e["kind"] == "load" and e["count"] == 2 and e["rt"] == e["rt2"]:
        return None
    if e["mode"] in ("pre", "post") and not e["simd"] and e["rn"] != 31 and e["rn"] in regs:
        return None
    return e


def described(word, mnemonic, operands, pc):
    """What the disassembler says of the instruction, in the terms of the decoder"""
    ops = operands

    # SVE and SME have loads and stores with the mnemonics of the plain ones
    if re.match(r"(z|p|za)\d*\b|zt0", ops):
        return None

    if mnemonic == "sys":
        # The disassembler leaves the register out when it is the zero register
        m = re.fullmatch(r"#3, C7, C4, #1(?:, (\w+))?", ops)
        if m:
            return dict(kind="zva", rt=reg(m.group(1) or "xzr")[1])
        return None

    # One register
    m = re.fullmatch(r"(ld|st)(u?)r(s?)([bhw]?)", mnemonic)
    if m:
        load = m.group(1) == "ld"
        first, rest = ops.split(", ", 1)
        cls, rt = reg(first)
        e = address(rest, pc, None)
        if m.group(2) and e["mode"] != "base":
            raise ValueError(operands)
        if cls in "wx":
            if m.group(3) and not load:
                raise ValueError(mnemonic)
            size = {"b": 1, "h": 2, "w": 4, "": 4 if cls == "w" else 8}[m.group(4)]
            e.update(kind="load" if load else "store", size=size, count=1, simd=0,
                     sext=1 if m.group(3) else 0, reg64=1 if load and cls == "x" else 0, rt=rt)
        else:
            if m.group(3) or m.group(4):
                raise ValueError(mnemonic)
            e.update(kind="load" if load else "store", size=SIMD[cls], count=1, simd=1, sext=0,
                     reg64=0, rt=rt)
        return e

    # A pair
    m = re.fullmatch(r"(ld|st)(n?)p(sw)?", mnemonic)
    if m:
        load = m.group(1) == "ld"
        first, second, rest = ops.split(", ", 2)
        cls, rt = reg(first)
        cls2, rt2 = reg(second)
        e = address(rest, pc, None)
        if cls != cls2 or (m.group(2) and e["mode"] != "base"):
            raise ValueError(operands)
        if m.group(3):
            e.update(kind="load", size=4, count=2, simd=0, sext=1, reg64=1, rt=rt, rt2=rt2)
        elif cls in "wx":
            e.update(kind="load" if load else "store", size=4 if cls == "w" else 8, count=2,
                     simd=0, sext=0, reg64=1 if load and cls == "x" else 0, rt=rt, rt2=rt2)
        else:
            e.update(kind="load" if load else "store", size=SIMD[cls], count=2, simd=1, sext=0,
                     reg64=0, rt=rt, rt2=rt2)
        return e

    # Load-acquire and store-release
    m = re.fullmatch(r"(ldar|stlr)([bh]?)", mnemonic)
    if m:
        first, rest = ops.split(", ", 1)
        cls, rt = reg(first)
        e = address(rest, pc, None)
        if e["mode"] != "base" or e["imm"]:
            return None  # the unscaled forms with an offset or writeback, which are not taken
        if word & 0x001f7c00 != 0x001f7c00:
            return None  # Rs and Rt2 should be all ones: the disassembler does not look
        load = m.group(1) == "ldar"
        size = GP1.get(m.group(2), 4 if cls == "w" else 8)
        e.update(kind="load" if load else "store", size=size, count=1, simd=0, sext=0,
                 reg64=1 if load and cls == "x" else 0, rt=rt)
        return e

    # The atomic operations
    m = re.fullmatch(r"(?:ld(add|clr|eor|set|smax|smin|umax|umin)|(swp))(a?l?)([bh]?)", mnemonic)
    if m:
        first, second, rest = ops.split(", ", 2)
        cls, rs = reg(first)
        cls2, rt = reg(second)
        e = address(rest, pc, None)
        if e["mode"] != "base" or e["imm"] or cls != cls2:
            raise ValueError(operands)
        size = GP1.get(m.group(4), 4 if cls == "w" else 8)
        e.update(kind="atomic", size=size, rs=rs, rt=rt,
                 op=8 if m.group(2) else ATOMIC.index(m.group(1)))
        return e

    m = re.fullmatch(r"cas(a?l?)([bh]?)", mnemonic)
    if m:
        first, second, rest = ops.split(", ", 2)
        cls, rs = reg(first)
        cls2, rt = reg(second)
        e = address(rest, pc, None)
        if e["mode"] != "base" or e["imm"] or cls != cls2:
            raise ValueError(operands)
        e.update(kind="cas", size=GP1.get(m.group(2), 4 if cls == "w" else 8), rs=rs, rt=rt)
        return e

    return None


def decoder_says(line):
    """A line of xinsn-model dump as a dict, or None for a refusal"""
    word, rest = line.split(" ", 1)
    if rest.strip() == "-":
        return None
    kind, *fields = rest.split()
    d = dict(kind=kind)
    for f in fields:
        k, v = f.split("=")
        d[k] = v if k == "mode" else int(v)
    return d


def main():
    model = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 400000
    rnd = random.Random(0x51c0)

    words = []
    for i in range(n):
        w = rnd.getrandbits(32)
        if i % 5:
            w = (w | (1 << 27)) & ~(1 << 25)
        words.append(w)
    # Registers 31, which are the stack pointer or zero depending on where they stand
    for i in range(n // 20):
        w = (rnd.getrandbits(32) | (1 << 27)) & ~(1 << 25)
        words.append(w | rnd.choice((0x1f, 0x3e0, 0x1f0000, 0x7c00, 0x3ff)))
    words += [0xd50b7420 | rt for rt in range(32)]
    # Its neighbours among the system instructions: other cache operations, other registers
    words += [0xd5087620 | rt for rt in range(32)]
    words += [0xd50b7420 ^ (1 << bit) for bit in range(5, 32)]

    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "words.bin")
        with open(raw, "wb") as f:
            f.write(struct.pack(f"<{len(words)}I", *words))
        dis = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-maarch64", "-M", "no-aliases", raw],
                             check=True, capture_output=True, text=True).stdout
    said = subprocess.run([model, "dump"], input="".join(f"{w:08x}\n" for w in words),
                          check=True, capture_output=True, text=True).stdout.splitlines()

    lines = [l for l in dis.splitlines() if re.match(r"\s*[0-9a-f]+:\t", l)]
    if len(lines) != len(words) or len(said) != len(words):
        sys.exit(f"{len(words)} words, {len(lines)} lines of disassembly, {len(said)} of the decoder")

    failures = 0
    taken = {}
    for i, (w, l, s) in enumerate(zip(words, lines, said)):
        parts = l.split("\t")
        if int(parts[1].strip(), 16) != w or int(s.split()[0], 16) != w:
            sys.exit(f"out of step at word {i}: {l!r} {s!r}")
        text = parts[2:]
        mnemonic = text[0].strip() if text else ""
        operands = text[1].split("//")[0].strip() if len(text) > 1 else ""
        try:
            want = expected(w, mnemonic, operands, 4 * i)
        except (ValueError, KeyError) as e:
            sys.exit(f"cannot read the disassembly {l!r}: {e!r}")
        got = decoder_says(s)

        ok = (want is None) == (got is None)
        if ok and want is not None:
            ok = all(got.get(k) == v for k, v in want.items())
            taken[mnemonic] = taken.get(mnemonic, 0) + 1
        if not ok:
            failures += 1
            if failures <= 40:
                print(f"FAIL: {w:08x} {mnemonic} {operands}\n   disassembler: {want}\n"
                      f"   decoder:      {got}")

    print(f"test_xinsn: {len(words)} words, {sum(taken.values())} of them instructions the "
          f"decoder takes ({len(taken)} mnemonics), {failures} failed")
    # Every kind of instruction that the decoder is for has been seen
    for need in ("ldr", "str", "ldrb", "ldrsw", "ldursh", "sturh", "ldp", "stp", "ldnp", "stnp",
                 "ldpsw", "ldar", "stlrb", "ldadd", "ldclral", "ldumaxb", "swp", "swpalh", "cas",
                 "casalb", "sys"):
        if need not in taken:
            failures += 1
            print(f"FAIL: no {need} among the words that were taken")
    if failures:
        sys.exit(1)
    print("ALL OK")


if __name__ == "__main__":
    main()
