#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads the device tree that the model test of src/selfboot.c wrote (test.c, test_adt())
with the parser of the proxy client, which shares no code with what wrote it: the tree is
still one, and /chosen/memory-map says what the firmware is to find there."""
import sys, pathlib, struct

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

from m1n1.adt import ADTNode, ADTNodeStruct

# What load_adt() does, without its look at /arm-io/pmgr, which the tree of test.c has not
tree = ADTNode(ADTNodeStruct.parse(pathlib.Path(sys.argv[1]).read_bytes()))
memory_map = tree["chosen"]["memory-map"]

def entry(name):
    value = memory_map.getprop(name)
    if isinstance(value, bytes) and len(value) == 16:
        value = struct.unpack("<QQ", value)
    return None if value is None else tuple(value)

# The layout of R21, as in test.c
EXPECTED = {
    "DeviceTree": (0x1001a9a8000, 0x84000),
    "BootArgs": (0x1001ca00000, 0x4000),
    "RAMDisk": (0x1001cc00000, 0x28200000),
    # The page after the boot arguments: version, count and 391 lines
    "AICToGIC": (0x1001ca04000, 8 + 4 * 391),
    "MemoryMapReserved-0": None,
    "MemoryMapReserved-1": None,
    "MemoryMapReserved-2": (0, 0),
    "SEPFW": (0x10005a6c000, 0x5d0000),
}

failed = 0
for name, want in EXPECTED.items():
    got = entry(name)
    if got != want:
        failed += 1
        print(f"FAILED: {name} is {got}, not {want}")
# What follows the node that was changed is still found
if tree["cpus"]["cpu0"].state != "running":
    failed += 1
    print("FAILED: /cpus/cpu0 is not as it was")

print(f"check_adt.py: {len(EXPECTED) + 1} checks, {failed} failed")
sys.exit(1 if failed else 0)
