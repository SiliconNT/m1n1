// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for how src/hv/hv_vgic.c forwards an AIC line as an SPI: one line of the
 * table of AIC lines and SPIs (hv_aicmap()), raised and lowered by the guest itself with the
 * software trigger of AIC, which stands in for a device that holds the line high.
 *
 * The loader (run_sgitest.py --aic-line) writes into the image header where the software
 * trigger of the line is set and cleared, its bit, and the SPI the table has for it. What
 * has to hold (SiliconNT docs/plans/0003, "做完的标准"):
 *
 *   - raised while the guest has not enabled its SPI: nothing arrives
 *   - enabled: the SPI arrives, with the number of the table
 *   - still raised when the guest ends it: it arrives again; lowered: no more
 *   - disabled while raised: nothing arrives; enabled again: it does
 *   - disabled by the guest while it handles it: nothing more after the end, though raised
 *
 * The guest never reads AIC: that would take events away from the hypervisor. It runs on the
 * boot CPU only and prints on the hypervisor's virtual UART; lines start with "HVLOG: " so
 * that the hypervisor copies them to its own console. It ends with PSCI SYSTEM_OFF, which
 * stops it for good (src/hv/hv_psci.c). Entry points, vectors and the header are those of
 * sgitest (sgitest-head.S). Nothing here waits with WFI.
 */

#define GUEST_NAME "aictest"

#include "gic.h"
#include "sgitest.h"
#include "guest.h"

/* Written by the loader, see sgitest-head.S */
extern u64 hdr_aic_set;
extern u64 hdr_aic_clr;
extern u64 hdr_aic_bit;
extern u64 hdr_aic_spi;

void mmio_write32(u64 addr, u32 val);

#define write_sysreg(r, v) asm volatile("msr " #r ", %0" : : "r"((u64)(v)))
#define isb()              asm volatile("isb")
#define irq_mask()         asm volatile("msr daifset, #2")
/* SError, IRQ and FIQ on. The FIQ must never come: the vector reports it */
#define irq_unmask() asm volatile("msr daifclr, #7")

/* How long to wait for an interrupt that is to come, and to see that one does not */
#define ARRIVES_MS 200
#define STAYS_MS   100

static u64 ticks_per_ms;
static u64 mpidr;
static u32 spi;
static u32 nr_pass, nr_fail;

/*
 * The device and the guest's handler for its SPI. The MMU is off, so there are no caches to
 * worry about; the handler runs on this CPU only.
 */
static volatile u32 taken;      // the SPI, taken and ended
static volatile u32 other;      // interrupts with another INTID
static volatile u32 other_last; // ...and the last such INTID
static volatile u32 spurious;   // ICC_IAR1_EL1 read 1023
static volatile u32 lower_at;   // lower the line when `taken` reaches this; 0: never
static volatile int disable_in_handler;

/* ---- output ---- */

static void result(const char *name, int ok)
{
    line(ok ? "PASS " : "FAIL ");
    put(name);
    put(" (taken ");
    put_dec(taken);
    put(")\n");
    if (ok)
        nr_pass++;
    else
        nr_fail++;
}

/* ---- time ---- */

