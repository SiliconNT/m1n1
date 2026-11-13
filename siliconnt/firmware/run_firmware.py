#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load the SiliconNT UEFI firmware (MACBOOKPROM4_EFI.fd of the siliconnt-firmware
repository) and run it as the guest of the hypervisor, on the boot CPU only.

The m1n1 on the target has to be built from this tree: the firmware needs the emulated GIC
(src/hv/hv_vgic.c) and the WFI trap (src/hv/hv_wfi.c).

What the firmware is handed, and where things go. Guest RAM starts at the top of the heap
this script keeps for itself, above m1n1 and m1n1's own heap (HV._prepare_load()), so what
m1n1 occupies is not part of the memory the boot arguments describe:

    phys_base             start of guest RAM (boot arguments: phys_base, mem_size)
    phys_base + 32 MB     Apple device tree, as edited by HV.setup_adt()
    next 2 MB boundary    the firmware image, entered at its first byte
    after the image       Apple boot arguments, one 16 KB page
    ...
    mem_top - 128 MB      the firmware's own work area: stack, hand-off blocks, DXE core
    mem_top               end of guest RAM

Entry: start of the image, x0 = address of the boot arguments, SP_EL1 = 0. The image
header leaves its load address in x1 and jumps to the entry code. The firmware uses the
boot arguments and the device tree in place and reserves both ranges; it copies neither.
SEPFW, the trust cache and the preoslog buffer, which HV.load_raw() copies for macOS, are
not copied: the firmware reads none of them.

Before anything is written to the target, the script checks that these ranges fit in guest
RAM, that the framebuffer lies outside it, and that nothing iBoot listed in
/chosen/memory-map overlaps them. --check-only stops there.

The firmware prints on the hypervisor's virtual UART. That goes to the secondary USB
serial port (/dev/m1n1-sec), which this script reads and prints with a "FW> " prefix.

