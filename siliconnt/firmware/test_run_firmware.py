#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run run_firmware.py against a fake target, without any hardware.

The proxy, ProxyUtils and HV are replaced by stand-ins that answer with the numbers the M4
MacBook Pro reported on 2026-11-13 (R9a: SiliconNT docs/lab/2026-11-13-wfi-trap-plan/);
m1n1.utils and m1n1.sysreg are the real ones. The secondary serial port is a pty, and the
kernel debugger is a TCP client on this machine. This checks what the script decides, what
it would write where and what it passes between the two, not the hypervisor.

Usage: test_run_firmware.py [firmware image]   (needs build/m1n1.bin of this tree)
"""
import sys, os, io, pathlib, runpy, struct, types, contextlib, threading, time, signal, enum
import re, hashlib, tempfile, socket, select

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "run_firmware.py"
PROXYCLIENT = HERE.parents[1] / "proxyclient"
M1N1_BIN = HERE.parents[1] / "build" / "m1n1.bin"
# The image run_firmware.py loads when it is not given one, and this test then, too
DEFAULT_IMAGE = (pathlib.Path.home() / "src/siliconnt-firmware/Build/SiliconNT-AARCH64/"
                 "DEBUG_CLANGPDB/FV/SILICONNT_EFI.fd")
sys.path.append(str(PROXYCLIENT))

from m1n1.utils import align, align_up
from m1n1 import sysreg
from m1n1.tgtypes import BootArgs_r3

# What the M4 reported (R9a, hello-before-r9a.txt and wfitest-1cpu.txt)
M4 = dict(
    m1n1_base=0x10002e70000,
    ba_phys_base=0x10002e70000,
    ba_mem_size=0x3d87a0000,
    # all of the 16 GB, mem_size_act (R23: docs/lab/2026-11-15-selfboot-rehearsal-plan/)
    ba_mem_size_actual=0x400000000,
    devtree_size=0x84000,
    heap_top=0x10018eb8000,
    fb_base=0x103e004c000, fb_stride=0x2f40, fb_width=3024, fb_height=1964,
    uart=0x3ad200000,
)
MEM_TOP = M4["ba_phys_base"] + M4["ba_mem_size"]
assert MEM_TOP == 0x103db610000

def aicmap(lines, version=1, count=None):
    """A table of AIC lines and SPIs as m1n1 writes it: version, count, the lines."""
    count = len(lines) if count is None else count
    return struct.pack(f"<II{len(lines)}I", version, count, *lines)

# 391 lines from 0 to 1505, as the M4 has them; which ones in between does not matter here
AICMAP_M4_LINES = [0] + list(range(100, 489)) + [1505]
AICMAP_M4 = aicmap(AICMAP_M4_LINES)
assert len(AICMAP_M4_LINES) == 391

# For --loader m1n1. Three of the ranges of /arm-io that R21 mapped (SiliconNT
# docs/lab/2026-11-15-boot-disk-4k-plan/run.txt); the UART lies in the first.
ARM_IO = [(0x200000000, 0x3a0000000), (0x800000000, 0x2e0000000), (0x1cb0000000, 0x10000000)]
# What m1n1 keeps for itself above its heap on this M4: 32 MB + 16 GB / 512 + 16 MB
# (selfboot_m1n1_heap() of src/selfboot.c)
M1N1_KEPT = 80 << 20
# ProxyUtils keeps that for m1n1's heap, and 128 MB for its own, below the start of guest RAM
HOST_HEAP_BASE = M4["heap_top"] - (128 << 20)
M1N1_HEAP_TOP = HOST_HEAP_BASE - M1N1_KEPT
# Where the proxy says the boot arguments are (P_GET_BOOTARGS), and their revision
BOOTARGS_ADDR, BOOTARGS_REV = 0x10005058000, 3
RAM_BASE = M4["heap_top"] & ~0xffffffff

class EXC(enum.IntEnum):
    SYNC = 0
    IRQ = 1
    FIQ = 2
    SERROR = 3

# proxyclient/m1n1/hv/types.py
class HV_EVENT(enum.IntEnum):
    HOOK_VM = 1
    VTIMER = 2
    USER_INTERRUPT = 3
    WDT_BARK = 4
    CPU_SWITCH = 5
    VIRTIO = 6
    PANIC = 7
    GUEST_RESET = 8
    GUEST_OFF = 9
    GUEST_HVC = 10

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

# The M4 has ten cores, and m1n1 starts on the one with index 6 (SiliconNT docs/findings/0017,
# section 12)
M4_CPUS = 10
M4_BOOT_CPU = 6

class FakeCpus:
    """/cpus as HV.setup_adt() of proxyclient/m1n1/hv/__init__.py uses it: its children one
    after the other, each with a name and a state, and a child deleted by its name."""
    def __init__(self):
        self.nodes = [FakeNode(name=f"cpu{i}", _path=f"/cpus/cpu{i}",
                               state="running" if i == M4_BOOT_CPU else "waiting")
                      for i in range(M4_CPUS)]
    def __iter__(self):
        return iter(self.nodes)
    def __len__(self):
        return len(self.nodes)
    def __delitem__(self, name):
        for node in self.nodes:
            if node.name == name:
                self.nodes.remove(node)
                return
        raise KeyError(name)

def make_fakes(case):
    """Fresh stand-in modules for one run. case: dict of overrides."""
    m1n1_image = M1N1_BIN.read_bytes()
    rec = types.SimpleNamespace(loaded=None, bootargs_top=None, hv_start=None, stopped=None,
                                secondaries_started=None, smp_at_start=None, guest_cpus=None,
                                hv_inited=False, memory_map=None, context_printed=False,
                                entered=None, sigints=[], writes=[], writes_at_entry=None,
                                readbacks=[], printed=b"", kd={}, xtrack=None,
                                xtrack_state=None,
                                # what ProxyUtils was told to keep for m1n1; whether tracking
                                # was on at each HV.pt_update() of the script
                                m1n1_heap=None, pt_updates=[],
                                # --loader m1n1
                                selfboot=None, selfboot_sent=0.0, selfboot_done=False, interrupts=[],
                                timeout_at_entry="not entered", debugger_at_entry=None,
                                pt_walks=[], heap_reads=[], context_fault=None, image_base=None,
                                # --bugcheck: what rec.stopped was at each read of guest memory
                                guest_reads=[],
                                # P_HV_AICMAP: where m1n1 was asked to write the table, and
                                # what it wrote there
                                aicmap_calls=[], tables=[],
                                # --no-host: P_SELFBOOT_RUN said there is no host; how many
                                # times the port to m1n1 was flushed, and when
                                no_host=None, flushes=[])
    heap_top = case.get("heap_top", M4["heap_top"])

    def stage2(addr):
        """What hv_pt_walk() of src/hv/hv_vm.c returns for the tables selfboot_map() of
        src/selfboot_run.c builds: the page itself with the valid bit for memory and device
        registers, a software entry without it for the UART, 0 for what is not mapped.
        xonly_pt: pages made execute-only by execution tracking (src/hv/hv_vm.c,
        hv_pt_set_scanned()), each mapped to the page given: no access, PTE_SW_SCANNED and
        PTE_SW_XONLY."""
        if addr in case.get("xonly_pt", {}):
            return case["xonly_pt"][addr] | 3 | 1 << 56 | 1 << 58
        if addr == case.get("bad_pt"):
            return 0
        if addr == case.get("extra_pt"):
            return addr | 3
        if addr == case.get("wrong_pt"):
            return addr + 0x1000 | 1
        if M4["uart"] <= addr < M4["uart"] + 0x4000:
            return 1 << 50 | addr & 0xfff
        if any(base <= addr < base + size for base, size in ARM_IO):
            return addr | 1
        if RAM_BASE <= addr < M4["ba_phys_base"] or heap_top <= addr < MEM_TOP:
            return addr | 3 | 2 << 53
        return 0

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
        def __init__(self):
            self.handlers = {}
            # The port to m1n1. While the guest runs, a byte written to it brings the
            # hypervisor back with HV/USER_INTERRUPT (hv_tick(), src/hv/hv.c).
            def write(data):
                rec.interrupts.append((time.monotonic(), bytes(data)))
                rec.sigints.append(time.monotonic())
            def flush():
                rec.flushes.append(rec.no_host)
            self.dev = types.SimpleNamespace(timeout=3, write=write, flush=flush)
        def set_handler(self, reason, code, handler):
            self.handlers[(reason, code)] = handler
        def unkhandler(self, data):
            # what m1n1 prints on its console arrives between the replies
            sys.stdout.write("".join("TTY> " + line for line in
                                     bytes(data).decode().splitlines(keepends=True)))
        def readstruct(self, addr, stype):
            if stype is BootArgs_r3:
                assert addr == BOOTARGS_ADDR
                return types.SimpleNamespace(mem_size_actual=case.get(
                    "mem_size_actual", M4["ba_mem_size_actual"]))
            assert stype == "ExcInfo"
            return types.SimpleNamespace(elr=rec.image_base + 0x8600)
        def readmem(self, addr, size):
            # the table of AIC lines and SPIs that m1n1 wrote (M1N1Proxy.hv_aicmap() below)
            for base, written in rec.tables:
                if base <= addr and addr + size <= base + len(written):
                    return written[addr - base:addr - base + size]
            # what ProxyUtils.compressed_writemem() below has put into guest memory
            for base, written in rec.writes:
                if base <= addr and addr + size <= base + len(written):
                    rec.readbacks.append((addr, size))
                    data = bytearray(written[addr - base:addr - base + size])
                    if len(rec.readbacks) == case.get("bad_readback"):
                        data[-1] ^= 1
                    return bytes(data)
            # --bugcheck: memory of the guest as the case has it, (first, after last, read)
            first, top, read = case.get("guest_mem", (0, 0, None))
            if first <= addr < top:
                rec.guest_reads.append(rec.stopped)
                return read(addr, size)
            off = addr - M4["m1n1_base"]
            data = m1n1_image[off:off + size]
            return case.get("version", data)

    class M1N1Proxy:
        def __init__(self, iface, debug=False):
            self.iface = iface
        def get_bootargs_rev(self):
            return BOOTARGS_ADDR, case.get("bootargs_rev", BOOTARGS_REV)
        def heapblock_alloc(self, size):
            assert size == 0
            rec.heap_reads.append(rec.selfboot_done)
            return M1N1_HEAP_TOP + (case.get("heap_growth", 31 << 20) if rec.selfboot_done else 0)
        def hv_pt_walk(self, addr):
            rec.pt_walks.append((addr, rec.selfboot_done))
            return stage2(addr)
        def exit(self, value):
            rec.stopped = value
        def selfboot_run(self, phys_base, image_size, disk_size, no_host=False):
            """selfboot_run_loaded() of src/selfboot_run.c, as the host sees it: lines on
            m1n1's console, then nothing until the hypervisor has an event for the host, and
            the reply once the guest has been left. With no_host the command goes out and
            nothing is waited for (proxy.py sends it with no_reply)."""
            rec.no_host = no_host
            rec.selfboot = (phys_base, image_size, disk_size)
            if no_host:
                rec.selfboot_sent = time.monotonic()
                rec.writes_at_entry = len(rec.writes)
                return None
            rec.selfboot_sent = time.monotonic()
            rec.writes_at_entry = len(rec.writes)
            rec.timeout_at_entry = self.iface.dev.timeout
            rec.debugger_at_entry = bool(rec.kd)
            rec.image_base = align_up(phys_base + (32 << 20) + align(M4["devtree_size"]), 0x200000)
            if case.get("selfboot_ret"):
                self.iface.unkhandler(b"selfboot: the firmware image has no arm64 magic\n")
                return case["selfboot_ret"]
            self.iface.unkhandler(f"selfboot: guest RAM 0x{phys_base:x} .. 0x{MEM_TOP:x}\n".encode())
            time.sleep(case.get("setup_time", 0))
            self.iface.unkhandler(f"selfboot: entering the firmware at 0x{rec.image_base:x}, "
                                  f"x0 = 0x{rec.image_base + align(image_size):x}\n".encode())
            rec.entered = time.monotonic()
            guest_prints(rec, b"Apple Silicon Project Mu Firmware (arm64/arm64e)\r\n"
                              b"If you can see this")
            time.sleep(0.5)  # let the serial reader see it
            case.get("guest", lambda rec: None)(rec)
            reason = START[case.get("stop_reason", "HV")]
            if reason == START.HV:
                code = HV_EVENT[case.get("stop_code", "USER_INTERRUPT")]
                if code == HV_EVENT.USER_INTERRUPT:
                    deadline = time.monotonic() + 10
                    while not rec.interrupts and time.monotonic() < deadline:
                        time.sleep(0.02)
                    if not rec.interrupts:
                        raise RuntimeError("nobody stops the guest: the real call would not return")
                code = int(code)  # UartInterface.handle_boot() passes these as they come
            else:
                code = EXC[case.get("stop_code", "SYNC")]
            self.iface.handlers[(reason, code)](reason, code, 0x10007ff0000)
            if rec.stopped != "EXIT_GUEST":
                raise RuntimeError("the event was not answered: the real call would not return")
            self.iface.unkhandler(b"HV: Exiting hypervisor (main CPU)\n"
                                  b"HV: debug registers: 3650 reads and 3650 writes of the guest "
                                  b"answered\n")
            rec.selfboot_done = True
            return 0
        def hv_aicmap(self, dest):
            """hv_aicmap() of src/hv/hv_layout.c: the table of the M4, 391 lines from 0 to
            1505 (SiliconNT docs/findings/0058), unless the case has another, or m1n1 refuses
            (-1)."""
            rec.aicmap_calls.append(dest)
            if "aicmap_ret" in case:
                return case["aicmap_ret"]
            table = case.get("aicmap_table", AICMAP_M4)
            rec.tables.append((dest, table))
            return len(table)
        def hv_xtrack(self):
            rec.xtrack = True
            # what was done already, and what was not, when tracking was turned on
            rec.xtrack_state = (rec.loaded is not None, len(rec.writes), rec.hv_start is None)
            return case.get("xtrack_ret", 0)
        def hv_start(self, entry, *regs):
            rec.hv_start = (entry, *regs)
            rec.entered = time.monotonic()
            rec.writes_at_entry = len(rec.writes)

    class EXC_RET:
        EXIT_GUEST = "EXIT_GUEST"

    class START(enum.IntEnum):
        EXCEPTION = 1
        EXCEPTION_LOWER = 2
        HV = 3

    video = types.SimpleNamespace(base=case.get("fb_base", M4["fb_base"]), stride=M4["fb_stride"],
                                  width=M4["fb_width"], height=M4["fb_height"])
    ba = types.SimpleNamespace(phys_base=M4["ba_phys_base"], mem_size=M4["ba_mem_size"],
                               devtree_size=M4["devtree_size"], video=video)

    class ProxyUtils:
        def __init__(self, p, heap_size, m1n1_heap):
            rec.m1n1_heap = m1n1_heap
            self.base = M4["m1n1_base"]
            self.heap_top = heap_top
            self.heap_base = heap_top - heap_size
            self.ba = ba
            uart = types.SimpleNamespace(get_reg=lambda i: (M4["uart"], 0x4000))
            arm_io = types.SimpleNamespace(ranges=[
                types.SimpleNamespace(parent_addr=base, size=size) for base, size in ARM_IO])
            self.adt = FakeADT({"chosen": {"memory-map": memory_map}, "/arm-io": arm_io,
                                "/arm-io/uart0": uart, "cpus": FakeCpus()})
            self.regs = {
                sysreg.HCR_EL2: sysreg.HCR(IMO=1, FMO=1, VM=1, TWI=1).value,
                sysreg.CNTHCTL_EL2: 4,
                sysreg.ICH_HCR_EL2: 1,
                sysreg.ICH_VTR_EL2: 0x90580007,
                # docs/lab/2026-11-11-r1-r2-plan/r1.txt, line 8
                sysreg.SCTLR_EL12: case.get("sctlr_el1", 0x30d50980),
                sysreg.CPACR_EL12: 0,
            }
            self.regs.update(case.get("regs", {}))
        def mrs(self, reg):
            if reg in case.get("mrs_fails", ()):
                raise RuntimeError("the proxy did not answer")
            return self.regs.get(reg, 0)
        def compressed_writemem(self, dest, data, progress=None):
            # The real one sends a gzip stream and has m1n1 unpack it at dest. m1n1 compares
            # the CRC-32 of what it unpacked with the one in the stream (src/tinf/tinfgzip.c)
            # and returns an error instead of the size if they differ, which trips
            # "assert decompressed_size == len(data)" in proxyclient/m1n1/proxyutils.py.
            if not rec.writes:
                case.get("at_upload", lambda rec: None)(rec)
            if len(rec.writes) + 1 == case.get("bad_write"):
                raise AssertionError
            rec.writes.append((dest, bytes(data)))
        def print_context(self, ctx, is_fault=True):
            rec.context_printed = True
            rec.context_fault = is_fault
            # in the middle of what the script does about an event
            case.get("at_context", lambda rec: None)(rec)
            print(f"  ELR    = 0x{ctx.elr:x}")

    class HV:
        def __init__(self, iface, p, u):
            self.p, self.u = p, u
            self.smp = True
            uart = types.SimpleNamespace(get_reg=lambda i: (M4["uart"], 0x4000))
            # A tree of its own, as the real one has: load_adt(self.u.get_adt())
            self.adt = FakeADT({"/arm-io/uart0": uart, "chosen": {"memory-map": FakeNode()},
                                "cpus": FakeCpus()})
            rec.memory_map = self.adt["chosen"]["memory-map"]
        def init(self):
            # The two things the real one does by self.smp, in its order: whether it has the
            # proxy start the secondary CPUs, and, in setup_adt(), which it calls as
            # self.setup_adt() after that, what it leaves of /cpus. That the real one has no
            # third use, and these two as they are here, is checked at the end of this file.
            rec.secondaries_started = self.smp
            self.mem_top = self.u.ba.phys_base + self.u.ba.mem_size
            rec.hv_inited = True
            self.setup_adt()
            # The first thing after the checks, long before the guest runs
            case.get("at_init", lambda rec: None)(rec)
        def setup_adt(self):
            if not self.smp:
                for cpu in list(self.adt["cpus"]):
                    if cpu.state != "running":
                        del self.adt["cpus"][cpu.name]
            # A setup_adt() that drops a node whatever self.smp is: not the real one of today
            if case.get("hv_drops_cpu"):
                del self.adt["cpus"][case["hv_drops_cpu"]]
        def _prepare_load(self):
            # as in proxyclient/m1n1/hv/__init__.py
            self.phys_base = guest_base = self.u.heap_top
            guest_base += 32 << 20
            self.guest_base = guest_base
            self.mem_size = self.mem_top - self.phys_base
        def _load_executable_raw(self, image, base, label):
            rec.loaded = (base, len(image), label)
        def pt_update(self):
            rec.pt_updates.append((rec.loaded is not None, rec.xtrack))
        def _write_boot_args(self, top_of_kernel_data):
            rec.bootargs_top = top_of_kernel_data
            rec.bootargs_at = self.guest_base + self.bootargs_off
        def start(self):
            # The real one sends the proxy a byte on SIGINT, and the hypervisor comes back
            # with HV/USER_INTERRUPT. Here the signal is only noted.
            signal.signal(signal.SIGINT, lambda *a: rec.sigints.append(time.monotonic()))
            # The real one builds the device tree of the guest from self.adt here
            rec.smp_at_start = self.smp
            rec.guest_cpus = [(cpu.name, cpu.state) for cpu in self.adt["cpus"]]
            self.p.hv_start(self.entry, self.guest_base + self.bootargs_off)
            # The "firmware" prints once it runs: opening a serial port drops what came before
            guest_prints(rec, b"Apple Silicon Project Mu Firmware (arm64/arm64e)\r\n"
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
                           START=START, EXC=EXC, ExcInfo="ExcInfo"),
        "m1n1.proxyutils": dict(ProxyUtils=ProxyUtils, bootstrap_port=lambda iface, p: None),
        "m1n1.hv": dict(HV=HV, HV_EVENT=HV_EVENT),
        "m1n1.hw": dict(),
        "m1n1.hw.pmu": dict(PMU=PMU),
    }
    for name, contents in mods.items():
        mod = types.ModuleType(name)
        mod.__dict__.update(contents)
        # As in proxyclient/m1n1/proxy.py: "from ... import *" brings the functions and the
        # classes and nothing else. ExcInfo is neither (R23: the script died on that name).
        mod.__all__ = [name for name, value in contents.items()
                       if callable(value) or isinstance(value, type)]
        sys.modules[name] = mod
    return rec

def guest_prints(rec, data):
    """The guest writes to its UART: bytes arrive on the secondary serial port."""
    rec.printed += data
    while data:
        data = data[os.write(rec.master, data):]

def guest_reads(rec, size, timeout=3):
    """What has been written to the secondary serial port, which the guest's UART receives."""
    data = b""
    deadline = time.monotonic() + timeout
    while len(data) < size and time.monotonic() < deadline:
        if select.select([rec.master], [], [], 0.02)[0]:
            data += os.read(rec.master, size - len(data))
    return data

