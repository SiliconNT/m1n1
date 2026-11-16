# SPDX-License-Identifier: GPL-2.0-or-later
"""What the Windows kernel recorded of a bug check, read from the memory of the stopped guest.

Without a kernel debugger nothing leaves the machine when Windows stops with a bug check:
the stop code is on the screen, the four parameters are not (R17, R24 of SiliconNT docs/lab).
They are in the kernel's memory. KeBugCheck2 stores the code and the parameters in
KiBugCheckData, five 64-bit values, the first time it is called, and sets
KiBugCheckDataInitialized (ntoskrnl.exe 10.0.26300.9457, KeBugCheck2+0xa4 .. +0xf0).

run_firmware.py calls report() when the guest has stopped, before it leaves the guest.
report() needs three registers of the guest and a function that reads physical memory:

  - VBAR_EL1 is KiArm64ExceptionVectors while the kernel runs, which gives the address the
    kernel was loaded at. The PE header found there is compared with this build's.
  - TTBR1_EL1 and TCR_EL1 give the stage 1 tables of the kernel's half of the address
    space. They are walked here, in software, so that it does not matter whether the guest
    was stopped in EL1 or in EL0. Only the 4 KB granule with up to 48 bits is known, which
    is what this kernel uses (Arm ARM DDI 0487 M.d, D8.2.8 and D8.3: tables D8-16 to D8-18).
  - Stage 2 maps the guest's RAM to itself with both loaders (HV.pt_update() and
    selfboot_map()), so what stage 1 gives is an address to read at. Nothing outside the
    guest's RAM is read: not a table and not a page.

What is written to the directory: bugcheck.json with everything found, and the memory of the
kernel image, of every module a parameter points into, and of two pages at a parameter that
points into no module. Those files are memory of Windows: they belong where the Windows
binaries are kept, not into a repository.

The numbers below are of one build. Public symbols of ntkrnlmp.pdb; the fields of
_KLDR_DATA_TABLE_ENTRY from its type records.
"""
import json, pathlib, re, struct, time

BUILD = "10.0.26300.9457"
# IMAGE_FILE_HEADER.TimeDateStamp and IMAGE_OPTIONAL_HEADER64.SizeOfImage of its ntoskrnl.exe
TIME_DATE_STAMP = 0x409ce488
SIZE_OF_IMAGE = 0x124a000
MACHINE_ARM64 = 0xaa64
RVA = {
    "KiArm64ExceptionVectors": 0x636800,
    "KiBugCheckDataInitialized": 0xc57204,
    "KiBugCheckData": 0xdbbf40,
    "PsLoadedModuleList": 0xd6ac50,
    "KdDebuggerNotPresent": 0xdbbfc4,
    "KdDebuggerEnabled": 0xdbbff8,
}
# _KLDR_DATA_TABLE_ENTRY: InLoadOrderLinks at 0
LDR_DLL_BASE = 0x30
LDR_SIZE_OF_IMAGE = 0x40
LDR_BASE_DLL_NAME = 0x58

PAGE = 0x1000
# The output address of a descriptor without FEAT_LPA2: bits 47:12
OA = 0x0000fffffffff000
# Physical memory is read in pieces of at most this. ProxyUtils fetches the device tree of
# the M4, 0x84000 bytes, in one piece.
RUN = 1 << 19
MAX_MODULES = 1024
MAX_MODULE_SIZE = 64 << 20
MAX_NAME = 512

class Problem(Exception):
    """Something that keeps a step from being done; the text says what."""

class Unreadable(Problem):
    """An address of the guest that its tables do not map to its RAM."""

