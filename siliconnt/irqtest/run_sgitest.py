#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the sgitest guest (sgitest.c) under the hypervisor, or, given build/gictest.bin,
build/wfitest.bin, build/pscitest.bin or build/pmutest.bin as the payload, the gictest guest
(gictest.c), the wfitest guest (wfitest.c), the pscitest guest (pscitest.c) or the pmutest
guest (pmutest.c): the five share their entry code and image header.

The m1n1 on the target has to be built from this tree: the tests are about src/hv/hv_vgic.c,
src/hv/hv_wfi.c, src/hv/hv_psci.c, src/hv/hv_pmu.c and src/hv/hv_xtrack.c.

pmutest needs --xtrack: no memory of the guest is executable then until the hypervisor has
scanned it and replaced the accesses to PMUv3 registers, which the M4 does not have, whether
the code runs at EL1 or at EL0. With --guest-arg 0x100 pmutest also insists that its accesses
were replaced: that the cycle counter it reads is the count of the generic timer. It ends
with SYSTEM_OFF, as pscitest does.

build/xitest.bin (xitest.c) is a sixth guest that this script loads, also with --xtrack: it
executes every load and store that src/hv/hv_xinsn.c takes apart, where the hypervisor has
no part in it and where the hypervisor carries it out, and compares. It ends with SYSTEM_OFF.

build/aictest.bin (aictest.c) is a seventh, for how the hypervisor forwards an AIC line as
an SPI (src/hv/hv_vgic.c). It needs --aic-line N: the line it raises and lowers with the
software trigger of AIC, as die * max_irq + line, which has to be one of the table of AIC
lines and SPIs that m1n1 makes from its device tree (hv_aicmap(), src/hv/hv_layout.c), and
one that no device of the machine raises meanwhile. The script asks m1n1 for the table
(P_HV_AICMAP), looks the line up in it and tells the guest which SPI that is, and where in
AIC the software trigger of the line is set and cleared, worked out from the device tree and
the registers of AIC the way aic_init() of src/aic.c does. aictest ends with SYSTEM_OFF.

pscitest takes an argument, --guest-arg: 0x100 makes it expect exactly the answers of the
hypervisor's PSCI, and the low four bits choose the call it ends with (0 SYSTEM_OFF,
1 SYSTEM_RESET, 2 hvc #1, 3 CPU_ON, 4 CPU_OFF). None of those calls comes back: the
hypervisor stops the guest and says why, and this script leaves the guest right then.

build/cputest.bin (cputest.c) is an eighth: what a guest reads in the identification registers
of every core, and what a core that has just started sees of what the boot core wrote before
(SiliconNT R50). build/aicroute.bin (aicroute.c) is a ninth, about which core AIC reports a
line to. It takes --aic-line as aictest does, and --guest-arg: bit 0 the cores spin while they
wait, bit 1 they are in WFI, bits 31:8 the rounds for each pair of cores.

Without --secondary the guest runs on the boot CPU only and no other CPU is touched. Each
--secondary N adds the CPU with that m1n1 index (the cpu-id of its ADT node), --secondary all
every CPU there is: the script
starts it, one at a time and only after putting m1n1's idle loop into WFE mode, so that it
never waits in WFI (M4 loses CPU state there, src/kboot.c). The guest gets onto it the way
upstream gets an Apple guest onto its secondary CPUs: it writes the CPU start register of the
power manager, the hypervisor intercepts that, and this script answers with
hv_start_secondary(). It does not use the CPU_ON of the hypervisor's PSCI.

--park N (or all) starts a CPU in the same way and leaves it in m1n1's idle loop, where it never
enters the guest. With --aic-line, and --parked-trials, the script raises that line itself
once the guest has been stopped, and looks at which core gets the IRQ (aic_parked.py).

The guest sets up the GIC itself, the way the Windows kernel does: the script tells it where
the hypervisor emulates the Distributor and the Redistributors.

The guest prints to the hypervisor's virtual UART; lines that start with "HVLOG: " come back
on the proxy console, so they show up in the output of this script.

