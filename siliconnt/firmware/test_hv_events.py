#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the hypervisor and its host agree about the events the hypervisor sends.

An event is a number: hv_entry_type in src/hv/hv.h, HV_EVENT in proxyclient/m1n1/hv/types.py.
The host looks up a handler by that number (UartInterface.handle_boot()), and HV.init() is
where the handlers are registered. If the hypervisor sends an event the host has no handler
for, the host prints a line and never answers, and the core that sent it waits in the proxy
for good. So: the two lists are the same, and HV.init() registers a handler for every entry.

No hardware and no proxy: this reads the header and the source of HV.init().
"""
import sys, re, inspect, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT / "proxyclient"))

from m1n1.hv import HV
from m1n1.hv.types import HV_EVENT

failures = 0

def check(name, cond, detail=""):
    global failures
    if not cond:
        failures += 1
    print(f"{'ok  ' if cond else 'FAIL'} {name}{' (' + detail + ')' if detail and not cond else ''}")

# typedef enum _hv_entry_type { HV_HOOK_VM = 1, HV_VTIMER, ... } hv_entry_type;
header = (ROOT / "src" / "hv" / "hv.h").read_text()
body = re.search(r"typedef enum _hv_entry_type \{(.*?)\} hv_entry_type;", header, re.S).group(1)
in_c = {}
value = 0
for entry in re.sub(r"//[^\n]*", "", body).split(","):
    entry = entry.strip()
    if not entry:
        continue
    name, _, explicit = (part.strip() for part in entry.partition("="))
    value = int(explicit, 0) if explicit else value + 1
    in_c[name.removeprefix("HV_")] = value

in_python = {event.name: event.value for event in HV_EVENT}
check("hv_entry_type and HV_EVENT list the same events with the same numbers",
      in_c == in_python, f"C {in_c}, Python {in_python}")

registered = set(re.findall(r"set_handler\(START\.HV,\s*HV_EVENT\.(\w+)", inspect.getsource(HV.init)))
for name in in_c:
    check(f"HV.init() registers a handler for {name}", name in registered)

if failures:
    sys.exit(f"{failures} FAILED")
print("ALL OK")
