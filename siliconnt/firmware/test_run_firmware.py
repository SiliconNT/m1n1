#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run run_firmware.py against a fake target, without any hardware.

The proxy, ProxyUtils and HV are replaced by stand-ins that answer with the numbers the M4
MacBook Pro reported on 2026-11-13 (R9a: SiliconNT docs/lab/2026-11-13-wfi-trap-plan/);
m1n1.utils and m1n1.sysreg are the real ones. The secondary serial port is a pty. This
checks what the script decides and what it would write where, not the hypervisor.

Usage: test_run_firmware.py [firmware image]   (needs build/m1n1.bin of this tree)
"""
import sys, os, io, pathlib, runpy, struct, types, contextlib, threading, time, signal, enum
import re, hashlib, tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "run_firmware.py"
PROXYCLIENT = HERE.parents[1] / "proxyclient"
M1N1_BIN = HERE.parents[1] / "build" / "m1n1.bin"
sys.path.append(str(PROXYCLIENT))

from m1n1.utils import align, align_up
from m1n1 import sysreg

# What the M4 reported (R9a, hello-before-r9a.txt and wfitest-1cpu.txt)
M4 = dict(
    m1n1_base=0x10002e70000,
    ba_phys_base=0x10002e70000,
    ba_mem_size=0x3d87a0000,
    devtree_size=0x84000,
    heap_top=0x10018eb8000,
    fb_base=0x103e004c000, fb_stride=0x2f40, fb_width=3024, fb_height=1964,
    uart=0x3ad200000,
)
MEM_TOP = M4["ba_phys_base"] + M4["ba_mem_size"]
assert MEM_TOP == 0x103db610000

class FakeNode:
    def __init__(self, **props):
        self.__dict__["_properties"] = dict(props)
    def __getattr__(self, name):
        try:
            return self._properties[name]
        except KeyError:
            raise AttributeError(name)
    def __setattr__(self, name, value):
        self._properties[name] = value

class FakeADT(dict):
    pass

def make_fakes(case):
    """Fresh stand-in modules for one run. case: dict of overrides."""
    m1n1_image = M1N1_BIN.read_bytes()
    rec = types.SimpleNamespace(loaded=None, bootargs_top=None, hv_start=None, stopped=None,
                                hv_inited=False, memory_map=None, context_printed=False,
                                entered=None, sigints=[], writes=[], writes_at_entry=None,
                                readbacks=[])
    heap_top = case.get("heap_top", M4["heap_top"])

    memory_map = FakeNode(**{
        # below guest RAM, as iBoot left them under m1n1 (addresses made up, the names are
        # the ones HV.load_raw() reads)
        "SEPFW": (0x10000a00000, 0x5d0000),
        "TrustCache": (0x10000900000, 0x74000),
        "DeviceTree": (0x10004f00000, 0x84000),
        "BootArgs": (0x10005058000, 0x4000),
        "preoslog": struct.pack("<QQ", 0x10000800000, 0x40000),
        "Something-odd": b"\x01\x02\x03",
    })
    for name, value in case.get("memory_map", {}).items():
        setattr(memory_map, name, value)

    class UartInterface:
        def readmem(self, addr, size):
            # what ProxyUtils.compressed_writemem() below has put into guest memory
            for base, written in rec.writes:
                if base <= addr and addr + size <= base + len(written):
                    rec.readbacks.append((addr, size))
                    data = bytearray(written[addr - base:addr - base + size])
                    if len(rec.readbacks) == case.get("bad_readback"):
                        data[-1] ^= 1
                    return bytes(data)
            off = addr - M4["m1n1_base"]
            data = m1n1_image[off:off + size]
            return case.get("version", data)

    class M1N1Proxy:
        def __init__(self, iface, debug=False):
            pass
        def hv_start(self, entry, *regs):
            rec.hv_start = (entry, *regs)
            rec.entered = time.monotonic()
            rec.writes_at_entry = len(rec.writes)

    class EXC_RET:
        EXIT_GUEST = "EXIT_GUEST"

    class START(enum.IntEnum):
        EXCEPTION_LOWER = 2
        HV = 3

    video = types.SimpleNamespace(base=case.get("fb_base", M4["fb_base"]), stride=M4["fb_stride"],
                                  width=M4["fb_width"], height=M4["fb_height"])
    ba = types.SimpleNamespace(phys_base=M4["ba_phys_base"], mem_size=M4["ba_mem_size"],
                               devtree_size=M4["devtree_size"], video=video)

    class ProxyUtils:
        def __init__(self, p, heap_size, m1n1_heap):
            self.base = M4["m1n1_base"]
            self.heap_top = heap_top
            self.ba = ba
            self.adt = FakeADT(chosen={"memory-map": memory_map})
            self.regs = {
                sysreg.HCR_EL2: sysreg.HCR(IMO=1, FMO=1, VM=1, TWI=1).value,
                sysreg.CNTHCTL_EL2: 4,
                sysreg.ICH_HCR_EL2: 1,
                sysreg.ICH_VTR_EL2: 0x90580007,
                # docs/lab/2026-11-11-r1-r2-plan/r1.txt, line 8
                sysreg.SCTLR_EL12: case.get("sctlr_el1", 0x30d50980),
                sysreg.CPACR_EL12: 0,
            }
        def mrs(self, reg):
            return self.regs.get(reg, 0)
        def compressed_writemem(self, dest, data, progress=None):
            # The real one sends a gzip stream and has m1n1 unpack it at dest. m1n1 compares
            # the CRC-32 of what it unpacked with the one in the stream (src/tinf/tinfgzip.c)
            # and returns an error instead of the size if they differ, which trips
            # "assert decompressed_size == len(data)" in proxyclient/m1n1/proxyutils.py.
            if len(rec.writes) + 1 == case.get("bad_write"):
                raise AssertionError
            rec.writes.append((dest, bytes(data)))
        def print_context(self, ctx, is_fault=True):
            rec.context_printed = True
            print(f"  ELR    = 0x{ctx.elr:x}")

    class HV:
        def __init__(self, iface, p, u):
            self.p, self.u = p, u
            self.smp = True
            uart = types.SimpleNamespace(get_reg=lambda i: (M4["uart"], 0x4000))
            self.adt = FakeADT({"/arm-io/uart0": uart, "chosen": {"memory-map": FakeNode()}})
            rec.memory_map = self.adt["chosen"]["memory-map"]
        def init(self):
            assert self.smp is False
            self.mem_top = self.u.ba.phys_base + self.u.ba.mem_size
            rec.hv_inited = True
        def _prepare_load(self):
            # as in proxyclient/m1n1/hv/__init__.py
            self.phys_base = guest_base = self.u.heap_top
            guest_base += 32 << 20
            self.guest_base = guest_base
            self.mem_size = self.mem_top - self.phys_base
        def _load_executable_raw(self, image, base, label):
            rec.loaded = (base, len(image), label)
        def _write_boot_args(self, top_of_kernel_data):
            rec.bootargs_top = top_of_kernel_data
            rec.bootargs_at = self.guest_base + self.bootargs_off
        def start(self):
            # The real one sends the proxy a byte on SIGINT, and the hypervisor comes back
            # with HV/USER_INTERRUPT. Here the signal is only noted.
            signal.signal(signal.SIGINT, lambda *a: rec.sigints.append(time.monotonic()))
            self.p.hv_start(self.entry, self.guest_base + self.bootargs_off)
            # The "firmware" prints once it runs: opening a serial port drops what came before
            os.write(rec.master, b"Apple Silicon Project Mu Firmware (arm64/arm64e)\r\n"
                                 b"If you can see this")
            time.sleep(0.5)  # let the serial reader see it
            case.get("guest", lambda rec: None)(rec)
            # Stopped inside the image: by the timer or Ctrl-C (HV/USER_INTERRUPT), by the
            # hypervisor for good (HV/GUEST_*), or by an exception nobody handled. For all
            # but the first, HV.handle_exception() has printed the context by now.
            self.ctx = types.SimpleNamespace(elr=self.entry + 0x8600)
            self.exc_reason = START[case.get("stop_reason", "HV")]
            self.exc_code = types.SimpleNamespace(name=case.get(
                "stop_code", "USER_INTERRUPT" if self.exc_reason == START.HV else "SYNC"))
            rec.stopped = self.run_shell()

    class PMU:
        def __init__(self, u):
            pass
        def reset_panic_counter(self):
            pass

    mods = {
        "m1n1.proxy": dict(UartInterface=UartInterface, M1N1Proxy=M1N1Proxy, EXC_RET=EXC_RET,
                           START=START),
        "m1n1.proxyutils": dict(ProxyUtils=ProxyUtils, bootstrap_port=lambda iface, p: None),
        "m1n1.hv": dict(HV=HV),
        "m1n1.hw": dict(),
        "m1n1.hw.pmu": dict(PMU=PMU),
    }
    for name, contents in mods.items():
        mod = types.ModuleType(name)
        mod.__dict__.update(contents)
        mod.__all__ = list(contents)
        sys.modules[name] = mod
    return rec

def run(case, *argv):
    rec = make_fakes(case)
    master, slave = os.openpty()
    rec.master = master
    out = io.StringIO()
    code = 0
    old_argv = sys.argv
    old_sigint = signal.getsignal(signal.SIGINT)
    sys.argv = [str(SCRIPT), "--serial", os.ttyname(slave), *argv]
    try:
        with contextlib.redirect_stdout(out):
            runpy.run_path(str(SCRIPT), run_name="__main__")
    except SystemExit as e:
        code = e.code or 0
    finally:
        sys.argv = old_argv
        signal.signal(signal.SIGINT, old_sigint)
        os.close(master)
        os.close(slave)
    return rec, out.getvalue(), code

failures = 0
def check(name, cond, detail=""):
    global failures
    print(f"{'PASS' if cond else 'FAIL'}: {name}" + (f" ({detail})" if detail and not cond else ""))
    if not cond:
        failures += 1

image_args = sys.argv[1:2]
if image_args:
    image_len = pathlib.Path(image_args[0]).stat().st_size
else:
    image_len = (pathlib.Path.home() / "src/siliconnt-firmware/Build/MacBookProM4-AARCH64/"
                 "DEBUG_CLANGPDB/FV/MACBOOKPROM4_EFI.fd").stat().st_size

# Expected places for the R9a numbers
ADT_BASE = M4["heap_top"] + (32 << 20)
IMAGE_BASE = align_up(ADT_BASE + align(M4["devtree_size"]), 0x200000)
BOOTARGS = IMAGE_BASE + align(image_len)

# 1. --check-only: decides, touches nothing
rec, out, code = run({}, "--check-only", *image_args)
check("check-only exits 0", code == 0, out[-400:])
check("check-only does not initialize the hypervisor", not rec.hv_inited)
check("check-only prints the image base", f"firmware image       0x{IMAGE_BASE:x}" in out, out)
check("check-only marks nothing inside guest RAM", "\n  * " not in out, out)
check("16-byte memory-map entry was read", "preoslog" in out and "Something-odd" not in out)
check("check-only prints the EL1 state the firmware starts in",
      "SCTLR_EL12 = 0x30d50980 (M=0 C=0 I=0)" in out and "CPACR_EL12 = 0x0 (FPEN=0)" in out, out)

# 2. the whole run
rec, out, code = run({}, "--seconds", "30", *image_args)
check("full run exits 0", code == 0, out[-600:])
check("image loaded at the planned base", rec.loaded == (IMAGE_BASE, image_len, "firmware"),
      f"{rec.loaded}")
check("image base is 2 MB aligned and above the device tree",
      IMAGE_BASE % 0x200000 == 0 and IMAGE_BASE >= ADT_BASE + M4["devtree_size"])
check("boot arguments right after the image", rec.bootargs_at == BOOTARGS, f"{rec.bootargs_at}")
check("top_of_kernel_data is the end of the boot arguments page",
      rec.bootargs_top == BOOTARGS + 0x4000)
check("entered at the first byte with x0 = boot arguments", rec.hv_start == (IMAGE_BASE, BOOTARGS),
      f"{rec.hv_start}")
check("memory-map DeviceTree and BootArgs updated",
      rec.memory_map.DeviceTree == (ADT_BASE, align(M4["devtree_size"])) and
      rec.memory_map.BootArgs == (BOOTARGS, 0x4000))
check("everything below the firmware work area", BOOTARGS + 0x4000 <= MEM_TOP - 0x8000000)
check("a stop leaves the guest", rec.stopped == "EXIT_GUEST")
check("a stop by the timer says where the firmware was and prints the context",
      f"Firmware stopped at PC 0x{IMAGE_BASE + 0x8600:x} (firmware image + 0x8600)" in out and
      rec.context_printed, out[-900:])
check("the final dump has the firmware's own exception registers",
      all(f"{name:18s} = 0x" in out.split("==== firmware stopped ====")[-1]
          for name in ("ELR_EL12", "ESR_EL12", "FAR_EL12", "SPSR_EL12", "CPACR_EL12", "SP_EL1")),
      out[-1500:])
check("firmware output is shown, complete lines and the unfinished last one",
      "FW> Apple Silicon Project Mu Firmware (arm64/arm64e)\n" in out and
      "FW> If you can see this\n" in out, out[-600:])
check("without --disk there is no RAMDisk entry and no boot disk anywhere",
      not hasattr(rec.memory_map, "RAMDisk") and rec.writes == [] and rec.readbacks == [] and
      "boot disk" not in out.lower(), out)

# 3. something of iBoot's where the image would go
rec, out, code = run({"memory_map": {"Mystery": (IMAGE_BASE + 0x100000, 0x4000)}}, *image_args)
check("memory-map overlap with the image stops the run",
      code != 0 and "PROBLEM: memory-map entry Mystery overlaps the firmware image" in out and
      rec.loaded is None and not rec.hv_inited, out[-600:])

# 4. something of iBoot's in guest RAM but out of the way: reported, not fatal
rec, out, code = run({"memory_map": {"Mystery": (0x10200000000, 0x4000)}}, "--check-only", *image_args)
check("memory-map entry elsewhere in guest RAM is marked, not fatal",
      code == 0 and "  * Mystery" in out, out[-600:])

# 5. something in the top 128 MB
rec, out, code = run({"memory_map": {"Mystery": (MEM_TOP - 0x4000, 0x4000)}}, *image_args)
check("memory-map overlap with the work area stops the run",
      code != 0 and "overlaps the firmware work area" in out and rec.loaded is None)

# 6. framebuffer inside guest RAM
rec, out, code = run({"fb_base": 0x10300000000}, *image_args)
check("framebuffer inside guest RAM stops the run",
      code != 0 and "PROBLEM: the framebuffer lies inside guest RAM" in out and rec.loaded is None)

# 7. another m1n1
rec, out, code = run({"version": b"##m1n1_ver##v0.0.0-something-else"}, *image_args)
check("another m1n1 stops the run", code != 0 and not rec.hv_inited and rec.loaded is None)

# 8. guest RAM too small for the image
rec, out, code = run({"heap_top": MEM_TOP - 0x8000000 - (40 << 20)}, *image_args)
check("no room below the work area stops the run",
      code != 0 and "PROBLEM" in out and rec.loaded is None, out[-600:])

# 9. stopped by an exception: HV.handle_exception() has printed the context, not again
rec, out, code = run({"stop_reason": "EXCEPTION_LOWER"}, "--seconds", "30", *image_args)
check("a stop by an exception does not print the context a second time",
      code == 0 and "Firmware stopped at PC" in out and not rec.context_printed, out[-900:])

# 10. EL1 MMU left on
rec, out, code = run({"sctlr_el1": 0x30d50981}, *image_args)
check("EL1 MMU on stops the run",
      code != 0 and "PROBLEM: the EL1 MMU is on" in out and rec.loaded is None and
      not rec.hv_inited, out[-600:])

# ---- how a run ends ------------------------------------------------------------------

def wait_for_sigint(rec, timeout=5):
    deadline = time.monotonic() + timeout
    while not rec.sigints and time.monotonic() < deadline:
        time.sleep(0.02)

# 11. the time runs out: the script interrupts the hypervisor
rec, out, code = run({"guest": wait_for_sigint}, "--seconds", "1", *image_args)
check("--seconds 1 interrupts the run about a second after the guest is entered",
      code == 0 and len(rec.sigints) == 1 and 0.9 < rec.sigints[0] - rec.entered < 2.5,
      f"{rec.sigints} {rec.entered}")
check("a stop by the timer says so and prints the context",
      "Run ended: --seconds 1 ran out (HV/USER_INTERRUPT)" in out and rec.context_printed,
      out[-900:])

# 12. no limit, and the guest powers itself off (PSCI SYSTEM_OFF, src/hv/hv_psci.c)
rec, out, code = run({"guest": lambda rec: time.sleep(1.5), "stop_code": "GUEST_OFF"},
                     "--seconds", "0", *image_args)
check("--seconds 0 does not interrupt the run", code == 0 and rec.sigints == [], f"{rec.sigints}")
check("a guest that powers itself off ends the run, with the reason",
      code == 0 and rec.stopped == "EXIT_GUEST" and
      "Run ended: the guest called PSCI SYSTEM_OFF (HV/GUEST_OFF)" in out and
      "Firmware stopped at PC" in out and out.endswith("Hypervisor exited.\n"), out[-900:])
check("a stop by the hypervisor does not print the context a second time",
      not rec.context_printed)

# 13. no limit, and Ctrl-C
def ctrl_c(rec):
    os.kill(os.getpid(), signal.SIGINT)
    wait_for_sigint(rec)
rec, out, code = run({"guest": ctrl_c}, "--seconds", "0", *image_args)
check("Ctrl-C ends a run without a limit, and is not taken for the timer",
      code == 0 and rec.stopped == "EXIT_GUEST" and rec.context_printed and
      "Run ended: interrupted (Ctrl-C) (HV/USER_INTERRUPT)" in out, out[-900:])

# 14. the other two stops for good, and the timer of a run that ended before it
rec, out, code = run({"stop_code": "GUEST_RESET"}, "--seconds", "1", *image_args)
check("a guest that resets itself ends the run, with the reason",
      code == 0 and rec.stopped == "EXIT_GUEST" and not rec.context_printed and
      "Run ended: the guest called PSCI SYSTEM_RESET (HV/GUEST_RESET)" in out, out[-900:])
try:
    time.sleep(1.5)
    stray = False
except KeyboardInterrupt:
    stray = True
check("the timer does not go off after the run has ended", not stray)
rec, out, code = run({"stop_code": "GUEST_HVC"}, *image_args)
check("a guest stopped for a call that cannot be carried out ends the run, with the reason",
      code == 0 and rec.stopped == "EXIT_GUEST" and not rec.context_printed and
      "Run ended: the guest made a call that the hypervisor cannot carry out" in out and
      "(HV/GUEST_HVC)" in out, out[-900:])

# 15. an exception nobody handled, as in 9
rec, out, code = run({"stop_reason": "EXCEPTION_LOWER"}, *image_args)
check("an exception nobody handled is named as the reason",
      code == 0 and "Run ended: a guest exception that nobody handled (EXCEPTION_LOWER/SYNC)" in out,
      out[-900:])

# 16. a negative time
rec, out, code = run({}, "--seconds", "-1", *image_args)
check("a negative --seconds is refused", code != 0 and rec.loaded is None and not rec.hv_inited)

# ---- the boot disk -------------------------------------------------------------------

tmp = tempfile.TemporaryDirectory()

def make_disk(name, size, gpt=True):
    """A file of zeroes with its own offset written every 64 KB, so that a piece in the
    wrong place shows, the GPT signature where the header of a disk with 512-byte sectors
    is, and something at the very end."""
    data = bytearray(size)
    for off in range(0, size - 0x108, 0x10000):
        struct.pack_into("<Q", data, off + 0x100, off | 1 << 63)
    if gpt:
        data[512:520] = b"EFI PART"
    data[-8:] = b"disk end"[:len(data[-8:])]
    path = pathlib.Path(tmp.name) / name
    path.write_bytes(data)
    return str(path), bytes(data)

# Whole 4 KB pages, but not whole 16 KB pages, and more than one piece
DISK_LEN = (40 << 20) + 0x1000
disk_path, disk_data = make_disk("disk.img", DISK_LEN)
DISK_BASE = align_up(BOOTARGS + 0x4000, 0x200000)
untouched = lambda rec: rec.loaded is None and rec.writes == [] and not rec.hv_inited

# 17. --check-only with a disk: decides, touches nothing
rec, out, code = run({}, "--check-only", "--disk", disk_path, *image_args)
check("check-only with a disk exits 0 and writes nothing", code == 0 and untouched(rec), out[-600:])
check("check-only prints where the disk would go",
      f"boot disk            0x{DISK_BASE:x} .. 0x{DISK_BASE + DISK_LEN:x}" in out, out)
check("the disk image is identified by size and SHA-256",
      f"size 0x{DISK_LEN:x}" in out and hashlib.sha256(disk_data).hexdigest() in out, out)

# 18. the whole run with a disk
rec, out, code = run({}, "--seconds", "30", "--disk", disk_path, *image_args)
check("full run with a disk exits 0", code == 0, out[-600:])
check("the disk starts on the first 2 MB boundary after the boot arguments",
      rec.writes and rec.writes[0][0] == DISK_BASE and DISK_BASE % 0x200000 == 0 and
      0 <= DISK_BASE - (BOOTARGS + 0x4000) < 0x200000, f"{rec.writes[:1]}"[:80])
pieces_follow = all(a_base + len(a) == b_base
                    for (a_base, a), (b_base, b) in zip(rec.writes, rec.writes[1:]))
check("the disk arrives whole, in pieces that follow each other",
      pieces_follow and b"".join(data for base, data in rec.writes) == disk_data)
check("it goes up in more than one piece, none larger than 32 MB",
      len(rec.writes) > 1 and max(len(data) for base, data in rec.writes) <= 32 << 20,
      f"{[len(data) for base, data in rec.writes]}")
check("both ends are read back, one 4 KB page each",
      rec.readbacks == [(DISK_BASE, 0x1000), (DISK_BASE + DISK_LEN - 0x1000, 0x1000)],
      f"{rec.readbacks}")
check("the disk is in place before the guest is entered",
      rec.writes_at_entry == len(rec.writes) and rec.hv_start == (IMAGE_BASE, BOOTARGS))
check("memory-map RAMDisk is the base and the exact size of the image",
      getattr(rec.memory_map, "RAMDisk", None) == (DISK_BASE, DISK_LEN),
      f"{getattr(rec.memory_map, '_properties', None)}")
check("with a disk the image, DeviceTree and BootArgs are where they are without one",
      rec.loaded == (IMAGE_BASE, image_len, "firmware") and rec.bootargs_at == BOOTARGS and
      rec.memory_map.DeviceTree == (ADT_BASE, align(M4["devtree_size"])) and
      rec.memory_map.BootArgs == (BOOTARGS, 0x4000))
check("top_of_kernel_data is the end of the disk, rounded up to 16 KB",
      rec.bootargs_top == align(DISK_BASE + DISK_LEN) and rec.bootargs_top % 0x4000 == 0,
      f"{rec.bootargs_top}")
check("the upload reports size, time and rate",
      re.search(rf"Boot disk: 0x{DISK_LEN:x} bytes in \d+\.\d s, \d+\.\d MB/s", out), out)

# 19. images that are refused before any port is opened
odd_path, _ = make_disk("odd.img", (1 << 20) + 512)
empty_path, _ = make_disk("empty.img", 0, gpt=False)
plain_path, _ = make_disk("plain.img", 1 << 20, gpt=False)
for what, path, message in (("a size that is not a multiple of 4096", odd_path, "not a multiple of 0x1000"),
                            ("an empty file", empty_path, "empty"),
                            ("no GPT signature", plain_path, "EFI PART")):
    rec, out, code = run({}, "--disk", path, *image_args)
    check(f"a disk image with {what} is refused offline",
          isinstance(code, str) and message in code and untouched(rec) and
          "m1n1 expected" not in out, f"{code!r}")

# 20. --image-only looks at the disk image too
rec, out, code = run({}, "--image-only", "--disk", disk_path, *image_args)
check("--image-only accepts a good disk image",
      code == 0 and hashlib.sha256(disk_data).hexdigest() in out and "m1n1 expected" not in out, out)
rec, out, code = run({}, "--image-only", "--disk", plain_path, *image_args)
check("--image-only refuses a bad disk image", code != 0)

# 21. the smallest disk there can be
tiny_path, tiny_data = make_disk("tiny.img", 0x1000)
rec, out, code = run({}, "--disk", tiny_path, *image_args)
check("a disk of one 4 KB page is placed",
      code == 0 and rec.writes == [(DISK_BASE, tiny_data)] and
      rec.memory_map.RAMDisk == (DISK_BASE, 0x1000) and rec.bootargs_top == DISK_BASE + 0x4000,
      out[-600:])

# 22. no room for the disk below the firmware work area: the image and the boot arguments
# end 64 MB above the start of guest RAM, which leaves 34 MB from the next 2 MB boundary
small = {"heap_top": MEM_TOP - 0x8000000 - (100 << 20)}
for mode in ((), ("--check-only",)):
    rec, out, code = run(small, *mode, "--disk", disk_path, *image_args)
    check(f"a disk that reaches into the work area stops the run {' '.join(mode)}".rstrip(),
          code != 0 and "PROBLEM: the boot disk reaches into the firmware work area" in out and
          out.count("PROBLEM") == 1 and untouched(rec), out[-600:])
rec, out, code = run(small, "--check-only", *image_args)
check("the same machine without a disk is fine", code == 0, out[-600:])

# 23. something of iBoot's where the disk would go
rec, out, code = run({"memory_map": {"Mystery": (DISK_BASE + (20 << 20), 0x4000)}},
                     "--disk", disk_path, *image_args)
check("memory-map overlap with the disk stops the run",
      code != 0 and "PROBLEM: memory-map entry Mystery overlaps the boot disk" in out and
      untouched(rec), out[-600:])
rec, out, code = run({"memory_map": {"Mystery": (DISK_BASE + DISK_LEN, 0x4000)}},
                     "--check-only", "--disk", disk_path, *image_args)
check("memory-map entry right after the disk is not an overlap", code == 0, out[-600:])

# 24. a piece that m1n1 could not unpack into what was sent
rec, out, code = run({"bad_write": 2}, "--disk", disk_path, *image_args)
check("a piece of the disk that arrives damaged stops the run before the guest is entered",
      isinstance(code, str) and "did not arrive intact" in code and rec.hv_start is None and
      len(rec.writes) == 1, f"{code!r}")

# 25. something else than the disk where its last page should be
rec, out, code = run({"bad_readback": 2}, "--disk", disk_path, *image_args)
check("a disk that reads back differently stops the run before the guest is entered",
      isinstance(code, str) and "reads back" in code and rec.hv_start is None, f"{code!r}")

print(f"{failures} failure(s)")
sys.exit(1 if failures else 0)
