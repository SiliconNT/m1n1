#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Leaves a guest whose host script has died while the guest was running

Does what run_sgitest.py does when its time is up, and nothing else: a byte to the serial port,
which has the hypervisor stop the guest and report to the host, and the answer "leave the guest".
After that m1n1 is back at its proxy and answers the P_HV_START that the dead script sent.
"""
import pathlib, sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[2] / "proxyclient"))

from m1n1.proxy import *

USER_INTERRUPT = 3 # HV_EVENT, m1n1/hv/types.py

iface = UartInterface()
p = M1N1Proxy(iface)
iface.dev.timeout = 15
left = []

def stopped(reason, code, info):
    print(f"Stopped: {reason.name}/{code}")
    p.exit(EXC_RET.EXIT_GUEST)
    left.append(reason)

iface.set_handler(START.HV, USER_INTERRUPT, stopped)
iface.dev.write(b"!")
iface.reply(iface.REQ_PROXY)
if not left:
    sys.exit("No guest has been stopped")
iface.nop()
print(f"The proxy answers: m1n1 base 0x{p.get_base():x}")
