#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# What a run of nohost-qemu.c on QEMU is to show (make qemu-nohost).
#
#   check_nohost_qemu.sh <run> <log> <exit status of timeout>
#
# <run> is host-<n> or nohost-<n>, n the STOP_ARG of stoptest.S: 0 hvc #1, 1 SYSTEM_OFF,
# 2 SYSTEM_RESET, 3 CPU_ON of a CPU that is not there. With a host the host is told, with the
# event of src/hv/hv.h (HV_GUEST_HVC 10, HV_GUEST_OFF 9, HV_GUEST_RESET 8), and nothing is
# shown on the screen. Without one the devices of nohost-qemu.c do not answer: what is tried is
# tried and given up on, the screen says why and has the guest's registers, and the core stays
# in EL2, so that QEMU has to be stopped (timeout says 124).
set -u
run=$1
log=$2
status=$3

fail() {
    echo "qemu-nohost: $run: $*"
    exit 1
}
has() {
    grep -q -E -- "$1" "$log" || fail "no line like: $1"
}
hasnt() {
    ! grep -q -E -- "$1" "$log" || fail "a line like: $1"
}

has "^nohost-qemu: start"
hasnt "went on after the call|hv_start_secondary\(\) was called|m1n1 restarts|an exception of|an interrupt|IOMMU|out of heap|did not take the hvc|would copy past"

case $run in
host-*)
    [ "$status" = 0 ] || fail "QEMU did not end by itself (status $status)"
    case $run in
    host-0 | host-3) event=10 ;;
    host-1) event=9 ;;
    host-2) event=8 ;;
    *) fail "not a run" ;;
    esac
    has "^nohost-qemu: the host was told: reason 3, event $event$"
    hasnt "^SCREEN>"
    hasnt "^LOG> HV: (restarting|powering)"
    ;;
nohost-*)
    [ "$status" = 124 ] || fail "QEMU ended (status $status): the core was to stay in EL2"
    hasnt "the host was told"
    has "^SCREEN> HV: there is no host: the guest does not run again"
    has "^SCREEN> HV: the guest's registers, on CPU 0"
    grep '^SCREEN>' "$log" | tail -1 | grep -q "Hold the power button to turn the machine off" ||
        fail "the last line on the screen is not the one to hold the power button"
    case $run in
    nohost-0)
        has "^SCREEN> HV: hvc #0x1 from the guest"
        has "^SCREEN> HV: the guest made a call that the hypervisor cannot carry out"
        has "^SCREEN> x0-x3: 0001000100010001 "
        has "^SCREEN> x16-x19: [0-9a-f]{16} [0-9a-f]{16} [0-9a-f]{16} 1919191919191919"
        hasnt "^SCREEN> HV: (restarting|powering)"
        ;;
    nohost-1)
        has "^SCREEN> HV: PSCI: SYSTEM_OFF from the guest"
        has "^SCREEN> HV: the guest called PSCI SYSTEM_OFF"
        has "^SCREEN> HV: powering the machine off"
        has "^SCREEN> HV: power: the SMC cannot be used"
        hasnt "PMU byte|restarting"
        ;;
    nohost-2)
        has "^SCREEN> HV: PSCI: SYSTEM_RESET from the guest"
        has "^SCREEN> HV: the guest called PSCI SYSTEM_RESET"
        has "^SCREEN> HV: restarting the machine"
        has "^SCREEN> HV: power: PMU byte 0xf801 was not written"
        has "^SCREEN> HV: the watchdog has not restarted the machine within 500 ms"
        hasnt "powering"
        ;;
    nohost-3)
        has "^SCREEN> HV: PSCI: CPU_ON from the guest on CPU 0"
        has "^SCREEN> HV: MPIDR 0x10104 \(x1 = 0x10104\) is none of the CPUs in EL2"
        has "^SCREEN> HV: CPU 0: MPIDR 0x0, in the guest"
        has "^SCREEN> HV: CPU 1: MPIDR 0x1, waiting in EL2"
        hasnt "^SCREEN> HV: CPU 2:"
        has "^SCREEN> HV: the guest made a call that the hypervisor cannot carry out"
        has "^SCREEN> x0-x3: 00000000c4000003 0000000000010104 "
        hasnt "^SCREEN> HV: (restarting|powering)"
        ;;
    *)
        fail "not a run"
        ;;
    esac
    ;;
*)
    fail "not a run"
    ;;
esac
echo "qemu-nohost: $run: $(grep -m1 -E '^nohost-qemu: the host was told|^SCREEN> HV: the guest (made|called)' "$log")"
