#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the anstest guest (anstest.c) under the hypervisor: the disk controller core of the
SiliconNT drivers repository, on the internal disk controller (ANS) of the machine, read only.

    run_anstest.py --mode polling|interrupts [--expect-sha256 HEX] [--expect-burst-crc HEX]
                   [build/anstest.bin]

The m1n1 on the target has to be built from this tree (the same check as run_sgitest.py).

What the script works out and writes into the guest's parameter block, all from the device
tree m1n1 has, the way the firmware does it for the ACPI node of the controller (SiliconNT
findings/0062 conclusion 1 and the algorithm after it, findings/0071 conclusions 31, 32):

  - the four register ranges: NVMe (/arm-io/ans reg 9 with nvme-secure-bar, else reg 3; the
    length always that of reg 3), coprocessor and mailbox (reg 0), SART (/arm-io/sart-ans
    reg 0), and the controller's power state register in the power manager, 4 bytes
  - the SART version (sart-version of /arm-io/sart-ans; 0 for a sart,t8015 without one)
  - nvme-secure-bar
  - for --mode interrupts: the SPIs of the controller's line (interrupts[nvme-interrupt-idx]
    of /arm-io/ans) and of the mailbox's "message received" line (interrupts[2]), from the
    table of AIC lines and SPIs that m1n1 makes (P_HV_AICMAP), as run_sgitest.py does
  - where its memory for the device is: --pool-off from the start of the image, --pool-size

Before the guest is entered the script stops (exit status 5, nothing touched) when any of this
cannot be worked out, when the power state register found the way findings/0062 says differs
from the one proxyclient's own ADT code finds, when that register (read once, through the
proxy) does not say the controller is powered (actual state 0xf), when the hypervisor puts a
hook of its own on any of the four ranges, when HCR_EL2.DC is set (the guest's accesses would
be cached), or when the memory for the device is not free RAM above everything that
load_raw() put there. Any other failure before the guest is entered also ends with 5.

After the guest has stopped, the script reads its result block, the command log and the
blocks of its step 3 (anstest.c), and checks every logged command once more with a whitelist
of its own, that the TCB says the same as the command, that queues and identify point into
the memory given to the core and the PRPs of reads into the read buffers (the PRP lists
themselves are checked by the guest only: their pages are used again), and that the doorbells
rung are the logged commands that were allowed. It prints the SHA-256 of the blocks of
step 3: tools/m1n1_nvme_probe.py of SiliconNT hashes the same blocks in the same order.
--expect-sha256 and --expect-burst-crc compare with what an earlier run printed. Nothing read
from the disk is written to a file.

Exit status, decided in this order:
  4  the guest did not finish (an exception, or --seconds ran out), or the script failed after
     the guest was entered. The controller may still be up: nothing else is to be brought up
     in this boot
  3  the command log holds a command that is not on the whitelist, or one that points
     somewhere else, or a doorbell that was not rung, or does not add up; or the guest
     refused to set CC.EN
  6  the coprocessor crashed
  5  stopped before the guest touched the controller
  1  the controller did not come up: ans_probe() found it running, or ans_start() failed (the
     core brings it down again)
  2  it came up, but something else did not hold (reads, data, the way down, the counts, the
     interrupts, the SART, or what --expect-* asked for)
  0  all as expected