def debugger_reads(sock, size, timeout=3):
    """Up to size bytes from the relay; fewer if it closes the connection or nothing comes."""
    data = b""
    sock.settimeout(timeout)
    try:
        while len(data) < size:
            more = sock.recv(size - len(data))
            if not more:
                return data + b"<closed>"
            data += more
    except TimeoutError:
        return data + b"<nothing>"
    return data

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def run(case, *argv, kd_port=None):
    """kd_port: None for a port that is free now, "default" to leave the option out."""
    rec = make_fakes(case)
    master, slave = os.openpty()
    rec.master = master
    rec.kd_port = 51000 if kd_port == "default" else kd_port or free_port()
    if kd_port != "default":
        argv = ("--kd-port", str(rec.kd_port), *argv)
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
    image_len = DEFAULT_IMAGE.stat().st_size

# Expected places for the R9a numbers
ADT_BASE = M4["heap_top"] + (32 << 20)
IMAGE_BASE = align_up(ADT_BASE + align(M4["devtree_size"]), 0x200000)
BOOTARGS = IMAGE_BASE + align(image_len)
# The table of AIC lines and SPIs: the page after the boot arguments
AICMAP = BOOTARGS + 0x4000

# 1. --check-only: decides, touches nothing
rec, out, code = run({}, "--check-only", *image_args)
check("check-only exits 0", code == 0, out[-400:])
check("check-only does not initialize the hypervisor", not rec.hv_inited)
check("check-only prints the image base", f"firmware image       0x{IMAGE_BASE:x}" in out, out)
check("check-only marks nothing inside guest RAM", "\n  * " not in out, out)
check("16-byte memory-map entry was read", "preoslog" in out and "Something-odd" not in out)
check("check-only prints the EL1 state the firmware starts in",
      "SCTLR_EL12 = 0x30d50980 (M=0 C=0 I=0)" in out and "CPACR_EL12 = 0x0 (FPEN=0)" in out, out)
