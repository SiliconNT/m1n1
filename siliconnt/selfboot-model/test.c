/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/selfboot.c.
 *
 * The real file is compiled for the build machine. What it gets from the rest of m1n1 is
 * stood in for here: the device tree functions of src/adt.h it calls, over a device tree
 * this file makes, in the format of Apple's (a node is two counts, its properties, its
 * children; a property is a name of 32 bytes, a length, the value padded to 4 bytes).
 *
 * The numbers the layout is checked against are those of a run on the M4 (R21): what
 * run_firmware.py printed there, and what the firmware printed that it found.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "selfboot.h"
#include "types.h"
#include "utils.h"

static int checks, failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failed++;                                                                              \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        u64 _a = (a), _b = (b);                                                                    \
        checks++;                                                                                  \
        if (_a != _b) {                                                                            \
            failed++;                                                                              \
            printf("FAILED %s:%d: %s is 0x%lx, not 0x%lx\n", __FILE__, __LINE__, #a, _a, _b);      \
        }                                                                                          \
    } while (0)

/* ---- the device tree functions of src/adt.h that selfboot.c calls ---------------------- */

void *adt;

#define PROP_SIZE(p) ((p)->size & 0x7fffffff)

static const struct adt_node_hdr *node_at(int offset)
{
    return (const struct adt_node_hdr *)((u8 *)adt + offset);
}

int adt_get_property_count(const void *dt, int offset)
{
    return node_at(offset)->property_count;
}

int adt_first_property_offset(const void *dt, int offset)
{
    return offset + sizeof(struct adt_node_hdr);
}

int adt_next_property_offset(const void *dt, int offset)
{
    const struct adt_property *p = ADT_PROP(adt, offset);
    return offset + sizeof(*p) + ((PROP_SIZE(p) + 3) & ~3);
}

const struct adt_property *adt_get_property(const void *dt, int nodeoffset, const char *name)
{
    int off = adt_first_property_offset(dt, nodeoffset);
    for (int i = adt_get_property_count(dt, nodeoffset); i; i--) {
        const struct adt_property *p = ADT_PROP(adt, off);
        if (!strcmp(p->name, name))
            return p;
        off = adt_next_property_offset(dt, off);
    }
    return NULL;
}

/* The offset after the node at `offset` and everything below it */
static int node_end(int offset)
{
    int off = adt_first_property_offset(adt, offset);
    for (int i = node_at(offset)->property_count; i; i--)
        off = adt_next_property_offset(adt, off);
    for (int i = node_at(offset)->child_count; i; i--)
        off = node_end(off);
    return off;
}

int adt_path_offset(const void *dt, const char *path)
{
    int offset = 0;

    while (*path == '/')
        path++;
    while (*path) {
        size_t len = strcspn(path, "/");
        int child = adt_first_property_offset(adt, offset);
        int i;

        for (i = node_at(offset)->property_count; i; i--)
            child = adt_next_property_offset(adt, child);
        for (i = node_at(offset)->child_count; i; i--) {
            const struct adt_property *name = adt_get_property(adt, child, "name");
            if (name && strlen((const char *)name->value) == len &&
                !memcmp(name->value, path, len))
                break;
            child = node_end(child);
        }
        if (!i)
            return -1;
        offset = child;
        path += len;
        while (*path == '/')
            path++;
    }
    return offset;
}

/* ---- making a device tree ---------------------------------------------------------------- */

static u8 *put_node(u8 *p, u32 properties, u32 children)
{
    struct adt_node_hdr hdr = {properties, children};
    memcpy(p, &hdr, sizeof(hdr));
    return p + sizeof(hdr);
}

static u8 *put_prop(u8 *p, const char *name, const void *value, u32 len)
{
    struct adt_property *prop = (struct adt_property *)p;
    memset(prop, 0, sizeof(*prop) + ((len + 3) & ~3));
    strncpy(prop->name, name, sizeof(prop->name) - 1);
    prop->size = len;
    memcpy(prop->value, value, len);
    return p + sizeof(*prop) + ((len + 3) & ~3);
}

static u8 *put_name(u8 *p, const char *name)
{
    return put_prop(p, "name", name, strlen(name) + 1);
}

