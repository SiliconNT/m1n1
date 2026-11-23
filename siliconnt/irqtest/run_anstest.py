#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the anstest guest (anstest.c) under the hypervisor: the disk controller core of the
SiliconNT drivers repository, on the internal disk controller (ANS) of the machine, read only.

    run_anstest.py --mode polling|interrupts [build/anstest.bin]

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
from the one proxyclient's own ADT code finds, when the hypervisor puts a hook of its own on
any of the four ranges, when HCR_EL2.DC is set (the guest's accesses would be cached), or when
the memory for the device is not free RAM above everything that load_raw() put there.

After the guest has stopped, the script reads its result block, the command log and the
blocks of its step 3 (anstest.c), checks every logged command against a whitelist of its own,
and prints the SHA-256 of the blocks: tools/m1n1_nvme_probe.py of SiliconNT hashes the same
blocks in the same order. Nothing read from the disk is written to a file.

Exit status, decided in this order:
  4  the guest did not finish: an exception, or --seconds ran out. The controller may still be
     up: nothing else is to be brought up in this boot
  6  the coprocessor crashed
  3  the command log holds a command that is not on the whitelist, or a doorbell that was not
     rung, or is not complete
  1  the controller did not come up: ans_probe() found it running, or ans_start() failed (the
     core brings it down again)
  2  it came up, but something else did not hold (reads, data, the way down, the counts, the
     interrupts)
  5  stopped before the guest touched the controller
  0  all as expected

--qemu-image OUT writes the image with QEMU's addresses in the parameter block instead (RAM
where the controller would be, see the Makefile, qemu-ans) and needs no target.
"""
import sys, os, pathlib, argparse, signal, struct, threading, hashlib, collections

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

parser = argparse.ArgumentParser(description="Run the anstest guest")
parser.add_argument("--mode", choices=("polling", "interrupts"), required=True)
parser.add_argument("--seconds", type=int, default=60,
                    help="how long the guest may run before it is stopped (default 60)")
parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                    help="the raw m1n1.bin that is supposed to be running on the target")
parser.add_argument("--pool-off", type=lambda s: int(s, 0), default=0x10000000,
                    help="where the guest's memory for the device starts, from the start of "
                         "the image (default 0x10000000)")
parser.add_argument("--pool-size", type=lambda s: int(s, 0), default=0x4000000,
                    help="its size (default 0x4000000)")
parser.add_argument("--burst-mib", type=int, default=32,
                    help="MiB the guest reads in its step 5 (default 32)")
parser.add_argument("--no-trace", action="store_true",
                    help="do not have the guest print the register writes outside ANS_READY")
parser.add_argument("--qemu-image", type=pathlib.Path, metavar="OUT",
                    help="write the image with QEMU's addresses to OUT and exit")
parser.add_argument("payload", type=pathlib.Path, nargs="?",
                    default=HERE / "build" / "anstest.bin")
args = parser.parse_args()

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
    "start_ticks stop_ticks dma_allocs dma_frees dma_bytes").split()
RESULT_FMT = f"<8s{len(RESULT_FIELDS)}Q"
SIGNED_FIELDS = ("probe", "start")
OUTCOMES = {0: "NONE (did not get to the end)", 1: "OK", 2: "PRECONDITION", 3: "PROBE_FAILED",
            4: "START_FAILED", 5: "CRASHED", 6: "WRONG"}
STATES = {0: "ANS_OFF", 1: "ANS_STARTING", 2: "ANS_READY", 3: "ANS_STOPPING", 4: "ANS_FAILED",
          5: "ANS_CRASHED"}
CMDLOG_WORDS = 10
RGN_NAMES = ("NVMe", "coprocessor and mailbox", "SART", "power manager register")

# QEMU: RAM where the controller's ranges would be (Makefile: ANS_QEMU_ASC, ANS_QEMU_PMGR)
QEMU_RANGES = ((0x48000000, 0x60000), (0x48100000, 0x88000), (0x48200000, 0xc000),
               (0x48300538, 4))
QEMU_SPIS = (40, 41)
QEMU_POOL = (0x2000000, 0x4000000)


def find_block(image, magic, what):
    off = image.find(magic)
    if off < 0 or image.find(magic, off + 1) >= 0:
        sys.exit(f"{args.payload}: {'no' if off < 0 else 'more than one'} {what} "
                 f"({magic.decode()}): not an anstest image of this tree")
    return off


def fill_params(image, ranges, spis, sart_version, secure_bar, pool, flags):
    off = find_block(image, PARAMS_MAGIC, "parameter block")
    values = struct.unpack_from(PARAMS_FMT, image, off)
    if any(values[1:]):
        sys.exit(f"{args.payload}: the parameter block is filled in already")
    struct.pack_into(PARAMS_FMT, image, off, PARAMS_MAGIC, 1, MODES[args.mode],
                     *[r[0] for r in ranges], *[r[1] for r in ranges], *spis, sart_version,
                     int(secure_bar), pool[0], pool[1], args.burst_mib, flags)


image = bytearray(args.payload.read_bytes())
if image[MAGIC_OFF:MAGIC_OFF + len(HDR_MAGIC)] != HDR_MAGIC:
    sys.exit(f"{args.payload}: not an image with the header of sgitest-head.S")
result_off = find_block(image, RESULT_MAGIC, "result block")

if args.qemu_image:
    fill_params(image, QEMU_RANGES, QEMU_SPIS, 3, True, QEMU_POOL, FLAG_TRACE_WRITES)
    args.qemu_image.write_bytes(bytes(image))
    sys.exit(0)

if struct.unpack_from("<Q", image, UART_BASE_OFF)[0] != 0:
    sys.exit(f"{args.payload}: UART base already filled in")

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.sysreg import *
from m1n1.hv import HV
from m1n1.hw.pmu import PMU


def not_run(msg):
    print(f"NOT RUN: {msg}")
    print("The guest was not entered; the disk controller was not touched.")
    sys.exit(EXIT_NOT_RUN)


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

version_got = iface.readmem(u.base + version_off, len(version_want))
print(f"m1n1 expected: {version_want[len(VERSION_MARK):].decode()}")
print(f"m1n1 running:  {version_got[len(VERSION_MARK):].decode(errors='replace')!r}")
if version_got != version_want:
    sys.exit("The m1n1 on the target is not the one built from this tree: "
             "chainload build/m1n1.bin first (chainload.py -r)")

adt = u.adt


def prop32(node, name):
    """A property of one 32-bit value, or None"""
    value = node._properties.get(name)
    if isinstance(value, bytes):
        return struct.unpack_from("<I", value)[0] if len(value) >= 4 else None
    return value


def controller():
    """The four ranges, the SART version, nvme-secure-bar, the two AIC lines; or not_run()"""
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


def aic_spis(aic_lines):
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


ranges, sart_version, secure_bar, aic_lines = controller()
spis = aic_spis(aic_lines) if args.mode == "interrupts" else (0, 0)

hv = HV(iface, p, u)
hv.smp = False
hv.init()

hcr = u.mrs(HCR_EL2)
if hcr & (1 << 12):
    not_run(f"HCR_EL2 is 0x{hcr:x}: DC is set, the guest's accesses with the MMU off would be "
            "cached")

uart_base = hv.adt["/arm-io/uart0"].get_reg(0)[0]
struct.pack_into("<Q", image, UART_BASE_OFF, uart_base)
struct.pack_into("<Q", image, NR_CPUS_OFF, 1)
struct.pack_into("<Q", image, GICD_BASE_OFF, GICD_BASE)
struct.pack_into("<Q", image, GICR_BASE_OFF, GICR_BASE)
fill_params(image, ranges, spis, sart_version, secure_bar, (args.pool_off, args.pool_size),
            0 if args.no_trace else FLAG_TRACE_WRITES)

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

# The hypervisor's own hooks, which map_essential() puts in while hv.start() runs: none may be
# on the controller's ranges. The power state register shares its page with hooked ones (the
# power state registers of the UART and the USB ports): EL2 then carries out the guest's
# accesses to it itself (SPTE_MAP of src/hv/hv_vm.c).
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
                not_run(f"the hypervisor hooks 0x{hbase:x} + 0x{hsize:x}, in the {name} range")
            if (hbase ^ base) & ~0x3fff == 0:
                print(f"Hypervisor hook 0x{hbase:x} + 0x{hsize:x} is on the 16K page of the "
                      f"{name} range")
    return pt_update()


hv.pt_update = checked_pt_update

# Nothing of the memory for the device may sit dirty in a cache over what the guest and the
# controller write uncached
p.dc_civac(pool_lo, pool_hi - pool_lo)

PMU(u).reset_panic_counter()

stopped = {}


def stop_guest(entry_msg=None, exit_msg=None):
    stopped["reason"] = f"{hv.exc_reason.name}/{getattr(hv.exc_code, 'name', hv.exc_code)}"
    print(f"Guest stopped: {stopped['reason']}")
    return EXC_RET.EXIT_GUEST


hv.run_shell = stop_guest

timer = threading.Timer(args.seconds, lambda: os.kill(os.getpid(), signal.SIGINT))
timer.daemon = True
enter_guest = p.hv_start


def enter_guest_timed(*hv_start_args):
    timer.start()
    return enter_guest(*hv_start_args)


p.hv_start = enter_guest_timed

print(f"Mode: {args.mode}")
hv.start()
timer.cancel()
print("Hypervisor exited.")

# ---- what the guest left ----


def read_guest(addr, size):
    p.dc_civac(addr & ~0x3f, align_up(addr + size, 0x40) - (addr & ~0x3f))
    return iface.readmem(addr, size)


raw = read_guest(image_base + result_off, struct.calcsize(RESULT_FMT))
values = struct.unpack(RESULT_FMT, raw)
res = dict(zip(RESULT_FIELDS, values[1:]))
for name in SIGNED_FIELDS:
    if res[name] >= 1 << 63:
        res[name] -= 1 << 64
if values[0] != RESULT_MAGIC or not res["finished"]:
    print(f"The guest did not finish (exception vector + 1: {res['exception']}, "
          f"stop: {stopped.get('reason', '-')})")
    print("The controller may still be up. Do not bring it up again in this boot.")
    sys.exit(EXIT_UNFINISHED)

print("==== result ====")
for name in RESULT_FIELDS:
    value = res[name]
    shown = OUTCOMES.get(value, value) if name == "outcome" else \
        STATES.get(value, value) if name in ("state_started", "state_end") else \
        hex(value) if name.endswith(("_addr", "_crc")) else value
    print(f"  {name:20s} {shown}")
freq = res["cntfrq"] or 1
print(f"  up in {res['start_ticks'] * 1000 // freq} ms, down in {res['stop_ticks'] * 1000 // freq} ms")

# The command log: checked once more, with this script's own whitelist
def allowed(queue, sqe):
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


ADMIN_NAMES = {0x00: "delete I/O SQ", 0x01: "create I/O SQ", 0x04: "delete I/O CQ",
               0x05: "create I/O CQ", 0x06: "identify"}
commands_ok = not res["env_refused"] and not res["cmdlog_full"]
count = res["cmdlog_count"]
log = read_guest(res["cmdlog_addr"], count * CMDLOG_WORDS * 8) if count else b""
kinds = collections.Counter()
print(f"==== command log: {count} commands ====")
for i in range(count):
    entry = log[i * CMDLOG_WORDS * 8:(i + 1) * CMDLOG_WORDS * 8]
    meta = struct.unpack_from("<Q", entry, 0)[0]
    queue, tag, guest_allowed = meta & 0xff, (meta >> 8) & 0xff, (meta >> 16) & 1
    sqe = entry[8:72]
    w0, nsid = struct.unpack_from("<II", sqe, 0)
    cdw10, cdw11, cdw12 = struct.unpack_from("<III", sqe, 40)
    ok = allowed(queue, sqe)
    if not ok or not guest_allowed:
        commands_ok = False
        print(f"  #{i}: NOT ON THE WHITELIST: queue {queue} tag {tag} opcode 0x{w0 & 0xff:02x} "
              f"nsid {nsid} cdw10 0x{cdw10:x} cdw11 0x{cdw11:x} (the guest said "
              f"{'allowed' if guest_allowed else 'not allowed'})")
    if queue == 0:
        print(f"  #{i}: admin {ADMIN_NAMES.get(w0 & 0xff, hex(w0 & 0xff))}: nsid {nsid} "
              f"cdw10 0x{cdw10:x} cdw11 0x{cdw11:x}")
        kinds[f"admin {ADMIN_NAMES.get(w0 & 0xff, hex(w0 & 0xff))}"] += 1
    else:
        kinds[f"read nsid {nsid}" if w0 & 0xff == 0x02 else f"I/O opcode 0x{w0 & 0xff:02x}"] += 1
for kind, n in sorted(kinds.items()):
    print(f"  {n:5d} x {kind}")
print(f"Every logged command on this script's whitelist: {'yes' if commands_ok else 'NO'}")

# The blocks of step 3, hashed as m1n1_nvme_probe.py hashes what it reads
if res["readback_blocks"] and res["block_size"]:
    data = read_guest(res["readback_addr"], res["readback_blocks"] * res["block_size"])
    print(f"SHA-256 of the {res['readback_blocks']} blocks of step 3: "
          f"{hashlib.sha256(data).hexdigest()}")
    del data
if res["burst_reads"]:
    print(f"Burst: {res['burst_reads']} reads, {res['burst_bytes']} bytes, "
          f"{res['burst_bytes'] * freq / max(res['burst_ticks'], 1) / 2**20:.0f} MiB/s, "
          f"CRC-32 of the CRC-32s 0x{res['burst_crc']:08x}")

outcome = res["outcome"]
if outcome == 5:
    status = EXIT_CRASHED
elif outcome == 2:
    status = EXIT_NOT_RUN
elif not commands_ok:
    status = EXIT_COMMANDS
elif outcome in (3, 4):
    status = EXIT_NOT_UP
elif outcome == 1:
    status = EXIT_OK
else:
    status = EXIT_WRONG
print(f"Exit status {status}")
sys.exit(status)
