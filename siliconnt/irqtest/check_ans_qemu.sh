#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# What a run of anstest.bin on QEMU is to show (make qemu-ans).
#
#   check_ans_qemu.sh <run> <log> <exit status of timeout>
#
# <run> is <polling or interrupts>-<refused or silent>. There is RAM where the controller's
# ranges would be, so the controller cannot come up: refused, the power manager register
# reads 0 and the core refuses the reset before the start, so the coprocessor's RUN bit is
# never set; silent, it reads "active", the reset is done, RUN is set, the coprocessor never
# says HELLO, and the core clears RUN and resets again. Either way the guest says the start
# failed, no NVMe command is sent, and the only registers written are RUN, the mailbox's send
# registers and the power manager register. The guest ends with SYSTEM_OFF, which ends QEMU.
set -u
run=$1
log=$2
status=$3

fail() {
    echo "qemu-ans: $run: $*"
    exit 1
}
has() {
    grep -q -E -- "$1" "$log" || fail "no line like: $1"
}
hasnt() {
    ! grep -q -E -- "$1" "$log" || fail "a line like: $1"
}

[ "$status" = 0 ] || fail "QEMU did not end by itself (status $status)"
has "^HVLOG: anstest: start, ${run%%-*}$"
has "^HVLOG: anstest: FAIL ans_start: "
has "^HVLOG: anstest: core: ans: controller failed$"
has "^HVLOG: anstest: DONE outcome 4$"
hasnt "EXCEPTION|NOT RUNG|refused a|env: admin command|core: nvme: denied|forbidden"
has "^HVLOG: anstest: counts: .* commands 0, not rung 0, log full 0, bad accesses 0$"

# Every register write: RUN (asc+44), the mailbox's send registers, the power manager register
writes=$(grep -E "^HVLOG: anstest: W(32|64) " "$log")
others=$(echo "$writes" | grep -v -E "^HVLOG: anstest: W(32 asc\+44|64 asc\+880[08]|32 pmgr\+0) " |
    grep -v '^$')
[ -z "$others" ] || fail "writes to registers it has no business with: $others"

case ${run#*-} in
refused)
    has "^HVLOG: anstest: core: pmgr: will not reset ANS, power state register 00000000 is not active$"
    hasnt "W32 asc\+44 = 00000010|W32 pmgr"
    ;;
silent)
    [ "$(grep -c '^HVLOG: anstest: core: pmgr: resetting ANS$' "$log")" = 2 ] ||
        fail "not two resets, one before the start and one after RUN is cleared"
    has "^HVLOG: anstest: W32 asc\+44 = 00000010$"
    has "^HVLOG: anstest: core: nvme: timeout while waiting for HELLO$"
    # RUN set, later cleared, then the reset: in that order
    order=$(grep -n -E "W32 asc\+44 = 000000(1|0)0$|core: pmgr: resetting ANS$" "$log" | cut -d: -f1 |
        tr '\n' ' ')
    set -- $order
    [ $# = 4 ] || fail "RUN and the resets: $order"
    first_reset=$1 run_set=$2 run_clear=$3 second_reset=$4
    grep -n "" "$log" | grep -q "^$run_set:.*= 00000010$" || fail "RUN not set after the first reset"
    grep -n "" "$log" | grep -q "^$run_clear:.*= 00000000$" || fail "RUN not cleared before the second reset"
    ;;
*)
    fail "not a run"
    ;;
esac

case ${run%%-*} in
interrupts)
    has "^HVLOG: anstest: before ans_start: 0 interrupts"
    has "^HVLOG: anstest: after the failed start: 0 interrupts"
    ;;
polling)
    hasnt "interrupts of the controller"
    ;;
esac
echo "qemu-ans: $run: $(grep -m1 -E '^HVLOG: anstest: FAIL ans_start' "$log" | sed 's/^HVLOG: anstest: //')"
