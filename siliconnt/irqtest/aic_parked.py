# SPDX-License-Identifier: GPL-2.0-or-later
"""Which core does AIC report a raised line to while no core can take it: the boot core is in
EL2, where the guest has been stopped (IRQs masked), and the others are in m1n1's idle loop
(WFE, IRQs masked, MMU off). For run_sgitest.py --park, once the guest has been stopped
(SiliconNT R50, docs/findings/0098, conclusion 19).

Looks at ISR_EL1 of every core, and reads AIC's event register, which acknowledges the line and
masks it, only on a core whose ISR_EL1 says that it has an IRQ. The line is raised with the
software trigger of AIC, no device has a part in it, and it is left lowered and masked.
"""
import sys, time

ISR_I = 1 << 7
CALL_ISR, CALL_LOAD32 = 0x18, 0x20  # el2probe.S


class Parked:
    def __init__(self, p, u, iface, boot, parked, regs, blob, isr_boot):
        """regs: addresses of AIC for the line, see aic_line() of run_sgitest.py. isr_boot():
        ISR_EL1 of the boot core."""
        self.p, self.boot, self.parked, self.regs = p, boot, parked, regs
        self.isr_boot = isr_boot
        self.code = u.memalign(0x4000, 0x4000)
        iface.writemem(self.code, blob)
        p.dc_civac(self.code, len(blob))
        p.ic_ivau(self.code, len(blob))

    def isr(self, cpu):
        if cpu == self.boot:
            return self.isr_boot()
        return self.p.smp_call_sync(cpu, self.code + CALL_ISR)

    def event(self, cpu):
        if cpu == self.boot:
            return self.p.read32(self.regs["event"])
        return self.p.smp_call_sync(cpu, self.code + CALL_LOAD32, self.regs["event"])

    def with_irq(self):
        """The cores whose ISR_EL1 has the I bit, the boot core first"""
        return [cpu for cpu in [self.boot] + self.parked if self.isr(cpu) & ISR_I]

    def quiet(self):
        """Masked and lowered"""
        self.p.write32(self.regs["mask_set"], self.regs["bit"])
        self.p.write32(self.regs["sw_clr"], self.regs["bit"])

    def raise_unmasked(self):
        self.p.write32(self.regs["sw_set"], self.regs["bit"])
        self.p.write32(self.regs["mask_clr"], self.regs["bit"])


def probe(parked, trials, waits=(0.0, 0.1, 1.0)):
    """Raise the line `trials` times. Each time: who has an IRQ at each of `waits` seconds, with
    nobody reading the event; then the event read on each of them, the first again if there
    are several; then who still has one. Returns a list of dictionaries, one per trial, or
    None if a core had an IRQ before anything was raised (then nothing is raised)."""
    parked.quiet()
    time.sleep(0.01)
    before = parked.with_irq()
    print(f"AIC, the line lowered and masked: cores with an IRQ {before}")
    if before:
        print("  an IRQ that is not of this test: it is left alone, and nothing is raised")
        return None
    results = []
    for n in range(trials):
        parked.raise_unmasked()
        seen = []
        for wait in waits:
            time.sleep(wait)
            seen.append(parked.with_irq())
        events = [(cpu, parked.event(cpu)) for cpu in seen[-1]]
        after = parked.with_irq()
        parked.quiet()
        time.sleep(0.005)
        left = parked.with_irq()
        results.append({"seen": seen, "events": events, "after": after, "left": left})
        print(f"  raised #{n}: cores with an IRQ after {', '.join(f'{w:g}' for w in waits)} s (one "
            f"after the other): {seen}; events " +
            " ".join(f"CPU {c}: 0x{e:x}" for c, e in events) +
            f"; after that {after}; lowered and masked {left}")
        sys.stdout.flush()
        if left:
            print("  an IRQ is left after the line has been lowered and masked: stopping here")
            break
    counts = {}
    for r in results:
        key = tuple(r["seen"][0])
        counts[key] = counts.get(key, 0) + 1
    print("Cores with an IRQ right after the raise, and how often: " +
        ", ".join(f"{list(k)}: {v}" for k, v in sorted(counts.items())))
    return results
