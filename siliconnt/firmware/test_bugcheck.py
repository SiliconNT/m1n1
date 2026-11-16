#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run bugcheck.py against a model of a stopped guest, without any hardware.

The model is physical memory as a set of 4 KB pages, with the stage 1 tables of TTBR1_EL1
built in it the way the Arm ARM describes them for the 4 KB granule, and in that memory
what bugcheck.py looks for: the header of the Windows kernel, the variables of a bug check
and the list of loaded modules. Nothing in it comes from a Windows binary except the
numbers bugcheck.py itself carries.

Usage: test_bugcheck.py
test_run_firmware.py takes windows_model() from here.
"""
import sys, json, pathlib, struct, tempfile, types

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bugcheck

PAGE = 0x1000
RAM_BASE = 0x10020000000
RAM_TOP = RAM_BASE + (256 << 20)
# TCR_EL1 as Windows had it when it was stopped in R17 and R21 (SiliconNT docs/lab/
# 2026-11-15-boot-disk-4k-plan/run.txt): TG1 = 4 KB, T1SZ = 17, so 47 bits of address and a
# level 0 table of 256 entries
TCR = 0x615b5513511
assert TCR >> 30 & 3 == 2 and TCR >> 16 & 0x3f == 17 and not TCR >> 59 & 1
# The same with T1SZ = 16
TCR_48 = 2 << 30 | 16 << 16
KERNEL = 0xfffff8013c000000
DRIVER = 0xfffff80140000000
DRIVER_SIZE = 0x5000
POOL = 0xffffa00000001000

class Model:
    """Physical memory, and what is mapped where above the start of TTBR1's half."""
    def __init__(self, tcr=TCR):
        self.tcr = tcr
        self.low = (1 << (64 - (tcr >> 16 & 0x3f))) - 1   # the bits of an address the tables resolve
        self.pages = {}
        self.next = RAM_BASE
        self.va = {}          # page of virtual memory -> page of physical memory
        self.reads = []       # every read_phys(): (address, size)
        self.fail_at = None   # a physical page whose read raises
        self.root = self.alloc()

    def alloc(self, count=1):
        base = self.next
        for i in range(count):
            self.pages[base + i * PAGE] = bytearray(PAGE)
        self.next += count * PAGE
        return base

    def read_phys(self, addr, size):
        self.reads.append((addr, size))
        assert RAM_BASE <= addr and addr + size <= RAM_TOP, f"read outside RAM: 0x{addr:x}"
        data = b""
        while size:
            page = addr & ~(PAGE - 1)
            if page == self.fail_at:
                raise OSError("the link went away")
            n = min(size, page + PAGE - addr)
            data += self.pages.get(page, bytes(PAGE))[addr - page:addr - page + n]
            addr += n
            size -= n
        return data

    def _table(self, table, index, entry=None):
        """The table a table descriptor points to, made if it is not there."""
        off = index * 8
        desc, = struct.unpack_from("<Q", self.pages[table], off)
        if not desc & 1:
            desc = self.alloc() | 3
            struct.pack_into("<Q", self.pages[table], off, desc)
        return desc & 0x0000fffffffff000

    def map(self, va, pa=None):
        """One page, by a page descriptor at level 3."""
        if pa is None:
            pa = self.alloc()
        table = self.root
        for level in range(3):
            table = self._table(table, (va & self.low) >> (39 - 9 * level) & 0x1ff)
        # valid, page, AF, and attribute bits above the address that the walk has to drop
        struct.pack_into("<Q", self.pages[table], (va >> 12 & 0x1ff) * 8,
                         pa | 3 | 1 << 10 | 1 << 54 | 1 << 53)
        self.va[va & ~(PAGE - 1)] = pa
        return pa

    def map_block(self, va):
        """2 MB, by a block descriptor at level 2."""
        assert va % (2 << 20) == 0
        pa = (self.next + (2 << 20) - 1) & ~((2 << 20) - 1)
        self.next = pa
        self.alloc(512)
        table = self.root
        for level in range(2):
            table = self._table(table, (va & self.low) >> (39 - 9 * level) & 0x1ff)
        struct.pack_into("<Q", self.pages[table], (va >> 21 & 0x1ff) * 8, pa | 1 | 1 << 10 | 1 << 54)
        for i in range(512):
            self.va[va + i * PAGE] = pa + i * PAGE
        return pa

    def write(self, va, data):
        for i, byte in enumerate(data):
            page = (va + i) & ~(PAGE - 1)
            self.pages[self.va[page]][(va + i) - page] = byte

    def mem(self):
        return bugcheck.GuestMemory(self.read_phys, self.root, self.tcr, (RAM_BASE, RAM_TOP))

