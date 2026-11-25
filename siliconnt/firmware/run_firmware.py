#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load the SiliconNT UEFI firmware (SILICONNT_EFI.fd of the siliconnt-firmware
repository) and run it as the guest of the hypervisor, on the boot CPU only.

The m1n1 on the target has to be built from this tree: the firmware needs the emulated GIC
(src/hv/hv_vgic.c) and the WFI trap (src/hv/hv_wfi.c), and Windows, which the firmware starts
from the boot disk, the PMUv3 registers (src/hv/hv_pmu.c), which the M4 does not have.

Execution tracking (--xtrack, src/hv/hv_xtrack.c) is how Windows gets them: no memory of the
guest is executable until the hypervisor has scanned it and replaced the accesses to those
registers, whether the code runs at EL1 or at EL0. It is turned on after everything has been
put into guest memory and mapped, and before the guest is entered: memory is taken apart into
4 KB pages then, all of it at once, as m1n1 does it when it loads by itself.

What the firmware is handed, and where things go. Guest RAM starts at the top of the heap
this script keeps for itself, above m1n1 and m1n1's own heap (HV._prepare_load()), so what
m1n1 occupies is not part of the memory the boot arguments describe:

    phys_base             start of guest RAM (boot arguments: phys_base, mem_size)
    phys_base + 32 MB     Apple device tree, as edited by HV.setup_adt()
    next 2 MB boundary    the firmware image, entered at its first byte
    after the image       Apple boot arguments, one 16 KB page
    next page             the table of AIC lines and SPIs, one 16 KB page
    next 2 MB boundary    the boot disk image, with --disk
    ...
    mem_top - 128 MB      the firmware's own work area: stack, hand-off blocks, DXE core
    mem_top               end of guest RAM

Entry: start of the image, x0 = address of the boot arguments, SP_EL1 = 0. The image
header leaves its load address in x1 and jumps to the entry code. The firmware uses the
boot arguments and the device tree in place and reserves both ranges; it copies neither.
SEPFW, the trust cache and the preoslog buffer, which HV.load_raw() copies for macOS, are
not copied: the firmware reads none of them.

The boot disk is where the Windows boot manager and loader read their files: an image of
a disk with a GPT. The firmware has no storage driver; it is to register the memory the
image lies in as a RAM disk, in place. It learns the address and the size in bytes from
an entry RAMDisk in /chosen/memory-map of the device tree, next to DeviceTree and
BootArgs. Without --disk there is no such entry.

The table of AIC lines and SPIs says which GIC interrupt each line of Apple's interrupt
controller is for the guest: the version (1), the count, then the lines in ascending order,
each 32 bits; the k-th is SPI 32 + k. m1n1 writes it to its page (P_HV_AICMAP, hv_aicmap()
of src/hv/hv_layout.c), from its own device tree, with the same code it writes it with when
it loads the firmware itself; the hypervisor forwards the lines by the same table. The
firmware finds the address and the size in bytes in the entry AICToGIC of
/chosen/memory-map.

Before anything is written to the target, the script checks that these ranges fit in guest
RAM, that the framebuffer lies outside it, that nothing iBoot listed in
/chosen/memory-map overlaps them, and that the EL1 MMU is off, which the entry code
expects. --check-only stops there.

The firmware prints on the hypervisor's virtual UART. That goes to the secondary USB
serial port (/dev/m1n1-sec), which this script reads and prints with a "FW> " prefix,
bytes that are not text as escapes.

The same UART is the serial port of the Windows kernel debugger, and the debugger runs on
Windows while the USB device is in WSL2. The script listens on 127.0.0.1, --kd-port, and
passes bytes unchanged between one TCP connection at a time and the secondary serial port:
WinDbgX -k com:ipport=51000,port=127.0.0.1. Start this script first: the debugger exits if
nobody listens, and it does not connect again when its connection is closed. Lines that
start with "KD> " say who connected and who left.

The script needs no input: it lets the firmware run for --seconds, then interrupts the
hypervisor, prints where the firmware was and the registers that tell what it was doing,
leaves the guest and exits. With --seconds 0 there is no limit and Ctrl-C does the same.
An unhandled guest exception ends the run the same way, and so does a guest that the
hypervisor has stopped for good: PSCI SYSTEM_OFF, SYSTEM_RESET, or a call it cannot carry
out (HV_GUEST_OFF, HV_GUEST_RESET, HV_GUEST_HVC in src/hv/hv.h). The line that starts with
"Run ended:" says which it was.

Until the firmware's CpuDxe driver installs exception vectors, VBAR_EL1 is 0 (hv_init()).
An exception the firmware takes itself before that shows up as an instruction abort at
0x200 that the hypervisor cannot handle; ELR_EL12, ESR_EL12 and FAR_EL12 in the final
dump then describe the original exception.

--loader says who does all of this. The default, host, is this script with the HV class of
proxyclient, as described above. With --loader m1n1 the script only uploads the firmware
image and the boot disk to where the layout above has them and sends one command,
P_SELFBOOT_RUN. m1n1 then does the rest with the code it runs when it starts without a
host (selfboot_run_loaded(), src/selfboot_run.c): it works out the same layout from the
start of guest RAM and the two sizes, checks both files where they lie, writes the boot
arguments and the device tree, sets up the hypervisor, maps the memory, turns execution
tracking on and enters the firmware. Two things are not as with the host's loader
(SiliconNT docs/findings/0044, 0099): the guest's debug exceptions go to the guest and not to
EL2, and the few registers the host answers for itself are the devices.

Without a host nobody answers what the hypervisor cannot deal with by itself, so with
--loader m1n1 this script does not answer either: the first event that reaches it ends the
run, and the "Run ended:" line says what the machine would have done without a host. The one
event that is expected is the one the script asks for, by the timer or Ctrl-C. After the
guest has been left the script reports how far the top of m1n1's heap has moved, and looks
up in the stage 2 tables whether what m1n1 mapped is what it was to map.

With --no-host as well, the script says in the command that it is not staying, and leaves
once the command is sent. m1n1 then runs the guest as if it had read the two files from its
SSD itself, with no host: no virtual UART, no serial port for the firmware; PSCI SYSTEM_RESET
and SYSTEM_OFF restart and power off the machine, and anything else the hypervisor cannot
deal with is shown on its screen (src/hv/hv_stop.c). What happens is seen on the machine
only. This is how a machine without a host is tested next to one (SiliconNT
docs/findings/0068).

