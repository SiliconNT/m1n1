/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/hv/hv_layout.c: the ranges of /arm-io as the hypervisor reads
 * them, and whether the machine has something where the guest finds its GIC.
 *
 * The real file is compiled for the build machine. The device tree functions are those of
 * model.c, over trees this file makes, in the format of Apple's; the memory of the machine is
 * the two variables m1n1 has it in. The test is here, not with those of the other files of
 * src/hv/ (siliconnt/vgic-model), because the device trees are here.
 *
 * The machine that has to pass is the M4 as it was in R21: the ranges of /arm-io that
 * run_firmware.py printed there, and its memory. Everything that has to be refused is that
 * machine with one thing more, or its tree with one thing wrong.
 */
#include <unistd.h>

#include "hv.h"
#include "memory.h"
#include "model.h"
#include "smp.h"
#include "utils.h"

u64 ram_base, mem_size_actual;

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the device tree ---------------------------------------------------------------------- */

struct io {
    u64 bus, cpu, size;
};

/* What a tree is made with; reset() has the M4 */
static struct io io[R21_MMIO_COUNT + 2];
static unsigned io_count;
static u32 bus_cells, cpu_cells, size_cells;
static bool with_arm_io, with_ranges, with_bus_cells, with_size_cells, with_cpu_cells;
static u32 ranges_extra; /* bytes after the last entry */
static bool ranges_flagged; /* the top bit of the length of "ranges" is set */
/* How long the three properties that say how many cells a number has are: 4 bytes, really */
static u32 bus_cells_len, cpu_cells_len, size_cells_len;

static u8 tree[0x2000];

/* A number in cells of 4 bytes; what a third cell would hold is 0 */
static u8 *put_cells(u8 *p, u64 value, u32 cells)
{
    memset(p, 0, 4 * cells);
    memcpy(p, &value, cells < 2 ? 4 * cells : 8);
    return p + 4 * cells;
}

/* A property that says how many cells: the number, and nothing in what follows it if it is
 * made longer than it should be */
static u8 *put_count(u8 *p, const char *name, u32 count, u32 len)
{
    u32 value[4] = {count, 0, 0, 0};

    return put_prop(p, name, value, len);
}

static void make_tree(void)
{
    /* Room for every number in three cells, and for what is put after the last entry */
    static u8 ranges[COUNT(io) * 36 + 32];
    u8 *r = ranges, *p = tree;

    for (unsigned i = 0; i < io_count; i++) {
        r = put_cells(r, io[i].bus, bus_cells);
        r = put_cells(r, io[i].cpu, cpu_cells);
        r = put_cells(r, io[i].size, size_cells);
    }
    memset(r, 0x5a, ranges_extra);
    r += ranges_extra;

    memset(tree, 0, sizeof(tree));
    p = put_node(p, 2 + with_cpu_cells, 1 + with_arm_io);
    p = put_name(p, "device-tree");
    if (with_cpu_cells)
        p = put_count(p, "#address-cells", cpu_cells, cpu_cells_len);
    p = put_prop(p, "#size-cells", &(u32){2}, 4);

    /* A node before the one that is looked for, with a "ranges" that is not the one */
    p = put_node(p, 2, 0);
    p = put_name(p, "chosen");
    p = put_prop(p, "ranges", &(u64[3]){0, HV_VGIC_DIST_BASE, HV_VGIC_DIST_SIZE}, 24);

    if (with_arm_io) {
        p = put_node(p, 1 + with_bus_cells + with_size_cells + with_ranges, 0);
        p = put_name(p, "arm-io");
        if (with_bus_cells)
            p = put_count(p, "#address-cells", bus_cells, bus_cells_len);
        if (with_size_cells)
            p = put_count(p, "#size-cells", size_cells, size_cells_len);
        if (with_ranges) {
            struct adt_property *prop = (struct adt_property *)p;

            p = put_prop(p, "ranges", ranges, r - ranges);
            if (ranges_flagged)
                prop->size |= 0x80000000;
        }
    }
    adt = tree;
}