static u8 *put_range(u8 *p, const char *name, u64 base, u64 size)
{
    u64 value[2] = {base, size};
    return put_prop(p, name, value, sizeof(value));
}

/* What iBoot lists that says nothing: neither a base nor a size */
#define NOTHING 0xffffffffffffffffUL

/* One more entry of /chosen/memory-map, for the tests that need one in the way */
static const char *extra_name;
static u64 extra_base, extra_size;
/* How many of iBoot's spare entries, MemoryMapReserved-n with base and size 0, there are */
static int spare_entries = 3;
static bool with_device_tree_entry = true;

static u8 tree[0x4000];
static u32 tree_size;

/* The start of the tree of the M4 as far as selfboot.c looks at it, with entries of R21 */
static void make_tree(void)
{
    u8 *p = tree;
    char name[32];
    int entries = 8 + spare_entries + (extra_name ? 1 : 0) + (with_device_tree_entry ? 1 : 0);

    memset(tree, 0, sizeof(tree));
    p = put_node(p, 2, 2);
    p = put_name(p, "device-tree");
    p = put_prop(p, "model", "Mac16,1", 8);

    /* A node before the one that is looked for, with a child of its own */
    p = put_node(p, 1, 1);
    p = put_name(p, "cpus");
    p = put_node(p, 2, 0);
    p = put_name(p, "cpu0");
    p = put_prop(p, "state", "running", 8);

    p = put_node(p, 2, 1);
    p = put_name(p, "chosen");
    p = put_prop(p, "chip-id", &(u32){0x8132}, 4);

    p = put_node(p, entries, 0);
    p = put_name(p, "memory-map");
    p = put_range(p, "AuxKC", NOTHING, NOTHING);
    p = put_range(p, "BootArgs", 0x100061ac000, 0x4000);
    p = put_range(p, "BootKC-entry", 0x9588800, 0);
    if (with_device_tree_entry)
        p = put_range(p, "DeviceTree", 0x10005490000, 0x84000);
    p = put_range(p, "Kernel-mach_header", 0x10005588000, 0);
    for (int i = 0; i < spare_entries; i++) {
        snprintf(name, sizeof(name), "MemoryMapReserved-%d", i);
        p = put_range(p, name, 0, 0);
    }
    p = put_range(p, "SEPFW", 0x10005a6c000, 0x5d0000);
    p = put_range(p, "TrustCache", 0x10006138000, 0x74000);
    if (extra_name)
        p = put_range(p, extra_name, extra_base, extra_size);
    /* Not a range: selfboot.c is to pass over it */
    p = put_prop(p, "odd", "x", 2);

    tree_size = p - tree;
    adt = tree;
}

static void default_tree(void)
{
    extra_name = NULL;
    spare_entries = 3;
    with_device_tree_entry = true;
    make_tree();
}

/* ---- the M4 of R21 ----------------------------------------------------------------------- */

#define R21_PHYS_BASE 0x100189a8000UL /* the top of the heap the host kept */
#define R21_MEM_TOP   0x103db610000UL
#define R21_IMAGE     0x1e00000UL
#define R21_DISK      0x28200000UL

/* Through a pointer, and compared with memcmp(): the padding of the structure is 0 too */
static void r21_ba(struct boot_args *out)
{
    static struct boot_args ba;

    memset(&ba, 0, sizeof(ba));
    ba.revision = 3;
    ba.version = 2;
    ba.virt_base = 0xfffffe0007004000;
    ba.phys_base = 0x10002e70000;
    ba.mem_size = R21_MEM_TOP - 0x10002e70000;
    ba.top_of_kernel_data = 0x10006ab0000;
    ba.video.base = 0x103e004c000;
    ba.video.display = 1;
    ba.video.stride = 0x2f40;
    ba.video.width = 0xbd0;
    ba.video.height = 0x7ac;
    ba.video.depth = 0x1001e;
    ba.machine_type = 0;
    ba.devtree = (void *)0xfffffe0009624000;
    ba.devtree_size = 0x84000;
    strcpy(ba.rv3.cmdline, "-v");
    ba.rv3.boot_flags = 0x10;
    ba.rv3.mem_size_actual = 0x400000000;
    memcpy(out, &ba, sizeof(ba));
}