The script needs no input: it lets the guest run for --seconds, then interrupts the
hypervisor, prints the list registers of the boot CPU, leaves the guest and exits. An
unhandled guest exception ends the run the same way, and so does a guest that the hypervisor
has stopped for good (HV_GUEST_RESET, HV_GUEST_OFF, HV_GUEST_HVC in src/hv/hv.h).
"""
import sys, os, pathlib, argparse, signal, struct, threading

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

parser = argparse.ArgumentParser(description="Run the vGIC SGI test guest")
parser.add_argument("--secondary", action="append", default=[], metavar="N",
                    help="also run the guest on the CPU with this m1n1 index (repeatable), or "
                         "on all of them")
parser.add_argument("--park", action="append", default=[], metavar="N",
                    help="start the CPU with this m1n1 index, or all that are not given with "
                         "--secondary, and leave it in m1n1's idle loop (repeatable). It must not "
                         "have been in the hypervisor before in this boot")
parser.add_argument("--parked-trials", type=int, default=0, metavar="N",
                    help="with --park and --aic-line: raise the line this often after the guest "
                         "has been stopped, and look at which core gets the IRQ")
parser.add_argument("--seconds", type=int, default=60,
                    help="how long the guest may run before it is stopped (default 60)")
parser.add_argument("--stop-after-done", type=float, metavar="S",
                    help="stop the guest this long after it has printed \"DONE pass fail = \", "
                         "if that is before --seconds are over")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--guest-arg", type=lambda s: int(s, 0), default=0, metavar="N",
                    help="what goes into the argument field of the image header (default 0)")
parser.add_argument("--aic-line", type=lambda s: int(s, 0), metavar="N",
                    help="for aictest: the AIC line it raises and lowers, as die * max_irq + "
                         "line; one of the table of AIC lines and SPIs")
parser.add_argument("--xtrack", action="store_true",
                    help="track execution: scan the memory of the guest before it runs "
                         "(for pmutest)")
parser.add_argument("--check-only", action="store_true",
                    help="only check which m1n1 is running and, for each --secondary, where "
                         "that CPU would start; then exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?",
                    default=HERE / "build" / "sgitest.bin")
args = parser.parse_args()

# Layout of the image header, see sgitest-head.S
MAGIC = b"SNTSGI03"
MAGIC_OFF = 0x08
UART_BASE_OFF = 0x10
NR_CPUS_OFF = 0x18
SECONDARY_OFF = 0x20
START_REG_OFF = 0x28
MAX_CPUS = 16
START_VAL_OFF = START_REG_OFF + 8 * MAX_CPUS
GICD_BASE_OFF = START_VAL_OFF + 8 * MAX_CPUS
GICR_BASE_OFF = GICD_BASE_OFF + 8
ARG_OFF = GICR_BASE_OFF + 8
AIC_SET_OFF = ARG_OFF + 8
AIC_CLR_OFF = AIC_SET_OFF + 8
AIC_BIT_OFF = AIC_CLR_OFF + 8
AIC_SPI_OFF = AIC_BIT_OFF + 8

# Where the hypervisor puts the Distributor and the Redistributors: HV_VGIC_DIST_BASE and
# HV_VGIC_REDIST_BASE in src/hv/hv.h
GICD_BASE = 0x50_0000_0000
GICR_BASE = 0x51_0000_0000

image = bytearray(args.payload.read_bytes())
if image[MAGIC_OFF:MAGIC_OFF + len(MAGIC)] != MAGIC:
    sys.exit(f"{args.payload}: not an sgitest, gictest, wfitest, pscitest, pmutest, xitest, "
             "aictest, cputest or aicroute image of this tree")
if struct.unpack_from("<Q", image, UART_BASE_OFF)[0] != 0:
    sys.exit(f"{args.payload}: UART base already filled in")
secondary_entry_off = struct.unpack_from("<Q", image, SECONDARY_OFF)[0]

if args.parked_trials and (not args.park or args.aic_line is None):
    sys.exit("--parked-trials needs --park and --aic-line")

def pick_cpus(option, names, candidates):
    """The CPUs an option names, out of the candidates (m1n1 indices, in their order)"""
    if names == ["all"]:
        return list(candidates)
    try:
        picked = [int(n) for n in names]
    except ValueError:
        sys.exit(f"{option}: a number, or all and nothing else")
    if len(set(picked)) != len(picked):
        sys.exit(f"{option}: the same CPU twice")
    for index in picked:
        if index not in candidates:
            sys.exit(f"{option} {index}: no such secondary CPU")
    return picked

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

others = [index for index in sorted(cpu_nodes) if index != boot_index]
args.secondary = pick_cpus("--secondary", args.secondary, others)
args.park = pick_cpus("--park", args.park, [i for i in others if i not in args.secondary])
if len(args.secondary) > MAX_CPUS - 1:
    sys.exit(f"--secondary: the guest has room for {MAX_CPUS} CPUs")

# A secondary CPU starts executing at its RVBAR. Where that is locked (M4), it has to be the
# start of the m1n1 that is running now, which is the case after chainloading the raw image
# onto the base of the one iBoot loaded. Only the registers of the CPUs asked for are read;
# reading one changes nothing, m1n1 does the same before it starts a CPU (src/smp.c).
RVBAR_ADDR = 0xfffffffff000
for index in args.secondary + args.park:
    rvbar = p.read64(cpu_nodes[index].cpu_impl_reg[0])
    print(f"CPU {index}: RVBAR register 0x{rvbar:x}, m1n1 base 0x{u.base:x}")
    if not u.cpu_features.apple_sysregs_unlocked and (rvbar & RVBAR_ADDR) != u.base:
        sys.exit(f"CPU {index}: its RVBAR is locked to 0x{rvbar & RVBAR_ADDR:x}, which is "
                 "not where this m1n1 starts. It would not come up in this image.")

if args.check_only:
    sys.exit(0)

# slot in the guest -> m1n1 CPU index; slot 0 is the boot CPU
slots = [boot_index] + args.secondary

def heap_top():
    """Asking for 0 bytes returns the top of m1n1's heap (src/heapblock.c)"""
    return p.heapblock_alloc(0)