With --bugcheck the script looks, when the guest has stopped and before it leaves it, for
what the Windows kernel has recorded of a bug check: the stop code and its four parameters,
which without a kernel debugger appear nowhere else. It prints them and writes the memory
they may be about to the directory given (bugcheck.py). This is the same with both loaders.
"""
import sys, os, pathlib, argparse, signal, struct, threading, hashlib, time, socket, select

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))
sys.path.append(str(HERE))

import bugcheck

DEFAULT_IMAGE = (pathlib.Path.home() / "src" / "siliconnt-firmware" / "Build" /
                 "SiliconNT-AARCH64" / "DEBUG_CLANGPDB" / "FV" / "SILICONNT_EFI.fd")

parser = argparse.ArgumentParser(description="Run the SiliconNT UEFI firmware under the hypervisor")
parser.add_argument("--seconds", type=int, default=120,
                    help="how long the firmware may run before it is stopped (default 120); "
                         "0: until Ctrl-C or until the guest stops by itself. With --loader "
                         "m1n1 the time counts from the command to m1n1, which includes what "
                         "m1n1 does before it enters the guest")
parser.add_argument("--loader", choices=("host", "m1n1"), default="host",
                    help="who lays out guest memory, sets up the hypervisor and enters the "
                         "firmware: this script (default), or m1n1 with the code it runs when "
                         "it starts without a host; the script then only uploads the two files")
parser.add_argument("--no-host", action="store_true",
                    help="with --loader m1n1: tell m1n1 that this script is not staying, and "
                         "leave once the command is sent; m1n1 runs the guest as it does when "
                         "it starts without a host")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--serial", default=os.environ.get("M1N1SECDEVICE", "/dev/m1n1-sec"),
                    help="the secondary USB serial port, where the firmware prints")
parser.add_argument("--serial-log", type=pathlib.Path,
                    help="also write the raw bytes the firmware prints to this file")
parser.add_argument("--kd-port", type=int, default=51000,
                    help="the TCP port on 127.0.0.1 where the kernel debugger connects to "
                         "the secondary serial port (default 51000); 0: no relay")
parser.add_argument("--wait-debugger", type=int, default=0, metavar="SECONDS",
                    help="do not enter the firmware before the kernel debugger has connected; "
                         "after this many seconds without one, stop, the guest not run "
                         "(default 0: do not wait)")
parser.add_argument("--disk", type=pathlib.Path,
                    help="the image of a disk with a GPT, put into guest memory after the boot "
                         "arguments for the firmware to register as a RAM disk")
parser.add_argument("--xtrack", choices=("on", "off"), default="on",
                    help="execution tracking: scan the memory of the guest before it runs "
                         "(default), or do not")
parser.add_argument("--bugcheck", type=pathlib.Path, metavar="DIR",
                    help="when the guest has stopped, look in its memory for what the Windows "
                         "kernel recorded of a bug check, print it, and write the memory its "
                         "parameters may be about to this directory. Those files are memory of "
                         "Windows: the directory is not to be inside a repository")
parser.add_argument("--image-only", action="store_true",
                    help="only check the firmware image and the disk image; do not open any port")
parser.add_argument("--check-only", action="store_true",
                    help="check the image, which m1n1 is running and where everything "
                         "would go on this machine; write nothing to guest memory, then exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?", default=DEFAULT_IMAGE)
args = parser.parse_args()
if args.seconds < 0:
    parser.error("--seconds: not a time")
if not 0 <= args.kd_port <= 65535:
    parser.error("--kd-port: not a port")
if args.wait_debugger < 0:
    parser.error("--wait-debugger: not a time")
if args.wait_debugger and not args.kd_port:
    parser.error("--wait-debugger: with --kd-port 0 there is no relay for a debugger to connect to")
if args.loader == "m1n1" and not args.disk:
    parser.error("--loader m1n1: m1n1 does not start the firmware without a boot disk (--disk)")
if args.loader == "m1n1" and args.xtrack == "off":
    parser.error("--loader m1n1: m1n1 always turns execution tracking on")
if args.no_host and args.loader != "m1n1":
    parser.error("--no-host: only m1n1 can go on without a host (--loader m1n1)")
if args.no_host and (args.wait_debugger or args.bugcheck):
    parser.error("--no-host: without a host there is no serial port for a debugger, and "
                 "nobody to look at the guest when it stops")

PAGE = 0x4000

# The firmware keeps the top of RAM for itself: PcdSystemMemoryUefiRegionSize in
# Platform/SiliconNTPkg/SiliconNT.dsc of siliconnt-firmware
UEFI_REGION_SIZE = 0x8000000
# HV._prepare_load() starts the guest's own data this far above the start of guest RAM
GUEST_GAP = 32 << 20
# Where the image goes: a multiple of every alignment its modules ask for (64 KB at most)
IMAGE_ALIGN = 0x200000
BOOTARGS_SIZE = PAGE
# The table of AIC lines and SPIs: SELFBOOT_AICMAP_SIZE in src/selfboot.h
AICMAP_SIZE = PAGE
AICMAP_VERSION = 1

# The boot disk starts on a 2 MB boundary like the image. The firmware's pages are 4 KB,
# and it is to reserve exactly the pages the disk lies in.
DISK_ALIGN = 0x200000
DISK_PAGE = 0x1000
# The primary GPT header is the second sector of a disk with 512-byte sectors
GPT_OFFSET = 512
GPT_SIGNATURE = b"EFI PART"
# The disk goes up in pieces: a compressed piece has to fit into the 128 MB of the target's
# memory that this script keeps as its heap (ProxyUtils below), and P_GZDEC takes its sizes
# in 32 bits (src/proxy.c)
DISK_PIECE = 16 << 20

# ---- the image, offline --------------------------------------------------------------

image = args.payload.read_bytes()

def check_image(image):
    """The Linux arm64 image header the firmware's flash layout puts at offset 0
    (SiliconNT.fdf), and what the entry code relies on."""
    if len(image) < 0x40:
        sys.exit(f"{args.payload}: too short for an image header")
    code0, code1 = struct.unpack_from("<II", image, 0)
    image_size, = struct.unpack_from("<Q", image, 0x10)
    if image[0x38:0x3c] != b"ARM\x64":
        sys.exit(f"{args.payload}: no arm64 image magic at offset 0x38")
    if code0 != 0x10000001:
        sys.exit(f"{args.payload}: the first instruction is not 'adr x1, .'; the entry code "
                 "takes the load address from x1")
    if code1 >> 26 != 0b000101:
        sys.exit(f"{args.payload}: the second instruction is not a branch")
    entry_code = 4 + ((code1 & 0x3ffffff) << 2)
    if image_size != len(image):
        sys.exit(f"{args.payload}: header says 0x{image_size:x} bytes, file has 0x{len(image):x}")
    if len(image) % PAGE:
        sys.exit(f"{args.payload}: size is not a multiple of 0x{PAGE:x}")
    if entry_code >= len(image):
        sys.exit(f"{args.payload}: the header branches outside the image")
    return entry_code

entry_code = check_image(image)
print(f"Firmware image: {args.payload}")
print(f"  size 0x{len(image):x}, SHA-256 {hashlib.sha256(image).hexdigest()}")
print(f"  entered at offset 0, entry code at offset 0x{entry_code:x}")

disk = args.disk.read_bytes() if args.disk else None

def check_disk(disk):
    """What the firmware needs of the boot disk to register it and to find a partition."""
    if not disk:
        sys.exit(f"{args.disk}: empty")
    if len(disk) % DISK_PAGE:
        sys.exit(f"{args.disk}: size 0x{len(disk):x} is not a multiple of 0x{DISK_PAGE:x}")
    if disk[GPT_OFFSET:GPT_OFFSET + len(GPT_SIGNATURE)] != GPT_SIGNATURE:
        sys.exit(f"{args.disk}: no GPT header: no '{GPT_SIGNATURE.decode()}' at offset "
                 f"{GPT_OFFSET}")

if disk is not None:
    check_disk(disk)
    print(f"Boot disk: {args.disk}")
    print(f"  size 0x{len(disk):x} ({len(disk) / (1 << 20):.1f} MB), "
          f"SHA-256 {hashlib.sha256(disk).hexdigest()}")
if args.image_only:
    sys.exit(0)

# ---- the target ----------------------------------------------------------------------

from m1n1.proxy import *
# Not a function or a class, so not among what the line above brings
from m1n1.proxy import ExcInfo
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.sysreg import *
from m1n1.hv import HV, HV_EVENT
from m1n1.hw.pmu import PMU
from m1n1.tgtypes import BootArgs_r1, BootArgs_r2, BootArgs_r3

# Where the version string sits in the image, and what it says in this build. The raw image
# is loaded as it is, so an offset into the file is an offset from the base in memory.
VERSION_MARK = b"##m1n1_ver##"
m1n1_image = args.m1n1.read_bytes()
version_off = m1n1_image.find(VERSION_MARK)
if version_off < 0:
    sys.exit(f"{args.m1n1}: no version string")
version_want = m1n1_image[version_off:m1n1_image.index(b"\0", version_off)]

iface = UartInterface()
p = M1N1Proxy(iface, debug=False)
bootstrap_port(iface, p)

def m1n1_heap(mem_size):
    """What m1n1 keeps for itself above its heap: the shadow pages of execution tracking,
    a stage 2 table of 4 KB for every 2 MB of memory, and room for the rest. The same as
    selfboot_m1n1_heap() of src/selfboot.c, which m1n1 checks the start of guest RAM against
    with --loader m1n1. mem_size is all the memory there is, mem_size_actual."""
    return (32 << 20) + mem_size // 512 + (16 << 20)

# ProxyUtils needs it when it is made, before it reads the boot arguments itself: read them
# here as it does
ba_addr, ba_rev = p.get_bootargs_rev()
if ba_rev > 3:
    sys.exit(f"The boot arguments are of revision {ba_rev}, which ProxyUtils does not know")
ba_struct = {2: BootArgs_r2, 3: BootArgs_r3}.get(ba_rev, BootArgs_r1)
mem_size_actual = iface.readstruct(ba_addr, ba_struct).mem_size_actual

u = ProxyUtils(p, heap_size=128 * 1024 * 1024, m1n1_heap=m1n1_heap(mem_size_actual))

# An m1n1 without the changes of this tree has no GIC to offer the firmware
version_got = iface.readmem(u.base + version_off, len(version_want))
print(f"m1n1 expected: {version_want[len(VERSION_MARK):].decode()}")
print(f"m1n1 running:  {version_got[len(VERSION_MARK):].decode(errors='replace')!r}")
if version_got != version_want:
    sys.exit("The m1n1 on the target is not the one built from this tree: "
             "chainload build/m1n1.bin first (chainload.py -r)")

# ---- where everything goes on this machine -------------------------------------------

def overlaps(a_base, a_size, b_base, b_size):
    return a_base < b_base + b_size and b_base < a_base + a_size

class Layout:
    """Mirrors HV._prepare_load(), so that it can be checked before the hypervisor is
    initialized. load_firmware() asserts that the two agree."""
    def __init__(self, u, image_len, disk_len):
        self.phys_base = u.heap_top
        self.mem_top = u.ba.phys_base + u.ba.mem_size
        self.adt_base = self.phys_base + GUEST_GAP
        self.adt_size = align(u.ba.devtree_size)
        self.image_base = align_up(self.adt_base + self.adt_size, IMAGE_ALIGN)
        self.image_size = image_len
        self.bootargs = self.image_base + align(image_len)
        self.aicmap = self.bootargs + BOOTARGS_SIZE
        self.end = self.aicmap + AICMAP_SIZE
        self.disk_base = align_up(self.end, DISK_ALIGN)
        self.disk_size = disk_len
        # top_of_kernel_data in the boot arguments: where what was put here ends
        self.top = align(self.disk_base + disk_len) if disk_len else self.end
        self.uefi_base = self.mem_top - UEFI_REGION_SIZE
        self.used = [
            ("device tree", self.adt_base, self.adt_size),
            ("firmware image", self.image_base, self.image_size),
            ("boot arguments", self.bootargs, BOOTARGS_SIZE),
            ("AIC to GIC table", self.aicmap, AICMAP_SIZE),
            ("firmware work area", self.uefi_base, UEFI_REGION_SIZE),
        ]
        if disk_len:
            self.used.insert(4, ("boot disk", self.disk_base, self.disk_size))

layout = Layout(u, len(image), len(disk) if disk is not None else 0)
problems = []

print(f"m1n1: base 0x{u.base:x}, iBoot's memory 0x{u.ba.phys_base:x} .. 0x{layout.mem_top:x}")
print(f"Guest RAM: 0x{layout.phys_base:x} .. 0x{layout.mem_top:x} "
      f"({(layout.mem_top - layout.phys_base) >> 20} MB)")
for name, base, size in layout.used:
    print(f"  {name:20s} 0x{base:x} .. 0x{base + size:x}")
    if base < layout.phys_base or base + size > layout.mem_top:
        problems.append(f"{name} does not fit in guest RAM")
if layout.end > layout.uefi_base:
    problems.append("the image, the boot arguments and the table reach into the firmware "
                    "work area")
elif layout.disk_size and layout.disk_base + layout.disk_size > layout.uefi_base:
    problems.append("the boot disk reaches into the firmware work area: there is room for "
                    f"{max(layout.uefi_base - layout.disk_base, 0) >> 20} MB")

# The firmware declares all of guest RAM as memory and maps the framebuffer separately
video = u.ba.video
fb_size = align(video.stride * video.height)
print(f"Framebuffer: 0x{video.base:x} .. 0x{video.base + fb_size:x} "
      f"({video.width} x {video.height}, stride 0x{video.stride:x})")
if overlaps(video.base, fb_size, layout.phys_base, layout.mem_top - layout.phys_base):
    problems.append("the framebuffer lies inside guest RAM")

# What iBoot left in memory. The firmware reserves none of it, so anything inside guest
# RAM is ordinary free memory to it.
print("/chosen/memory-map (entries inside guest RAM are marked):")
for name, value in sorted(u.adt["chosen"]["memory-map"]._properties.items()):
    if isinstance(value, bytes) and len(value) == 16:
        value = struct.unpack("<QQ", value)
    if not (isinstance(value, (tuple, list)) and len(value) == 2 and
            all(isinstance(v, int) for v in value)):
        continue
    base, size = value
    inside = size and overlaps(base, size, layout.phys_base, layout.mem_top - layout.phys_base)
    print(f"  {'*' if inside else ' '} {name:28s} 0x{base:x} .. 0x{base + size:x}")
    for used_name, used_base, used_size in layout.used:
        if size and overlaps(base, size, used_base, used_size):
            problems.append(f"memory-map entry {name} overlaps the {used_name}")

# The EL1 state the firmware starts in. The hypervisor leaves these two registers as it
# finds them. The entry code runs with the MMU off until it has built its own page tables,
# and enables FP/SIMD access itself, so CPACR_EL1 is only printed.
print("EL1 state the firmware starts in:")
try:
    sctlr_el1 = u.mrs(SCTLR_EL12)
    cpacr_el1 = u.mrs(CPACR_EL12)
except Exception as e:
    problems.append(f"SCTLR_EL12 and CPACR_EL12 cannot be read ({e})")
else:
    print(f"  SCTLR_EL12 = 0x{sctlr_el1:x} (M={sctlr_el1 & 1} C={sctlr_el1 >> 2 & 1} "
          f"I={sctlr_el1 >> 12 & 1})")
    print(f"  CPACR_EL12 = 0x{cpacr_el1:x} (FPEN={cpacr_el1 >> 20 & 3})")
    if sctlr_el1 & 1:
        problems.append("the EL1 MMU is on (SCTLR_EL12.M = 1); the firmware's entry code "
                        "expects it off")

if problems:
    for problem in problems:
        print(f"PROBLEM: {problem}")
    sys.exit("Not going on: nothing was written to guest memory.")

# ---- the firmware's serial output, and the kernel debugger ----------------------------

class DebuggerRelay:
    """Listens on 127.0.0.1 for the kernel debugger and passes bytes, as they are, between
    it and the secondary serial port. One connection at a time; when it ends, the next one
    is taken. Connections are taken from the moment the port is opened; bytes pass once
    start() has been called."""
    def __init__(self, port, dev):
        self.where = f"127.0.0.1:{port}"
        self.dev = dev
        try:
            self.listener = socket.create_server(("127.0.0.1", port))
        except OSError as e:
            sys.exit(f"{self.where}: cannot listen for the kernel debugger ({e}); "
                     "--kd-port 0 runs without")
        self.client = None
        self.lock = threading.Lock()
        self.to_target = self.to_debugger = 0
        # Set once the first debugger has connected (--wait-debugger)
        self.connected = threading.Event()
        self.stopping = False
        self.thread = threading.Thread(target=self.run, daemon=True)

    def say(self, text):
        print("KD> " + text, flush=True)

    def accept(self):
        conn, addr = self.listener.accept()
        with self.lock:
            taken = self.client is not None
            if not taken:
                # send() below is not to wait for a debugger that does not read
                conn.setblocking(False)
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.client = conn
        if taken:
            conn.close()
            self.say(f"a second connection, from {addr[0]}:{addr[1]}, was closed: a debugger "
                     "is connected")
        else:
            self.say(f"debugger connected from {addr[0]}:{addr[1]}")
            self.connected.set()

    def drop(self, client, why):
        with self.lock:
            if self.client is not client:
                return
            self.client = None
        client.close()
        self.say(f"debugger gone: {why}")

    def run(self):
        while not self.stopping:
            client = self.client
            try:
                ready = select.select([self.listener] + ([client] if client else []),
                                      [], [], 0.2)[0]
            except (OSError, ValueError):
                # send_to_debugger() has dropped the client in the meantime
                continue
            for sock in ready:
                if sock is self.listener:
                    self.accept()
                    continue
                try:
                    data = sock.recv(65536)
                except BlockingIOError:
                    continue
                except OSError as e:
                    self.drop(sock, str(e))
                    continue
                if not data:
                    self.drop(sock, "it closed the connection")
                    continue
                try:
                    self.dev.write(data)
                except Exception as e:
                    self.say(f"{len(data)} bytes from the debugger not written to the serial "
                             f"port ({e})")
                    continue
                self.to_target += len(data)

    def send_to_debugger(self, data):
        """Called by the thread that reads the serial port."""
        client = self.client
        if client is None:
            return
        try:
            sent = client.send(data)
        except BlockingIOError:
            sent = 0
        except OSError as e:
            self.drop(client, str(e))
            return
        self.to_debugger += sent
        if sent < len(data):
            self.drop(client, "it does not read what the target sends")

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping = True
        if self.thread.is_alive():
            self.thread.join(2)
            self.say(f"{self.to_target} bytes to the target, {self.to_debugger} bytes to the "
                     "debugger")
        if self.client:
            self.client.close()
        self.listener.close()

# The debugger's protocol has no lines; this many bytes are shown at a time
LINE_MAX = 512

class SerialReader:
    """Prints what arrives on the secondary USB serial port, line by line, and hands it to
    the relay for the kernel debugger."""
    def __init__(self, path, log_path, kd_port):
        import serial
        try:
            self.dev = serial.Serial(path, 115200, timeout=0.2)
        except (OSError, serial.SerialException) as e:
            sys.exit(f"{path}: cannot open the secondary serial port, where the firmware "
                     f"prints ({e})")
        self.relay = DebuggerRelay(kd_port, self.dev) if kd_port else None
        self.log = open(log_path, "wb") if log_path else None
        self.pending = b""
        self.stopping = False
        # When the first byte arrived, by time.monotonic()
        self.first_byte = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def emit(self, line):
        """Text as it is; any other byte or character as an escape. The packets of the
        debugger's protocol are binary, and they are not to be taken for commands by the
        terminal this is printed on."""
        line = line.rstrip(b"\r")
        for start in range(0, max(len(line), 1), LINE_MAX):
            text = line[start:start + LINE_MAX].decode(errors="backslashreplace")
            print("FW> " + "".join(ch if ch.isprintable() or ch == "\t"
                                   else ch.encode("unicode_escape").decode()
                                   for ch in text), flush=True)

    def run(self):
        while not self.stopping:
            # What is there, or the first byte within the timeout. Asking for more would
            # hold a packet back until the timeout: a debugger waits for each one.
            data = self.dev.read(self.dev.in_waiting or 1)
            if not data:
                continue
            if self.first_byte is None:
                self.first_byte = time.monotonic()
            if self.log:
                self.log.write(data)
                self.log.flush()
            if self.relay:
                self.relay.send_to_debugger(data)
            *lines, self.pending = (self.pending + data).split(b"\n")
            for line in lines:
                self.emit(line)
            while len(self.pending) > LINE_MAX:
                self.emit(self.pending[:LINE_MAX])
                self.pending = self.pending[LINE_MAX:]

    def start(self):
        self.thread.start()
        if self.relay:
            self.relay.start()

    def stop(self):
        self.stopping = True
        if self.thread.is_alive():
            self.thread.join(2)
        if self.relay:
            self.relay.stop()
        if self.pending:
            self.emit(self.pending)
            self.pending = b""
        if self.log:
            self.log.close()

# Both are open from here on. m1n1 drops what the guest prints while nobody has the serial
# port open, and the debugger gives up if nobody listens when it is started.
reader = SerialReader(args.serial, args.serial_log, args.kd_port)
print(f"Firmware output: {args.serial}")
if reader.relay:
    print(f"Kernel debugger: {reader.relay.where} "
          f"(WinDbgX -k com:ipport={args.kd_port},port=127.0.0.1)")

if args.check_only:
    reader.stop()
    sys.exit(0)

# ---- what both loaders use -----------------------------------------------------------

def dump_state(title):
    print(f"==== {title} ====")
    for name in ("HCR_EL2", "ISR_EL1", "ICC_SRE_EL2", "ICH_VTR_EL2", "ICH_HCR_EL2",
                 "ICH_VMCR_EL2", "ICH_MISR_EL2", "ICH_EISR_EL2", "ICH_ELRSR_EL2",
                 "ICH_AP1R0_EL2", "CNTV_CTL_EL02", "CNTV_CVAL_EL02", "CNTP_CTL_EL02",
                 "VM_TMR_FIQ_ENA_EL2", "CNTHCTL_EL2", "SCTLR_EL12", "CPACR_EL12",
                 "TCR_EL12", "TTBR0_EL12", "VBAR_EL12", "SP_EL1",
                 # the last exception the firmware took itself, in EL1
                 "ELR_EL12", "SPSR_EL12", "ESR_EL12", "FAR_EL12"):
        print(f"{name:18s} = 0x{u.mrs(globals()[name]):x}")
    for i in range((u.mrs(ICH_VTR_EL2) & 0x1f) + 1):
        print(f"{f'ICH_LR{i}_EL2':18s} = 0x{u.mrs(globals()[f'ICH_LR{i}_EL2']):x}")
    sys.stdout.flush()

def load_disk(disk, base):
    """The boot disk goes up the way the image does, compressed, but in pieces. Each piece
    is checked where it lands: a gzip stream ends with the CRC-32 of the data, m1n1
    compares that with the CRC-32 of what it has unpacked (tinf_gzip_uncompress(),
    src/tinf/tinfgzip.c) and P_GZDEC returns an error instead of the size if they differ,
    which ProxyUtils.compressed_writemem() asserts it does not. The two ends are read back
    as well: that says the disk is at this address, which a checksum does not."""
    print(f"Loading the boot disk (0x{len(disk):x} bytes) at 0x{base:x}...", flush=True)
    started = shown = time.monotonic()
    for off in range(0, len(disk), DISK_PIECE):
        piece = disk[off:off + DISK_PIECE]
        try:
            u.compressed_writemem(base + off, piece)
        except AssertionError:
            sys.exit(f"Boot disk: the piece at offset 0x{off:x} did not arrive intact. "
                     "The guest was not entered.")
        now = time.monotonic()
        if now - shown >= 5:
            shown = now
            done = off + len(piece)
            print(f"  0x{done:x} bytes ({100 * done // len(disk)}%) after "
                  f"{now - started:.0f} s", flush=True)
    elapsed = max(time.monotonic() - started, 0.001)
    for off in sorted({0, len(disk) - DISK_PAGE}):
        if iface.readmem(base + off, DISK_PAGE) != disk[off:off + DISK_PAGE]:
            sys.exit(f"Boot disk: the page at offset 0x{off:x} reads back as something else. "
                     "The guest was not entered.")
    print(f"Boot disk: 0x{len(disk):x} bytes in {elapsed:.1f} s, "
          f"{len(disk) / (1 << 20) / elapsed:.1f} MB/s; every piece passed m1n1's CRC-32 "
          "check, the first and the last page read back the same")

def guest_ran(since):
    if since is not None:
        print(f"The guest ran for {time.monotonic() - since:.1f} s")

def bugcheck_report():
    """--bugcheck. Called while the guest is stopped and has not been left: its registers
    and its tables are still its own. Whatever goes wrong here is said and nothing more:
    the event that stopped the guest still has to be answered."""
    if not args.bugcheck:
        return
    try:
        regs = {"VBAR_EL1": u.mrs(VBAR_EL12), "TTBR1_EL1": u.mrs(TTBR1_EL12),
                "TCR_EL1": u.mrs(TCR_EL12)}
        bugcheck.report(iface.readmem, regs, (layout.phys_base, layout.mem_top), args.bugcheck)
    except Exception as e:
        print(f"PROBLEM: what Windows recorded of a bug check was not read "
              f"({type(e).__name__}: {e})")
    sys.stdout.flush()

# The events with which the hypervisor says that it has stopped the guest for good
# (src/hv/hv_psci.c), by their names in HV_EVENT (proxyclient/m1n1/hv/types.py)
GUEST_STOPS = {
    "GUEST_OFF": "the guest called PSCI SYSTEM_OFF",
    "GUEST_RESET": "the guest called PSCI SYSTEM_RESET",
    "GUEST_HVC": "the guest made a call that the hypervisor cannot carry out and that must "
                 "not return: PSCI CPU_ON or CPU_OFF, or an hvc other than #0 (m1n1 has "
                 "printed which)",
}
timed_out = threading.Event()

# --seconds has run out. What SIGINT does is the loader's business.
def time_out():
    timed_out.set()
    os.kill(os.getpid(), signal.SIGINT)

def wait_for_debugger():
    # The Windows kernel looks for the debugger once, early, and goes on without it: one that
    # connects later sees nothing of a bug check (R17).
    if not args.wait_debugger:
        return
    print(f"Waiting for the kernel debugger on {reader.relay.where}, up to "
          f"{args.wait_debugger} s", flush=True)
    if not reader.relay.connected.wait(args.wait_debugger):
        sys.exit(f"Not going on: no kernel debugger connected within {args.wait_debugger} s. "
                 "The guest has not run.")

# ---- --loader m1n1 -------------------------------------------------------------------

# A stage 2 entry as hv_pt_walk() returns it (src/hv/hv_vm.c): PTE_VALID, and
# PTE_TARGET_MASK, the page of the machine it maps
PTE_VALID = 1
PTE_TARGET = (1 << 50) - (1 << 12)
# Left to software (PTE_SW_XONLY): a page of the guest's code with instructions replaced, which
# the core executes from a shadow page of the hypervisor (src/hv/hv_xtrack.c), so the entry
# points there and not to the page itself
PTE_SW_XONLY = 1 << 58
STAGE2_PAGE = 0x1000
# hv_pt_walk() asserts that the address is one the tables cover; the M4's cover 42 bits
# ("HV: Initializing for 42-bit PA range")
STAGE2_BITS = 42
# The last line of selfboot_enter() before the guest runs
ENTERING = b"selfboot: entering the firmware"

def load_image(image, base):
    """The firmware image, as it is. m1n1 flushes the caches for it (selfboot_enter())."""
    print(f"Loading the firmware image (0x{len(image):x} bytes) at 0x{base:x}...", flush=True)
    try:
        u.compressed_writemem(base, image, True)
    except AssertionError:
        sys.exit("Firmware image: it did not arrive intact. The guest was not entered.")
    for off in sorted({0, len(image) - PAGE}):
        if iface.readmem(base + off, PAGE) != image[off:off + PAGE]:
            sys.exit(f"Firmware image: the page at offset 0x{off:x} reads back as something "
                     "else. The guest was not entered.")
    print("Firmware image: it passed m1n1's CRC-32 check, the first and the last page read "
          "back the same")

def check_stage2(io_ranges):
    """What m1n1 mapped for the guest (selfboot_map(), src/selfboot_run.c), looked up in the
    stage 2 tables, which are still there once the guest has been left. Each page has to be
    mapped to itself or not at all: both ends of every range of /arm-io, of guest RAM and
    of the memory below iBoot's are; m1n1 and the heap of this script are not; the UART is
    a hook of the hypervisor. A page of memory may also be execute-only, mapped to a shadow
    page: code that the guest ran, with instructions replaced. A shadow page is m1n1's, never
    a page of the guest's memory. Returns how many are not as they should be."""
    uart = u.adt["/arm-io/uart0"].get_reg(0)[0] & ~(STAGE2_PAGE - 1)
    ram_base = layout.phys_base & ~0xffffffff
    guest_memory = [(layout.phys_base, layout.mem_top)]
    if ram_base < u.ba.phys_base:
        guest_memory.append((ram_base, u.ba.phys_base))
    pages = {}
    for base, size in io_ranges:
        for addr in (base, base + size - STAGE2_PAGE):
            pages[addr] = "mapped"
    pages[layout.phys_base] = pages[layout.mem_top - STAGE2_PAGE] = "memory"
    if ram_base < u.ba.phys_base:
        pages[ram_base] = pages[u.ba.phys_base - STAGE2_PAGE] = "memory"
    pages[u.base] = pages[layout.phys_base - STAGE2_PAGE] = "not mapped"
    pages[uart] = "hook"

    problems = xonly = 0
    for addr, want in sorted(pages.items()):
        if addr >> STAGE2_BITS:
            print(f"PROBLEM: 0x{addr:x} is beyond {STAGE2_BITS} bits, not looked up")
            problems += 1
            continue
        pte = p.hv_pt_walk(addr)
        if pte == 0:
            got = "not mapped"
        elif not pte & PTE_VALID:
            got = "hook"
        elif pte & PTE_SW_XONLY:
            target = pte & PTE_TARGET
            if target == addr:
                got = "execute-only, mapped to itself"
            elif any(base <= target < top for base, top in guest_memory):
                got = f"execute-only, mapped to 0x{target:x} in the guest's memory"
            else:
                got = "execute-only, mapped to a shadow page"
        elif (pte & PTE_TARGET) == addr:
            got = "mapped"
        else:
            got = "mapped to another page"
        if want == "memory" and got in ("mapped", "execute-only, mapped to a shadow page"):
            xonly += got != "mapped"
            continue
        if got != want:
            want = "mapped" if want == "memory" else want
            print(f"PROBLEM: 0x{addr:x} should be {want} and is {got} (entry 0x{pte:x})")
            problems += 1
    if problems:
        print(f"Stage 2: {problems} of {len(pages)} pages are not as they should be")
    else:
        print(f"Stage 2: as expected, {len(pages)} pages looked up: both ends of the "
              f"{len(io_ranges)} ranges of /arm-io and of the memory mapped ({xonly} of the "
              "pages of memory execute-only, mapped to a shadow page), m1n1 and this script's "
              "heap not mapped, the UART a hook")
    return problems