static void test_layout_of_r21(void)
{
    struct selfboot_layout l;
    struct boot_args ba;

    r21_ba(&ba);
    default_tree();
    memset(&l, 0xee, sizeof(l));
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), 0);
    CHECK_EQ(l.phys_base, R21_PHYS_BASE);
    CHECK_EQ(l.mem_top, R21_MEM_TOP);
    CHECK_EQ(l.adt_base, 0x1001a9a8000);
    CHECK_EQ(l.adt_size, 0x84000);
    CHECK_EQ(l.image_base, 0x1001ac00000);
    CHECK_EQ(l.uefi_base, 0x103d3610000);
    /* The image may reach to one page, for the boot arguments, below the work area */
    CHECK_EQ(selfboot_image_room(&l), 0x103d3610000 - 0x1001ac00000 - 0x4000);

    CHECK_EQ(selfboot_layout_image(&l, R21_IMAGE), 0);
    CHECK_EQ(l.image_size, R21_IMAGE);
    CHECK_EQ(l.bootargs, 0x1001ca00000);
    CHECK_EQ(l.disk_base, 0x1001cc00000);
    CHECK_EQ(selfboot_disk_room(&l), 0x103d3610000 - 0x1001cc00000);

    CHECK_EQ(selfboot_layout_disk(&l, R21_DISK), 0);
    CHECK_EQ(l.disk_size, R21_DISK);
    CHECK_EQ(l.top, 0x10044e00000);
}

static void test_layout_rounds(void)
{
    struct selfboot_layout l;
    struct boot_args ba;

    r21_ba(&ba);
    default_tree();
    /* A device tree that is not a whole number of pages gets whole pages */
    ba.devtree_size = 0x80001;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), 0);
    CHECK_EQ(l.adt_size, 0x84000);

    /* One that ends on a 2 MB boundary: the image starts there */
    CHECK_EQ(selfboot_layout_init(&l, 0x10018000000, &ba), 0);
    CHECK_EQ(l.adt_base, 0x1001a000000);
    ba.devtree_size = 0x200000;
    CHECK_EQ(selfboot_layout_init(&l, 0x10018000000, &ba), 0);
    CHECK_EQ(l.image_base, 0x1001a200000);

    /* An image that ends one page below a 2 MB boundary: the boot arguments fill it up, the
     * disk starts at the boundary */
    CHECK_EQ(selfboot_layout_image(&l, 0x200000 - 0x4000), 0);
    CHECK_EQ(l.bootargs, 0x1001a3fc000);
    CHECK_EQ(l.disk_base, 0x1001a400000);
    /* A disk that is whole 4 KB pages, not whole 16 KB pages */
    CHECK_EQ(selfboot_layout_disk(&l, 0x5000), 0);
    CHECK_EQ(l.top, 0x1001a408000);
}

static void test_layout_refuses_what_does_not_fit(void)
{
    struct selfboot_layout l;
    struct boot_args ba;
    u64 room;

    r21_ba(&ba);
    default_tree();
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), 0);
    room = selfboot_image_room(&l);
    CHECK_EQ(selfboot_layout_image(&l, room + 0x4000), -1);
    CHECK_EQ(selfboot_layout_image(&l, room), 0);
    CHECK_EQ(l.bootargs + SELFBOOT_BOOTARGS_SIZE, l.uefi_base);
    /* Nothing is left for a disk then */
    CHECK_EQ(selfboot_disk_room(&l), 0);
    CHECK_EQ(selfboot_layout_disk(&l, 0x1000), -1);

    CHECK_EQ(selfboot_layout_image(&l, R21_IMAGE), 0);
    room = selfboot_disk_room(&l);
    CHECK_EQ(selfboot_layout_disk(&l, room + 0x1000), -1);
    CHECK_EQ(selfboot_layout_disk(&l, room), 0);
    CHECK_EQ(l.top, l.uefi_base);

    /* Guest RAM that has no room for the device tree and the work area */
    CHECK_EQ(selfboot_layout_init(&l, R21_MEM_TOP - SELFBOOT_UEFI_REGION - 0x100000, &ba), -1);
    CHECK_EQ(selfboot_layout_init(&l, R21_MEM_TOP + 0x4000, &ba), -1);
}