print(f"heap top before the hypervisor: 0x{heap_top():x}")

if args.secondary or args.park:
    # Before any secondary CPU exists: its idle loop is to use WFE, never WFI. A CPU that is
    # running m1n1 already (an earlier run in this boot) is left as it is
    print("m1n1 idle loop: WFE mode")
    p.smp_set_wfe_mode(True)
    for index in args.secondary + args.park:
        print(f"Starting CPU {index}...")
        alive = p.smp_start_secondary(index)
        print(f"CPU {index}: {'running m1n1' if alive else 'did NOT start'}")
        if not alive:
            sys.exit("Not going on. Any CPU that did start stays in m1n1's idle loop.")

hv = HV(iface, p, u)
# The script has started the secondary CPUs it wants; hv_init() has the others wait in EL2. The
# guest's device tree is to have the boot CPU only.
hv.smp = False
hv.init()
print(f"heap top after hv_init(): 0x{heap_top():x}")

def dump_state(title):
    # The registers of the CPU the proxy is running on: the boot CPU before the guest runs,
    # afterwards whichever CPU brought the guest to a stop
    print(f"==== {title} ====")
    for name in ("HCR_EL2", "ISR_EL1", "ICC_SRE_EL2", "ICH_VTR_EL2", "ICH_HCR_EL2",
                 "ICH_VMCR_EL2", "ICH_MISR_EL2", "ICH_EISR_EL2", "ICH_ELRSR_EL2",
                 "ICH_AP1R0_EL2", "CNTV_CTL_EL02", "CNTP_CTL_EL02", "VM_TMR_FIQ_ENA_EL2",
                 "CNTHCTL_EL2"):
        print(f"{name:18s} = 0x{u.mrs(globals()[name]):x}")
    for i in range((u.mrs(ICH_VTR_EL2) & 0x1f) + 1):
        print(f"{f'ICH_LR{i}_EL2':18s} = 0x{u.mrs(globals()[f'ICH_LR{i}_EL2']):x}")
    sys.stdout.flush()

dump_state(f"after hypervisor init, before the guest, on CPU {boot_index}")

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