/* The M4 of R21 */
static void reset(void)
{
    struct boot_args ba;

    for (unsigned i = 0; i < R21_MMIO_COUNT; i++)
        io[i] = (struct io){r21_mmio[i][0] - R21_BUS_OFFSET, r21_mmio[i][0],
                            r21_mmio[i][1] - r21_mmio[i][0]};
    io_count = R21_MMIO_COUNT;
    bus_cells = cpu_cells = size_cells = 2;
    with_arm_io = with_ranges = with_bus_cells = with_size_cells = with_cpu_cells = true;
    bus_cells_len = cpu_cells_len = size_cells_len = 4;
    ranges_extra = 0;
    ranges_flagged = false;
    make_tree();

    r21_ba(&ba);
    ram_base = ba.phys_base & ~0xffffffffUL;
    mem_size_actual = ba.rv3.mem_size_actual;
}

/* One more device, among those of the M4 and not after them */
static void add_device(u64 base, u64 size)
{
    memmove(&io[12], &io[11], (io_count - 11) * sizeof(io[0]));
    io[11] = (struct io){base - R21_BUS_OFFSET, base, size};
    io_count++;
    make_tree();
}

/* ---- what the code under test says -------------------------------------------------------- */

static char said[8192];

/* Asks, and keeps what was printed meanwhile; it is printed here too */
static bool ranges_free(void)
{
    FILE *tmp = tmpfile();
    int out;
    bool ret;
    size_t n;

    fflush(stdout);
    out = dup(STDOUT_FILENO);
    if (!tmp || out < 0 || dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        fprintf(stderr, "layout_test: cannot keep what is printed\n");
        exit(2);
    }
    ret = hv_vgic_ranges_free();
    fflush(stdout);
    dup2(out, STDOUT_FILENO);
    close(out);

    rewind(tmp);
    n = fread(said, 1, sizeof(said) - 1, tmp);
    said[n] = 0;
    fclose(tmp);
    fputs(said, stdout);
    return ret;
}

static bool says(const char *what)
{
    return strstr(said, what) != NULL;
}

static bool says_range(u64 base, u64 end)
{
    char text[64];

    snprintf(text, sizeof(text), "0x%lx .. 0x%lx", base, end);
    return says(text);
}

/* The two ranges, by what the code under test is to call them */
#define DIST        "Distributor"
#define REDIST      "Redistributors"
#define DIST_END    (HV_VGIC_DIST_BASE + HV_VGIC_DIST_SIZE)
#define REDIST_SPAN (MAX_CPUS * HV_VGIC_REDIST_SIZE)
#define REDIST_END  (HV_VGIC_REDIST_BASE + REDIST_SPAN)

static const struct target {
    const char *name, *other;
    u64 base, end;
} targets[] = {
    {DIST, REDIST, HV_VGIC_DIST_BASE, DIST_END},
    {REDIST, DIST, HV_VGIC_REDIST_BASE, REDIST_END},
};

/* ---- the tests ---------------------------------------------------------------------------- */

static void test_the_ranges_of_the_m4(void)
{
    struct hv_io_ranges it;
    u64 base = 0, size = 0;

    reset();
    CHECK_EQ(hv_io_ranges(&it), 0);
    for (unsigned i = 0; i < R21_MMIO_COUNT; i++) {
        CHECK(hv_io_ranges_next(&it, &base, &size));
        CHECK_EQ(base, r21_mmio[i][0]);
        CHECK_EQ(base + size, r21_mmio[i][1]);
    }
    /* After the last one there is none, however often it is asked */
    base = size = 0x1234;
    CHECK(!hv_io_ranges_next(&it, &base, &size));
    CHECK(!hv_io_ranges_next(&it, &base, &size));
    CHECK_EQ(base, 0x1234);
    CHECK_EQ(size, 0x1234);
}

static void test_the_m4_has_nothing_there(void)
{
    reset();
    CHECK(ranges_free());
    /* It says what it looked at: every range, and where the memory is */
    CHECK(says("27 ranges"));
    CHECK(says_range(0x10000000000, 0x10400000000));

    /* The ranges are those of hv.h of this tree: this test knows where they must be */
    CHECK_EQ(HV_VGIC_DIST_BASE, 0x5000000000);
    CHECK_EQ(DIST_END, 0x5000010000);
    CHECK_EQ(HV_VGIC_REDIST_BASE, 0x5100000000);
    CHECK_EQ(REDIST_END, 0x5100400000);

    /* Where a device is on the bus of /arm-io is not where the guest has it */
    io[3].bus = HV_VGIC_DIST_BASE;
    io[4].bus = HV_VGIC_REDIST_BASE;
    make_tree();
    CHECK(ranges_free());
}