static void test_layout_refuses_a_framebuffer_in_guest_ram(void)
{
    struct selfboot_layout l;
    struct boot_args ba;

    r21_ba(&ba);
    default_tree();
    /* Its last page inside */
    ba.video.base = R21_PHYS_BASE - 0x16a8000 + 0x4000;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), -1);
    /* Just below, and just above */
    ba.video.base = R21_PHYS_BASE - 0x16a8000;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), 0);
    ba.video.base = R21_MEM_TOP;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), 0);
    ba.video.base = R21_MEM_TOP - 0x4000;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), -1);
}

static int init_with_entry(const char *name, u64 base, u64 size)
{
    struct selfboot_layout l;
    struct boot_args ba;

    r21_ba(&ba);
    default_tree();
    extra_name = name;
    extra_base = base;
    extra_size = size;
    make_tree();
    return selfboot_layout_init(&l, R21_PHYS_BASE, &ba);
}

static void test_layout_refuses_memory_map_entries_in_the_way(void)
{
    /* In the device tree of the guest, in the room for the files, in the work area */
    CHECK_EQ(init_with_entry("InTheTree", 0x1001a9a8000 + 0x80000, 0x8000), -1);
    CHECK_EQ(init_with_entry("InTheImage", 0x1001ac00000, 0x4000), -1);
    CHECK_EQ(init_with_entry("FarUp", 0x103d3610000 - 0x4000, 0x4000), -1);
    CHECK_EQ(init_with_entry("InTheWorkArea", R21_MEM_TOP - 0x4000, 0x4000), -1);
    /* Around all of it */
    CHECK_EQ(init_with_entry("Around", R21_PHYS_BASE, R21_MEM_TOP - R21_PHYS_BASE), -1);

    /* In guest RAM below the device tree: memory the firmware takes for free, as on the
     * host; and next to guest RAM on either side */
    CHECK_EQ(init_with_entry("InTheGap", R21_PHYS_BASE, SELFBOOT_GUEST_GAP), 0);
    CHECK_EQ(init_with_entry("Below", R21_PHYS_BASE - 0x4000, 0x4000), 0);
    CHECK_EQ(init_with_entry("Above", R21_MEM_TOP, 0x4000), 0);
    /* Entries that say nothing, wherever they point */
    CHECK_EQ(init_with_entry("Empty", 0x1001ac00000, 0), 0);
    CHECK_EQ(init_with_entry("Nothing", NOTHING, NOTHING), 0);
    /* One whose end is past the end of the address space */
    CHECK_EQ(init_with_entry("Wraps", 0xfffffffffffff000, 0x2000), 0);
}

static void test_layout_needs_the_memory_map(void)
{
    struct selfboot_layout l;
    struct boot_args ba;
    static u8 bare[64];

    r21_ba(&ba);
    memset(bare, 0, sizeof(bare));
    put_name(put_node(bare, 1, 0), "device-tree");
    adt = bare;
    CHECK_EQ(selfboot_layout_init(&l, R21_PHYS_BASE, &ba), -1);
}

/* ---- the two files ----------------------------------------------------------------------- */

static u8 image[0x10000];

/* The header the firmware image starts with (MacBookProM4.fdf): entered at offset 0 */
static void make_image(u64 size)
{
    memset(image, 0, sizeof(image));
    memcpy(image, &(u32){0x10000001}, 4);      /* adr x1, . */
    memcpy(image + 4, &(u32){0x14001fff}, 4);  /* b . + 0x7ffc: to offset 0x8000 */
    memcpy(image + 0x10, &size, 8);
    memcpy(image + 0x38, "ARM\x64", 4);
}