check("check-only prints where the table of AIC lines goes, and does not have it written",
      f"AIC to GIC table     0x{AICMAP:x} .. 0x{AICMAP + 0x4000:x}" in out and
      rec.aicmap_calls == [], out)

# 1a. what ProxyUtils keeps for m1n1 above its heap: what m1n1 keeps when it loads by itself,
# by the size of memory
check("m1n1 is kept 80 MB on 16 GB of memory", rec.m1n1_heap == M1N1_KEPT, f"{rec.m1n1_heap}")
rec, out, code = run({"mem_size_actual": 128 << 30}, "--check-only", *image_args)
check("...and 304 MB on 128 GB", rec.m1n1_heap == 304 << 20, f"{rec.m1n1_heap}")
rec, out, code = run({"bootargs_rev": 4}, "--check-only", *image_args)
check("boot arguments of a revision ProxyUtils does not know end the run before it is made",
      isinstance(code, str) and "revision 4" in code and rec.m1n1_heap is None, f"{code!r}")

# 2. the whole run
rec, out, code = run({}, "--seconds", "30", *image_args)
check("full run exits 0", code == 0, out[-600:])
check("image loaded at the planned base", rec.loaded == (IMAGE_BASE, image_len, "firmware"),
      f"{rec.loaded}")
check("image base is 2 MB aligned and above the device tree",
      IMAGE_BASE % 0x200000 == 0 and IMAGE_BASE >= ADT_BASE + M4["devtree_size"])
check("boot arguments right after the image", rec.bootargs_at == BOOTARGS, f"{rec.bootargs_at}")
check("top_of_kernel_data is the end of the page of the table after the boot arguments",
      rec.bootargs_top == AICMAP + 0x4000)
check("m1n1 writes the table of AIC lines to the page after the boot arguments, once",
      rec.aicmap_calls == [AICMAP], f"{rec.aicmap_calls}")
check("memory-map AICToGIC is that page and the size of the table in bytes",
      getattr(rec.memory_map, "AICToGIC", None) == (AICMAP, 8 + 4 * 391),
      f"{getattr(rec.memory_map, '_properties', None)}")
# What the firmware checks of the entry before it reads the table (AppleGetAicToGic() of
# AppleDTLib.c, siliconnt-firmware aic-to-gic 8b3fe9d): the size is 8 + 4 * the count the
# table has; the address is the start of a 4 KB page; all of it lies in guest RAM, which is
# the memory the boot arguments describe (HV._write_boot_args()); the lines go up strictly
aicmap_base, aicmap_size = getattr(rec.memory_map, "AICToGIC", (1, 0))
aicmap_written = dict(rec.tables).get(aicmap_base, b"")
aicmap_count = struct.unpack_from("<I", aicmap_written, 4)[0] if len(aicmap_written) >= 8 else -1
aicmap_lines = struct.unpack_from(f"<{max(aicmap_count, 0)}I", aicmap_written, 8)
check("AICToGIC is what the firmware accepts: size, page, in guest RAM, strictly ascending",
      aicmap_size == 8 + 4 * aicmap_count and aicmap_base % 0x1000 == 0 and
      M4["heap_top"] <= aicmap_base and aicmap_base + aicmap_size <= MEM_TOP and
      all(a < b for a, b in zip(aicmap_lines, aicmap_lines[1:])),
      f"{aicmap_base:#x} {aicmap_size:#x} {aicmap_count}")
check("the table m1n1 wrote is printed: its size, its count, its first and last line",
      f"AIC to GIC table at 0x{AICMAP:x}, 0x{8 + 4 * 391:x} bytes: 391 lines, AIC 0 is SPI 32, "
      "AIC 1505 is SPI 422" in out, out)
check("entered at the first byte with x0 = boot arguments", rec.hv_start == (IMAGE_BASE, BOOTARGS),
      f"{rec.hv_start}")
check("memory-map DeviceTree and BootArgs updated",
      rec.memory_map.DeviceTree == (ADT_BASE, align(M4["devtree_size"])) and
      rec.memory_map.BootArgs == (BOOTARGS, 0x4000))
check("everything below the firmware work area", AICMAP + 0x4000 <= MEM_TOP - 0x8000000)
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

# 10a. m1n1 writes no table of AIC lines and SPIs (it has more lines than SPIs, say)
rec, out, code = run({"aicmap_ret": -1}, *image_args)
check("no table of AIC lines stops the run before the guest is entered",
      isinstance(code, str) and "P_HV_AICMAP returned -1" in code and rec.hv_start is None and
      not hasattr(rec.memory_map, "AICToGIC"), f"{code!r}")

