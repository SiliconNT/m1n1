/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A device tree for the files under test: the functions of src/adt.h that they call, over a
 * table of the nodes they look at, as the M4 has them (R35, adt-1.bin: SiliconNT
 * docs/findings/0067, evidence 5). For the model tests of this directory; nothing in it needs
 * more of the C library than memcpy(), strcmp(), strncmp() and strlen().
 *
 * adt_get_reg() answers with the addresses of the CPU that the table has, where the devices
 * of the test are: there are no ranges to go through. adt_getprop_copy() is as
 * rust/src/adt.rs has it: an error unless the property is exactly as long as what it is
 * copied to, and a property that is longer would be copied past its end there, which here
 * stops the test (fake_adt_overrun()).
 */

#ifndef FAKE_ADT_H
#define FAKE_ADT_H

#include "adt.h"

#define FAKE_ADT_MAX_PROPS 8
#define FAKE_ADT_MAX_REGS  3

struct fake_prop {
    const char *name; /* NULL: not there */
    const void *value;
    u32 len;
};

struct fake_node {
    const char *name;
    int parent; /* index in fake_adt_nodes, -1 for the root */
    struct fake_prop props[FAKE_ADT_MAX_PROPS];
    u64 reg[FAKE_ADT_MAX_REGS][2]; /* where it is for the CPU, and how big */
    int nregs;
};

/* The nodes, by index; the offset of a node is FAKE_ADT_OFFSET(index) */
enum {
    FAKE_ROOT,
    FAKE_ARM_IO,
    FAKE_SMC,
    FAKE_SMC_NUB,
    FAKE_SPMI_A1,
    FAKE_HPM3,
    FAKE_SPMI0,
    FAKE_PMU_MAIN,
    FAKE_BTM,
    FAKE_SPMI1,
    FAKE_PMU_AUX,
    FAKE_WDT,
    FAKE_NODES,
};
#define FAKE_ADT_OFFSET(index) (0x100 * ((index) + 1))

/* Where the devices are for the CPU: the test decides, before fake_adt_reset() */
struct fake_adt_places {
    u64 smc, smc_size; /* reg 0 of /arm-io/smc: the coprocessor; its mailbox is 0x8000 in */
    u64 spmi_a1, spmi0, spmi1;
    u64 wdt, wdt_second;         /* reg 0 and reg 2 of /arm-io/wdt */
    u64 smc_sram, smc_sram_size; /* region-base, region-size of iop-smc-nub: its memory */
};

extern struct fake_node fake_adt_nodes[FAKE_NODES];

/* The table as the M4 has it, with the devices at those places */
void fake_adt_reset(const struct fake_adt_places *where);
/* A property of a node changed, added (value not NULL) or taken away (value NULL) */
void fake_adt_set(int index, const char *name, const void *value, u32 len);

/* The test's: called where m1n1 would write past the end of what it copies to */
void fake_adt_overrun(const char *name, u32 len, u32 room);
/* How many lookups of a node that is not in the table there were */
extern int fake_adt_misses;

#endif
