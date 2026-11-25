#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What the host knows of the CPUs the guest runs on.

The guest starts CPUs without the host: PSCI CPU_ON, src/hv/hv_psci.c. So the host does not keep
a list of its own, it asks the hypervisor (P_HV_STARTED_CPUS). This is the real HV of proxyclient
with a hypervisor that this file plays: which CPUs are in the guest is what hv_started_cpus of
src/hv/hv.c says, and going to another CPU (hv_switch_cpu()) fails for one that is not.

No hardware and no proxy. What hv_get_started_cpus() makes of hv_started_cpus is in cpus_test.c.
"""
import sys, re, types, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT / "proxyclient"))

from m1n1.hv import HV
from m1n1.proxy import M1N1Proxy
from m1n1.sysreg import DBGBVRn_EL1, MDSCR_EL1

failures = 0

def check(name, cond, detail=""):
    global failures
    if not cond:
        failures += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name}{' (' + str(detail) + ')' if detail and not cond else ''}")

# ---- the commands of the proxy have the same numbers on both sides

numbers = {}
value = None
body = re.search(r"typedef enum \{\s*(P_NOP = .*?)\n\} ProxyOp;", (ROOT / "src/proxy.h").read_text(),
                 re.S).group(1)
for entry in re.findall(r"\b(P_[A-Z0-9_]+)(?:\s*=\s*(0x[0-9a-f]+|\d+))?\s*,", body):
    value = int(entry[1], 0) if entry[1] else value + 1
    numbers[entry[0]] = value
wrong = {name: (hex(numbers[name]), hex(getattr(M1N1Proxy, name)))
         for name in numbers if name.startswith("P_HV_") and
         getattr(M1N1Proxy, name, None) not in (None, numbers[name])}
check("the proxy commands of the hypervisor have the same numbers in src/proxy.h and proxy.py",
      "P_HV_STARTED_CPUS" in numbers and not wrong, wrong)
check("P_HV_STARTED_CPUS comes after the last one there was",
      numbers.get("P_HV_STARTED_CPUS") == 0xc17 == getattr(M1N1Proxy, "P_HV_STARTED_CPUS", None) and
      sorted(numbers.values()).count(0xc17) == 1)

# ---- a machine: the CPUs of an M4, by the names its device tree has for them

NODES = [types.SimpleNamespace(cpu_id=i, reg=i if i < 6 else 0x100 | (i - 6),
                               state="running" if i == 6 else "waiting") for i in range(10)]

class Proxy:
    """The hypervisor, as far as HV asks it here"""
    def __init__(self):
        self.started = set()
        self.asked = 0
        self.switches = []
    def hv_started_cpus(self):
        self.asked += 1
        return sum(1 << cpu for cpu in self.started)
    def hv_switch_cpu(self, cpu):
        self.switches.append(cpu)
        return cpu in self.started
    def hv_exit_cpu(self, cpu=-1):
        pass

def machine():
    proxy = Proxy()
    hv = HV(None, proxy, types.SimpleNamespace(adt={"cpus": NODES}, msr=lambda *a, **kw: None))
    asked = proxy.asked
    hv.ctx = types.SimpleNamespace(cpu_id=6, regs=[0] * 31, elr=0)

    def switch_context(exit=None):
        # _switch_context(): the CPU that was asked for is the one the host talks to now
        hv.ctx = types.SimpleNamespace(cpu_id=proxy.switches[-1], regs=[0] * 31, elr=0)
        for cpu in leave_on_switch.pop(hv.ctx.cpu_id, ()):
            proxy.started.discard(cpu)
    leave_on_switch = {}
    hv._switch_context = switch_context
    hv.log = lambda *a, **kw: None
    return hv, proxy, asked, leave_on_switch

# ---- what is in the guest, whoever started it

hv, proxy, asked, _ = machine()
check("making an HV asks the hypervisor nothing", asked == 0, asked)
check("before there is a guest there is no CPU in it", hv.started_cpus == {})

proxy.started = {6}
check("the boot CPU", hv.started_cpus == {6: (0, 1, 0)}, hv.started_cpus)

# CPU_ON of the guest: the host has not been told
for cpus in ({6, 7}, {6, 7, 8, 9, 0}, set(range(10)), {6, 3}, {3}):
    proxy.started = cpus
    check(f"CPUs {sorted(cpus)} are in the guest",
          hv.started_cpus == {i: (0, 0, i) if i < 6 else (0, 1, i - 6) for i in cpus},
          hv.started_cpus)
    check(f"...and those are the ones cpus() goes to",
          [(cpu, hv.ctx.cpu_id) for cpu in hv.cpus()] == [(cpu, cpu) for cpu in sorted(cpus)])

# What gdbserver asks
check("a CPU that is not in the guest is not among them", 0 not in hv.started_cpus)

# ---- CPUs that leave the guest

hv, proxy, _, leave = machine()
proxy.started = set(range(10))
proxy.started.discard(4)
check("a CPU that has left the guest (PSCI CPU_OFF) is not gone to",
      list(hv.cpus()) == [0, 1, 2, 3, 5, 6, 7, 8, 9] and 4 not in proxy.switches)

# One that leaves while the host goes round: they all run while it goes from one to the next
hv, proxy, _, leave = machine()
proxy.started = {1, 2, 6, 7}
leave[1] = (2, 7)
try:
    seen = list(hv.cpus())
except Exception as e:
    seen = repr(e)
check("one that leaves the guest while cpus() goes round is left out", seen == [1, 6], seen)

# A breakpoint goes to every CPU there is, and the host ends up where it was
hv, proxy, _, leave = machine()
proxy.started = {6, 7, 0}
set_on = []
hv.u.msr = lambda reg, val, **kw: set_on.append(hv.ctx.cpu_id) if reg == DBGBVRn_EL1(0) else None
hv.add_hw_bp(0xfffff80000001000)
check("a breakpoint is set on every CPU in the guest", set_on == [0, 6, 7] and hv.ctx.cpu_id == 6,
      (set_on, hv.ctx.cpu_id))

# ---- the registers the host keeps for a CPU

hv, proxy, _, _ = machine()
check("a CPU the host has not heard of has registers to keep, and they are its own",
      hv.sysreg[9].setdefault(MDSCR_EL1, 0) == 0 and not hv.sysreg[8] and
      hv.sysreg[9] == {MDSCR_EL1: 0})

if failures:
    sys.exit(f"{failures} FAILED")
print("ALL OK")