static void test_a_device_in_the_way(void)
{
    for (unsigned t = 0; t < COUNT(targets); t++) {
        const struct target *g = &targets[t];
        /* A device: where it starts and ends, and whether that is in the way */
        const struct {
            u64 base, end;
            bool hit;
        } cases[] = {
            {g->base - 0x4000, g->base, false},      /* ends where the range starts */
            {g->base - 0x4000, g->base + 1, true},   /* one byte into it */
            {g->base - 1, g->base, false},           /* the byte before it */
            {g->base, g->base + 1, true},            /* its first byte */
            {g->base + 0x4000, g->base + 0x8000, true}, /* inside */
            {g->base, g->end, true},                 /* all of it */
            {g->base - 0x10000000, g->end + 0x10000000, true}, /* around it */
            {g->end - 1, g->end, true},              /* its last byte */
            {g->end - 1, g->end + 0x4000, true},     /* from its last byte on */
            {g->end, g->end + 0x4000, false},        /* starts where it ends */
            {g->end, g->end + 1, false},             /* the byte after it */
        };

        for (unsigned c = 0; c < COUNT(cases); c++) {
            reset();
            add_device(cases[c].base, cases[c].end - cases[c].base);
            CHECK_EQ(ranges_free(), !cases[c].hit);
            /* Which of the two, where it is, what is in the way and where that is */
            CHECK_EQ(says(g->name), cases[c].hit);
            CHECK(!says(g->other));
            CHECK_EQ(says_range(g->base, g->end), cases[c].hit);
            CHECK_EQ(says("a range of /arm-io"), cases[c].hit);
            CHECK_EQ(says_range(cases[c].base, cases[c].end), cases[c].hit);
            CHECK(!says("memory") || !cases[c].hit);
        }

        /* A device of no size is not there */
        reset();
        add_device(g->base, 0);
        CHECK(ranges_free());
        add_device(g->base + 0x4000, 0);
        CHECK(ranges_free());
    }
}

static void test_every_redistributor_counts(void)
{
    /* Not only those of the cores that run: the last one m1n1 can have a core for */
    reset();
    add_device(HV_VGIC_REDIST_BASE + (MAX_CPUS - 1) * HV_VGIC_REDIST_SIZE, 0x4000);
    CHECK(!ranges_free());
    CHECK(says(REDIST));

    reset();
    add_device(HV_VGIC_REDIST_BASE + (MAX_CPUS - 1) * HV_VGIC_REDIST_SIZE + 0x1c000, 0x4000);
    CHECK(!ranges_free());

    reset();
    add_device(HV_VGIC_REDIST_BASE + MAX_CPUS * HV_VGIC_REDIST_SIZE, 0x4000);
    CHECK(ranges_free());
}

static void test_more_than_one_in_the_way(void)
{
    /* One device over both ranges: both are named */
    reset();
    add_device(0x4000000000, 0x2000000000);
    CHECK(!ranges_free());
    CHECK(says(DIST) && says(REDIST));
    CHECK(says_range(0x4000000000, 0x6000000000));

    /* Two devices, one in each: neither hides the other, wherever they are in the list */
    reset();
    add_device(HV_VGIC_REDIST_BASE + 0x40000, 0x4000);
    io[io_count++] = (struct io){0, HV_VGIC_DIST_BASE + 0x8000, 0x1000};
    make_tree();
    CHECK(!ranges_free());
    CHECK(says(DIST) && says(REDIST));
    CHECK(says_range(HV_VGIC_REDIST_BASE + 0x40000, HV_VGIC_REDIST_BASE + 0x44000));
    CHECK(says_range(HV_VGIC_DIST_BASE + 0x8000, HV_VGIC_DIST_BASE + 0x9000));

    /* The first and the last of the list */
    reset();
    io[0].cpu = HV_VGIC_DIST_BASE;
    make_tree();
    CHECK(!ranges_free());
    reset();
    io[io_count - 1].cpu = HV_VGIC_DIST_BASE;
    make_tree();
    CHECK(!ranges_free());

    /* A device and the memory */
    reset();
    add_device(HV_VGIC_DIST_BASE, 0x4000);
    ram_base = 0x5100000000;
    CHECK(!ranges_free());
    CHECK(says("a range of /arm-io") && says("memory"));
    CHECK(says(DIST) && says(REDIST));
}