def pe_header(timestamp, size_of_image):
    """As much of a PE32+ header as bugcheck.py reads, laid out as in the kernel's file."""
    header = bytearray(PAGE)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3c, 0x120)
    header[0x120:0x124] = b"PE\0\0"
    struct.pack_into("<HHI", header, 0x124, 0xaa64, 34, timestamp)
    struct.pack_into("<H", header, 0x138, 0x20b)
    struct.pack_into("<I", header, 0x138 + 56, size_of_image)
    return bytes(header)

def module_entry(m, at, name_at, base, size, name):
    """A _KLDR_DATA_TABLE_ENTRY at `at`, its name at `name_at`. The links are set by the caller."""
    text = name.encode("utf-16-le")
    m.write(name_at, text)
    m.write(at + 0x30, struct.pack("<Q", base))
    m.write(at + 0x40, struct.pack("<I", size))
    m.write(at + 0x58, struct.pack("<HHIQ", len(text), len(text) + 2, 0, name_at))

def windows_model(code=0x109, parameters=None, timestamp=bugcheck.TIME_DATE_STAMP):
    """A guest stopped inside the Windows kernel. code None: no bug check has happened."""
    m = Model()
    if parameters is None:
        # a pool address, something that is no address, an address in the driver, a small number
        parameters = (POOL + 0x800, 0xa3a03f5891c8b4e8, DRIVER + 0x1234, 1)
    # The kernel: its header, 2 MB of code as one block, and the pages of the variables. The
    # rest of its 0x124a000 bytes is not mapped, as memory of the kernel that is paged out.
    m.map(KERNEL)
    m.write(KERNEL, pe_header(timestamp, bugcheck.SIZE_OF_IMAGE))
    m.map_block(KERNEL + 0x200000)
    m.write(KERNEL + 0x25ac00, b"\x7f\x23\x03\xd5" * 4)
    for rva in (bugcheck.RVA["KiArm64ExceptionVectors"], bugcheck.RVA["KiBugCheckDataInitialized"],
                bugcheck.RVA["PsLoadedModuleList"], bugcheck.RVA["KiBugCheckData"]):
        if (KERNEL + rva) & ~(PAGE - 1) not in m.va:
            m.map(KERNEL + rva)
    if code is not None:
        m.write(KERNEL + bugcheck.RVA["KiBugCheckDataInitialized"], struct.pack("<I", 1))
        m.write(KERNEL + bugcheck.RVA["KiBugCheckData"], struct.pack("<5Q", code, *parameters))
    m.write(KERNEL + bugcheck.RVA["KdDebuggerEnabled"], b"\x01")
    m.write(KERNEL + bugcheck.RVA["KdDebuggerNotPresent"], b"\x01")

    # A driver of five pages; the fourth is not there
    for i in (0, 1, 2, 4):
        m.map(DRIVER + i * PAGE)
        m.write(DRIVER + i * PAGE, bytes([0x40 + i]) * PAGE)

    # The module list, in a page of pool: the kernel first, then the driver
    m.map(POOL)
    m.map(POOL + PAGE)
    m.write(POOL + PAGE, b"\xee" * PAGE)
    head = KERNEL + bugcheck.RVA["PsLoadedModuleList"]
    first, second = POOL, POOL + 0x100
    module_entry(m, first, POOL + 0x400, KERNEL, bugcheck.SIZE_OF_IMAGE, "ntoskrnl.exe")
    module_entry(m, second, POOL + 0x500, DRIVER, DRIVER_SIZE, "stor port.sys")
    for at, flink, blink in ((head, first, second), (first, second, head), (second, head, first)):
        m.write(at, struct.pack("<QQ", flink, blink))

    m.regs = {"VBAR_EL1": KERNEL + bugcheck.RVA["KiArm64ExceptionVectors"],
              "TTBR1_EL1": m.root | 1, "TCR_EL1": TCR}
    m.ram = (RAM_BASE, RAM_TOP)
    m.kernel, m.driver, m.pool, m.parameters = KERNEL, DRIVER, POOL, parameters
    return m