def m1n1_hands_over_for_good():
    """--no-host: P_SELFBOOT_RUN with no host. Nothing comes back that this script could see:
    the reply only if m1n1 does not enter the guest, which it says on its screen as well,
    and m1n1's console leaves the USB port before the guest runs (selfboot_enter())."""
    print(f"Handing over to m1n1 (P_SELFBOOT_RUN) for good: guest RAM from "
          f"0x{layout.phys_base:x}, image 0x{len(image):x} bytes, disk 0x{len(disk):x} bytes, "
          "no host")
    p.selfboot_run(layout.phys_base, len(image), len(disk), no_host=True)
    # Out of this machine before the port is closed
    iface.dev.flush()
    print("Sent. This script does not stay: from here on what happens is on the machine's "
          "screen. If m1n1 did not enter the guest it says why there, and waits for a host.")

def m1n1_loads():
    """Uploads the two files, hands over to m1n1 and waits until the guest has been left.
    Returns the exit code of the script: 1 if m1n1 did not enter the guest, 2 if stage 2 is
    not what it should be. With --no-host it leaves once the command is sent, and returns 0."""
    class run:
        stopping = False   # an event has arrived: the run is ending
        over = False       # the command has returned
        entering = None    # when m1n1 said that it enters the guest
        console = b""

    io_ranges = [(r.parent_addr, r.size) for r in u.adt["/arm-io"].ranges]
    print(f"/arm-io: {len(io_ranges)} ranges for m1n1 to map")
    for base, size in io_ranges:
        print(f"  0x{base:x} .. 0x{base + size:x}")

    load_image(image, layout.image_base)
    load_disk(disk, layout.disk_base)
    if args.no_host:
        m1n1_hands_over_for_good()
        return 0

    def guest_event(reason, code, info):
        run.stopping = True
        name = HV_EVENT(code).name if reason == START.HV else code.name
        event = f"{reason.name}/{name}"
        # An exception of m1n1 itself, or the hypervisor giving up. What they expect for an
        # answer is not "leave the guest", and nothing more is sent.
        if reason == START.EXCEPTION or (reason == START.HV and name in ("PANIC", "WDT_BARK")):
            print(f"Run ended: m1n1 itself is in trouble ({event}); it has printed what it "
                  "knows above")
            sys.exit("m1n1 was not answered and waits. Power the M4 off.")
        interrupted = reason == START.HV and name == "USER_INTERRUPT"
        if interrupted:
            why = f"--seconds {args.seconds} ran out" if timed_out.is_set() else "interrupted (Ctrl-C)"
        elif reason == START.HV and name in GUEST_STOPS:
            why = GUEST_STOPS[name]
        else:
            why = ("the hypervisor could not deal with this by itself; without a host the "
                   "machine would stop here and show why on its screen")
        print(f"Run ended: {why} ({event})")
        guest_ran(run.entering)
        # First, before anything that has not been done on the machine before: this needs
        # only registers and memory read
        bugcheck_report()
        ctx = iface.readstruct(info, ExcInfo)
        where = ""
        if layout.image_base <= ctx.elr < layout.image_base + layout.image_size:
            where = f" (firmware image + 0x{ctx.elr - layout.image_base:x})"
        print(f"Firmware stopped at PC 0x{ctx.elr:x}{where}")
        try:
            u.print_context(ctx, reason == START.EXCEPTION_LOWER and name in ("SYNC", "SERROR"))
        except Exception as e:
            print(f"(context not printed: {e})")
        dump_state("firmware stopped")
        p.exit(EXC_RET.EXIT_GUEST)

    for code in EXC:
        iface.set_handler(START.EXCEPTION_LOWER, code, guest_event)
        iface.set_handler(START.EXCEPTION, code, guest_event)
    for code in HV_EVENT:
        iface.set_handler(START.HV, code, guest_event)

    # What m1n1 prints arrives between the replies and is shown as "TTY> ..."
    show_console = iface.unkhandler
    def console(data):
        seen = run.console + bytes(data)
        if run.entering is None and ENTERING in seen:
            run.entering = time.monotonic()
        # Enough to find the line when it comes in pieces
        run.console = seen[-len(ENTERING):]
        show_console(data)
    iface.unkhandler = console

    # HV.interrupt(): a byte on the port the proxy listens on brings the hypervisor back
    # with HV/USER_INTERRUPT (hv_tick(), src/hv/hv.c)
    def interrupt(signum=None, frame=None):
        if not run.stopping and not run.over:
            iface.dev.write(b"!")

    wait_for_debugger()

    heap_before = p.heapblock_alloc(0)
    print(f"Handing over to m1n1 (P_SELFBOOT_RUN): guest RAM from 0x{layout.phys_base:x}, "
          f"image 0x{len(image):x} bytes, disk 0x{len(disk):x} bytes")
    if not args.seconds:
        print("No time limit: Ctrl-C ends the run")
    sys.stdout.flush()
    timer = threading.Timer(args.seconds, time_out)
    timer.daemon = True
    reply_timeout = iface.dev.timeout
    iface.dev.timeout = None
    old_sigint = signal.signal(signal.SIGINT, interrupt)
    reader.first_byte = None
    sent = time.monotonic()
    if args.seconds:
        timer.start()
    try:
        # Returns once the guest has been left, or at once if m1n1 does not enter it
        ret = p.selfboot_run(layout.phys_base, len(image), len(disk))
    finally:
        run.over = True
        timer.cancel()
        signal.signal(signal.SIGINT, old_sigint)
        iface.dev.timeout = reply_timeout

    if ret:
        print(f"m1n1 did not enter the guest (P_SELFBOOT_RUN returned {ret}): it has said why "
              "above, in a line that starts with 'TTY> selfboot:'. Nothing has run.")
        return 1

    # m1n1's console and the firmware's output come over USB when they come: both times
    # are upper bounds for what m1n1 does before the guest runs.
    if run.entering is not None:
        print(f"m1n1 said it was entering the firmware {run.entering - sent:.1f} s after the "
              "command")
    if reader.first_byte is not None:
        print(f"the firmware's first output came {reader.first_byte - sent:.1f} s after the "
              "command")
    heap_after = p.heapblock_alloc(0)
    print(f"m1n1's heap: its top went from 0x{heap_before:x} to 0x{heap_after:x}, "
          f"{(heap_after - heap_before) / (1 << 20):.1f} MiB; "
          f"{(u.heap_base - heap_after) / (1 << 20):.1f} MiB are left below its limit")
    return 2 if check_stage2(io_ranges) else 0