"""
import sys, os, pathlib, argparse, signal, struct, threading, hashlib, collections, traceback

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

EXIT_OK, EXIT_NOT_UP, EXIT_WRONG, EXIT_COMMANDS, EXIT_UNFINISHED, EXIT_NOT_RUN, EXIT_CRASHED = \
    0, 1, 2, 3, 4, 5, 6

# The image header of sgitest-head.S, as run_sgitest.py has it
HDR_MAGIC = b"SNTSGI02"
MAGIC_OFF = 0x08
UART_BASE_OFF = 0x10
NR_CPUS_OFF = 0x18
MAX_CPUS = 4
GICD_BASE_OFF = 0x28 + 16 * MAX_CPUS
GICR_BASE_OFF = GICD_BASE_OFF + 8
# HV_VGIC_DIST_BASE and HV_VGIC_REDIST_BASE in src/hv/hv.h
GICD_BASE = 0x50_0000_0000
GICR_BASE = 0x51_0000_0000

# struct anstest_params and struct anstest_result of anstest.c
PARAMS_MAGIC = b"SNTANS01"
PARAMS_FMT = "<8s18Q"
MODES = {"polling": 1, "interrupts": 2}
FLAG_TRACE_WRITES = 1
RESULT_MAGIC = b"SNTANSR1"
RESULT_FIELDS = (
    "finished outcome exception probe start state_started state_end blocks block_size "
    "mdts_bytes msgs_ignored msgs_refused msgs_lost nvmmu_inval_failed stray_completions "
    "readback_addr readback_blocks gpt_checks partitions scatter_equal guards_intact "
    "burst_reads burst_bytes burst_ticks burst_crc burst_first read_errors read_timeouts "
    "cmdlog_addr cmdlog_count cmdlog_full env_refused env_bad_access iosq_check irq_ctrl "
    "irq_mbox irq_other irq_spurious irq_before_start irq_after_stop storms cntfrq "
    "start_ticks stop_ticks dma_allocs dma_frees dma_bytes doorbells_rung sart_refused "
    "sart_entries_in_use sart_in_use_intact sart_ours_removed selftest_cases selftest_wrong "
    "arena_lo arena_hi data_lo data_hi burst_lo burst_hi ns_format").split()
RESULT_FMT = f"<8s{len(RESULT_FIELDS)}Q"
SIGNED_FIELDS = ("probe", "start")
OUTCOMES = {0: "NONE (did not get to the end)", 1: "OK", 2: "PRECONDITION", 3: "PROBE_FAILED",
            4: "START_FAILED", 5: "CRASHED", 6: "WRONG"}
STATES = {0: "ANS_OFF", 1: "ANS_STARTING", 2: "ANS_READY", 3: "ANS_STOPPING", 4: "ANS_FAILED",
          5: "ANS_CRASHED"}
CMDLOG_WORDS = 29
RGN_NAMES = ("NVMe", "coprocessor and mailbox", "SART", "power manager register")

ADMIN_NAMES = {0x00: "delete I/O SQ", 0x01: "create I/O SQ", 0x04: "delete I/O CQ",
               0x05: "create I/O CQ", 0x06: "identify"}

entered = False  # the guest has been entered: a failure from here on is exit status 4


class NotRun(Exception):
    pass


def not_run(msg):
    raise NotRun(msg)


def find_block(image, magic, what):
    off = image.find(magic)
    if off < 0 or image.find(magic, off + 1) >= 0:
        not_run(f"{'no' if off < 0 else 'more than one'} {what} ({magic.decode()}) in the "
                "image: not an anstest image of this tree")
    return off


def fill_params(args, image, ranges, spis, sart_version, secure_bar, pool, flags):
    off = find_block(image, PARAMS_MAGIC, "parameter block")
    values = struct.unpack_from(PARAMS_FMT, image, off)
    if any(values[1:]):
        not_run("the parameter block is filled in already")
    struct.pack_into(PARAMS_FMT, image, off, PARAMS_MAGIC, 1, MODES[args.mode],
                     *[r[0] for r in ranges], *[r[1] for r in ranges], *spis, sart_version,
                     int(secure_bar), pool[0], pool[1], args.burst_mib, flags)


def prop32(node, name):
    """A property of one 32-bit value, or None"""
    value = node._properties.get(name)
    if isinstance(value, bytes):
        return struct.unpack_from("<I", value)[0] if len(value) >= 4 else None
    return value


def controller(adt):
    """The four ranges, the SART version, nvme-secure-bar, the two AIC lines"""
    try:
        ans = adt["/arm-io/ans"]
        sart = adt["/arm-io/sart-ans"]
    except KeyError as e:
        not_run(f"no {e} in the device tree")
    secure_bar = "nvme-secure-bar" in ans._properties
    nvme = (ans.get_reg(9 if secure_bar else 3)[0], ans.get_reg(3)[1])
    asc = ans.get_reg(0)
    sart_range = sart.get_reg(0)

    sart_version = prop32(sart, "sart-version")
    if sart_version is None:
        if "sart,t8015" not in sart.compatible:
            not_run("/arm-io/sart-ans has no sart-version and is no sart,t8015")
        sart_version = 0

    # The power state register (findings/0062, after conclusion 1)
    pm_path = "/arm-io/pmgr-child" if "pmgr2,arch" in adt["/arm-io/pmgr"].compatible \
        else "/arm-io/pmgr"
    pm = adt[pm_path]
    found = [d for d in pm.devices if d.name in ("ANS", "ANS2") and not d.flags.no_ps]
    names = sorted(d.name for d in found)
    if names not in (["ANS"], ["ANS2"]):
        not_run(f"{pm_path} devices: {names or 'neither ANS nor ANS2'}")
    dev = found[0]
    if "ps-regs" in pm._properties:
        psreg = pm.ps_regs[dev.psreg]
        reg_idx, off = psreg.reg, psreg.offset + dev.psidx * 8
    else:
        group = pm.ps_groups[dev.group]
        reg_idx, off = group.reg, dev.offset
    gates = ans._properties.get("clock-gates")
    if isinstance(gates, bytes) and len(gates) >= 4:
        die = struct.unpack_from("<I", gates)[0] >> 28
    else:
        die = (nvme[0] >> 37) & 3
    pmgr_reg = pm.get_reg(reg_idx)[0] + off + die * 0x20_0000_0000
    theirs = adt.pmgr_dev_get_addr(dev) + die * 0x20_0000_0000
    if theirs != pmgr_reg:
        not_run(f"the power state register of {dev.name}: 0x{pmgr_reg:x} the way findings/0062 "
                f"says, 0x{theirs:x} by proxyclient's pmgr_dev_get_addr()")

    lines = list(ans.interrupts)
    idx = prop32(ans, "nvme-interrupt-idx")
    if idx is None or not 4 <= idx < len(lines):
        not_run(f"nvme-interrupt-idx {idx} with {len(lines)} interrupts")
    ranges = (nvme, asc, sart_range, (pmgr_reg, 4))
    for name, (base, size) in zip(RGN_NAMES, ranges):
        print(f"{name}: 0x{base:x} + 0x{size:x}")
    print(f"power state register: {pm_path} device {dev.name}, die {die}")
    print(f"SART version {sart_version}; nvme-secure-bar {'yes' if secure_bar else 'no'}")
    print(f"AIC lines: controller {lines[idx]} (interrupts[{idx}]), mailbox {lines[2]} "
          f"(interrupts[2])")
    return ranges, sart_version, secure_bar, (lines[idx], lines[2])


def aic_spis(p, iface, u, aic_lines):
    """The SPIs that m1n1's table has for the two AIC lines (run_sgitest.py, aic_line())"""
    buf = u.malloc(0x4000)
    size = p.hv_aicmap(buf)
    if size < 8:
        not_run(f"m1n1 wrote no table of AIC lines and SPIs (P_HV_AICMAP returned {size})")
    table = iface.readmem(buf, size)
    version, count = struct.unpack_from("<II", table)
    table_lines = struct.unpack_from(f"<{count}I", table, 8)
    print(f"AIC to GIC table: version {version}, {count} lines")
    spis = []
    for line in aic_lines:
        if line not in table_lines:
            not_run(f"AIC line {line} is not in the table")
        spis.append(32 + table_lines.index(line))
    print(f"SPIs: controller {spis[0]}, mailbox {spis[1]}")
    return spis


