/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The Distributor and Redistributor accesses of the Windows kernel's GICv3 support
 * (ntoskrnl.exe 10.0.26300.9457: HalpGic3ValidateIoUnit, HalpGic3InitializeIoUnit,
 * HalpGic3FindRedistributor, HalpGic3InitializeLocalUnit, HalpGic3SetLineState), in the same
 * order and with the same access widths, for the test guests.
 *
 * The loader writes the two base addresses into the image header (sgitest-head.S).
 */

#ifndef GIC_H
#define GIC_H

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

extern u64 hdr_gicd_base;
extern u64 hdr_gicr_base;

#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ISPENDR    0x0200
#define GICD_IPRIORITYR 0x0400
#define GICD_ICFGR      0x0c00
#define GICD_IROUTER    0x6000
#define GICD_PIDR2      0xffe8

#define GICR_TYPER      0x0008
#define GICR_STRIDE     0x20000 /* GICv3: RD_base and SGI_base */
#define GICR_IGROUPR0   0x10080
#define GICR_ISENABLER0 0x10100
#define GICR_ICENABLER0 0x10180
#define GICR_ISPENDR0   0x10200
#define GICR_IPRIORITYR 0x10400
#define GICR_ICFGR0     0x10c00
#define GICR_ICFGR1     0x10c04

/* Single loads and stores of the width named, which is what the hypervisor has to emulate */
static inline u8 rd8(u64 addr)
{
    return *(volatile u8 *)addr;
}

static inline u32 rd32(u64 addr)
{
    return *(volatile u32 *)addr;
}

static inline u64 rd64(u64 addr)
{
    return *(volatile u64 *)addr;
}

static inline void wr8(u64 addr, u8 val)
{
    *(volatile u8 *)addr = val;
}

static inline void wr32(u64 addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline void wr64(u64 addr, u64 val)
{
    *(volatile u64 *)addr = val;
}

/* What GICR_TYPER[63:32] and GICD_IROUTER hold for a CPU with this MPIDR_EL1 */
static inline u64 gic_affinity(u64 mpidr)
{
    return (((mpidr >> 32) & 0xff) << 24) | (mpidr & 0xffffff);
}

/*
 * HalpGic3ValidateIoUnit and HalpGic3InitializeIoUnit, with the MADT saying "GIC version 3".
 * Returns 0, or the number of the check that failed (Windows would not start).
 */
static inline int gic_dist_init(void)
{
    u64 gicd = hdr_gicd_base;
    u32 pidr2 = rd32(gicd + GICD_PIDR2);
    u32 typer = rd32(gicd + GICD_TYPER);
    u32 lines = ((typer & 0x1f) + 1) * 32;
    u32 ctlr;

    if ((pidr2 & 0xf0) < 0x30)
        return 1; /* ArchRev */

    /* SecurityExtn is 0 on everything this runs on: a single Security state */
    if (typer & (1 << 10))
        return 2;

    ctlr = rd32(gicd + GICD_CTLR);
    if (!(ctlr & (1 << 6)))
        return 3; /* DS */
    if (ctlr & (1 << 0)) {
        ctlr &= ~(1U << 0);
        wr32(gicd + GICD_CTLR, ctlr);
    }
    if (ctlr & (1 << 1)) {
        ctlr &= ~(1U << 1);
        wr32(gicd + GICD_CTLR, ctlr);
    }
    if (!(ctlr & (1 << 4))) {
        wr32(gicd + GICD_CTLR, ctlr | (1 << 4));
        ctlr = rd32(gicd + GICD_CTLR);
        if (!(ctlr & (1 << 4)))
            return 4; /* ARE */
    }

    for (u32 n = 1; n < lines / 32; n++) {
        wr32(gicd + GICD_ICENABLER + 4 * n, 0xffffffff);
        wr32(gicd + GICD_IGROUPR + 4 * n, 0xffffffff);
    }
    for (u32 n = 32; n < lines && n < 1020; n++)
        wr64(gicd + GICD_IROUTER + 8 * n, 0);

    wr32(gicd + GICD_CTLR, ctlr | (1 << 1));
    return 0;
}

/* HalpGic3FindRedistributor: the frame whose GICR_TYPER has this affinity, 0 if there is none */
static inline u64 gic_find_redist(u64 mpidr)
{
    u64 gicr = hdr_gicr_base;

    for (;;) {
        u64 typer = rd64(gicr + GICR_TYPER);

        if ((typer >> 32) == gic_affinity(mpidr))
            return gicr;
        if (typer & (1 << 4)) /* Last */
            return 0;
        gicr += GICR_STRIDE;
    }
}

/* The Redistributor part of HalpGic3InitializeLocalUnit */
static inline void gic_local_init(u64 gicr)
{
    wr32(gicr + GICR_IGROUPR0, 0xffffffff);
    wr32(gicr + GICR_ICENABLER0, ~0xff00U); /* SGIs 8-15 are left to the firmware */
}

/* HalpGic3SetLineState for an SGI or a PPI: priority by the byte, trigger, enable */
static inline void gic_enable_local(u64 gicr, u32 intid, u8 priority)
{
    u64 cfg = gicr + (intid < 16 ? GICR_ICFGR0 : GICR_ICFGR1);

    wr8(gicr + GICR_IPRIORITYR + intid, priority);
    wr32(cfg, rd32(cfg) & ~(2U << (2 * (intid % 16))));
    wr32(gicr + GICR_ISENABLER0, 1U << intid);
}

/* ...and for an SPI, with the CPU it goes to */
static inline void gic_enable_spi(u32 intid, u8 priority, u64 mpidr, int edge)
{
    u64 gicd = hdr_gicd_base;
    u64 cfg = gicd + GICD_ICFGR + 4 * (intid / 16);
    u32 bit = 2U << (2 * (intid % 16));

    wr8(gicd + GICD_IPRIORITYR + intid, priority);
    wr32(cfg, edge ? rd32(cfg) | bit : rd32(cfg) & ~bit);
    wr64(gicd + GICD_IROUTER + 8 * intid, gic_affinity(mpidr));
    wr32(gicd + GICD_ISENABLER + 4 * (intid / 32), 1U << (intid % 32));
}

#endif
