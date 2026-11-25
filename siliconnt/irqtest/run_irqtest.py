#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the irqtest guest (irqtest.S) under the hypervisor, on the boot CPU only.

The m1n1 on the target has to be built from this tree: the test is about src/hv/hv_vgic.c.

The guest prints to the hypervisor's virtual UART; lines that start with "HVLOG: " come back
on the proxy console, so they show up in the output of this script.

By default the script needs no input: it lets the guest run for --seconds, then interrupts
the hypervisor, prints the list registers and the timer state, leaves the guest and exits.
An unhandled guest exception ends the run the same way. With --shell it behaves like
run_guest.py instead: ^C or a guest exception drops into the hypervisor shell.
"""
import sys, os, pathlib, argparse, signal, struct, threading

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

parser = argparse.ArgumentParser(description="Run the vGIC interrupt test guest")
parser.add_argument("--shell", action="store_true",
                    help="interactive hypervisor shell on ^C or on a guest exception")
parser.add_argument("--seconds", type=int, default=20,
                    help="how long the guest may run before it is stopped (default 20)")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--check-only", action="store_true",
                    help="only check which m1n1 is running on the target, then exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?",
                    default=HERE / "build" / "irqtest.bin")
args = parser.parse_args()

MAGIC = b"SNTIRQ02"
MAGIC_OFF = 0x08
UART_BASE_OFF = 0x10
GICD_BASE_OFF = 0x18
GICR_BASE_OFF = 0x20

# Where the hypervisor puts the Distributor and the Redistributors: HV_VGIC_DIST_BASE and
# HV_VGIC_REDIST_BASE in src/hv/hv.h
GICD_BASE = 0x50_0000_0000
GICR_BASE = 0x51_0000_0000

image = bytearray(args.payload.read_bytes())
if image[MAGIC_OFF:MAGIC_OFF + len(MAGIC)] != MAGIC:
    sys.exit(f"{args.payload}: not an irqtest image")
if struct.unpack_from("<Q", image, UART_BASE_OFF)[0] != 0:
    sys.exit(f"{args.payload}: UART base already filled in")

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.shell import run_shell
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

# An m1n1 without the changes of this tree would hand IRQs to the guest. Make sure it is this
# build before going anywhere near that.
version_got = iface.readmem(u.base + version_off, len(version_want))
print(f"m1n1 expected: {version_want[len(VERSION_MARK):].decode()}")
print(f"m1n1 running:  {version_got[len(VERSION_MARK):].decode(errors='replace')!r}")
if version_got != version_want:
    sys.exit("The m1n1 on the target is not the one built from this tree: "
             "chainload build/m1n1.bin first (chainload.py -r)")
if args.check_only:
    sys.exit(0)

hv = HV(iface, p, u)
# Boot CPU only: the secondary CPUs wait in EL2 and are not shown to the guest
hv.smp = False
hv.init()

nr_lr = (u.mrs(ICH_VTR_EL2) & 0x1f) + 1

def dump_state(title):
    print(f"==== {title} ====")
    for name in ("HCR_EL2", "ISR_EL1", "ICC_SRE_EL2", "ICH_VTR_EL2", "ICH_HCR_EL2",
                 "ICH_VMCR_EL2", "ICH_MISR_EL2", "ICH_EISR_EL2", "ICH_ELRSR_EL2",
                 "ICH_AP1R0_EL2", "CNTV_CTL_EL02", "CNTV_CVAL_EL02", "CNTP_CTL_EL02",
                 "VM_TMR_FIQ_ENA_EL2", "CNTHCTL_EL2", "ID_AA64MMFR0_EL1"):
        print(f"{name:18s} = 0x{u.mrs(globals()[name]):x}")
    for i in range(nr_lr):
        print(f"{f'ICH_LR{i}_EL2':18s} = 0x{u.mrs(globals()[f'ICH_LR{i}_EL2']):x}")
    sys.stdout.flush()

dump_state("after hypervisor init, before the guest")

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

struct.pack_into("<Q", image, UART_BASE_OFF, uart_base)
struct.pack_into("<Q", image, GICD_BASE_OFF, GICD_BASE)
struct.pack_into("<Q", image, GICR_BASE_OFF, GICR_BASE)
print(f"GIC Distributor at 0x{GICD_BASE:x}, Redistributors from 0x{GICR_BASE:x}")
hv.load_raw(bytes(image), entryoffset=0)

# Does nothing unless M1N1_PMU_RESET=1 (proxyclient/m1n1/hw/pmu.py)
PMU(u).reset_panic_counter()

if not args.shell:
    # HV.handle_exception() calls run_shell() on a user interrupt and on anything it could
    # not handle. Report and leave the guest instead of waiting for input.
    def stop_guest(entry_msg=None, exit_msg=None):
        dump_state("guest stopped")
        return EXC_RET.EXIT_GUEST

    hv.run_shell = stop_guest

    # Count from the moment the guest is entered: HV.start() has installed its SIGINT
    # handler by then, and the setup before that takes a while.
    timer = threading.Timer(args.seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
    timer.daemon = True
    enter_guest = p.hv_start

    def enter_guest_timed(*hv_start_args):
        timer.start()
        return enter_guest(*hv_start_args)

    p.hv_start = enter_guest_timed

hv.start()

if args.shell:
    run_shell(hv.shell_locals, "Hypervisor exited. Entering shell.")
else:
    print("Hypervisor exited.")