if args.loader == "m1n1":
    reader.start()
    try:
        code = m1n1_loads()
    finally:
        reader.stop()
    if code != 1 and not args.no_host:
        print("Hypervisor exited.")
    sys.exit(code)

# ---- --loader host: the hypervisor ---------------------------------------------------

hv = HV(iface, p, u)
# Boot CPU only: with hv.smp False, HV.init() does not start the secondary CPUs. By the same
# hv.smp, HV.setup_adt(), which it calls later, would take the nodes of the cores that do not
# run out of /cpus of the guest's device tree. The firmware is to find every core of the
# machine there, as it does when m1n1 does the loading (src/selfboot.c hands it m1n1's own
# tree): /cpus is what it makes the MADT from. So for HV.setup_adt(), and for nothing else,
# hv.smp says to leave the nodes.
hv.smp = False
hv_setup_adt = hv.setup_adt

def setup_adt_with_every_cpu():
    hv.smp = True
    try:
        hv_setup_adt()
    finally:
        hv.smp = False

hv.setup_adt = setup_adt_with_every_cpu
hv.init()

guest_cpus = [cpu.name for cpu in hv.adt["cpus"]]
m1n1_cpus = [cpu.name for cpu in u.adt["cpus"]]
if guest_cpus != m1n1_cpus:
    sys.exit(f"/cpus of the guest's device tree has {len(guest_cpus)} nodes after HV.init() "
             f"({' '.join(guest_cpus)}), m1n1's own has {len(m1n1_cpus)}: the firmware would "
             "not see the machine as it does when m1n1 loads it. The guest has not run.")
