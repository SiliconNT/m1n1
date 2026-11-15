/* SPDX-License-Identifier: GPL-2.0-or-later */
/* See model.h */

#include "model.h"

int checks, failed;

/* ---- the device tree functions of src/adt.h that selfboot.c and selfboot_run.c call ------ */

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

/* rust/src/adt.rs: the value and, if asked for, its length; NULL if there is none */
const void *adt_getprop(const void *dt, int nodeoffset, const char *name, u32 *lenp)
{
    const struct adt_property *p = adt_get_property(dt, nodeoffset, name);

    if (!p)
        return NULL;
    if (lenp)
        *lenp = p->size;
    return p->value;
}

/* rust/src/adt.rs: the length, or an error if the property is not exactly as long as `out` */
int adt_getprop_copy(const void *dt, int nodeoffset, const char *name, void *out, size_t len)
{
    const struct adt_property *p = adt_get_property(dt, nodeoffset, name);

    if (!p)
        return -1;
    if (PROP_SIZE(p) != len)
        return -2;
    memcpy(out, p->value, len);
    return len;
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

/* The nodes on the way, after the root, end with a 0 */
int adt_path_offset_trace(const void *dt, const char *path, int *offsets)
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
            if (name && strlen((const char *)name->value) == len && !memcmp(name->value, path, len))
                break;
            child = node_end(child);
        }
        if (!i)
            return -1;
        offset = child;
        if (offsets)
            *offsets++ = offset;
        path += len;
        while (*path == '/')
            path++;
    }
    if (offsets)
        *offsets = 0;
    return offset;
}

int adt_path_offset(const void *dt, const char *path)
{
    return adt_path_offset_trace(dt, path, NULL);
}

static u64 cells(const u8 *p, u32 count)
{
    u64 value = 0;
    memcpy(&value, p, 4 * count);
    return value;
}

/*
 * Entry `idx` of a property like "reg" of the last node of `path`, as an address of the CPU:
 * through the "ranges" of the node above it. One level, which is all a test has here.
 */
int adt_get_reg(const void *dt, int *path, const char *prop, int idx, u64 *paddr, u64 *psize)
{
    u32 ac, sc, pac, len;
    const u8 *reg, *ranges;
    int depth = 0, parent;
    u64 addr;

    while (path[depth])
        depth++;
    if (depth != 2)
        return -1;
    parent = path[0];
    if (ADT_GETPROP(dt, parent, "#address-cells", &ac) < 0 ||
        ADT_GETPROP(dt, parent, "#size-cells", &sc) < 0 ||
        ADT_GETPROP(dt, 0, "#address-cells", &pac) < 0)
        return -1;
    reg = adt_getprop(dt, path[1], prop, &len);
    if (!reg || len < (idx + 1) * 4 * (ac + sc))
        return -1;
    reg += idx * 4 * (ac + sc);
    addr = cells(reg, ac);

    ranges = adt_getprop(dt, parent, "ranges", &len);
    if (!ranges)
        return -1;
    for (u32 off = 0; off + 4 * (ac + pac + sc) <= len; off += 4 * (ac + pac + sc)) {
        u64 bus = cells(ranges + off, ac);
        u64 size = cells(ranges + off + 4 * (ac + pac), sc);

        if (addr >= bus && addr - bus < size) {
            if (paddr)
                *paddr = addr - bus + cells(ranges + off + 4 * ac, pac);
            if (psize)
                *psize = cells(reg + 4 * ac, sc);
            return 0;
        }
    }
    return -1;
}

/* ---- making a device tree ---------------------------------------------------------------- */

u8 *put_node(u8 *p, u32 properties, u32 children)
{
    struct adt_node_hdr hdr = {properties, children};
    memcpy(p, &hdr, sizeof(hdr));
    return p + sizeof(hdr);
}

u8 *put_prop(u8 *p, const char *name, const void *value, u32 len)
{
    struct adt_property *prop = (struct adt_property *)p;
    memset(prop, 0, sizeof(*prop) + ((len + 3) & ~3));
    strncpy(prop->name, name, sizeof(prop->name) - 1);
    prop->size = len;
    memcpy(prop->value, value, len);
    return p + sizeof(*prop) + ((len + 3) & ~3);
}

u8 *put_name(u8 *p, const char *name)
{
    return put_prop(p, "name", name, strlen(name) + 1);
}

u8 *put_range(u8 *p, const char *name, u64 base, u64 size)
{
    u64 value[2] = {base, size};
    return put_prop(p, name, value, sizeof(value));
}

/* ---- the M4 of R21 ----------------------------------------------------------------------- */

/* Through a pointer, and compared with memcmp(): the padding of the structure is 0 too */
void r21_ba(struct boot_args *out)
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