def report(m, out_dir, **changes):
    lines = []
    result = bugcheck.report(m.read_phys, {**m.regs, **changes}, m.ram, out_dir, lines.append)
    return result, "\n".join(lines)

failures = 0
def check(name, cond, detail=""):
    global failures
    print(f"{'PASS' if cond else 'FAIL'}: {name}" + (f" ({detail})" if detail and not cond else ""))
    if not cond:
        failures += 1

def main():
    # ---- the tables ----------------------------------------------------------------------
    for tcr, bits in ((TCR, 47), (TCR_48, 48)):
        m = Model(tcr)
        page = m.map(KERNEL + 0x3000)
        block = m.map_block(KERNEL + 0x200000)
        mem = m.mem()
        check(f"{bits} bits: a page descriptor and a 2 MB block descriptor",
              mem.translate(KERNEL + 0x3abc) == page + 0xabc and
              mem.translate(KERNEL + 0x200000 + 0x123456) == block + 0x123456)
        check(f"{bits} bits: an address just below TTBR1's half is not looked up",
              mem.translate((1 << 64) - (1 << bits) - 0x1000) is None and
              mem.translate((1 << 64) - (1 << bits)) is None)
    m = Model()
    page = m.map(KERNEL + 0x3000)
    block = m.map_block(KERNEL + 0x200000)
    mem = m.mem()
    check("a page descriptor: the page, with the offset",
          mem.translate(KERNEL + 0x3abc) == page + 0xabc, f"{mem.translate(KERNEL + 0x3abc)}")
    check("a 2 MB block descriptor: the block, with the offset in it",
          mem.translate(KERNEL + 0x200000 + 0x123456) == block + 0x123456)
    check("no descriptor at level 3: not mapped", mem.translate(KERNEL + 0x4000) is None)
    check("no descriptor at level 0: not mapped", mem.translate(0xffff800000000000) is None)
    check("an address of TTBR0's half is not looked up", mem.translate(0x7ff612340000) is None)
    check("an address between the two halves is not looked up",
          mem.translate(0x00ff000000000000) is None and mem.translate(0xfff0000000000000) is None)
    m.write(KERNEL + 0x3ff8, bytes(range(8)))
    m.map(KERNEL + 0x5000)
    check("a read inside a page", mem.read(KERNEL + 0x3ff8, 8) == bytes(range(8)))
    try:
        mem.read(KERNEL + 0x3ff8, 16)
        crossed = None
    except bugcheck.Unreadable as e:
        crossed = str(e)
    check("a read that runs into a page that is not mapped says which address",
          crossed is not None and f"0x{KERNEL + 0x4000:x}" in crossed, f"{crossed}")
    before = len(m.reads)
    mem.read(KERNEL + 0x3000, 8)
    check("tables that have been read are not read again", len(m.reads) == before + 1,
          f"{m.reads[before:]}")

    # A table descriptor that points outside guest RAM: not followed
    m2 = Model()
    m2.map(KERNEL)
    struct.pack_into("<Q", m2.pages[m2.root], ((KERNEL & m2.low) >> 39) * 8, 0x200000000 | 3)
    check("a table outside guest RAM is not read", m2.mem().translate(KERNEL) is None)
    # A page outside guest RAM: mapped as far as the tables go, but not ours to read
    m2 = Model()
    m2.map(KERNEL, 0x23b100000)
    check("a page outside guest RAM is not read",
          m2.mem().translate(KERNEL) is None and all(a >= RAM_BASE for a, s in m2.reads))
    # A descriptor at level 3 without bit 1 is not a page
    m2 = Model()
    pa = m2.map(KERNEL)
    for table in list(m2.pages):
        for off in range(0, PAGE, 8):
            desc, = struct.unpack_from("<Q", m2.pages[table], off)
            if desc & 0x0000fffffffff000 == pa and desc & 3 == 3 and table != pa:
                struct.pack_into("<Q", m2.pages[table], off, desc & ~2)
    check("a level 3 descriptor that is not a page descriptor: not mapped",
          m2.mem().translate(KERNEL) is None)

    for name, tcr in (("the 16 KB granule", 1 << 30 | 16 << 16), ("52-bit addresses (DS)", TCR | 1 << 59),
                      ("more than 48 bits", 2 << 30 | 12 << 16), ("a region of 2^20 bytes", 2 << 30 | 44 << 16)):
        try:
            bugcheck.GuestMemory(m.read_phys, m.root, tcr, (RAM_BASE, RAM_TOP))
            refused = False
        except bugcheck.Problem:
            refused = True
        check(f"{name} is refused", refused)

    # T1SZ = 25: three levels, starting at level 1
    m3 = Model()
    va = 0xffffff8000000000 + 0x7654000
    l1 = m3.root
    l2 = m3._table(l1, va >> 30 & 0x1ff)
    l3 = m3._table(l2, va >> 21 & 0x1ff)
    pa = m3.alloc()
    struct.pack_into("<Q", m3.pages[l3], (va >> 12 & 0x1ff) * 8, pa | 3)
    mem3 = bugcheck.GuestMemory(m3.read_phys, m3.root, 2 << 30 | 25 << 16, (RAM_BASE, RAM_TOP))
    check("T1SZ = 25: the walk starts at level 1", mem3.translate(va + 0x10) == pa + 0x10)
    check("T1SZ = 25: an address below the region is not looked up",
          mem3.translate(0xfffff8013c000000) is None)

    # ---- the whole report ----------------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "r"
        m = windows_model()
        result, text = report(m, out)
        check("the kernel is found from VBAR_EL1 and its header is recognized",
              result["kernel"] == {"base": KERNEL, "verified": True, "build": bugcheck.BUILD} and
              f"0x{KERNEL:x}" in text, text)
        check("the bug check code and the four parameters",
              result["bugcheck"] == {"code": 0x109, "parameters": list(m.parameters)} and
              "Bug check 0x109" in text and
              all(f"parameter {i + 1} = 0x{p:x}" in text for i, p in enumerate(m.parameters)), text)
        check("a parameter inside a module is named by module and offset",
              "(stor port.sys + 0x1234)" in text, text)
        check("the debugger variables are shown",
              result["debugger"] == {"KdDebuggerEnabled": 1, "KdDebuggerNotPresent": 1} and
              "KdDebuggerEnabled = 1" in text and "KdDebuggerNotPresent = 1" in text, text)
        check("the loaded modules",
              result["modules"] == [{"base": KERNEL, "size": bugcheck.SIZE_OF_IMAGE, "name": "ntoskrnl.exe"},
                                    {"base": DRIVER, "size": DRIVER_SIZE, "name": "stor port.sys"}],
              f"{result['modules']}")
        dumps = {d["what"]: d for d in result["dumps"]}
        check("the memory of the kernel, of the module a parameter points into, and of the pages "
              "at a parameter that points into no module are written",
              sorted(dumps) == ["ntoskrnl.exe", "parameter 1", "stor port.sys"], f"{sorted(dumps)}")
        kernel_file = (out / dumps["ntoskrnl.exe"]["file"]).read_bytes()
        check("the kernel's file has the size of the image, and what is in memory at its place",
              len(kernel_file) == bugcheck.SIZE_OF_IMAGE and kernel_file[:2] == b"MZ" and
              kernel_file[0x25ac00:0x25ac10] == b"\x7f\x23\x03\xd5" * 4)
        check("what is not in memory is zeros in the file and listed as ranges",
              kernel_file[0x1000:0x200000] == bytes(0x1ff000) and
              dumps["ntoskrnl.exe"]["missing"][0] == [0x1000, 0x200000] and
              [0x400000, 0x636000] in dumps["ntoskrnl.exe"]["missing"],
              f"{dumps['ntoskrnl.exe']['missing'][:4]}")
        driver_file = (out / dumps["stor port.sys"]["file"]).read_bytes()
        check("the driver's file, with its missing page",
              dumps["stor port.sys"]["file"] == "stor_port.sys.mem" and
              driver_file == b"\x40" * PAGE + b"\x41" * PAGE + b"\x42" * PAGE + bytes(PAGE) + b"\x44" * PAGE and
              dumps["stor port.sys"]["missing"] == [[0x3000, 0x4000]], f"{dumps['stor port.sys']}")
        pages_file = (out / dumps["parameter 1"]["file"]).read_bytes()
        check("the page a parameter points into and the one after it",
              dumps["parameter 1"]["base"] == POOL and len(pages_file) == 2 * PAGE and
              pages_file[PAGE:] == b"\xee" * PAGE)
        saved = json.loads((out / "bugcheck.json").read_text())
        check("the same is in bugcheck.json", saved == result)
        check("nothing was a problem", result["problems"] == [], f"{result['problems']}")
        runs = [s for a, s in m.reads if s > PAGE]
        check("pages that follow each other in physical memory are read in pieces of up to "
              "bugcheck.RUN bytes", max(runs, default=0) == bugcheck.RUN, f"{sorted(set(runs))}")

        # no bug check: say so, write nothing but the summary
        out = pathlib.Path(tmp) / "none"
        m = windows_model(code=None)
        result, text = report(m, out)
        check("no bug check recorded: said, and no memory written",
              result["bugcheck"] is None and "No bug check" in text and result["dumps"] == [] and
              sorted(p.name for p in out.iterdir()) == ["bugcheck.json"], text)

        # another kernel build: the values are read all the same, and marked
        out = pathlib.Path(tmp) / "other"
        m = windows_model(timestamp=0x12345678)
        result, text = report(m, out)
        check("a header that is not this build's: said, values read all the same",
              result["kernel"]["verified"] is False and result["bugcheck"]["code"] == 0x109 and
              "not verified" in text and len(result["problems"]) == 1, text)

        # not in the kernel
        out = pathlib.Path(tmp) / "fw"
        m = windows_model()
        result, text = report(m, out, VBAR_EL1=0x10023456800)
        check("VBAR_EL1 not in TTBR1's half: the guest is not in the Windows kernel, nothing read",
              result["kernel"] is None and result["bugcheck"] is None and
              "not in the Windows kernel" in text and len(m.reads) == 0, text)
        m = windows_model()
        result, text = report(m, pathlib.Path(tmp) / "fw2", VBAR_EL1=m.regs["VBAR_EL1"] + 0x800)
        check("VBAR_EL1 that does not put the kernel on a page boundary: nothing read",
              result["kernel"] is None and "not in the Windows kernel" in text and
              len(m.reads) == 0, text)
        m = windows_model()
        result, text = report(m, pathlib.Path(tmp) / "mmu", TCR_EL1=1 << 30 | 16 << 16)
        check("tables this cannot walk: said, nothing read",
              result["kernel"] is None and len(result["problems"]) == 1 and len(m.reads) == 0, text)

        # a module list that does not come back to its head
        out = pathlib.Path(tmp) / "loop"
        m = windows_model()
        m.write(POOL + 0x100, struct.pack("<Q", POOL))   # the driver's Flink: back to the kernel's
        result, text = report(m, out)
        check("a module list that goes round without its head ends, and the parameters are there",
              len(result["modules"]) == 2 and result["bugcheck"]["code"] == 0x109 and
              any("module list" in p for p in result["problems"]), f"{result['problems']}")

        # the link fails in the middle of the kernel's memory: what was read before stays
        out = pathlib.Path(tmp) / "link"
        m = windows_model()
        m.fail_at = m.va[KERNEL + 0x300000]
        result, text = report(m, out)
        check("a read that fails while memory is written does not lose the parameters",
              result["bugcheck"]["code"] == 0x109 and "parameter 4 = 0x1" in text and
              any("the link went away" in p for p in result["problems"]) and
              json.loads((out / "bugcheck.json").read_text())["bugcheck"]["code"] == 0x109, text)

    print(f"{failures} failure(s)")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