# 10b. a table that is not one of version 1: another version, a count that is not the number
# of lines, lines out of order or twice, a size that is not whole lines
for what, case in (("another version", {"aicmap_table": aicmap([1, 2], version=2)}),
                   ("a count that is wrong", {"aicmap_table": aicmap([1, 2], count=3)}),
                   ("lines out of order", {"aicmap_table": aicmap([2, 1])}),
                   ("a line twice", {"aicmap_table": aicmap([1, 1])}),
                   ("half a line", {"aicmap_table": aicmap([1, 2]) + b"\0\0"})):
    rec, out, code = run(case, *image_args)
    check(f"a table with {what} stops the run before the guest is entered",
          isinstance(code, str) and rec.hv_start is None and
          not hasattr(rec.memory_map, "AICToGIC"), f"{code!r}")

# 10c. a table without lines is a table all the same
rec, out, code = run({"aicmap_table": aicmap([])}, "--seconds", "30", *image_args)
check("a table without lines is handed to the firmware",
      code == 0 and rec.memory_map.AICToGIC == (AICMAP, 8) and
      f"AIC to GIC table at 0x{AICMAP:x}, 0x8 bytes: no lines" in out, out[-600:])

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
      "PSCI CPU_ON for a core that EL2 does not have, or an hvc other than #0" in out and
      "CPU_OFF" not in out and "(HV/GUEST_HVC)" in out, out[-900:])

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
DISK_BASE = align_up(AICMAP + 0x4000, 0x200000)
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
check("the disk starts on the first 2 MB boundary after the table",
      rec.writes and rec.writes[0][0] == DISK_BASE and DISK_BASE % 0x200000 == 0 and
      0 <= DISK_BASE - (AICMAP + 0x4000) < 0x200000, f"{rec.writes[:1]}"[:80])
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
check("with a disk the image, DeviceTree, BootArgs and AICToGIC are where they are without one",
      rec.loaded == (IMAGE_BASE, image_len, "firmware") and rec.bootargs_at == BOOTARGS and
      rec.memory_map.DeviceTree == (ADT_BASE, align(M4["devtree_size"])) and
      rec.memory_map.BootArgs == (BOOTARGS, 0x4000) and
      rec.memory_map.AICToGIC == (AICMAP, 8 + 4 * 391))
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

# ---- the relay for the kernel debugger -----------------------------------------------

ALL = bytes(range(256))
debugger = lambda rec: socket.create_connection(("127.0.0.1", rec.kd_port), timeout=3)
PACKETS = [b"\x30\x30\x30\x30" + bytes([i]) for i in range(10)]
ANSWERS = [b"\x69\x69\x69\x69" + bytes([i]) for i in range(10)]

# 26. a debugger, a second one that has to wait its turn, and one after the first has left
def kd_session(rec):
    kd = rec.kd
    a = debugger(rec)
    a.sendall(ALL)
    kd["to_target"] = guest_reads(rec, 256)
    guest_prints(rec, b"\n" + ALL[::-1])
    kd["to_debugger"] = debugger_reads(a, 257)
    # the way the protocol goes: a packet from the target, an answer, the next packet
    kd["packets"], kd["answers"] = [], []
    started = time.monotonic()
    for packet, answer in zip(PACKETS, ANSWERS):
        guest_prints(rec, packet)
        kd["packets"].append(debugger_reads(a, len(packet)))
        a.sendall(answer)
        kd["answers"].append(guest_reads(rec, len(answer)))
    kd["exchange_time"] = time.monotonic() - started
    b = debugger(rec)
    kd["second"] = debugger_reads(b, 1)
    b.close()
    a.sendall(b"still here")
    kd["first_after_second"] = guest_reads(rec, 10)
    a.close()
    time.sleep(0.5)
    guest_prints(rec, b"\r\nprinted with no debugger\r\n")
    time.sleep(0.3)
    c = debugger(rec)
    c.sendall(b"second debugger")
    kd["again_to_target"] = guest_reads(rec, 15)
    guest_prints(rec, b"for the second one\n")
    kd["again_to_debugger"] = debugger_reads(c, 19)
    kd["left_open"] = c

log_path = pathlib.Path(tmp.name) / "serial.log"
rec, out, code = run({"guest": kd_session}, "--serial-log", str(log_path), *image_args)
kd = rec.kd
check("a run with a debugger exits 0", code == 0, out[-900:])
check("the script says where it listens",
      f"Kernel debugger: 127.0.0.1:{rec.kd_port}" in out, out[:3000])
check("debugger to target: all 256 byte values arrive unchanged", kd.get("to_target") == ALL,
      f"{kd.get('to_target')}")
check("target to debugger: all 256 byte values arrive unchanged",
      kd.get("to_debugger") == b"\n" + ALL[::-1], f"{kd.get('to_debugger')}")
check("ten packets and ten answers pass, one after the other",
      kd.get("packets") == PACKETS and kd.get("answers") == ANSWERS,
      f"{kd.get('packets')} {kd.get('answers')}")
check("the ten exchanges take less than half a second", kd.get("exchange_time", 99) < 0.5,
      f"{kd.get('exchange_time')}")
check("a second connection is closed while a debugger is connected, and the first goes on",
      kd.get("second") == b"<closed>" and kd.get("first_after_second") == b"still here",
      f"{kd.get('second')} {kd.get('first_after_second')}")
check("the script goes on when the debugger disconnects",
      "FW> printed with no debugger\n" in out and rec.stopped == "EXIT_GUEST", out[-900:])
check("another debugger can connect later, and bytes pass both ways",
      kd.get("again_to_target") == b"second debugger" and
      kd.get("again_to_debugger") == b"for the second one\n",
      f"{kd.get('again_to_target')} {kd.get('again_to_debugger')}")
check("the relay closes the connection when the run is over",
      "left_open" in kd and debugger_reads(kd["left_open"], 1, 1) == b"<closed>")
try:
    with socket.create_server(("127.0.0.1", rec.kd_port)):
        port_free = True
except OSError:
    port_free = False
check("and no longer listens", port_free)
check("who connected, who left and how much went through is reported",
      out.count("KD> debugger connected") == 2 and out.count("KD> debugger gone") == 1 and
      out.count("KD> a second connection") == 1 and
      "KD> 331 bytes to the target, 326 bytes to the debugger" in out,
      "\n".join(line for line in out.splitlines() if line.startswith("KD>")))
check("what the target sends is shown without control characters: they are escaped",
      all(ch.isprintable() or ch in "\n\t" for ch in out) and
      "FW> \\xff\\xfe" in out and "\\x1b" in out and "\\x00" in out,
      repr([line for line in out.splitlines() if line.startswith("FW>")])[:1500])
check("text is shown as before",
      "FW> Apple Silicon Project Mu Firmware (arm64/arm64e)\n" in out and
      "FW> If you can see this\n" in out and "FW> for the second one\n" in out)
check("the serial log has the raw bytes the target sent, all of them",
      log_path.read_bytes() == rec.printed and len(rec.printed) > 400)

# 27. a debugger that connects before the target has been touched; and data without lines
def early(rec):
    rec.kd["nothing_written"] = rec.loaded is None and rec.writes == []
    try:
        rec.kd["early"] = debugger(rec)
        rec.kd["early"].sendall(b"\x69\x69\x69\x69early")
    except OSError as e:
        rec.kd["early_error"] = e

FOR_EARLY = b"\nfor the early one\n" + b"\x01" * 2000 + b"\n"
def early_guest(rec):
    if "early" in rec.kd:
        rec.kd["early_to_target"] = guest_reads(rec, 9)
        guest_prints(rec, FOR_EARLY)
        # Whether it also gets what the guest printed in the first moment is not decided
        # here: the relay may or may not have taken the connection by then
        got = debugger_reads(rec.kd["early"], len(rec.printed), 1)
        rec.kd["early_to_debugger"] = got.removesuffix(b"<nothing>")

rec, out, code = run({"at_init": early, "guest": early_guest}, "--disk", disk_path, *image_args)
kd = rec.kd
check("the relay listens before anything is written to the target",
      code == 0 and kd.get("nothing_written") and "early" in kd, f"{kd.get('early_error')}")
check("what a debugger sent before the guest ran reaches the guest",
      kd.get("early_to_target") == b"\x69\x69\x69\x69early", f"{kd.get('early_to_target')}")
check("and it gets what the guest prints",
      kd.get("early_to_debugger", b"").endswith(FOR_EARLY) and
      rec.printed.endswith(kd["early_to_debugger"]), f"{kd.get('early_to_debugger')}"[:300])
check("data without line ends is shown in pieces, all of it",
      out.count("\\x01") == 2000 and max(len(line) for line in out.splitlines()) <= 4 + 4 * 512,
      f"{out.count(chr(92) + 'x01')} {max(len(line) for line in out.splitlines())}")

# 28. a debugger that stops reading does not hold up the firmware's output
def stuck(rec):
    kd = rec.kd
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", rec.kd_port))
    time.sleep(0.3)
    writer = threading.Thread(target=guest_prints,
                              args=(rec, b"\n" + (b"x" * 99 + b"\n") * 80000), daemon=True)
    writer.start()
    writer.join(30)
    kd["all_printed"] = not writer.is_alive()
    if not kd["all_printed"]:
        # Held up after all. Closing the connection lets the output go on, so that this
        # test comes to its end.
        s.close()
        writer.join(30)
        return
    time.sleep(0.5)
    c = debugger(rec)
    time.sleep(0.3)
    guest_prints(rec, b"after the stuck one\n")
    kd["after_stuck"] = debugger_reads(c, 20)
    c.close()
    s.close()