static void test_image_checks(void)
{
    make_image(0x10000);
    CHECK_EQ(selfboot_check_image(image, 0x10000), 0);

    CHECK_EQ(selfboot_check_image(image, 0x3c), -1);

    make_image(0x10000);
    image[0x3b] = 0;
    CHECK_EQ(selfboot_check_image(image, 0x10000), -1);

    /* The entry code takes the load address from x1 */
    make_image(0x10000);
    memcpy(image, &(u32){0x10000000}, 4);
    CHECK_EQ(selfboot_check_image(image, 0x10000), -1);

    make_image(0x10000);
    memcpy(image + 4, &(u32){0xd503201f}, 4); /* nop */
    CHECK_EQ(selfboot_check_image(image, 0x10000), -1);

    /* A branch to the last instruction of the image, and one past it */
    make_image(0x10000);
    memcpy(image + 4, &(u32){0x14000000 | ((0x10000 - 8) >> 2)}, 4);
    CHECK_EQ(selfboot_check_image(image, 0x10000), 0);
    memcpy(image + 4, &(u32){0x14000000 | ((0x10000 - 4) >> 2)}, 4);
    CHECK_EQ(selfboot_check_image(image, 0x10000), -1);

    /* The header says another size than the file has */
    make_image(0xc000);
    CHECK_EQ(selfboot_check_image(image, 0x10000), -1);

    /* Not whole 16 KB pages */
    make_image(0xf000);
    CHECK_EQ(selfboot_check_image(image, 0xf000), -1);
}

static u8 disk[0x2000];

static void test_disk_checks(void)
{
    memset(disk, 0, sizeof(disk));
    memcpy(disk + 512, "EFI PART", 8);
    CHECK_EQ(selfboot_check_disk(disk, 0x2000), 0);
    CHECK_EQ(selfboot_check_disk(disk, 0x1000), 0);

    CHECK_EQ(selfboot_check_disk(disk, 0), -1);
    /* Not whole 4 KB pages */
    CHECK_EQ(selfboot_check_disk(disk, 0x1800), -1);

    /* A GPT of 4096-byte sectors is not what the firmware reads */
    memset(disk, 0, sizeof(disk));
    memcpy(disk + 4096, "EFI PART", 8);
    CHECK_EQ(selfboot_check_disk(disk, 0x2000), -1);
}

/* ---- what the firmware is handed --------------------------------------------------------- */

static struct selfboot_layout r21_layout(void)
{
    struct selfboot_layout l;
    struct boot_args ba;

    r21_ba(&ba);
    default_tree();
    selfboot_layout_init(&l, R21_PHYS_BASE, &ba);
    selfboot_layout_image(&l, R21_IMAGE);
    selfboot_layout_disk(&l, R21_DISK);
    return l;
}

static void test_boot_args(void)
{
    struct selfboot_layout l = r21_layout();
    struct boot_args cur, out, expect;

    r21_ba(&cur);
    memset(&out, 0xee, sizeof(out));
    selfboot_boot_args(&out, &cur, &l);

    CHECK_EQ(out.phys_base, R21_PHYS_BASE);
    /* What the firmware printed as the length of its RAM in R21 */
    CHECK_EQ(out.mem_size, 0x3C2C68000);
    CHECK_EQ(out.top_of_kernel_data, 0x10044e00000);
    CHECK_EQ(out.virt_base, 0xfffffe00109a8000);
    /* The firmware takes devtree - virt_base + phys_base for the address of the tree
     * (PrePi.c); in R21 it printed 0x1001A9A8000 */
    CHECK_EQ((u64)out.devtree - out.virt_base + out.phys_base, 0x1001a9a8000);

    /* Everything else is as m1n1 has it */
    memcpy(&expect, &cur, sizeof(cur));
    expect.phys_base = out.phys_base;
    expect.mem_size = out.mem_size;
    expect.top_of_kernel_data = out.top_of_kernel_data;
    expect.virt_base = out.virt_base;
    expect.devtree = out.devtree;
    CHECK(!memcmp(&out, &expect, sizeof(out)));
    CHECK_EQ(out.devtree_size, 0x84000);
    CHECK_EQ(out.video.base, 0x103e004c000);
    CHECK_EQ(out.rv3.mem_size_actual, 0x400000000);
}

static u8 guest_tree[sizeof(tree) + 64];

static const struct adt_property *guest_prop(const char *name)
{
    void *m1n1_tree = adt;
    const struct adt_property *p;
    int node;

    adt = guest_tree + 32;
    node = adt_path_offset(adt, "/chosen/memory-map");
    p = node < 0 ? NULL : adt_get_property(adt, node, name);
    adt = m1n1_tree;
    return p;
}