print(f"/cpus of the guest's device tree: all {len(guest_cpus)} nodes of m1n1's own; "
      "only the boot CPU runs")

dump_state("after hypervisor init, before the firmware")

hcr = HCR(u.mrs(HCR_EL2))
print(f"HCR_EL2: {hcr!s}")
if not (hcr.IMO and hcr.FMO and hcr.VM and hcr.TWI):
    sys.exit("HCR_EL2 is not IMO=1 FMO=1 VM=1 TWI=1: this m1n1 was not built from this tree")
# The core waits out a WFI of the guest in WFE; the event stream is what ends a WFE for sure
if not u.mrs(CNTHCTL_EL2) & 4:
    sys.exit("CNTHCTL_EL2.EVNTEN is 0: this m1n1 was not built from this tree")
if not u.mrs(ICH_HCR_EL2) & 1:
    sys.exit("ICH_HCR_EL2.En is 0: this m1n1 was not built from this tree")
if u.mrs(ICH_MISR_EL2):
    sys.exit("ICH_MISR_EL2 is not 0 before the guest has run")

uart_base = hv.adt["/arm-io/uart0"].get_reg(0)[0]
print(f"Virtual UART at 0x{uart_base:x}")

def write_aicmap(base):
    """Has m1n1 write the table of AIC lines and SPIs to its page and prints what it says.
    Returns its size in bytes, as the firmware is told it."""
    size = p.hv_aicmap(base)
    if not 8 <= size <= AICMAP_SIZE or size % 4:
        sys.exit(f"m1n1 wrote no table of AIC lines and SPIs (P_HV_AICMAP returned {size}): it "
                 "has said why above. The guest has not run.")
    table = iface.readmem(base, size)
    version, count = struct.unpack_from("<II", table)
    lines = struct.unpack_from(f"<{(size - 8) // 4}I", table, 8)
    if version != AICMAP_VERSION or count != len(lines) or list(lines) != sorted(set(lines)):
        sys.exit(f"The table of AIC lines and SPIs m1n1 wrote is not one of version "
                 f"{AICMAP_VERSION}: version {version}, count {count}, {len(lines)} lines. The "
                 "guest has not run.")
    if count:
        print(f"AIC to GIC table at 0x{base:x}, 0x{size:x} bytes: {count} lines, AIC "
              f"{lines[0]} is SPI 32, AIC {lines[-1]} is SPI {32 + count - 1}")
    else:
        print(f"AIC to GIC table at 0x{base:x}, 0x{size:x} bytes: no lines")
    return size