rec, out, code = run({"guest": stuck}, *image_args)
kd = rec.kd
check("8 MB of output get through while a debugger does not read",
      code == 0 and kd.get("all_printed") and out.count("FW> " + "x" * 99 + "\n") == 80000,
      f"{kd.get('all_printed')} {out.count('FW> ' + 'x' * 99)}")
check("the debugger that does not read is dropped, and the next one is served",
      "KD> debugger gone: it does not read" in out and
      kd.get("after_stuck") == b"after the stuck one\n", f"{kd.get('after_stuck')}")

# 28b. a debugger that falls behind for a moment is kept, and misses nothing. The debugger
# does not connect again once its connection is closed, so a burst of the firmware's output
# must not cost it the connection.
BURST = b"\n" + b"".join(b"%07d " % i + b"y" * 91 + b"\n" for i in range(10000))
def behind(rec):
    kd = rec.kd
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", rec.kd_port))
    time.sleep(0.3)
    writer = threading.Thread(target=guest_prints, args=(rec, BURST), daemon=True)
    writer.start()
    time.sleep(1.0)
    s.settimeout(5)
    got = b""
    try:
        while len(got) < len(BURST):
            piece = s.recv(65536)
            if not piece:
                break
            got += piece
    except OSError:
        pass
    writer.join(30)
    kd["behind_got"] = got
    s.close()

rec, out, code = run({"guest": behind}, *image_args)
kd = rec.kd
check("a debugger that falls behind by 1 MB for a second is not dropped and gets every byte",
      code == 0 and kd.get("behind_got") == BURST and "it does not read" not in out,
      f"{len(kd.get('behind_got', b''))} of {len(BURST)}; "
      f"{[l for l in out.splitlines() if l.startswith('KD>')]}")

# 29. the port is taken
with socket.create_server(("127.0.0.1", 0)) as taken:
    for mode in ((), ("--check-only",)):
        rec, out, code = run({}, *mode, *image_args, kd_port=taken.getsockname()[1])
        check(f"a port that is taken stops the run {' '.join(mode)}".rstrip(),
              isinstance(code, str) and f"127.0.0.1:{rec.kd_port}" in code and untouched(rec),
              f"{code!r}")

# 30. no relay
rec, out, code = run({}, "--kd-port", "0", *image_args, kd_port="default")
check("--kd-port 0: no relay, the firmware's output as before",
      code == 0 and "KD>" not in out and "Kernel debugger" not in out and
      "FW> If you can see this\n" in out, out[-600:])

rec, out, code = run({}, "--kd-port", "70000", *image_args, kd_port="default")
check("a port number that is none is refused", code != 0 and untouched(rec))

# 31. the port when none is given. It may be taken on this machine; the refusal names it too.
rec, out, code = run({}, "--check-only", *image_args, kd_port="default")
check("the default port is 51000",
      (code == 0 and "Kernel debugger: 127.0.0.1:51000" in out) or
      (isinstance(code, str) and "127.0.0.1:51000" in code), f"{code!r}")

# 32. execution tracking
rec, out, code = run({}, "--seconds", "1", *image_args)
check("execution tracking is on by default",
      code == 0 and rec.xtrack is True and "Execution tracking: on" in out, f"{rec.xtrack} {out[-300:]}")
check("it is turned on after the image is loaded and before the guest is entered",
      rec.xtrack_state == (True, 0, True) and rec.hv_start is not None, f"{rec.xtrack_state}")
check("the memory is mapped once, after the image is loaded and before tracking is turned on",
      rec.pt_updates == [(True, None)], f"{rec.pt_updates}")

rec, out, code = run({}, "--seconds", "1", "--xtrack", "el1", *image_args)
check("there is no tracking of EL1 alone to ask for", code != 0 and rec.hv_start is None,
      f"{code!r}")

rec, out, code = run({}, "--seconds", "1", "--xtrack", "off", *image_args)
check("--xtrack off leaves it off and says what that means, and the mapping to HV.start()",
      code == 0 and rec.xtrack is None and rec.hv_start is not None and
      "Execution tracking: off" in out and rec.pt_updates == [], f"{rec.xtrack} {rec.pt_updates}")

rec, out, code = run({"xtrack_ret": 0xffffffffffffffff}, "--seconds", "1", *image_args)
check("a hypervisor that refuses stops the run before the guest is entered",
      code != 0 and rec.xtrack is True and rec.hv_start is None, f"{code!r}")

rec, out, code = run({}, "--check-only", *image_args)
check("check-only does not turn it on", code == 0 and rec.xtrack is None)

rec, out, code = run({}, "--seconds", "1", "--disk", str(disk_path), *image_args)
check("with a disk, it is turned on after the whole disk is in guest memory",
      code == 0 and rec.xtrack_state is not None and rec.xtrack_state[1] == rec.writes_at_entry
      and rec.xtrack_state[1] > 0 and rec.xtrack_state[2], f"{rec.xtrack_state} {rec.writes_at_entry}")

# 33. --wait-debugger: the guest is not entered before a debugger has connected
def connect_now(rec):
    rec.kd["early"] = debugger(rec)

def connect_later(rec):
    def late():
        rec.kd["late"] = debugger(rec)
    timer = threading.Timer(0.7, late)
    timer.daemon = True
    timer.start()

def order(out):
    """Where the three lines are in the output; -1 for one that is not there."""
    return [out.find(text) for text in ("Waiting for the kernel debugger", "KD> debugger connected",
                                        "Entering the firmware")]

rec, out, code = run({"at_init": connect_now}, "--seconds", "1", "--wait-debugger", "5", *image_args)
check("--wait-debugger: with a debugger that is there already, the guest is entered",
      code == 0 and rec.hv_start is not None and -1 < order(out)[1] < order(out)[2],
      f"{code!r} {order(out)}")

rec, out, code = run({"at_init": connect_later}, "--seconds", "1", "--wait-debugger", "5", *image_args)
check("--wait-debugger: the guest is entered only after a debugger that comes later has connected",
      code == 0 and rec.hv_start is not None and -1 < order(out)[0] < order(out)[1] < order(out)[2],
      f"{code!r} {order(out)}")

started = time.monotonic()
rec, out, code = run({}, "--seconds", "1", "--wait-debugger", "1", *image_args)
check("--wait-debugger: with no debugger the run stops and the guest is not entered",
      isinstance(code, str) and "debugger" in code and rec.hv_start is None and
      "Entering the firmware" not in out and time.monotonic() - started < 10, f"{code!r}")
try:
    with socket.create_server(("127.0.0.1", rec.kd_port)):
        port_free = True
except OSError:
    port_free = False
check("and the relay is closed", port_free)

rec, out, code = run({}, "--wait-debugger", "5", "--kd-port", "0", *image_args, kd_port="default")
check("--wait-debugger without a relay is refused", code != 0 and untouched(rec), f"{code!r}")

rec, out, code = run({}, "--seconds", "1", *image_args)
check("without --wait-debugger the guest is entered with no debugger, as before",
      code == 0 and rec.hv_start is not None and "Waiting for the kernel debugger" not in out)

# ---- --loader m1n1: m1n1 lays out guest memory and enters the firmware itself ----------

image_data = (pathlib.Path(image_args[0]) if image_args else DEFAULT_IMAGE).read_bytes()
M1N1 = ("--loader", "m1n1", "--disk", disk_path)
# Nothing of the host's own loader: no HV object, no boot arguments or device tree written,
# no command to turn execution tracking on, no table of AIC lines asked for (m1n1 writes it
# itself, selfboot_enter() of src/selfboot_run.c)
no_hv = lambda rec: (not rec.hv_inited and rec.loaded is None and rec.hv_start is None and
                     rec.xtrack is None and rec.memory_map is None and rec.bootargs_top is None
                     and rec.aicmap_calls == [])
tty = lambda out: "\n".join(line for line in out.splitlines() if not line.startswith("FW>"))

# 34. the whole run, ended by the timer
rec, out, code = run({"guest": wait_for_sigint}, *M1N1, "--seconds", "1", *image_args)
check("m1n1 loader: a full run exits 0", code == 0, f"{code!r} {out[-900:]}")
check("m1n1 loader: the image goes up whole, to where the layout has it",
      rec.writes[:1] == [(IMAGE_BASE, image_data)], f"{[(hex(b), len(d)) for b, d in rec.writes[:1]]}")
check("m1n1 loader: both ends of the image are read back, one 16 KB page each",
      rec.readbacks[:2] == [(IMAGE_BASE, 0x4000), (IMAGE_BASE + image_len - 0x4000, 0x4000)],
      f"{rec.readbacks}")
check("m1n1 loader: the disk goes up whole after it, and both of its ends are read back",
      len(rec.writes) > 2 and rec.writes[1][0] == DISK_BASE and
      b"".join(data for base, data in rec.writes[1:]) == disk_data and
      rec.readbacks[2:] == [(DISK_BASE, 0x1000), (DISK_BASE + DISK_LEN - 0x1000, 0x1000)],
      f"{rec.readbacks}")
check("m1n1 loader: m1n1 is told where guest RAM starts and how long the two files are",
      rec.selfboot == (M4["heap_top"], image_len, DISK_LEN), f"{rec.selfboot}")