static bool guest_range_is(const char *name, u64 base, u64 size)
{
    const struct adt_property *p = guest_prop(name);
    u64 want[2] = {base, size};

    return p && p->size == sizeof(want) && !memcmp(p->value, want, sizeof(want));
}

static void test_adt(const char *out_path)
{
    struct selfboot_layout l = r21_layout();
    static u8 before[sizeof(tree)];

    memcpy(before, tree, sizeof(tree));
    memset(guest_tree, 0xee, sizeof(guest_tree));
    CHECK_EQ(selfboot_adt(guest_tree + 32, tree_size, &l), 0);

    CHECK(guest_range_is("DeviceTree", 0x1001a9a8000, 0x84000));
    CHECK(guest_range_is("BootArgs", 0x1001ca00000, 0x4000));
    /* The size of the disk in bytes */
    CHECK(guest_range_is("RAMDisk", 0x1001cc00000, R21_DISK));
    /* The first spare entry became RAMDisk; the others are as they were */
    CHECK(!guest_prop("MemoryMapReserved-0"));
    CHECK(guest_range_is("MemoryMapReserved-1", 0, 0));
    CHECK(guest_range_is("MemoryMapReserved-2", 0, 0));
    CHECK(guest_range_is("SEPFW", 0x10005a6c000, 0x5d0000));
    CHECK(guest_range_is("AuxKC", NOTHING, NOTHING));

    /* Nothing before the copy or after it was written, and m1n1's tree is as it was */
    for (int i = 0; i < 32; i++) {
        CHECK_EQ(guest_tree[i], 0xee);
        CHECK_EQ(guest_tree[32 + tree_size + i], 0xee);
    }
    CHECK(!memcmp(before, tree, sizeof(tree)));

    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        CHECK(f && fwrite(guest_tree + 32, 1, tree_size, f) == tree_size);
        if (f)
            fclose(f);
    }
}

static void test_adt_takes_a_spare_entry_that_is_free(void)
{
    struct selfboot_layout l = r21_layout();
    int node = adt_path_offset(adt, "/chosen/memory-map");
    struct adt_property *spare = (void *)adt_get_property(adt, node, "MemoryMapReserved-0");
    u64 taken[2] = {0x10006000000, 0x4000};

    /* iBoot has used the first one */
    memcpy(spare->value, taken, sizeof(taken));
    CHECK_EQ(selfboot_adt(guest_tree + 32, tree_size, &l), 0);
    CHECK(guest_range_is("MemoryMapReserved-0", 0x10006000000, 0x4000));
    CHECK(guest_range_is("RAMDisk", 0x1001cc00000, R21_DISK));
    CHECK(!guest_prop("MemoryMapReserved-1"));
    CHECK(guest_range_is("MemoryMapReserved-2", 0, 0));
}

static void test_adt_refuses_a_tree_without_what_it_needs(void)
{
    struct selfboot_layout l = r21_layout();

    default_tree();
    spare_entries = 0;
    make_tree();
    CHECK_EQ(selfboot_adt(guest_tree + 32, tree_size, &l), -1);

    default_tree();
    with_device_tree_entry = false;
    make_tree();
    CHECK_EQ(selfboot_adt(guest_tree + 32, tree_size, &l), -1);

    default_tree();
}

int main(int argc, char **argv)
{
    test_layout_of_r21();
    test_layout_rounds();
    test_layout_refuses_what_does_not_fit();
    test_layout_refuses_a_framebuffer_in_guest_ram();
    test_layout_refuses_memory_map_entries_in_the_way();
    test_layout_needs_the_memory_map();
    test_image_checks();
    test_disk_checks();
    test_boot_args();
    test_adt(argc > 1 ? argv[1] : NULL);
    test_adt_takes_a_spare_entry_that_is_free();
    test_adt_refuses_a_tree_without_what_it_needs();

    printf("%d checks, %d failed\n", checks, failed);
    if (!failed)
        printf("ALL OK\n");
    return failed ? 1 : 0;
}