def load_firmware(hv, image, disk, layout):
    """What HV.load_raw() does, without the parts only macOS needs, entering the image at
    its first byte."""
    hv._prepare_load()
    if (hv.phys_base, hv.guest_base, hv.mem_top) != (layout.phys_base, layout.adt_base,
                                                     layout.mem_top):
        sys.exit(f"HV._prepare_load() put guest RAM at 0x{hv.phys_base:x} .. 0x{hv.mem_top:x} "
                 f"and the guest at 0x{hv.guest_base:x}, not where this script checked")

    hv.adt_base = layout.adt_base
    hv.guest_base = layout.image_base
    hv.bootargs_off = layout.bootargs - layout.image_base
    hv.entry = layout.image_base

    print(f"Physical memory: 0x{hv.phys_base:x} .. 0x{hv.mem_top:x}")
    print(f"Guest region start: 0x{hv.guest_base:x}")
    hv._load_executable_raw(image, layout.image_base, "firmware")

    # The device tree itself is uploaded by HV.start()
    memory_map = hv.adt["chosen"]["memory-map"]
    memory_map.DeviceTree = (layout.adt_base, layout.adt_size)
    memory_map.BootArgs = (layout.bootargs, BOOTARGS_SIZE)
    memory_map.AICToGIC = (layout.aicmap, write_aicmap(layout.aicmap))
    if disk is not None:
        load_disk(disk, layout.disk_base)
        # The size in bytes, not rounded: it is the size of the disk
        memory_map.RAMDisk = (layout.disk_base, layout.disk_size)

    hv._write_boot_args(layout.top)