check("m1n1 loader: both files are in place before the command",
      rec.writes_at_entry == len(rec.writes) and rec.writes_at_entry > 2)
check("m1n1 loader: nothing of the host's own loader is used", no_hv(rec))
check("m1n1 loader: the reply is waited for without a timeout", rec.timeout_at_entry is None,
      f"{rec.timeout_at_entry}")
check("m1n1 loader: the timer sends the one byte that brings the hypervisor back, about a "
      "second after the command",
      len(rec.interrupts) == 1 and rec.interrupts[0][1] == b"!" and
      0.9 < rec.interrupts[0][0] - rec.selfboot_sent < 2.5, f"{rec.interrupts} {rec.selfboot_sent}")
check("m1n1 loader: the event is answered with 'leave the guest'", rec.stopped == "EXIT_GUEST")
check("m1n1 loader: a stop by the timer says so, says where the firmware was and prints the "
      "context, not as a fault",
      "Run ended: --seconds 1 ran out (HV/USER_INTERRUPT)" in out and
      f"Firmware stopped at PC 0x{IMAGE_BASE + 0x8600:x} (firmware image + 0x8600)" in out and
      rec.context_printed and rec.context_fault is False, out[-1500:])
check("m1n1 loader: the final dump has the firmware's own exception registers",
      all(f"{name:18s} = 0x" in out.split("==== firmware stopped ====")[-1]
          for name in ("ELR_EL12", "ESR_EL12", "FAR_EL12", "SPSR_EL12", "SP_EL1")), out[-1500:])
check("m1n1 loader: what m1n1 prints is shown, before and after the guest",
      "TTY> selfboot: entering the firmware at" in out and
      "TTY> HV: debug registers: 3650 reads and 3650 writes of the guest answered" in out,
      tty(out)[-1500:])
check("m1n1 loader: the firmware's output is shown",
      "FW> Apple Silicon Project Mu Firmware (arm64/arm64e)\n" in out and
      "FW> If you can see this\n" in out)
check("m1n1 loader: the ranges of /arm-io are listed before anything is written",
      "/arm-io: 3 ranges" in out and "  0x1cb0000000 .. 0x1cc0000000" in out and
      out.find("/arm-io: 3 ranges") < out.find("Loading the firmware image"), tty(out)[:3000])
check("m1n1 loader: how long m1n1 took to get to the guest is reported",
      re.search(r"m1n1 said it was entering the firmware \d+\.\d s after the command", out) and
      re.search(r"the firmware's first output came \d+\.\d s after the command", out),
      tty(out)[-1500:])
check("m1n1 loader: m1n1's heap is read right before the command and after the guest was left",
      rec.heap_reads == [False, True] and
      f"m1n1's heap: its top went from 0x{M1N1_HEAP_TOP:x} to 0x{M1N1_HEAP_TOP + (31 << 20):x}, "
      "31.0 MiB; 49.0 MiB are left below its limit" in out, f"{rec.heap_reads} {tty(out)[-1500:]}")
looked_up = {addr for addr, after in rec.pt_walks}
check("m1n1 loader: stage 2 is looked up after the guest has been left: both ends of every "
      "range of /arm-io, the UART, m1n1, the host's heap and both ends of guest RAM",
      rec.pt_walks and all(after for addr, after in rec.pt_walks) and
      {a for base, size in ARM_IO for a in (base, base + size - 0x1000)} <= looked_up and
      {M4["uart"], M4["m1n1_base"], M4["heap_top"] - 0x1000, M4["heap_top"], MEM_TOP - 0x1000,
       RAM_BASE} <= looked_up and "Stage 2: as expected" in out and "PROBLEM" not in out,
      f"{sorted(hex(a) for a in looked_up)} {tty(out)[-1500:]}")
check("m1n1 loader: no address beyond 42 bits is looked up (hv_pt_walk() asserts)",
      all(addr < 1 << 42 for addr in looked_up))
check("m1n1 loader: the run ends like one of the host's loader",
      out.endswith("Hypervisor exited.\n"), out[-300:])

# 35. m1n1 refuses: the guest is not entered
rec, out, code = run({"selfboot_ret": -1}, *M1N1, "--seconds", "1", *image_args)
check("m1n1 loader: a refusal by m1n1 is reported and the run fails",
      code == 1 and "m1n1 did not enter the guest (P_SELFBOOT_RUN returned -1)" in out and
      "TTY> selfboot: the firmware image has no arm64 magic" in out and
      "Hypervisor exited." not in out, f"{code!r} {tty(out)[-900:]}")
check("m1n1 loader: after a refusal nothing is sent, looked up or reported as a run",
      rec.interrupts == [] and rec.pt_walks == [] and rec.stopped is None and
      "Run ended" not in out and "Stage 2" not in out)
try:
    time.sleep(1.5)
    stray = False
except KeyboardInterrupt:
    stray = True
check("m1n1 loader: the timer does not go off after a refusal", not stray and rec.interrupts == [])

# 36. something the hypervisor could not deal with by itself: no host would be there
rec, out, code = run({"stop_reason": "EXCEPTION_LOWER"}, *M1N1, "--seconds", "30", *image_args)
check("m1n1 loader: a guest exception that reaches the host ends the run and is named for "
      "what it means without a host",
      code == 0 and rec.stopped == "EXIT_GUEST" and rec.interrupts == [] and
      "Run ended: the hypervisor could not deal with this by itself; without a host the "
      "machine would stop here and show why on its screen (EXCEPTION_LOWER/SYNC)" in out,
      f"{code!r} {tty(out)[-900:]}")
check("m1n1 loader: its context is printed as a fault",
      rec.context_printed and rec.context_fault is True)

rec, out, code = run({"stop_code": "HOOK_VM"}, *M1N1, "--seconds", "30", *image_args)
check("m1n1 loader: so does any other event of the hypervisor that nobody asked for",
      code == 0 and rec.stopped == "EXIT_GUEST" and
      "without a host the machine would stop here and show why on its screen (HV/HOOK_VM)"
      in out, tty(out)[-900:])

# 37. the guest stopped for good
rec, out, code = run({"stop_code": "GUEST_RESET"}, *M1N1, "--seconds", "30", *image_args)
check("m1n1 loader: a guest that resets itself ends the run, with the reason",
      code == 0 and rec.stopped == "EXIT_GUEST" and rec.interrupts == [] and
      "Run ended: the guest called PSCI SYSTEM_RESET (HV/GUEST_RESET)" in out, tty(out)[-900:])

def ctrl_c_late(rec):
    os.kill(os.getpid(), signal.SIGINT)
    time.sleep(0.2)
rec, out, code = run({"stop_code": "GUEST_RESET", "at_context": ctrl_c_late}, *M1N1,
                     "--seconds", "30", *image_args)
check("m1n1 loader: Ctrl-C while an event is being dealt with sends nothing: m1n1 is in the "
      "middle of talking to the script",
      code == 0 and rec.stopped == "EXIT_GUEST" and rec.interrupts == [] and
      "(HV/GUEST_RESET)" in out, f"{code!r} {rec.interrupts} {tty(out)[-600:]}")

# 38. m1n1 itself in trouble: nothing more is sent to it
for reason, stop_code in (("EXCEPTION", "SYNC"), ("HV", "PANIC"), ("HV", "WDT_BARK")):
    rec, out, code = run({"stop_reason": reason, "stop_code": stop_code}, *M1N1,
                         "--seconds", "30", *image_args)
    check(f"m1n1 loader: {reason}/{stop_code} is m1n1's own trouble: reported, not answered, "
          "the run fails",
          isinstance(code, str) and "Power the M4 off" in code and rec.stopped is None and
          rec.pt_walks == [] and rec.interrupts == [] and
          f"m1n1 itself is in trouble ({reason}/{stop_code})" in out,
          f"{code!r} {rec.stopped} {rec.pt_walks} {rec.interrupts} {tty(out)[-600:]}")

# 39. Ctrl-C, without a limit
rec, out, code = run({"guest": ctrl_c}, *M1N1, "--seconds", "0", *image_args)
check("m1n1 loader: Ctrl-C ends a run without a limit, and is not taken for the timer",
      code == 0 and rec.stopped == "EXIT_GUEST" and len(rec.interrupts) == 1 and
      "Run ended: interrupted (Ctrl-C) (HV/USER_INTERRUPT)" in out and
      "No time limit: Ctrl-C ends the run" in out, tty(out)[-900:])

# 40. what m1n1 cannot be asked for
rec, out, code = run({}, "--loader", "m1n1", *image_args)
check("m1n1 loader: without a boot disk it is refused, nothing touched",
      code != 0 and untouched(rec) and rec.selfboot is None, f"{code!r}")
rec, out, code = run({}, *M1N1, "--xtrack", "off", *image_args)
check("m1n1 loader: without execution tracking it is refused, nothing touched",
      code != 0 and untouched(rec) and rec.selfboot is None, f"{code!r}")

# 40a. --no-host: the two files go up, the command says there is no host, and the script leaves
rec, out, code = run({}, *M1N1, "--no-host", *image_args)
check("no host: the run exits 0 and says that what follows is on the machine",
      code == 0 and "Sent. This script does not stay" in out and
      "Hypervisor exited." not in out and "Run ended" not in out, f"{code!r} {tty(out)[-900:]}")