static u64 now(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

static void delay_ms(u64 ms)
{
    u64 end = now() + ms * ticks_per_ms;

    while (now() < end)
        ;
}

/* Wait until the SPI has been taken `count` times; false if it has not been by then */
static int wait_taken(u32 count, u64 ms)
{
    u64 end = now() + ms * ticks_per_ms;

    while (taken < count && now() < end)
        ;
    return taken >= count;
}

/* ---- the line and the SPI ---- */

static void line_raise(void)
{
    mmio_write32(hdr_aic_set, hdr_aic_bit);
}

static void line_lower(void)
{
    mmio_write32(hdr_aic_clr, hdr_aic_bit);
}

static void spi_enable(void)
{
    wr32(hdr_gicd_base + GICD_ISENABLER + 4 * (spi / 32), 1U << (spi % 32));
}

static void spi_disable(void)
{
    wr32(hdr_gicd_base + GICD_ICENABLER + 4 * (spi / 32), 1U << (spi % 32));
}

/* ---- exception handlers, called from sgitest-head.S ---- */

void irq_handler(void)
{
    u64 iar = read_sysreg(ICC_IAR1_EL1);
    u32 intid = iar & 0xffffff;

    if (intid == 1023) {
        /* Nothing to acknowledge, no EOI */
        spurious++;
        return;
    }

    if (intid == spi) {
        if (disable_in_handler)
            spi_disable();
        if (lower_at && taken + 1 >= lower_at)
            line_lower();
        taken++;
    } else {
        other++;
        other_last = intid;
    }

    write_sysreg(ICC_EOIR1_EL1, iar);
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

void secondary_main(int slot)
{
    (void)slot;
    for (;;)
        ;
}

/* ---- the tests ---- */

/* The GIC as the Windows kernel sets it up, on this CPU; 0, or what failed */
static int gic_init(void)
{
    u64 gicr, sre;
    int dist = gic_dist_init();

    if (dist)
        return dist;
    mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    gicr = gic_find_redist(mpidr);
    if (!gicr)
        return 10;

    sre = read_sysreg(ICC_SRE_EL1);
    if (!(sre & 1))
        write_sysreg(ICC_SRE_EL1, sre | 1);
    isb();
    write_sysreg(ICC_IGRPEN1_EL1, 0);
    write_sysreg(ICC_PMR_EL1, 0);
    write_sysreg(ICC_BPR1_EL1, 3);
    gic_local_init(gicr);
    write_sysreg(ICC_CTLR_EL1, read_sysreg(ICC_CTLR_EL1) & ~2UL); // EOImode = 0
    write_sysreg(ICC_IGRPEN1_EL1, 1);
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();
    return 0;
}

static void tests(void)
{
    u32 before;

    /* Raised while the SPI is not enabled: AIC keeps the line masked, nothing arrives */
    lower_at = 1;
    line_raise();
    delay_ms(STAYS_MS);
    result("a raised line whose SPI is not enabled does not arrive", taken == 0);

    /* Enabled (level, routed to this CPU): it arrives as the SPI of the table */
    gic_enable_spi(spi, 0x80, mpidr, 0);
    result("enabled, the raised line arrives as the SPI of the table",
           wait_taken(1, ARRIVES_MS) && other == 0);
    delay_ms(STAYS_MS);
    result("lowered in the handler, it arrives once", taken == 1);

    /* Still raised when it is ended: it arrives again, until it is lowered */
    before = taken;
    lower_at = before + 3;
    line_raise();
    result("still raised at the end, it arrives again, three times in all",
           wait_taken(before + 3, ARRIVES_MS));
    delay_ms(STAYS_MS);
    result("...and no more once it is lowered", taken == before + 3);

    /* Disabled while raised: nothing; enabled again: it arrives */
    spi_disable();
    before = taken;
    lower_at = before + 1;
    line_raise();
    delay_ms(STAYS_MS);
    result("disabled, the raised line does not arrive", taken == before);
    spi_enable();
    result("enabled again, it arrives", wait_taken(before + 1, ARRIVES_MS));
    delay_ms(STAYS_MS);
    result("...once", taken == before + 1);

    /* Disabled by the guest while it handles it: nothing after the end, though still raised */
    before = taken;
    lower_at = 0;
    disable_in_handler = 1;
    line_raise();
    result("disabled in the handler, it arrives once", wait_taken(before + 1, ARRIVES_MS));
    delay_ms(STAYS_MS);
    result("...and not again, though it is still raised", taken == before + 1);
    disable_in_handler = 0;
    lower_at = before + 2;
    spi_enable();
    result("enabled again, the line that is still raised arrives",
           wait_taken(before + 2, ARRIVES_MS));
    delay_ms(STAYS_MS);
    result("...once", taken == before + 2);

    spi_disable();
    line_lower();
    result("no interrupt arrived that no test asked for", other == 0);
}

/* PSCI SYSTEM_OFF: the hypervisor stops the guest for good and does not come back */
static void system_off(void)
{
    register u64 x0 asm("x0") = 0x84000008UL;

    asm volatile("hvc #0" : "+r"(x0) : : "memory");
}

void boot_main(void)
{
    int gic;

    ticks_per_ms = read_sysreg(cntfrq_el0) / 1000;
    spi = hdr_aic_spi;
    line("start, SPI ");
    put_dec(spi);
    put(", software trigger at 0x");
    put_hex(hdr_aic_set, 12);
    put(" and 0x");
    put_hex(hdr_aic_clr, 12);
    put(", bit 0x");
    put_hex(hdr_aic_bit, 8);
    put("\n");

    if (!hdr_aic_set || !hdr_aic_clr || !hdr_aic_bit || spi < 32 || spi > 1019) {
        line("FAIL the loader did not say which line (run_sgitest.py --aic-line)\n");
        nr_fail++;
        goto done;
    }
    gic = gic_init();
    if (gic) {
        line("FAIL the GIC is not usable, check ");
        put_dec(gic);
        put("\n");
        nr_fail++;
        goto done;
    }
    irq_unmask();

    tests();

    line("spurious (ICC_IAR1_EL1 read 1023) = ");
    put_dec(spurious);
    put(", other INTIDs: ");
    put_dec(other);
    if (other) {
        put(", the last ");
        put_dec(other_last);
    }
    put("\n");

done:
    irq_mask();
    line("DONE pass fail = ");
    put_dec(nr_pass);
    put(" ");
    put_dec(nr_fail);
    put("\n");
    system_off();
    for (;;)
        ;
}