load_firmware(hv, image, disk, layout)

# After the memory of the guest has been filled and mapped. HV._prepare_load() has asked for
# the memory, and HV.pt_update() maps it now, in 2 MB blocks; turning tracking on takes them
# apart into pages, all of them, before any core runs the guest (SiliconNT docs/DESIGN.md,
# "一页内存的三种状态"). HV.start() then only adds the registers it answers for itself. What
# the host writes from here on (the device tree, in HV.start()) is data.
if args.xtrack == "off":
    print("Execution tracking: off. A guest that reads the PMUv3 cycle counter takes an "
          "exception")
else:
    hv.pt_update()
    if p.hv_xtrack() != 0:
        sys.exit("The hypervisor refused to track execution. The guest has not run.")
    print("Execution tracking: on")

# Does nothing unless M1N1_PMU_RESET=1 (proxyclient/m1n1/hw/pmu.py)
PMU(u).reset_panic_counter()

# HV.handle_exception() calls run_shell() on a user interrupt and on anything it could not
# handle, which includes the three events above. Report and leave the guest instead of
# waiting for input.
def stop_guest(entry_msg=None, exit_msg=None):
    ctx = hv.ctx
    event = getattr(hv.exc_code, "name", str(hv.exc_code))
    # SIGINT, from the timer below or from the keyboard, comes back as this event
    interrupted = hv.exc_reason == START.HV and event == "USER_INTERRUPT"
    if interrupted:
        why = f"--seconds {args.seconds} ran out" if timed_out.is_set() else "interrupted (Ctrl-C)"
    elif hv.exc_reason == START.HV and event in GUEST_STOPS:
        why = GUEST_STOPS[event]
    else:
        why = "a guest exception that nobody handled"
    print(f"Run ended: {why} ({hv.exc_reason.name}/{event})")
    guest_ran(entered)
    bugcheck_report()
    where = ""
    if layout.image_base <= ctx.elr < layout.image_base + layout.image_size:
        where = f" (firmware image + 0x{ctx.elr - layout.image_base:x})"
    print(f"Firmware stopped at PC 0x{ctx.elr:x}{where}")
    if interrupted:
        # For everything else, HV.handle_exception() has printed this already
        try:
            u.print_context(ctx, False)
        except Exception as e:
            print(f"(context not printed: {e})")
    dump_state("firmware stopped")
    return EXC_RET.EXIT_GUEST

hv.run_shell = stop_guest

# Count from the moment the guest is entered: HV.start() has installed its SIGINT handler by
# then, and the setup before that takes a while.
timer = threading.Timer(args.seconds, time_out)
timer.daemon = True
enter_guest = p.hv_start
# When the guest was entered, by time.monotonic()
entered = None

def enter_guest_timed(*hv_start_args):
    global entered
    print(f"Entering the firmware at 0x{hv_start_args[0]:x}, x0 = 0x{hv_start_args[1]:x}")
    entered = time.monotonic()
    if args.seconds:
        timer.start()
    else:
        print("No time limit: Ctrl-C ends the run")
    return enter_guest(*hv_start_args)

p.hv_start = enter_guest_timed

reader.start()
try:
    wait_for_debugger()
    hv.start()
finally:
    # A run that has ended by itself is not to be interrupted afterwards
    timer.cancel()
    reader.stop()

print("Hypervisor exited.")