static void test_memory_in_the_way(void)
{
    for (unsigned t = 0; t < COUNT(targets); t++) {
        const struct target *g = &targets[t];
        /* 1 GB of memory: less than there is between the two ranges */
        const struct {
            u64 base, end;
            bool hit;
        } cases[] = {
            {g->base - 0x40000000, g->base, false},
            {g->base - 0x40000000, g->base + 0x4000, true},
            {g->base + 0x4000, g->base + 0x8000, true},
            {g->end - 0x4000, g->end + 0x40000000, true},
            {g->end, g->end + 0x40000000, false},
        };

        for (unsigned c = 0; c < COUNT(cases); c++) {
            reset();
            ram_base = cases[c].base;
            mem_size_actual = cases[c].end - cases[c].base;
            CHECK_EQ(ranges_free(), !cases[c].hit);
            CHECK_EQ(says(g->name), cases[c].hit);
            CHECK(!says(g->other));
            CHECK(!says("a range of /arm-io") || !cases[c].hit);
            if (cases[c].hit) {
                CHECK(says("memory"));
                CHECK(says_range(g->base, g->end));
                CHECK(says_range(cases[c].base, cases[c].end));
            }
        }
    }

    /* Memory from below the one to above the other */
    reset();
    ram_base = 0x4000000000;
    mem_size_actual = 0x2000000000;
    CHECK(!ranges_free());
    CHECK(says(DIST) && says(REDIST));

    /* A machine that says it has no memory has none in the way */
    reset();
    ram_base = HV_VGIC_DIST_BASE;
    mem_size_actual = 0;
    CHECK(ranges_free());
}

static void test_ranges_that_wrap_around(void)
{
    /* A range that goes past the last address there is counts up to there */
    reset();
    add_device(0x4000000000, 0xfffffff000000000);
    CHECK(!ranges_free());
    CHECK(says(DIST) && says(REDIST));

    reset();
    add_device(0x6000000000, 0xffffffffffffffff);
    CHECK(ranges_free());

    reset();
    add_device(0xfffffffffffff000, 0x2000);
    CHECK(ranges_free());

    reset();
    ram_base = HV_VGIC_REDIST_BASE + 0x4000;
    mem_size_actual = 0xffffffffffffffff;
    CHECK(!ranges_free());
    CHECK(says(REDIST) && !says(DIST));
}

static void test_cells_of_other_sizes(void)
{
    struct hv_io_ranges it;
    u64 base, size;

    /* The bus and the sizes in one cell, as on a bus of 32 bits: the devices are where the
     * CPU has them */
    reset();
    bus_cells = size_cells = 1;
    io[0] = (struct io){0x10000000, 0x2f0000000, 0x8000000};
    io[1] = (struct io){0x20000000, HV_VGIC_DIST_BASE - 0x8000000, 0x8000000};
    io_count = 2;
    make_tree();
    CHECK_EQ(hv_io_ranges(&it), 0);
    CHECK(hv_io_ranges_next(&it, &base, &size));
    CHECK_EQ(base, 0x2f0000000);
    CHECK_EQ(size, 0x8000000);
    CHECK(hv_io_ranges_next(&it, &base, &size));
    CHECK_EQ(base, HV_VGIC_DIST_BASE - 0x8000000);
    CHECK_EQ(size, 0x8000000);
    CHECK(!hv_io_ranges_next(&it, &base, &size));
    CHECK(ranges_free());
    CHECK(says("2 ranges"));

    /* The same, 64 KB further up */
    io[1].cpu = HV_VGIC_DIST_BASE - 0x7ff0000;
    make_tree();
    CHECK(!ranges_free());
    CHECK(says(DIST) && !says(REDIST));
    CHECK(says_range(HV_VGIC_DIST_BASE - 0x7ff0000, HV_VGIC_DIST_BASE + 0x10000));

    /* Addresses of the CPU in one cell: none of them reaches */
    reset();
    cpu_cells = 1;
    io[0] = (struct io){HV_VGIC_DIST_BASE, 0xf0000000, 0x10000000};
    io[1] = (struct io){HV_VGIC_REDIST_BASE, 0x80000000, 0x5100000000};
    io_count = 2;
    make_tree();
    CHECK_EQ(hv_io_ranges(&it), 0);
    CHECK(hv_io_ranges_next(&it, &base, &size));
    CHECK_EQ(base, 0xf0000000);
    CHECK_EQ(size, 0x10000000);
    CHECK(hv_io_ranges_next(&it, &base, &size));
    CHECK_EQ(base, 0x80000000);
    CHECK_EQ(size, 0x5100000000);
    CHECK(!hv_io_ranges_next(&it, &base, &size));
    /* ...but the second one is that large */
    CHECK(!ranges_free());
    io[1].size = 0x1000;
    make_tree();
    CHECK(ranges_free());
}

