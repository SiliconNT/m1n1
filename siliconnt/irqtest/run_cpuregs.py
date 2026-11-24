#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Start every secondary CPU and read the system registers of cpuregs.h on each core at EL2
(SiliconNT R50), before any of them has been in the hypervisor.

Only reads: the code it puts into the memory of the target (el2probe.S) reads system registers
and, for the wakeups at the end, writes a word of the proxy's heap. What it does that a
reader would not: it puts m1n1's idle loop into WFE mode (M4 loses CPU state in WFI,
src/kboot.c) and starts the secondary CPUs, one at a time, each only if the RVBAR register of
that CPU holds the base of the m1n1 that is running. Started, they stay in m1n1's idle loop.

The registers are read with exception vectors of the probe's own, so that one a core does not
have is reported and does not end in m1n1's handler, which would reboot the machine.

Then, for each core, it measures how long a core that waits like m1n1's idle loop (WFE, MMU
off) takes to come out after a SEV of the boot core.

The cores must not have run the hypervisor yet in this boot: their MMU is off here. Run it
before run_sgitest.py is given any secondary CPU, and once per boot.

--out writes everything as JSON, which tools/cpu_regs_check.py of the SiliconNT repository
reads.
"""
import sys, argparse, json, pathlib, re, struct, time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.append(str(HERE.parents[1] / "proxyclient"))

PROBE_MAGIC = 0x50525A30
WAKE_WAIT, WAKE_KICK = 0x08, 0x10
CODE_SIZE = 0x4000


def parse_regs(text):
    """The entries of CPU_REGS in cpuregs.h, in their order: (name, where)"""
    body = text[text.index("#define CPU_REGS"):]
    found = re.findall(r"\bREG\((\w+), (BOTH|EL2|LATE), (\d+), (\d+), (\d+), (\d+), (\d+)\)", body)
    if not found:
        raise ValueError("no registers in cpuregs.h")
    return [(name, where) for name, where, *_ in found]


def decode(data, regs):
    """What the probe wrote: name -> (value, ESR or 0)"""
    words = struct.unpack_from(f"<{2 * len(regs)}Q", data)
    return {name: (words[2 * i], words[2 * i + 1]) for i, (name, _) in enumerate(regs)}


def show(value):
    val, esr = value
    return f"!esr={esr & 0xffffffff:08x}" if esr else f"{val:016x}"


def main():
    parser = argparse.ArgumentParser(description="Read system registers at EL2 on every core")
    parser.add_argument("--m1n1", type=pathlib.Path, default=HERE.parents[1] / "build" / "m1n1.bin",
                        help="the raw m1n1.bin that is supposed to be running on the target")
    parser.add_argument("--out", type=pathlib.Path, help="where the JSON goes")
    parser.add_argument("--adt", type=pathlib.Path,
                        help="where the device tree of this boot goes, as it is (it has serial "
                             "numbers in it)")
    parser.add_argument("--wake-rounds", type=int, default=50)
    parser.add_argument("--check-only", action="store_true",
                        help="only check which m1n1 is running and the RVBAR of every CPU")
    args = parser.parse_args()

    regs = parse_regs((HERE / "cpuregs.h").read_text())
    blob = (HERE / "build" / "el2probe.bin").read_bytes()

    from m1n1.proxy import UartInterface, M1N1Proxy, REGION_RX_EL1
    from m1n1.proxyutils import ProxyUtils, bootstrap_port

    # As in run_sgitest.py: the build that is running has to be the one that is expected
    version_mark = b"##m1n1_ver##"
    image = args.m1n1.read_bytes()
    version_off = image.find(version_mark)
    if version_off < 0:
        sys.exit(f"{args.m1n1}: no version string")
    want = image[version_off:image.index(b"\0", version_off)]

    iface = UartInterface()
    p = M1N1Proxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024, m1n1_heap=128 * 1024 * 1024)

    got = iface.readmem(u.base + version_off, len(want))
    print(f"m1n1 expected: {want[len(version_mark):].decode()}")
    print(f"m1n1 running:  {got[len(version_mark):].decode(errors='replace')!r}")
    if got != want:
        sys.exit("The m1n1 on the target is not the one that is expected")

    cpus, boot = {}, None
    for node in u.adt["cpus"]:
        cpus[node.cpu_id] = node
        if node.state == "running":
            boot = node.cpu_id
    print(f"Boot CPU: {boot}")
    secondaries = [index for index in sorted(cpus) if index != boot]

    # The same check as in run_sgitest.py and, again, in smp_start_secondary()
    rvbar_addr = 0xfffffffff000
    for index in secondaries:
        rvbar = p.read64(cpus[index].cpu_impl_reg[0])
        print(f"CPU {index}: RVBAR register 0x{rvbar:x}, m1n1 base 0x{u.base:x}")
        if not u.cpu_features.apple_sysregs_unlocked and (rvbar & rvbar_addr) != u.base:
            sys.exit(f"CPU {index} would not come up in this image; none has been started")
    already = [index for index in secondaries if p.smp_is_alive(index)]
    if already:
        sys.exit(f"CPUs {already} are running m1n1 already: run this first in a boot")
    if args.adt:
        args.adt.write_bytes(u.get_adt())
        print(f"Device tree: {args.adt}")
    memory = {"phys_base": u.ba.phys_base, "mem_size": u.ba.mem_size,
              "top_of_kernel_data": u.ba.top_of_kernel_data,
              "mem_size_actual": getattr(u.ba, "mem_size_actual", 0)}
    print("Boot arguments: " + ", ".join(f"{k} 0x{v:x}" for k, v in memory.items()))
    if args.check_only:
        return

    code = u.memalign(CODE_SIZE, CODE_SIZE)
    out = u.memalign(CODE_SIZE, CODE_SIZE)
    flag = u.memalign(CODE_SIZE, CODE_SIZE)
    iface.writemem(code, blob)
    p.dc_civac(code, len(blob))
    p.ic_ivau(code, len(blob))
    size = 16 * len(regs)

    def run_probe(call):
        iface.writemem(out, b"\xee" * size)
        p.dc_civac(out, size)
        if call(out) != PROBE_MAGIC:
            sys.exit("The probe did not return what it should")
        p.dc_civac(out, size)
        return decode(iface.readmem(out, size), regs)

    result = {"boot": boot, "m1n1": want[len(version_mark):].decode(), "base": u.base,
              "memory": memory,
              "regs": [list(r) for r in regs], "cores": {}, "wake": {}}

    def save():
        if args.out:
            args.out.write_text(json.dumps(result, indent=1) + "\n")

    def report(index, values):
        node = cpus[index]
        result["cores"][str(index)] = {
            "reg": node.reg, "type": getattr(node, "cluster_type", "?"),
            "values": {name: list(v) for name, v in values.items()}}
        print(f"==== CPU {index} ({getattr(node, 'cluster_type', '?')}, reg 0x{node.reg:x}) ====")
        for name, where in regs:
            print(f"  {name:18s} {where:4s} {show(values[name])}")
        sys.stdout.flush()
        save()

    print(f"heap top before the secondary CPUs: 0x{p.heapblock_alloc(0):x}")
    report(boot, run_probe(lambda buf: p.call(code | REGION_RX_EL1, buf)))

    # Before any secondary CPU exists: its idle loop is to use WFE, never WFI
    print("m1n1 idle loop: WFE mode")
    p.smp_set_wfe_mode(True)
    for index in secondaries:
        start = time.monotonic()
        alive = p.smp_start_secondary(index)
        took = (time.monotonic() - start) * 1000
        print(f"CPU {index}: {'running m1n1' if alive else 'did NOT start'}, {took:.1f} ms as the "
              "host sees it")
        result["cores"].setdefault(str(index), {})["start_ms"] = took
        if not alive:
            save()
            sys.exit("Not going on. Any CPU that did start stays in m1n1's idle loop.")
        start_ms = took
        values = run_probe(lambda buf: p.smp_call_sync(index, code, buf))
        report(index, values)
        result["cores"][str(index)]["start_ms"] = start_ms
    print(f"heap top with every CPU running m1n1: 0x{p.heapblock_alloc(0):x}")

    freq = result["cores"][str(boot)]["values"]["CNTFRQ_EL0"][0]
    print(f"Out of WFE after a SEV of CPU {boot}, {args.wake_rounds} times each, in ns:")
    for index in secondaries:
        took = []
        for _ in range(args.wake_rounds):
            p.write64(flag, 0)
            p.dc_civac(flag, 64)
            p.write64(out, 0)
            p.dc_civac(out, 64)
            p.smp_call(index, code + WAKE_WAIT, flag, out)
            t0 = p.call((code + WAKE_KICK) | REGION_RX_EL1, flag)
            p.smp_wait(index)
            p.dc_civac(out, 64)
            took.append((p.read64(out) - t0) * 1_000_000_000 // freq)
        took.sort()
        result["wake"][str(index)] = took
        print(f"  CPU {index}: least {took[0]}, median {took[len(took) // 2]}, most {took[-1]}")
        sys.stdout.flush()
    save()
    print("Done. The secondary CPUs stay in m1n1's idle loop (WFE).")


if __name__ == "__main__":
    main()
