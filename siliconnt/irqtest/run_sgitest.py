#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the sgitest guest (sgitest.c) under the hypervisor, or, given build/gictest.bin as the
payload, the gictest guest (gictest.c): the two share their entry code and image header.

The m1n1 on the target has to be built from this tree: the test is about src/hv/hv_vgic.c.

Without --secondary the guest runs on the boot CPU only and no other CPU is touched. Each
--secondary N adds the CPU with that m1n1 index (the cpu-id of its ADT node): the script
starts it, one at a time and only after putting m1n1's idle loop into WFE mode, so that it
never waits in WFI (M4 loses CPU state there, src/kboot.c). The guest gets onto it the way
upstream gets an Apple guest onto its secondary CPUs: it writes the CPU start register of the
power manager, the hypervisor intercepts that, and this script answers with
hv_start_secondary(). There is no PSCI in the hypervisor yet.

The guest sets up the GIC itself, the way the Windows kernel does: the script tells it where
the hypervisor emulates the Distributor and the Redistributors.

The guest prints to the hypervisor's virtual UART; lines that start with "HVLOG: " come back
on the proxy console, so they show up in the output of this script.

The script needs no input: it lets the guest run for --seconds, then interrupts the
hypervisor, prints the list registers of the boot CPU, leaves the guest and exits. An
unhandled guest exception ends the run the same way.
"""
import sys, os, pathlib, argparse, signal, struct, threading

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

parser = argparse.ArgumentParser(description="Run the vGIC SGI test guest")
parser.add_argument("--secondary", type=int, action="append", default=[], metavar="N",
                    help="also run the guest on the CPU with this m1n1 index (repeatable)")
parser.add_argument("--seconds", type=int, default=60,
                    help="how long the guest may run before it is stopped (default 60)")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--check-only", action="store_true",
                    help="only check which m1n1 is running and, for each --secondary, where "
                         "that CPU would start; then exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?",
                    default=HERE / "build" / "sgitest.bin")
args = parser.parse_args()

# Layout of the image header, see sgitest-head.S
MAGIC = b"SNTSGI02"
MAGIC_OFF = 0x08
UART_BASE_OFF = 0x10
NR_CPUS_OFF = 0x18
SECONDARY_OFF = 0x20
START_REG_OFF = 0x28
MAX_CPUS = 4
START_VAL_OFF = START_REG_OFF + 8 * MAX_CPUS
GICD_BASE_OFF = START_VAL_OFF + 8 * MAX_CPUS
GICR_BASE_OFF = GICD_BASE_OFF + 8

# Where the hypervisor puts the Distributor and the Redistributors: HV_VGIC_DIST_BASE and
# HV_VGIC_REDIST_BASE in src/hv/hv.h
GICD_BASE = 0x50_0000_0000
GICR_BASE = 0x51_0000_0000

image = bytearray(args.payload.read_bytes())
if image[MAGIC_OFF:MAGIC_OFF + len(MAGIC)] != MAGIC:
    sys.exit(f"{args.payload}: not an sgitest or gictest image of this tree")
if struct.unpack_from("<Q", image, UART_BASE_OFF)[0] != 0:
    sys.exit(f"{args.payload}: UART base already filled in")
secondary_entry_off = struct.unpack_from("<Q", image, SECONDARY_OFF)[0]

if len(set(args.secondary)) != len(args.secondary):
    sys.exit("--secondary: the same CPU twice")
if len(args.secondary) > MAX_CPUS - 1:
    sys.exit(f"--secondary: the guest has room for {MAX_CPUS} CPUs")

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.sysreg import *
from m1n1.hv import HV
from m1n1.hw.pmu import PMU

# Where the version string sits in the image, and what it says in this build. The raw image
# is loaded as it is, so an offset into the file is an offset from the base in memory. (An
# m1n1 chainloaded from m1n1.macho has another layout and fails this check; on a machine
# with locked RVBARs it must not be used anyway.)
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

# An m1n1 without the changes of this tree would start every secondary CPU in hv_init(), hand
# IRQs to the guest and not know P_SMP_START_SECONDARY. Make sure it is this build first.
version_got = iface.readmem(u.base + version_off, len(version_want))
print(f"m1n1 expected: {version_want[len(VERSION_MARK):].decode()}")
print(f"m1n1 running:  {version_got[len(VERSION_MARK):].decode(errors='replace')!r}")
if version_got != version_want:
    sys.exit("The m1n1 on the target is not the one built from this tree: "
             "chainload build/m1n1.bin first (chainload.py -r)")

# The CPUs as iBoot described them, from the copy of the ADT in memory
cpu_nodes = {}
boot_index = None
print("CPUs: index die:cluster:core type state")
for node in u.adt["cpus"]:
    cpu_nodes[node.cpu_id] = node
    if node.state == "running":
        boot_index = node.cpu_id
    print(f"  {node.cpu_id:2d}  {node.reg >> 11}:{(node.reg >> 8) & 7}:{node.reg & 0xff}  "
          f"{getattr(node, 'cluster_type', '?')}  {node.state}")
print(f"Boot CPU: {boot_index}")

# A secondary CPU starts executing at its RVBAR. Where that is locked (M4), it has to be the
# start of the m1n1 that is running now, which is the case after chainloading the raw image
# onto the base of the one iBoot loaded. Only the registers of the CPUs asked for are read;
# reading one changes nothing, m1n1 does the same before it starts a CPU (src/smp.c).
RVBAR_ADDR = 0xfffffffff000
for index in args.secondary:
    if index not in cpu_nodes or index == boot_index:
        sys.exit(f"--secondary {index}: no such secondary CPU")
    rvbar = p.read64(cpu_nodes[index].cpu_impl_reg[0])
    print(f"CPU {index}: RVBAR register 0x{rvbar:x}, m1n1 base 0x{u.base:x}")
    if not u.cpu_features.apple_sysregs_unlocked and (rvbar & RVBAR_ADDR) != u.base:
        sys.exit(f"--secondary {index}: its RVBAR is locked to 0x{rvbar & RVBAR_ADDR:x}, which is "
                 "not where this m1n1 starts. It would not come up in this image.")

if args.check_only:
    sys.exit(0)

# slot in the guest -> m1n1 CPU index; slot 0 is the boot CPU
slots = [boot_index] + args.secondary

if args.secondary:
    # Before any secondary CPU exists: its idle loop is to use WFE, never WFI
    print("m1n1 idle loop: WFE mode")
    p.smp_set_wfe_mode(True)
    for index in args.secondary:
        print(f"Starting CPU {index}...")
        alive = p.smp_start_secondary(index)
        print(f"CPU {index}: {'running m1n1' if alive else 'did NOT start'}")
        if not alive:
            sys.exit("Not going on. Any CPU that did start stays in m1n1's idle loop.")

hv = HV(iface, p, u)
# The script has started the secondary CPUs it wants; HV.init() is to leave the rest alone
hv.smp = False
hv.init()

def dump_state(title):
    # The registers of the CPU the proxy is running on: the boot CPU before the guest runs,
    # afterwards whichever CPU brought the guest to a stop
    print(f"==== {title} ====")
    for name in ("HCR_EL2", "ISR_EL1", "ICC_SRE_EL2", "ICH_VTR_EL2", "ICH_HCR_EL2",
                 "ICH_VMCR_EL2", "ICH_MISR_EL2", "ICH_EISR_EL2", "ICH_ELRSR_EL2",
                 "ICH_AP1R0_EL2", "CNTV_CTL_EL02", "CNTP_CTL_EL02", "VM_TMR_FIQ_ENA_EL2"):
        print(f"{name:18s} = 0x{u.mrs(globals()[name]):x}")
    for i in range((u.mrs(ICH_VTR_EL2) & 0x1f) + 1):
        print(f"{f'ICH_LR{i}_EL2':18s} = 0x{u.mrs(globals()[f'ICH_LR{i}_EL2']):x}")
    sys.stdout.flush()

dump_state(f"after hypervisor init, before the guest, on CPU {boot_index}")

hcr = HCR(u.mrs(HCR_EL2))
print(f"HCR_EL2: {hcr!s}")
if not (hcr.IMO and hcr.FMO and hcr.VM):
    sys.exit("HCR_EL2 is not IMO=1 FMO=1 VM=1: this m1n1 was not built from this tree")
if not u.mrs(ICH_HCR_EL2) & 1:
    sys.exit("ICH_HCR_EL2.En is 0: this m1n1 was not built from this tree")
if u.mrs(ICH_MISR_EL2):
    sys.exit("ICH_MISR_EL2 is not 0 before the guest has run")

uart_base = hv.adt["/arm-io/uart0"].get_reg(0)[0]
print(f"Virtual UART at 0x{uart_base:x}")

# The register a guest writes to start a CPU, as HV.map_essential() hooks it
CPU_START_OFF = {0x8103: 0x54000, 0x6000: 0x54000, 0x6001: 0x54000, 0x6002: 0x54000,
                 0x8112: 0x34000, 0x8122: 0x34000, 0x8132: 0x34000, 0x8140: 0x34000,
                 0x8142: 0x34000, 0x6030: 0x34000}
pmgr_base = u.adt["/arm-io/pmgr"].get_reg(0)[0]
chip_id = u.adt["/chosen"].chip_id

struct.pack_into("<Q", image, UART_BASE_OFF, uart_base)
struct.pack_into("<Q", image, NR_CPUS_OFF, len(slots))
struct.pack_into("<Q", image, GICD_BASE_OFF, GICD_BASE)
struct.pack_into("<Q", image, GICR_BASE_OFF, GICR_BASE)
print(f"GIC Distributor at 0x{GICD_BASE:x}, Redistributors from 0x{GICR_BASE:x}")
for slot, index in enumerate(slots):
    if slot == 0:
        continue
    if chip_id not in CPU_START_OFF:
        sys.exit(f"CPU start register unknown for chip 0x{chip_id:x}")
    reg = cpu_nodes[index].reg
    die, cluster, core = reg >> 11, (reg >> 8) & 7, reg & 0xff
    start_reg = pmgr_base + CPU_START_OFF[chip_id] + die * 0x20_0000_0000 + 8 + 4 * cluster
    struct.pack_into("<Q", image, START_REG_OFF + 8 * slot, start_reg)
    struct.pack_into("<Q", image, START_VAL_OFF + 8 * slot, 1 << core)
    print(f"Guest slot {slot} = CPU {index} ({die}:{cluster}:{core}), started by writing "
          f"0x{1 << core:x} to 0x{start_reg:x}")

hv.load_raw(bytes(image), entryoffset=0)

def start_secondary(die, cluster, core):
    """The guest wrote the CPU start register. Stands in for HV.start_secondary(), which
    looks the CPU up in the guest's ADT and enters the image where the boot CPU does."""
    for slot, index in enumerate(slots):
        if slot and cpu_nodes[index].reg == ((die << 11) | (cluster << 8) | core):
            break
    else:
        hv.log(f"Guest asked for CPU {die}:{cluster}:{core}, which it was not given")
        return
    entry = hv.guest_base + secondary_entry_off
    hv.log(f"Entering the guest on CPU {index} (slot {slot}) at 0x{entry:x}")
    hv.sysreg[index] = {}
    hv.started_cpus[index] = (die, cluster, core)
    p.hv_start_secondary(index, entry, slot)

hv.start_secondary = start_secondary

# Does nothing unless M1N1_PMU_RESET=1 (proxyclient/m1n1/hw/pmu.py)
PMU(u).reset_panic_counter()

# HV.handle_exception() calls run_shell() on a user interrupt and on anything it could not
# handle. Report and leave the guest instead of waiting for input.
def stop_guest(entry_msg=None, exit_msg=None):
    cpu = hv.ctx.cpu_id if hv.ctx is not None else "?"
    dump_state(f"guest stopped, on CPU {cpu}")
    return EXC_RET.EXIT_GUEST

hv.run_shell = stop_guest

# Count from the moment the guest is entered: HV.start() has installed its SIGINT handler by
# then, and the setup before that takes a while.
timer = threading.Timer(args.seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
timer.daemon = True
enter_guest = p.hv_start

def enter_guest_timed(*hv_start_args):
    timer.start()
    return enter_guest(*hv_start_args)

p.hv_start = enter_guest_timed

hv.start()

print("Hypervisor exited.")
if args.secondary:
    print(f"CPUs {args.secondary} are back in m1n1's idle loop (WFE).")
