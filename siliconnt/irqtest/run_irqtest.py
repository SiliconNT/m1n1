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
parser.add_argument("payload", type=pathlib.Path, nargs="?",
                    default=HERE / "build" / "irqtest.bin")
args = parser.parse_args()

MAGIC = b"SNTIRQ01"
MAGIC_OFF = 0x08
UART_BASE_OFF = 0x10

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

iface = UartInterface()
p = M1N1Proxy(iface, debug=False)
bootstrap_port(iface, p)

u = ProxyUtils(p, heap_size=128 * 1024 * 1024,
               m1n1_heap=128 * 1024 * 1024)

hv = HV(iface, p, u)
# Boot CPU only: the secondary CPUs are neither started nor shown to the guest
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