def allowed(queue, sqe):
    """This script's own whitelist (findings/0071 conclusion 22)"""
    w0, nsid = struct.unpack_from("<II", sqe, 0)
    opcode, flags = w0 & 0xff, (w0 >> 8) & 0xff
    cdw10, cdw11 = struct.unpack_from("<II", sqe, 40)
    if flags:
        return False
    if queue == 1:
        return opcode == 0x02 and nsid == 1
    return {0x05: nsid == 0 and cdw10 & 0xffff == 1,
            0x01: nsid == 0 and cdw10 & 0xffff == 1 and cdw11 >> 16 == 1,
            0x06: (cdw10, nsid) in ((1, 0), (0, 1)),
            0x00: nsid == 0 and cdw10 == 1,
            0x04: nsid == 0 and cdw10 == 1}.get(opcode, False)


def where_ok(res, queue, sqe, tcb, bases):
    """None, or why what the command points at is not where it is to point. bases: the
    submission queue, completion queue and TCB table the guest found when it rang"""
    def inside(addr, size, lo, hi):
        return size > 0 and lo <= addr and addr + size <= hi

    def in_arena(addr, size):
        return inside(addr, size, res["arena_lo"], res["arena_hi"])

    def in_data(addr, size):
        return inside(addr, size, res["data_lo"], res["data_hi"]) or \
            inside(addr, size, res["burst_lo"], res["burst_hi"])

    w0 = struct.unpack_from("<I", sqe, 0)[0]
    opcode = w0 & 0xff
    prp1, prp2 = struct.unpack_from("<QQ", sqe, 24)
    cdw10, cdw11 = struct.unpack_from("<II", sqe, 40)
    sq, cq, tcbs = bases
    if not (in_arena(sq, 64 * 64) and in_arena(cq, 64 * 16) and in_arena(tcbs, 64 * 128)):
        return "a queue or the TCB table is not in the memory given to the core"
    if any(sqe[8:24]):
        return "CDW2, CDW3 or the metadata pointer set"
    cdw12 = struct.unpack_from("<I", sqe, 48)[0]
    tcb_head, = struct.unpack_from("<Q", tcb, 0)
    tcb_prp1, tcb_prp2 = struct.unpack_from("<QQ", tcb, 24)
    if (tcb_prp1, tcb_prp2) != (prp1, prp2):
        return "the TCB's PRPs are not the command's"
    if queue == 1:
        size = ((cdw12 & 0xffff) + 1) * res["block_size"]
        first = min(0x1000 - (prp1 & 0xfff), size)
        if (tcb_head >> 8) & 0xff != 1 or (tcb_head >> 32) & 0xffff != cdw12 & 0xffff:
            return "the TCB is not that of a read of these blocks"
        if not in_data(prp1, first):
            return "PRP1 outside the read buffers"
        rest = size - first
        if rest == 0 and prp2:
            return "PRP2 where none is needed"
        if 0 < rest <= 0x1000 and not in_data(prp2, rest):
            return "PRP2 outside the read buffers"
        if rest > 0x1000 and not in_arena(prp2, 8):
            return "the PRP list is not in the memory given to the core"
        return None
    if opcode == 0x06:
        return None if in_arena(prp1, 0x1000) and not prp2 else \
            "identify not into the memory given to the core"
    if opcode in (0x01, 0x05):
        if not cdw11 & 1 or cdw10 >> 16 >= 64:
            return "the queue is not physically contiguous, or larger than its memory"
        return None if in_arena(prp1, 64 * (64 if opcode == 0x01 else 16)) and not prp2 else \
            "the queue is not in the memory given to the core"
    return None if not prp1 and not prp2 else "a delete with memory"