class GuestMemory:
    """The kernel's half of the guest's address space, by its stage 1 tables."""
    def __init__(self, read_phys, ttbr1, tcr, ram):
        t1sz = tcr >> 16 & 0x3f
        if tcr >> 30 & 3 != 2:
            raise Problem(f"TCR_EL1 0x{tcr:x}: TTBR1's granule is not 4 KB")
        if tcr >> 59 & 1:
            raise Problem(f"TCR_EL1 0x{tcr:x}: 52-bit addresses (DS = 1)")
        if not 16 <= t1sz <= 39:
            raise Problem(f"TCR_EL1 0x{tcr:x}: T1SZ = {t1sz}, not one of 16 .. 39")
        self.read_phys = read_phys
        self.ram_base, self.ram_top = ram
        self.bits = 64 - t1sz
        # Each level resolves 9 bits, the first one what is left over (table D8-18)
        self.first = 4 - -(-(self.bits - 12) // 9)
        # The first table has as many entries as it resolves bits, and TTBR1_EL1 holds its
        # address down to the bit that its size leaves (table D8-10): a table of 256 entries
        # is half a page and may start in the middle of one. Windows has TTBR0's table in
        # the first half of a page and TTBR1's in the second.
        size = 8 << self.bits - (12 + 9 * (3 - self.first))
        self.root = ttbr1 & (1 << 48) - 1 & ~(size - 1)
        self.tables = {}

    def in_ram(self, pa, size=1):
        return self.ram_base <= pa and pa + size <= self.ram_top

    def entry(self, table, index):
        """A descriptor, or None if the table is not in the guest's RAM. Tables are read and
        kept a page at a time; all but the first start at a page."""
        page = table & ~(PAGE - 1)
        if page not in self.tables:
            if not self.in_ram(page, PAGE):
                return None
            self.tables[page] = self.read_phys(page, PAGE)
        return struct.unpack_from("<Q", self.tables[page], (table & PAGE - 1) + index * 8)[0]

    def translate(self, va):
        """The address in the guest's RAM, or None."""
        if va >> self.bits != (1 << (64 - self.bits)) - 1:
            return None
        table = self.root
        for level in range(self.first, 4):
            shift = 12 + 9 * (3 - level)
            desc = self.entry(table, (va & (1 << self.bits) - 1) >> shift & 0x1ff)
            if desc is None or not desc & 1:
                return None
            if level < 3 and desc & 2:
                table = desc & OA
                continue
            # A block at level 1 or 2, a page at level 3; bit 1 of a page descriptor is 1
            if level == 0 or (level == 3 and not desc & 2):
                return None
            pa = desc & OA & ~((1 << shift) - 1) | va & (1 << shift) - 1
            return pa if self.in_ram(pa) else None

    def read(self, va, size):
        data = b""
        while size:
            pa = self.translate(va)
            if pa is None:
                raise Unreadable(f"0x{va:x} is not in the guest's memory")
            n = min(size, PAGE - (va & PAGE - 1))
            data += self.read_phys(pa, n)
            va += n
            size -= n
        return data

    def read_image(self, va, size):
        """The pages from va on, whole. Returns the bytes, zeros where nothing is mapped, and
        the ranges that are not mapped, as offsets."""
        assert va % PAGE == 0 and size % PAGE == 0
        data = bytearray(size)
        missing = []
        runs = []   # (offset, address, size): pages that follow each other in physical memory
        for off in range(0, size, PAGE):
            pa = self.translate(va + off)
            if pa is None:
                if missing and missing[-1][1] == off:
                    missing[-1][1] = off + PAGE
                else:
                    missing.append([off, off + PAGE])
            elif runs and runs[-1][0] + runs[-1][2] == off and runs[-1][1] + runs[-1][2] == pa \
                    and runs[-1][2] < RUN:
                runs[-1][2] += PAGE
            else:
                runs.append([off, pa, PAGE])
        for off, pa, length in runs:
            data[off:off + length] = self.read_phys(pa, length)
        return bytes(data), missing

def file_name(what):
    return re.sub(r"[^A-Za-z0-9._-]", "_", what) + ".mem"

def kernel_header_problem(header):
    """None if this is the first page of this build's kernel, else what is different."""
    if header[:2] != b"MZ":
        return "no MZ"
    pe, = struct.unpack_from("<I", header, 0x3c)
    if pe > PAGE - 0x60 or header[pe:pe + 4] != b"PE\0\0":
        return "no PE header"
    machine, _, stamp = struct.unpack_from("<HHI", header, pe + 4)
    size, = struct.unpack_from("<I", header, pe + 24 + 56)
    if (machine, stamp, size) != (MACHINE_ARM64, TIME_DATE_STAMP, SIZE_OF_IMAGE):
        return (f"machine 0x{machine:x}, TimeDateStamp 0x{stamp:x}, SizeOfImage 0x{size:x}, not "
                f"0x{MACHINE_ARM64:x}, 0x{TIME_DATE_STAMP:x}, 0x{SIZE_OF_IMAGE:x}")
    return None

def loaded_modules(mem, head):
    """PsLoadedModuleList, as far as it can be followed. Returns the modules and, if the
    list did not end at its head, why."""
    modules = []
    seen = {head}
    entry, = struct.unpack("<Q", mem.read(head, 8))
    while entry != head:
        if entry in seen or len(modules) == MAX_MODULES:
            return modules, f"the module list does not come back to its head after {len(modules)} entries"
        seen.add(entry)
        try:
            base, = struct.unpack("<Q", mem.read(entry + LDR_DLL_BASE, 8))
            size, = struct.unpack("<I", mem.read(entry + LDR_SIZE_OF_IMAGE, 4))
            length, _, _, buffer = struct.unpack("<HHIQ", mem.read(entry + LDR_BASE_DLL_NAME, 16))
            try:
                name = mem.read(buffer, min(length, MAX_NAME)).decode("utf-16-le", "replace")
            except Unreadable:
                name = f"module-at-0x{base:x}"
            modules.append({"base": base, "size": size, "name": name})
            entry, = struct.unpack("<Q", mem.read(entry, 8))
        except Unreadable as e:
            return modules, f"the module list ends at an entry that cannot be read: {e}"
    return modules, None

def report(read_phys, regs, ram, out_dir, say=print):
    """Looks, prints what it finds with say(), writes the files to out_dir and returns what
    is in bugcheck.json. regs: VBAR_EL1, TTBR1_EL1 and TCR_EL1 of the guest. ram: the first
    address of the guest's RAM and the one after its last. A step that fails is said and
    listed under "problems"; the steps after it are still tried."""
    out_dir = pathlib.Path(out_dir)
    result = {"registers": dict(regs), "kernel": None, "debugger": None, "bugcheck": None,
              "modules": [], "dumps": [], "problems": []}

    def problem(text):
        result["problems"].append(text)
        say(f"PROBLEM: {text}")

    def dump(mem, what, base, size):
        started = time.monotonic()
        try:
            data, missing = mem.read_image(base, size)
        except Exception as e:
            problem(f"the memory of {what} was not read ({type(e).__name__}: {e})")
            return
        name = file_name(what)
        (out_dir / name).write_bytes(data)
        result["dumps"].append({"what": what, "base": base, "size": size, "file": name,
                                "missing": missing})
        absent = sum(end - start for start, end in missing)
        say(f"{what}: 0x{size:x} bytes at 0x{base:x} written to {out_dir / name} in "
            f"{time.monotonic() - started:.1f} s; 0x{absent:x} of them are not in memory "
            "(zeros in the file)")

    def look():
        say("==== what Windows recorded of a bug check ====")
        vbar = regs["VBAR_EL1"]
        base = vbar - RVA["KiArm64ExceptionVectors"]
        # The kernel is in TTBR1's half, and it is loaded at a page boundary
        if not vbar >> 63 or base % PAGE:
            say(f"VBAR_EL1 is 0x{vbar:x}: the guest is not in the Windows kernel of {BUILD}. "
                "Nothing was read.")
            return
        try:
            mem = GuestMemory(read_phys, regs["TTBR1_EL1"], regs["TCR_EL1"], ram)
        except Problem as e:
            problem(f"the guest's tables cannot be walked: {e}")
            return

        try:
            different = kernel_header_problem(mem.read(base, PAGE))
        except Unreadable as e:
            different = str(e)
        result["kernel"] = {"base": base, "verified": different is None, "build": BUILD}
        if different:
            problem(f"what is at 0x{base:x} is not the header of the kernel of {BUILD} "
                    f"({different}): what follows is not verified")
        else:
            say(f"Windows kernel {BUILD} at 0x{base:x}: VBAR_EL1 0x{vbar:x} is its "
                "KiArm64ExceptionVectors, the header in memory is this build's")

        try:
            result["debugger"] = {name: mem.read(base + RVA[name], 1)[0]
                                  for name in ("KdDebuggerEnabled", "KdDebuggerNotPresent")}
            say(", ".join(f"{name} = {value}" for name, value in result["debugger"].items()))
        except Unreadable as e:
            problem(f"the debugger variables were not read: {e}")

        try:
            modules, why = loaded_modules(mem, base + RVA["PsLoadedModuleList"])
        except Unreadable as e:
            modules, why = [], f"the module list was not read: {e}"
        result["modules"] = modules
        if why:
            problem(why)
        say(f"Loaded modules: {len(modules)}")

        def module_of(address):
            for module in modules:
                if module["base"] <= address < module["base"] + module["size"]:
                    return module
            if base <= address < base + SIZE_OF_IMAGE:
                return {"base": base, "size": SIZE_OF_IMAGE, "name": "ntoskrnl.exe"}
            return None

        try:
            initialized, = struct.unpack("<I", mem.read(base + RVA["KiBugCheckDataInitialized"], 4))
            data = struct.unpack("<5Q", mem.read(base + RVA["KiBugCheckData"], 40))
        except Unreadable as e:
            problem(f"the bug check variables were not read: {e}")
            return
        if not initialized:
            say("No bug check recorded: KiBugCheckDataInitialized is 0")
            return
        result["bugcheck"] = {"code": data[0], "parameters": list(data[1:])}
        say(f"Bug check 0x{data[0]:x}")
        for i, value in enumerate(data[1:], 1):
            module = module_of(value)
            where = f"  ({module['name']} + 0x{value - module['base']:x})" if module else ""
            say(f"  parameter {i} = 0x{value:x}{where}")

        # The parameters are out. Now the memory they may be about, the kernel's first.
        out_dir.mkdir(parents=True, exist_ok=True)
        dump(mem, "ntoskrnl.exe", base, SIZE_OF_IMAGE)
        done = {base}
        for i, value in enumerate(data[1:], 1):
            module = module_of(value)
            if module:
                if module["base"] in done:
                    continue
                done.add(module["base"])
                size = -(-module["size"] // PAGE) * PAGE
                if module["base"] % PAGE or not 0 < size <= MAX_MODULE_SIZE:
                    problem(f"the memory of {module['name']} was not read: 0x{module['size']:x} "
                            f"bytes at 0x{module['base']:x}")
                    continue
                dump(mem, module["name"], module["base"], size)
            elif mem.translate(value) is not None:
                dump(mem, f"parameter {i}", value & ~(PAGE - 1), 2 * PAGE)

    try:
        look()
    except Exception as e:
        problem(f"stopped looking: {type(e).__name__}: {e}")
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "bugcheck.json").write_text(json.dumps(result, indent=1) + "\n")
    except OSError as e:
        problem(f"bugcheck.json was not written: {e}")
    return result