def aic_line(line):
    """For aictest: the SPI the table of m1n1 has for this AIC line, and where its software
    trigger is set and cleared, and its bit there. AIC as aic_init() of src/aic.c reads it:
    on AIC2 and AIC3 every die has the configuration of each line, from extint-baseaddress
    of the device tree or else, on AIC2, from 0x2000; then the bitmaps of the software
    trigger (set, clear), of the mask (set, clear) and the state of the lines. Set goes by the
    die's stride for masking, clear by the one for unmasking, as aic_set_sw(), aic_mask() and
    aic_unmask() have it. Where the line is masked and unmasked is printed too. Also
    returns where AIC's global configuration is (aicglbcfg-offset of the device tree, else
    0x14 on AIC2 and AIC3, as aic_init() has it; None on AIC1), which the hypervisor enables
    while the guest runs (hv_vgic_start())."""
    buf = u.malloc(0x4000)
    size = p.hv_aicmap(buf)
    if size < 8:
        sys.exit(f"m1n1 wrote no table of AIC lines and SPIs (P_HV_AICMAP returned {size}): it "
                 "has said why above")
    table = iface.readmem(buf, size)
    version, count = struct.unpack_from("<II", table)
    lines = struct.unpack_from(f"<{count}I", table, 8)
    print(f"AIC to GIC table: version {version}, {count} lines, AIC {lines[0] if lines else '-'} "
          f".. {lines[-1] if lines else '-'}")
    if line not in lines:
        sys.exit(f"--aic-line {line}: not a line of the table")
    spi = 32 + lines.index(line)

    node = u.adt["/arm-io/aic"]
    base = node.get_reg(0)[0]
    def prop32(name, default=None):
        value = node._properties.get(name, default)
        if isinstance(value, bytes):
            value = struct.unpack_from("<I", value)[0]
        return value
    if "aic,1" in node.compatible:
        max_irq, set_stride, clr_stride, config_addr = 0x400, 0, 0, None
        sw_set, sw_clr, mask_set, mask_clr = 0x4000, 0x4080, 0x4100, 0x4180
    else:
        if "aic,2" in node.compatible:
            config, maxnumirq = prop32("extint-baseaddress", 0x2000), 0xc
        else:
            config, maxnumirq = prop32("extint-baseaddress"), prop32("maxnumirq-offset")
            if config is None or maxnumirq is None:
                sys.exit("/arm-io/aic: an AIC3 without extint-baseaddress or maxnumirq-offset")
        max_irq = p.read32(base + maxnumirq) & 0xffff
        words = 4 * (max_irq // 32)
        sw_set = config + 4 * max_irq
        sw_clr = sw_set + words
        mask_set = sw_clr + words
        mask_clr = mask_set + words
        cfg_addr = base + config + 4 * (line % max_irq) + \
            (line // max_irq) * prop32("extintrcfg-stride", 4 * max_irq + 5 * words)
        value = p.read32(cfg_addr)
        print(f"IRQ_CFG of AIC line {line}: 0x{value:08x} at 0x{cfg_addr:x} (target {value & 0xf})")
        die_size = 4 * max_irq + 5 * words
        set_stride = prop32("intmaskset-stride", die_size)
        clr_stride = prop32("intmaskclear-stride", die_size)
        config_addr = base + prop32("aicglbcfg-offset", 0x14)
    die, num = line // max_irq, line % max_irq
    word = 4 * (num // 32)
    set_addr = base + sw_set + die * set_stride + word
    clr_addr = base + sw_clr + die * clr_stride + word
    print(f"AIC line {line} (die {die}, line {num}) is SPI {spi}; its software trigger is set "
          f"at 0x{set_addr:x} and cleared at 0x{clr_addr:x}, bit {num % 32}")
    print(f"  it is masked at 0x{base + mask_set + die * set_stride + word:x} and unmasked at "
          f"0x{base + mask_clr + die * clr_stride + word:x}, the same bit")
    aic_regs.update(
        sw_set=set_addr, sw_clr=clr_addr, bit=1 << (num % 32),
        mask_set=base + mask_set + die * set_stride + word,
        mask_clr=base + mask_clr + die * clr_stride + word,
        event=None if config_addr is None else base + prop32("aic-iack-offset"))
    return spi, set_addr, clr_addr, 1 << (num % 32), config_addr

aic_config_addr = None
aic_regs = {}

def print_aic_config(when):
    """AIC's global configuration, read only; bit 0 enables AIC"""
    if aic_config_addr is not None:
        value = p.read32(aic_config_addr)
        print(f"AIC global configuration {when}: 0x{value:08x} at 0x{aic_config_addr:x} "
              f"(enable bit {value & 1}, prefer-pcpu bit {(value >> 28) & 1})")

if args.aic_line is not None:
    spi, set_addr, clr_addr, bit, aic_config_addr = aic_line(args.aic_line)
    struct.pack_into("<QQQQ", image, AIC_SET_OFF, set_addr, clr_addr, bit, spi)

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
struct.pack_into("<Q", image, ARG_OFF, args.guest_arg)
print(f"GIC Distributor at 0x{GICD_BASE:x}, Redistributors from 0x{GICR_BASE:x}")
print(f"Guest argument: 0x{args.guest_arg:x}")
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

if args.xtrack:
    # After the memory of the guest has been mapped and filled: this changes the mappings
    # that are there
    if p.hv_xtrack() != 0:
        sys.exit("The hypervisor refused to track execution")
    print("Execution tracking: on")

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
    p.hv_start_secondary(index, entry, slot)

hv.start_secondary = start_secondary

# Does nothing unless M1N1_PMU_RESET=1 (proxyclient/m1n1/hw/pmu.py)
PMU(u).reset_panic_counter()

# HV.handle_exception() calls run_shell() on a user interrupt and on anything it could not
# handle. Report and leave the guest instead of waiting for input.
def stop_guest(entry_msg=None, exit_msg=None):
    cpu = hv.ctx.cpu_id if hv.ctx is not None else "?"
    # START.HV with USER_INTERRUPT: the time ran out. With GUEST_RESET, GUEST_OFF or
    # GUEST_HVC: the hypervisor has stopped the guest for good (src/hv/hv_psci.c)
    print(f"Guest stopped: {hv.exc_reason.name}/{getattr(hv.exc_code, 'name', hv.exc_code)}")
    dump_state(f"guest stopped, on CPU {cpu}")
    if args.parked_trials and cpu == boot_index and aic_regs.get("event"):
        # hv.start() has the serial port wait forever; a core that does not answer must not
        timeout, iface.dev.timeout = iface.dev.timeout, 10
        try:
            import aic_parked
            blob = (HERE / "build" / "el2probe.bin").read_bytes()
            print_aic_config("with the guest stopped")
            aic_parked.probe(
                aic_parked.Parked(p, u, iface, boot_index, args.park, aic_regs, blob,
                                  lambda: u.mrs(ISR_EL1)), args.parked_trials)
        except Exception as e:
            print(f"The parked CPUs could not be looked at: {type(e).__name__}: {e}")
        finally:
            iface.dev.timeout = timeout
    return EXC_RET.EXIT_GUEST

hv.run_shell = stop_guest

# Count from the moment the guest is entered: HV.start() has installed its SIGINT handler by
# then, and the setup before that takes a while.
def interrupt_after(seconds):
    t = threading.Timer(seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
    t.daemon = True
    return t

timer = interrupt_after(args.seconds)

class DoneWatch:
    """Gets what m1n1 prints, in place of a log file; the last line of a guest ends the run"""
    def __init__(self):
        self.line = ""

    def write(self, text):
        self.line += text
        if not self.line.endswith("\n"):
            return
        if "DONE pass fail = " in self.line:
            timer.cancel()
            interrupt_after(args.stop_after_done).start()
            iface.tty_log = None
        self.line = ""

    def flush(self):
        pass

if args.stop_after_done is not None:
    iface.tty_log = DoneWatch()
enter_guest = p.hv_start

def enter_guest_timed(*hv_start_args):
    timer.start()
    return enter_guest(*hv_start_args)

p.hv_start = enter_guest_timed

print_aic_config("before the guest")
hv.start()
print_aic_config("after the guest")

print("Hypervisor exited.")
if args.secondary:
    print(f"CPUs {args.secondary} are back in m1n1's idle loop (WFE).")
print(f"heap top after the hypervisor: 0x{heap_top():x}")