check("no host: both files go up as without it, before the command",
      rec.writes[:1] == [(IMAGE_BASE, image_data)] and
      b"".join(data for base, data in rec.writes[1:]) == disk_data and
      rec.writes_at_entry == len(rec.writes), f"{[(hex(b), len(d)) for b, d in rec.writes[:2]]}")
check("no host: m1n1 is told where guest RAM starts, the two sizes, and that there is no host",
      rec.selfboot == (M4["heap_top"], image_len, DISK_LEN) and rec.no_host is True,
      f"{rec.selfboot} {rec.no_host}")
check("no host: the port is flushed after the command, and nothing else is sent or looked up",
      rec.flushes == [True] and rec.interrupts == [] and rec.stopped is None and
      rec.pt_walks == [] and rec.heap_reads == [] and no_hv(rec),
      f"{rec.flushes} {rec.interrupts} {rec.pt_walks} {rec.heap_reads}")
for extra in (("--loader", "host", "--disk", disk_path), ("--wait-debugger", "5"),
              ("--bugcheck", "/nonexistent")):
    rec, out, code = run({}, *M1N1, "--no-host", *extra, *image_args)
    check(f"no host: refused with {extra[0]}, nothing touched",
          code != 0 and untouched(rec) and rec.selfboot is None, f"{code!r}")
rec, out, code = run({}, "--no-host", *image_args)
check("no host: refused with the host's loader, the default, nothing touched",
      code != 0 and untouched(rec) and rec.selfboot is None, f"{code!r}")
rec, out, code = run({"guest": wait_for_sigint}, *M1N1, "--seconds", "1", *image_args)
check("no host: without the option the command says there is a host, as before",
      code == 0 and rec.no_host is False and rec.flushes == [], f"{rec.no_host} {rec.flushes}")

# 41. --check-only writes nothing
rec, out, code = run({}, *M1N1, "--check-only", *image_args)
check("m1n1 loader: check-only decides and touches nothing",
      code == 0 and untouched(rec) and rec.selfboot is None and
      f"boot disk            0x{DISK_BASE:x}" in out, f"{code!r}")

# 42. an upload that did not arrive
rec, out, code = run({"bad_write": 1}, *M1N1, *image_args)
check("m1n1 loader: an image that arrives damaged stops the run before the command",
      isinstance(code, str) and "did not arrive intact" in code and rec.selfboot is None,
      f"{code!r}")
rec, out, code = run({"bad_readback": 2}, *M1N1, *image_args)
check("m1n1 loader: an image that reads back differently stops the run before the command",
      isinstance(code, str) and "reads back" in code and rec.selfboot is None and
      len(rec.writes) == 1, f"{code!r}")
rec, out, code = run({"bad_write": 3}, *M1N1, *image_args)
check("m1n1 loader: a piece of the disk that arrives damaged stops the run before the command",
      isinstance(code, str) and "did not arrive intact" in code and rec.selfboot is None,
      f"{code!r}")

# 43. stage 2 is not what it should be
last_io = ARM_IO[1][0] + ARM_IO[1][1] - 0x1000
rec, out, code = run({"guest": wait_for_sigint, "bad_pt": last_io}, *M1N1, "--seconds", "1",
                     *image_args)
check("m1n1 loader: a range of /arm-io that is not mapped to the end is a problem",
      code == 2 and f"PROBLEM: 0x{last_io:x}" in out and "Stage 2: as expected" not in out,
      f"{code!r} {tty(out)[-900:]}")
rec, out, code = run({"guest": wait_for_sigint, "extra_pt": M4["m1n1_base"]}, *M1N1,
                     "--seconds", "1", *image_args)
check("m1n1 loader: m1n1 mapped for the guest is a problem",
      code == 2 and f"PROBLEM: 0x{M4['m1n1_base']:x}" in out, f"{code!r} {tty(out)[-900:]}")
rec, out, code = run({"guest": wait_for_sigint, "wrong_pt": ARM_IO[2][0]}, *M1N1,
                     "--seconds", "1", *image_args)
check("m1n1 loader: a page of /arm-io mapped to another page is a problem",
      code == 2 and f"PROBLEM: 0x{ARM_IO[2][0]:x} should be mapped and is mapped to another page"
      in out, f"{code!r} {tty(out)[-900:]}")
check("m1n1 loader: a run with a stage 2 problem still says that the hypervisor was left",
      out.endswith("Hypervisor exited.\n"), out[-300:])

# 43a. pages of memory that the guest ran code from, with instructions replaced: execute-only,
# mapped to a shadow page in m1n1's heap. That is right for memory, and for nothing else.
SHADOW = M1N1_HEAP_TOP + 0x10000
memory_ends = (M4["heap_top"], MEM_TOP - 0x1000, RAM_BASE, M4["ba_phys_base"] - 0x1000)
rec, out, code = run({"guest": wait_for_sigint,
                      "xonly_pt": {a: SHADOW + i * 0x1000 for i, a in enumerate(memory_ends)}},
                     *M1N1, "--seconds", "1", *image_args)
check("m1n1 loader: both ends of guest RAM and of the memory below iBoot's may be execute-only, "
      "mapped to a shadow page",
      code == 0 and "PROBLEM" not in out and "Stage 2: as expected" in out and
      "(4 of the pages of memory execute-only, mapped to a shadow page)" in out and
      set(memory_ends) <= {addr for addr, after in rec.pt_walks}, f"{code!r} {tty(out)[-900:]}")
for name, addr in (("a page of /arm-io", ARM_IO[2][0]), ("m1n1", M4["m1n1_base"]),
                   ("the host's heap", M4["heap_top"] - 0x1000)):
    rec, out, code = run({"guest": wait_for_sigint, "xonly_pt": {addr: SHADOW}}, *M1N1,
                         "--seconds", "1", *image_args)
    check(f"m1n1 loader: {name} execute-only, mapped to a shadow page, is a problem",
          code == 2 and f"PROBLEM: 0x{addr:x} should be" in out and
          "is execute-only, mapped to a shadow page" in out, f"{code!r} {tty(out)[-900:]}")
rec, out, code = run({"guest": wait_for_sigint, "xonly_pt": {MEM_TOP - 0x1000: MEM_TOP - 0x1000}},
                     *M1N1, "--seconds", "1", *image_args)
check("m1n1 loader: a page of memory execute-only and mapped to itself is a problem: no shadow page",
      code == 2 and f"PROBLEM: 0x{MEM_TOP - 0x1000:x} should be mapped and is execute-only, "
      "mapped to itself" in out, f"{code!r} {tty(out)[-900:]}")
in_ram, below = M4["heap_top"] + 0x10000, RAM_BASE + 0x10000
rec, out, code = run({"guest": wait_for_sigint,
                      "xonly_pt": {MEM_TOP - 0x1000: in_ram, M4["ba_phys_base"] - 0x1000: below}},
                     *M1N1, "--seconds", "1", *image_args)
check("m1n1 loader: a page of memory execute-only and mapped into the guest's memory, of RAM or "
      "below iBoot's, is a problem: that is no shadow page",
      code == 2 and "Stage 2: 2 of" in out and
      f"PROBLEM: 0x{MEM_TOP - 0x1000:x} should be mapped and is execute-only, mapped to "
      f"0x{in_ram:x} in the guest's memory" in out and
      f"PROBLEM: 0x{M4['ba_phys_base'] - 0x1000:x} should be mapped and is execute-only, mapped "
      f"to 0x{below:x} in the guest's memory" in out, f"{code!r} {tty(out)[-900:]}")
rec, out, code = run({"guest": wait_for_sigint, "wrong_pt": RAM_BASE}, *M1N1, "--seconds", "1",
                     *image_args)
check("m1n1 loader: a page of memory mapped to another page, and not execute-only, is a problem",
      code == 2 and f"PROBLEM: 0x{RAM_BASE:x} should be mapped and is mapped to another page" in out,
      f"{code!r} {tty(out)[-900:]}")

# 44. --wait-debugger: the command is not sent before a debugger has connected
rec, out, code = run({"at_upload": connect_later, "guest": wait_for_sigint}, *M1N1,
                     "--seconds", "1", "--wait-debugger", "5", *image_args)
handed = out.find("Handing over to m1n1")
check("m1n1 loader: --wait-debugger: the command goes out only after a debugger has connected",
      code == 0 and rec.debugger_at_entry and
      -1 < out.find("Waiting for the kernel debugger") < out.find("KD> debugger connected") < handed,
      f"{code!r} {rec.debugger_at_entry} {tty(out)[-900:]}")
rec, out, code = run({}, *M1N1, "--seconds", "1", "--wait-debugger", "1", *image_args)
check("m1n1 loader: --wait-debugger: with no debugger the command is not sent",
      isinstance(code, str) and "debugger" in code and rec.selfboot is None and
      "Handing over to m1n1" not in out, f"{code!r}")

# 45. the default is the host's loader, as before
rec, out, code = run({}, "--seconds", "1", "--disk", disk_path, *image_args)
check("without --loader the host loads, and m1n1 is not asked to",
      code == 0 and rec.hv_inited and rec.hv_start == (IMAGE_BASE, BOOTARGS) and
      rec.selfboot is None and rec.pt_walks == [] and rec.heap_reads == [])

# ---- --bugcheck: what Windows recorded of a bug check, read before the guest is left ----

from test_bugcheck import windows_model