/* A tree that does not say where the devices are: then nothing is known to be free */
static void refused(void)
{
    struct hv_io_ranges it;

    make_tree();
    CHECK_EQ(hv_io_ranges(&it), -1);
    CHECK(!ranges_free());
    CHECK(says("/arm-io"));
    CHECK(!says(DIST) && !says(REDIST));
}

static void test_trees_that_do_not_say(void)
{
    bool *const parts[] = {&with_arm_io, &with_ranges, &with_bus_cells, &with_size_cells,
                           &with_cpu_cells};
    u32 *const counts[] = {&bus_cells, &cpu_cells, &size_cells};

    for (unsigned i = 0; i < COUNT(parts); i++) {
        reset();
        *parts[i] = false;
        refused();
    }
    /* Numbers of no cells, and of more cells than a number of 64 bits has */
    for (unsigned i = 0; i < COUNT(counts); i++) {
        reset();
        *counts[i] = 0;
        refused();
        reset();
        *counts[i] = 3;
        refused();
    }
    /* A count of cells that is not a number of 32 bits, though it starts with one */
    u32 *const lengths[] = {&bus_cells_len, &cpu_cells_len, &size_cells_len};

    for (unsigned i = 0; i < COUNT(lengths); i++) {
        reset();
        *lengths[i] = 8;
        refused();
        reset();
        *lengths[i] = 2;
        refused();
        reset();
        *lengths[i] = 0;
        refused();
    }
    /* No entry, and less than one */
    reset();
    io_count = 0;
    refused();
    for (u32 extra = 4; extra < 24; extra += 4) {
        reset();
        io_count = 0;
        ranges_extra = extra;
        refused();
    }
    /* A length with its top bit set: nothing is read by it */
    reset();
    ranges_flagged = true;
    refused();
}

/* As everything of m1n1 that reads "ranges" has it: part of an entry after the last whole
 * one is not looked at */
static void test_what_follows_the_last_entry(void)
{
    struct hv_io_ranges it;
    u64 base, size;

    for (u32 extra = 4; extra < 24; extra += 4) {
        unsigned n = 0;

        reset();
        ranges_extra = extra;
        make_tree();
        CHECK_EQ(hv_io_ranges(&it), 0);
        while (hv_io_ranges_next(&it, &base, &size))
            n++;
        CHECK_EQ(n, R21_MMIO_COUNT);
        CHECK_EQ(base + size, r21_mmio[R21_MMIO_COUNT - 1][1]);
        CHECK(ranges_free());
        CHECK(says("27 ranges"));

        /* ...and what is in the last whole one counts */
        io[io_count - 1].cpu = HV_VGIC_REDIST_BASE;
        make_tree();
        CHECK(!ranges_free());
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_the_ranges_of_the_m4();
    test_the_m4_has_nothing_there();
    test_a_device_in_the_way();
    test_every_redistributor_counts();
    test_more_than_one_in_the_way();
    test_memory_in_the_way();
    test_ranges_that_wrap_around();
    test_cells_of_other_sizes();
    test_trees_that_do_not_say();
    test_what_follows_the_last_entry();

    printf("layout_test: %d checks, %d failed\n", checks, failed);
    if (!failed)
        printf("ALL OK\n");
    return failed ? 1 : 0;
}