The script needs no input: it lets the firmware run for --seconds, then interrupts the
hypervisor, prints the list registers, leaves the guest and exits. An unhandled guest
exception ends the run the same way.
"""
import sys, os, pathlib, argparse, signal, struct, threading, hashlib

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

DEFAULT_IMAGE = (pathlib.Path.home() / "src" / "siliconnt-firmware" / "Build" /
                 "MacBookProM4-AARCH64" / "DEBUG_CLANGPDB" / "FV" / "MACBOOKPROM4_EFI.fd")

parser = argparse.ArgumentParser(description="Run the SiliconNT UEFI firmware under the hypervisor")
parser.add_argument("--seconds", type=int, default=120,
                    help="how long the firmware may run before it is stopped (default 120)")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--serial", default=os.environ.get("M1N1SECDEVICE", "/dev/m1n1-sec"),
                    help="the secondary USB serial port, where the firmware prints")
parser.add_argument("--serial-log", type=pathlib.Path,
                    help="also write the raw bytes the firmware prints to this file")
parser.add_argument("--image-only", action="store_true",
                    help="only check the firmware image; do not open any port")
parser.add_argument("--check-only", action="store_true",
                    help="check the image, which m1n1 is running and where everything "
                         "would go on this machine; write nothing to guest memory, then exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?", default=DEFAULT_IMAGE)
args = parser.parse_args()

PAGE = 0x4000

# The firmware keeps the top of RAM for itself: PcdSystemMemoryUefiRegionSize in
# Platform/MacBookProM4Pkg/MacBookProM4.dsc of siliconnt-firmware
UEFI_REGION_SIZE = 0x8000000
# HV._prepare_load() starts the guest's own data this far above the start of guest RAM
GUEST_GAP = 32 << 20
# Where the image goes: a multiple of every alignment its modules ask for (64 KB at most)
IMAGE_ALIGN = 0x200000
BOOTARGS_SIZE = PAGE

# ---- the image, offline --------------------------------------------------------------

image = args.payload.read_bytes()

def check_image(image):
    """The Linux arm64 image header the firmware's flash layout puts at offset 0
    (MacBookProM4.fdf), and what the entry code relies on."""
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
if args.image_only:
    sys.exit(0)

# ---- the target ----------------------------------------------------------------------

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.sysreg import *
from m1n1.hv import HV
from m1n1.hw.pmu import PMU

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

u = ProxyUtils(p, heap_size=128 * 1024 * 1024,
               m1n1_heap=128 * 1024 * 1024)

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
    def __init__(self, u, image_len):
        self.phys_base = u.heap_top
        self.mem_top = u.ba.phys_base + u.ba.mem_size
        self.adt_base = self.phys_base + GUEST_GAP
        self.adt_size = align(u.ba.devtree_size)
        self.image_base = align_up(self.adt_base + self.adt_size, IMAGE_ALIGN)
        self.image_size = image_len
        self.bootargs = self.image_base + align(image_len)
        self.end = self.bootargs + BOOTARGS_SIZE
        self.uefi_base = self.mem_top - UEFI_REGION_SIZE
        self.used = [
            ("device tree", self.adt_base, self.adt_size),
            ("firmware image", self.image_base, self.image_size),
            ("boot arguments", self.bootargs, BOOTARGS_SIZE),
            ("firmware work area", self.uefi_base, UEFI_REGION_SIZE),
        ]

layout = Layout(u, len(image))
problems = []

print(f"m1n1: base 0x{u.base:x}, iBoot's memory 0x{u.ba.phys_base:x} .. 0x{layout.mem_top:x}")
print(f"Guest RAM: 0x{layout.phys_base:x} .. 0x{layout.mem_top:x} "
      f"({(layout.mem_top - layout.phys_base) >> 20} MB)")
for name, base, size in layout.used:
    print(f"  {name:20s} 0x{base:x} .. 0x{base + size:x}")
    if base < layout.phys_base or base + size > layout.mem_top:
        problems.append(f"{name} does not fit in guest RAM")
if layout.end > layout.uefi_base:
    problems.append("the image and the boot arguments reach into the firmware work area")

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

if problems:
    for problem in problems:
        print(f"PROBLEM: {problem}")
    sys.exit("Not going on: nothing was written to guest memory.")

# ---- the firmware's serial output ----------------------------------------------------

class SerialReader:
    """Prints what arrives on the secondary USB serial port, line by line."""
    def __init__(self, path, log_path):
        import serial
        try:
            self.dev = serial.Serial(path, 115200, timeout=0.2)
        except (OSError, serial.SerialException) as e:
            sys.exit(f"{path}: cannot open the secondary serial port, where the firmware "
                     f"prints ({e})")
        self.log = open(log_path, "wb") if log_path else None
        self.pending = b""
        self.stopping = False
        self.thread = threading.Thread(target=self.run, daemon=True)

    def emit(self, line):
        print("FW> " + line.rstrip(b"\r").decode(errors="replace"), flush=True)

    def run(self):
        while not self.stopping:
            data = self.dev.read(4096)
            if not data:
                continue
            if self.log:
                self.log.write(data)
                self.log.flush()
            *lines, self.pending = (self.pending + data).split(b"\n")
            for line in lines:
                self.emit(line)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping = True
        if self.thread.is_alive():
            self.thread.join(2)
        if self.pending:
            self.emit(self.pending)
            self.pending = b""
        if self.log:
            self.log.close()

reader = SerialReader(args.serial, args.serial_log)
print(f"Firmware output: {args.serial}")

if args.check_only:
    sys.exit(0)

# ---- the hypervisor ------------------------------------------------------------------

hv = HV(iface, p, u)
# Boot CPU only: the secondary CPUs are neither started nor shown to the guest
hv.smp = False
hv.init()

def dump_state(title):
    print(f"==== {title} ====")
    for name in ("HCR_EL2", "ISR_EL1", "ICC_SRE_EL2", "ICH_VTR_EL2", "ICH_HCR_EL2",
                 "ICH_VMCR_EL2", "ICH_MISR_EL2", "ICH_EISR_EL2", "ICH_ELRSR_EL2",
                 "ICH_AP1R0_EL2", "CNTV_CTL_EL02", "CNTV_CVAL_EL02", "CNTP_CTL_EL02",
                 "VM_TMR_FIQ_ENA_EL2", "CNTHCTL_EL2", "SCTLR_EL12", "VBAR_EL12"):
        print(f"{name:18s} = 0x{u.mrs(globals()[name]):x}")
    for i in range((u.mrs(ICH_VTR_EL2) & 0x1f) + 1):
        print(f"{f'ICH_LR{i}_EL2':18s} = 0x{u.mrs(globals()[f'ICH_LR{i}_EL2']):x}")
    sys.stdout.flush()

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

def load_firmware(hv, image, layout):
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

    hv._write_boot_args(layout.end)

load_firmware(hv, image, layout)

# Does nothing unless M1N1_PMU_RESET=1 (proxyclient/m1n1/hw/pmu.py)
PMU(u).reset_panic_counter()

# HV.handle_exception() calls run_shell() on a user interrupt and on anything it could not
# handle. Report and leave the guest instead of waiting for input.
def stop_guest(entry_msg=None, exit_msg=None):
    dump_state("firmware stopped")
    return EXC_RET.EXIT_GUEST

hv.run_shell = stop_guest

# Count from the moment the guest is entered: HV.start() has installed its SIGINT handler by
# then, and the setup before that takes a while.
timer = threading.Timer(args.seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
timer.daemon = True
enter_guest = p.hv_start

def enter_guest_timed(*hv_start_args):
    print(f"Entering the firmware at 0x{hv_start_args[0]:x}, x0 = 0x{hv_start_args[1]:x}")
    timer.start()
    return enter_guest(*hv_start_args)

p.hv_start = enter_guest_timed

reader.start()
try:
    hv.start()
finally:
    reader.stop()

print("Hypervisor exited.")