def windows(model, **case):
    """The fake target with the model's memory as part of guest RAM, and the guest's three
    registers that say where the kernel and its tables are."""
    assert M4["heap_top"] <= model.ram[0] and model.ram[1] <= MEM_TOP
    return {"guest_mem": (*model.ram, model.read_phys),
            "regs": {sysreg.VBAR_EL12: model.regs["VBAR_EL1"],
                     sysreg.TTBR1_EL12: model.regs["TTBR1_EL1"],
                     sysreg.TCR_EL12: model.regs["TCR_EL1"]}, **case}

RECORDED = "==== what Windows recorded of a bug check ===="
with tempfile.TemporaryDirectory() as bc_tmp:
    # 46. both loaders: the guest has reset itself after a bug check
    for loader, extra in (("host", ()), ("m1n1", M1N1)):
        model = windows_model()
        out_dir = pathlib.Path(bc_tmp) / loader
        rec, out, code = run(windows(model, stop_code="GUEST_RESET"), *extra, "--seconds", "30",
                             "--bugcheck", str(out_dir), *image_args)
        check(f"--bugcheck, {loader} loader: the code and the parameters are printed",
              code == 0 and "Bug check 0x109" in out and
              f"parameter 3 = 0x{model.parameters[2]:x}  (stor port.sys + 0x1234)" in out and
              "parameter 4 = 0x1" in out, f"{code!r} {tty(out)[-1500:]}")
        check(f"--bugcheck, {loader} loader: after the reason of the stop, before the guest is left",
              -1 < out.find("Run ended: the guest called PSCI SYSTEM_RESET") < out.find(RECORDED)
              < out.find("Hypervisor exited.") and rec.guest_reads and
              set(rec.guest_reads) == {None} and rec.stopped == "EXIT_GUEST",
              f"{set(rec.guest_reads)} {rec.stopped}")
        names = sorted(p.name for p in out_dir.iterdir()) if out_dir.exists() else None
        check(f"--bugcheck, {loader} loader: the files are in the directory",
              names == ["bugcheck.json", "ntoskrnl.exe.mem", "parameter_1.mem", "stor_port.sys.mem"],
              f"{names}")
        check(f"--bugcheck, {loader} loader: how long the guest ran is said",
              re.search(r"^The guest ran for \d+\.\d s$", out, re.M) is not None, tty(out)[-1500:])

        # 47. stopped by hand, no bug check
        model = windows_model(code=None)
        out_dir = pathlib.Path(bc_tmp) / (loader + "-none")
        rec, out, code = run(windows(model, guest=ctrl_c), *extra, "--seconds", "0",
                             "--bugcheck", str(out_dir), *image_args)
        check(f"--bugcheck, {loader} loader: stopped by Ctrl-C with no bug check recorded: said",
              code == 0 and rec.stopped == "EXIT_GUEST" and
              "No bug check recorded" in out and "Hypervisor exited." in out, tty(out)[-1500:])

        # 48. the guest is still in the firmware: its registers are not the kernel's
        out_dir = pathlib.Path(bc_tmp) / (loader + "-fw")
        rec, out, code = run({"stop_code": "GUEST_RESET"}, *extra, "--seconds", "30",
                             "--bugcheck", str(out_dir), *image_args)
        check(f"--bugcheck, {loader} loader: a guest that is not in the Windows kernel: said, "
              "nothing read, the guest left",
              code == 0 and rec.stopped == "EXIT_GUEST" and rec.guest_reads == [] and
              "the guest is not in the Windows kernel" in out, tty(out)[-1500:])

        # 49. reading fails in a way the reader does not expect: the event is answered all the same
        model = windows_model()
        out_dir = pathlib.Path(bc_tmp) / (loader + "-broken")
        rec, out, code = run(windows(model, stop_code="GUEST_RESET",
                                     mrs_fails=(sysreg.TTBR1_EL12,)), *extra, "--seconds", "30",
                             "--bugcheck", str(out_dir), *image_args)
        check(f"--bugcheck, {loader} loader: a failure while reading does not keep the script "
              "from leaving the guest",
              code == 0 and rec.stopped == "EXIT_GUEST" and "Hypervisor exited." in out and
              "PROBLEM: what Windows recorded of a bug check was not read (RuntimeError: the "
              "proxy did not answer)" in out, f"{code!r} {tty(out)[-1500:]}")

        # 50. without the option nothing of this happens
        model = windows_model()
        rec, out, code = run(windows(model, stop_code="GUEST_RESET"), *extra, "--seconds", "30",
                             *image_args)
        check(f"without --bugcheck, {loader} loader: guest memory is not read",
              code == 0 and rec.guest_reads == [] and RECORDED not in out)

    # 51. m1n1 itself in trouble: nothing is read either
    model = windows_model()
    rec, out, code = run(windows(model, stop_reason="HV", stop_code="PANIC"), *M1N1,
                         "--seconds", "30", "--bugcheck", str(pathlib.Path(bc_tmp) / "panic"),
                         *image_args)
    check("--bugcheck, m1n1 loader: when m1n1 itself is in trouble, nothing more is asked of it",
          isinstance(code, str) and rec.guest_reads == [] and RECORDED not in out, f"{code!r}")

# ---- /cpus of the device tree the host's loader hands the firmware ----

# 52. every core of the machine is there, as when m1n1 loads; it is m1n1 that starts them
ALL_CPUS = [(f"cpu{i}", "running" if i == M4_BOOT_CPU else "waiting") for i in range(M4_CPUS)]
rec, out, code = run({}, "--seconds", "1", *image_args)
check("host loader: the firmware gets every node of /cpus, each as m1n1 has it",
      code == 0 and rec.guest_cpus == ALL_CPUS, f"{code!r} {rec.guest_cpus}")
check("host loader: the secondary CPUs are not started",
      rec.secondaries_started is False and rec.smp_at_start is False,
      f"{rec.secondaries_started} {rec.smp_at_start}")
check("host loader: it says how many nodes the guest gets, and where the cores are",
      f"/cpus of the guest's device tree: all {M4_CPUS} nodes" in out and
      "the guest starts on the boot CPU, the others wait in EL2 for its CPU_ON" in out,
      out[-1500:])

# 53. an HV.init() that leaves fewer nodes than m1n1 has: the guest is not entered
rec, out, code = run({"hv_drops_cpu": "cpu3"}, "--seconds", "1", *image_args)
check("host loader: a device tree with a core missing from /cpus stops the run",
      isinstance(code, str) and f"has {M4_CPUS - 1} nodes after HV.init()" in code and
      "cpu2 cpu4" in code and rec.hv_inited and rec.hv_start is None and rec.loaded is None,
      f"{code!r} {rec.hv_start}")

# 54. the real HV goes by self.smp where the stand-in above does, and nowhere else
import ast

def is_self(node, attr):
    return (isinstance(node, ast.Attribute) and node.attr == attr and
            isinstance(node.value, ast.Name) and node.value.id == "self")

real_hv = next(node for node in ast.parse((PROXYCLIENT / "m1n1/hv/__init__.py").read_text()).body
               if isinstance(node, ast.ClassDef) and node.name == "HV")
methods = {fn.name: fn for fn in real_hv.body if isinstance(fn, ast.FunctionDef)}
smp_reads = {name: [node for node in ast.walk(fn)
                    if is_self(node, "smp") and isinstance(node.ctx, ast.Load)]
             for name, fn in methods.items()}
check("the real HV reads self.smp once in init(), once in setup_adt() and nowhere else",
      {name: len(reads) for name, reads in smp_reads.items() if reads} ==
      {"init": 1, "setup_adt": 1}, f"{ {n: len(r) for n, r in smp_reads.items() if r} }")

starts = [node for node in ast.walk(methods["init"])
          if isinstance(node, ast.If) and is_self(node.test, "smp")]
calls = [node for node in ast.walk(methods["init"])
         if isinstance(node, ast.Call) and is_self(node.func, "setup_adt")]
check("in init() it is whether the secondary CPUs are started, before self.setup_adt() is called",
      len(starts) == 1 and len(calls) == 1 and not starts[0].orelse and
      [ast.unparse(stmt) for stmt in starts[0].body] == ["self.p.smp_start_secondaries()"] and
      starts[0].end_lineno < calls[0].lineno,
      f"{[ast.unparse(node) for node in starts]} {[node.lineno for node in calls]}")

drops = [node for node in ast.walk(methods["setup_adt"])
         if isinstance(node, ast.If) and isinstance(node.test, ast.UnaryOp) and
         isinstance(node.test.op, ast.Not) and is_self(node.test.operand, "smp")]
dropped = ast.unparse(drops[0]) if len(drops) == 1 else ""
check("in setup_adt() it is whether the nodes of /cpus that are not running are deleted",
      "for cpu in list(self.adt['cpus'])" in dropped and "if cpu.state != 'running'" in dropped
      and "del self.adt['cpus'][cpu.name]" in dropped and not (drops and drops[0].orelse),
      dropped)

# 55. given no image, the script loads the one this test takes for it: the name of the
# firmware's platform package is in both, and they are to change together
rec, out, code = run({}, "--check-only")
check("without an image on the command line it is the one of the firmware's build as it is "
      "named now",
      code == 0 and f"Firmware image: {DEFAULT_IMAGE}\n" in out and
      hashlib.sha256(DEFAULT_IMAGE.read_bytes()).hexdigest() in out, out[:600])

print(f"{failures} failure(s)")
sys.exit(1 if failures else 0)