def run(args):
    global entered

    image = bytearray(args.payload.read_bytes())
    if image[MAGIC_OFF:MAGIC_OFF + len(HDR_MAGIC)] != HDR_MAGIC:
        not_run(f"{args.payload}: not an image with the header of sgitest-head.S")
    result_off = find_block(image, RESULT_MAGIC, "result block")

    if struct.unpack_from("<Q", image, UART_BASE_OFF)[0] != 0:
        not_run(f"{args.payload}: UART base already filled in")

    from m1n1.proxy import UartInterface, M1N1Proxy
    from m1n1.proxyutils import ProxyUtils, bootstrap_port
    from m1n1.utils import align_up
    from m1n1.sysreg import HCR_EL2
    from m1n1.hv import HV
    from m1n1.hw.pmu import PMU
    from m1n1.proxy import EXC_RET

    VERSION_MARK = b"##m1n1_ver##"
    m1n1_image = args.m1n1.read_bytes()
    version_off = m1n1_image.find(VERSION_MARK)
    if version_off < 0:
        not_run(f"{args.m1n1}: no version string")
    version_want = m1n1_image[version_off:m1n1_image.index(b"\0", version_off)]

    iface = UartInterface()
    p = M1N1Proxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024, m1n1_heap=128 * 1024 * 1024)

    version_got = iface.readmem(u.base + version_off, len(version_want))
    print(f"m1n1 expected: {version_want[len(VERSION_MARK):].decode()}")
    print(f"m1n1 running:  {version_got[len(VERSION_MARK):].decode(errors='replace')!r}")
    if version_got != version_want:
        not_run("the m1n1 on the target is not the one given with --m1n1: chainload it first "
                "(chainload.py -r)")

    ranges, sart_version, secure_bar, aic_lines = controller(u.adt)

    # The power state register, read once: the controller has to be powered (findings/0071
    # conclusion 31: PS_ACTUAL, bits 4-7, 0xf), or even reading its registers may fault
    ps = p.read32(ranges[3][0])
    print(f"power state register: 0x{ps:08x} (actual state 0x{(ps >> 4) & 0xf:x})")
    if ps == 0xffffffff or (ps >> 4) & 0xf != 0xf:
        not_run("the controller is not in the powered state")

    spis = aic_spis(p, iface, u, aic_lines) if args.mode == "interrupts" else (0, 0)

    hv = HV(iface, p, u)
    hv.smp = False
    hv.init()

    hcr = u.mrs(HCR_EL2)
    if hcr & (1 << 12):
        not_run(f"HCR_EL2 is 0x{hcr:x}: DC is set, the guest's accesses with the MMU off would "
                "be cached")

    uart_base = hv.adt["/arm-io/uart0"].get_reg(0)[0]
    struct.pack_into("<Q", image, UART_BASE_OFF, uart_base)
    struct.pack_into("<Q", image, NR_CPUS_OFF, 1)
    struct.pack_into("<Q", image, GICD_BASE_OFF, GICD_BASE)
    struct.pack_into("<Q", image, GICR_BASE_OFF, GICR_BASE)
    fill_params(args, image, ranges, spis, sart_version, secure_bar,
                (args.pool_off, args.pool_size), 0 if args.no_trace else FLAG_TRACE_WRITES)

    hv.load_raw(bytes(image), entryoffset=0)
    image_base = hv.guest_base
    loaded_end = hv.guest_base + hv.bootargs_off + 0x4000
    pool_lo = align_up(image_base + args.pool_off, 0x4000)
    pool_hi = image_base + args.pool_off + args.pool_size
    print(f"Guest image at 0x{image_base:x}; load_raw() used up to 0x{loaded_end:x}")
    print(f"Memory for the device: 0x{pool_lo:x} .. 0x{pool_hi:x}")
    if pool_lo < loaded_end or pool_hi > hv.mem_top:
        not_run(f"that is not between 0x{loaded_end:x} and the top of RAM 0x{hv.mem_top:x}")
    carveout_p = p.mcc_get_carveouts()
    while True:
        cbase = p.read64(carveout_p)
        csize = p.read64(carveout_p + 8)
        if not cbase:
            break
        if cbase < pool_hi and pool_lo < cbase + csize:
            not_run(f"it overlaps the carveout 0x{cbase:x} + 0x{csize:x}")
        carveout_p += 16

    # The hypervisor's own hooks, which map_essential() puts in while hv.start() runs: none
    # may be on the controller's ranges. The power state register shares its page with hooked
    # ones (the power state registers of the UART and the USB ports): EL2 then carries out the
    # guest's accesses to it itself (SPTE_MAP of src/hv/hv_vm.c).
    hooks = []
    map_hook = hv.map_hook

    def recording_map_hook(ipa, size, *a, **kw):
        hooks.append((ipa, size))
        return map_hook(ipa, size, *a, **kw)

    hv.map_hook = recording_map_hook
    pt_update = hv.pt_update

    def checked_pt_update():
        for hbase, hsize in hooks:
            for name, (base, size) in zip(RGN_NAMES, ranges):
                if hbase < base + size and base < hbase + hsize:
                    not_run(f"the hypervisor hooks 0x{hbase:x} + 0x{hsize:x}, in the {name} "
                            "range")
                if (hbase ^ base) & ~0x3fff == 0:
                    print(f"Hypervisor hook 0x{hbase:x} + 0x{hsize:x} is on the 16K page of "
                          f"the {name} range")
        return pt_update()

    hv.pt_update = checked_pt_update

    # Nothing of the memory for the device may sit dirty in a cache over what the guest and
    # the controller write uncached
    p.dc_civac(pool_lo, pool_hi - pool_lo)

    PMU(u).reset_panic_counter()

    stopped = {}

    def stop_guest(entry_msg=None, exit_msg=None):
        reason = getattr(hv, "exc_reason", None)
        code = getattr(hv, "exc_code", None)
        stopped["reason"] = f"{getattr(reason, 'name', reason)}/{getattr(code, 'name', code)}"
        print(f"Guest stopped: {stopped['reason']}")
        return EXC_RET.EXIT_GUEST

    hv.run_shell = stop_guest

    timer = threading.Timer(args.seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
    timer.daemon = True
    enter_guest = p.hv_start

    def enter_guest_timed(*hv_start_args):
        global entered
        entered = True
        timer.start()
        return enter_guest(*hv_start_args)

    p.hv_start = enter_guest_timed

    print(f"Mode: {args.mode}")
    hv.start()
    timer.cancel()
    signal.signal(signal.SIGINT, signal.default_int_handler)
    print("Hypervisor exited.")
    return report(args, p, iface, align_up, image_base + result_off, stopped)


def report(args, p, iface, align_up, result_addr, stopped):
    def read_guest(addr, size):
        lo = addr & ~0x3f
        p.dc_civac(lo, align_up(addr + size, 0x40) - lo)
        return iface.readmem(addr, size)

    raw = read_guest(result_addr, struct.calcsize(RESULT_FMT))
    values = struct.unpack(RESULT_FMT, raw)
    res = dict(zip(RESULT_FIELDS, values[1:]))
    for name in SIGNED_FIELDS:
        if res[name] >= 1 << 63:
            res[name] -= 1 << 64
    if values[0] != RESULT_MAGIC or not res["finished"]:
        print(f"The guest did not finish (exception vector + 1: {res['exception']}, "
              f"stop: {stopped.get('reason', '-')})")
        print("The controller may still be up. Do not bring it up again in this boot.")
        return EXIT_UNFINISHED

    print("==== result ====")
    for name in RESULT_FIELDS:
        value = res[name]
        shown = OUTCOMES.get(value, value) if name == "outcome" else \
            STATES.get(value, value) if name in ("state_started", "state_end") else \
            hex(value) if name.endswith(("_addr", "_crc", "_lo", "_hi")) else value
        print(f"  {name:20s} {shown}")
    freq = res["cntfrq"] or 1
    print(f"  up in {res['start_ticks'] * 1000 // freq} ms, "
          f"down in {res['stop_ticks'] * 1000 // freq} ms")
    if res["msgs_ignored"]:
        print(f"NOTE: {res['msgs_ignored']} messages of the coprocessor were ignored "
              "(not answered, as m1n1 does)")

    # The command log, checked once more
    count = res["cmdlog_count"]
    commands_ok = not res["env_refused"] and not res["cmdlog_full"]
    log = read_guest(res["cmdlog_addr"], count * CMDLOG_WORDS * 8) if count else b""
    kinds = collections.Counter()
    rung = 0
    print(f"==== command log: {count} commands ====")
    for i in range(count):
        entry = log[i * CMDLOG_WORDS * 8:(i + 1) * CMDLOG_WORDS * 8]
        meta = struct.unpack_from("<Q", entry, 0)[0]
        queue, tag, guest_allowed = meta & 0xff, (meta >> 8) & 0xff, (meta >> 16) & 1
        sqe, tcb = entry[8:72], entry[72:200]
        bases = struct.unpack_from("<QQQ", entry, 200)
        w0, nsid = struct.unpack_from("<II", sqe, 0)
        opcode = w0 & 0xff
        cdw10, cdw11, cdw12 = struct.unpack_from("<III", sqe, 40)
        why = None if allowed(queue, sqe) else "not on the whitelist"
        why = why or where_ok(res, queue, sqe, tcb, bases)
        rung += guest_allowed
        if why or not guest_allowed:
            commands_ok = False
            print(f"  #{i}: {why or 'not rung'}: queue {queue} tag {tag} opcode 0x{opcode:02x} "
                  f"nsid {nsid} cdw10 0x{cdw10:x} cdw11 0x{cdw11:x} (the guest "
                  f"{'rang it' if guest_allowed else 'did not ring it'})")
        name = f"admin {ADMIN_NAMES.get(opcode, hex(opcode))}" if queue == 0 else \
            f"read nsid {nsid}" if opcode == 0x02 else f"I/O opcode 0x{opcode:02x}"
        if queue == 0:
            print(f"  #{i}: {name}: nsid {nsid} cdw10 0x{cdw10:x} cdw11 0x{cdw11:x}")
        kinds[name] += 1
    for kind, n in sorted(kinds.items()):
        print(f"  {n:5d} x {kind}")
    if rung != res["doorbells_rung"]:
        commands_ok = False
        print(f"The log has {rung} commands rung, the guest counted {res['doorbells_rung']}")
    print(f"Every logged command on this script's whitelist and pointing where it is to point: "
          f"{'yes' if commands_ok else 'NO'}")

    expected_ok = True
    if res["readback_blocks"] and res["block_size"]:
        data = read_guest(res["readback_addr"], res["readback_blocks"] * res["block_size"])
        digest = hashlib.sha256(data).hexdigest()
        del data
        print(f"SHA-256 of the {res['readback_blocks']} blocks of step 3: {digest}")
        if args.expect_sha256 and digest != args.expect_sha256.lower():
            expected_ok = False
            print(f"NOT the expected {args.expect_sha256}")
    elif args.expect_sha256:
        expected_ok = False
    if res["burst_reads"]:
        print(f"Burst: {res['burst_reads']} reads, {res['burst_bytes']} bytes, "
              f"{res['burst_bytes'] * freq / max(res['burst_ticks'], 1) / 2**20:.0f} MiB/s, "
              f"CRC-32 of the CRC-32s 0x{res['burst_crc']:08x}")
    if args.expect_burst_crc is not None and res["burst_crc"] != args.expect_burst_crc:
        expected_ok = False
        print(f"NOT the expected burst CRC-32 0x{args.expect_burst_crc:08x}")

    outcome = res["outcome"]
    if not commands_ok:
        return EXIT_COMMANDS
    if outcome == 5:
        return EXIT_CRASHED
    if outcome == 2:
        return EXIT_NOT_RUN
    if outcome in (3, 4):
        return EXIT_NOT_UP
    if outcome == 1 and expected_ok:
        return EXIT_OK
    return EXIT_WRONG


def main():
    parser = argparse.ArgumentParser(description="Run the anstest guest")
    parser.add_argument("--mode", choices=("polling", "interrupts"), required=True)
    parser.add_argument("--seconds", type=int, default=60,
                        help="how long the guest may run before it is stopped (default 60)")
    parser.add_argument("--m1n1", type=pathlib.Path,
                        default=HERE.parents[1] / "build" / "m1n1.bin",
                        help="the raw m1n1.bin that is supposed to be running on the target")
    parser.add_argument("--pool-off", type=lambda s: int(s, 0), default=0x10000000,
                        help="where the guest's memory for the device starts, from the start "
                             "of the image (default 0x10000000)")
    parser.add_argument("--pool-size", type=lambda s: int(s, 0), default=0x4000000,
                        help="its size (default 0x4000000)")
    parser.add_argument("--burst-mib", type=int, default=32,
                        help="MiB the guest reads in its step 5 (default 32)")
    parser.add_argument("--expect-sha256", metavar="HEX",
                        help="the SHA-256 the blocks of step 3 must have")
    parser.add_argument("--expect-burst-crc", type=lambda s: int(s, 16), metavar="HEX",
                        help="the CRC-32 of the CRC-32s the burst must have")
    parser.add_argument("--no-trace", action="store_true",
                        help="do not have the guest print the register writes outside "
                             "ANS_READY")
    parser.add_argument("payload", type=pathlib.Path, nargs="?",
                        default=HERE / "build" / "anstest.bin")
    args = parser.parse_args()

    try:
        status = run(args)
    except NotRun as e:
        if entered:
            print(f"FAILED after the guest was entered: {e}")
            status = EXIT_UNFINISHED
        else:
            print(f"NOT RUN: {e}")
            print("The guest was not entered; the disk controller was not touched.")
            status = EXIT_NOT_RUN
    except BaseException:
        traceback.print_exc()
        if entered:
            print("FAILED after the guest was entered. The controller may still be up. Do not "
                  "bring it up again in this boot.")
            status = EXIT_UNFINISHED
        else:
            print("NOT RUN: the script failed before the guest was entered; the disk controller "
                  "was not touched.")
            status = EXIT_NOT_RUN
    print(f"Exit status {status}")
    sys.stdout.flush()
    os._exit(status)


if __name__ == "__main__":
    main()
