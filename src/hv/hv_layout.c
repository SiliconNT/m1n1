/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * What is where in the physical address space of a guest.
 *
 * A guest has the devices and the memory of the machine at the addresses they have. Where the
 * devices are is in the device tree, in the ranges of /arm-io; where the memory is, in the
 * boot arguments. Both are read when a guest is started, for the machine m1n1 runs on.
 *
 * The Distributor and the Redistributors of the GIC the guest is shown (hv_vgic.c) are at
 * two ranges that are the same on every machine (hv.h). That takes a machine that has
 * nothing of its own there, and hv_vgic_ranges_free() finds out whether this one is.
 *
 * Tested on the build machine by siliconnt/selfboot-model (layout_test.c, run_test.c).
 */

#include "hv.h"
#include "adt.h"
#include "memory.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

#define ARM_IO "/arm-io"

/* A number of one or two cells of a property */
static u64 cells(const u8 *p, u32 count)
{
    u64 value = 0;

    memcpy(&value, p, 4 * count);
    return value;
}

/* An entry of "ranges": the address on the bus, the address of the CPU, the size */
static u32 entry_size(const struct hv_io_ranges *it)
{
    return 4 * (it->bus_cells + it->cpu_cells + it->size_cells);
}

/*
 * How many cells a number has: a property of 4 bytes. Not with ADT_GETPROP(), which copies
 * all of a property before it looks at how long it is (adt_getprop_copy() of rust/src/adt.rs).
 */
static int cell_count(int node, const char *name, u32 *count)
{
    u32 len;
    const void *value = adt_getprop(adt, node, name, &len);

    if (!value || len != sizeof(*count))
        return -1;
    memcpy(count, value, sizeof(*count));
    return 0;
}

int hv_io_ranges(struct hv_io_ranges *it)
{
    int node = adt_path_offset(adt, ARM_IO);
    const u8 *ranges;
    u32 len;

    ranges = node < 0 ? NULL : adt_getprop(adt, node, "ranges", &len);
    if (!ranges || cell_count(node, "#address-cells", &it->bus_cells) < 0 ||
        cell_count(node, "#size-cells", &it->size_cells) < 0 ||
        cell_count(0, "#address-cells", &it->cpu_cells) < 0) {
        printf("HV: " ARM_IO " of the device tree has no ranges\n");
        return -1;
    }
    if (!it->bus_cells || it->bus_cells > 2 || !it->cpu_cells || it->cpu_cells > 2 ||
        !it->size_cells || it->size_cells > 2) {
        printf("HV: the ranges of " ARM_IO " have addresses of %d and %d cells and sizes of %d\n",
               it->bus_cells, it->cpu_cells, it->size_cells);
        return -1;
    }
    /*
     * Not one whole entry: then this is not what says where the devices are. Neither is it
     * with the top bit of its length set, which adt_getprop() passes on and nothing here
     * knows the meaning of. What follows the last whole entry is not looked at, as in
     * mmu_map_mmio() of memory.c.
     */
    if (len < entry_size(it) || len & 0x80000000) {
        printf("HV: the ranges of " ARM_IO " are 0x%x bytes, which is not entries of %d\n", len,
               entry_size(it));
        return -1;
    }
    it->next = ranges;
    it->end = ranges + len;
    return 0;
}

bool hv_io_ranges_next(struct hv_io_ranges *it, u64 *base, u64 *size)
{
    if ((u64)(it->end - it->next) < entry_size(it))
        return false;
    *base = cells(it->next + 4 * it->bus_cells, it->cpu_cells);
    *size = cells(it->next + 4 * (it->bus_cells + it->cpu_cells), it->size_cells);
    it->next += entry_size(it);
    return true;
}

/*
 * All the Redistributors a guest can ever be shown. hv_vgic.c has one for every core up to
 * the last that runs m1n1 when the hypervisor is initialized, which is not known when this
 * is first asked.
 */
#define VGIC_REDIST_SPAN (MAX_CPUS * HV_VGIC_REDIST_SIZE)

static const struct {
    const char *name;
    u64 base, size;
} vgic_ranges[] = {
    {"Distributor", HV_VGIC_DIST_BASE, HV_VGIC_DIST_SIZE},
    {"Redistributors", HV_VGIC_REDIST_BASE, VGIC_REDIST_SPAN},
};

/* The last address of a range that is not empty; of one that wraps around, the highest */
static u64 last_of(u64 base, u64 size)
{
    u64 last = base + size - 1;

    return last < base ? ~0UL : last;
}

/* Something of the machine: says which of the two ranges it lies in, if any */
static bool in_the_way(const char *what, u64 base, u64 size)
{
    bool is = false;

    if (!size)
        return false;
    for (size_t i = 0; i < sizeof(vgic_ranges) / sizeof(vgic_ranges[0]); i++) {
        u64 vgic_base = vgic_ranges[i].base, vgic_size = vgic_ranges[i].size;

        if (base > last_of(vgic_base, vgic_size) || vgic_base > last_of(base, size))
            continue;
        printf("HV: vGIC: the %s, 0x%lx .. 0x%lx, would be where the machine has %s, 0x%lx .. "
               "0x%lx\n",
               vgic_ranges[i].name, vgic_base, vgic_base + vgic_size, what, base, base + size);
        is = true;
    }
    return is;
}

bool hv_vgic_ranges_free(void)
{
    struct hv_io_ranges it;
    u64 base, size;
    u32 count = 0;
    bool clear = true;

    if (hv_io_ranges(&it) < 0)
        return false;
    while (hv_io_ranges_next(&it, &base, &size)) {
        clear &= !in_the_way("a range of " ARM_IO, base, size);
        count++;
    }
    /* All the memory there is, as mmu_add_default_mappings() of memory.c maps it */
    clear &= !in_the_way("memory", ram_base, mem_size_actual);

    if (clear)
        printf("HV: vGIC: none of the %d ranges of " ARM_IO " and no memory (0x%lx .. 0x%lx) "
               "where the guest finds the GIC\n",
               count, ram_base, ram_base + mem_size_actual);
    return clear;
}
