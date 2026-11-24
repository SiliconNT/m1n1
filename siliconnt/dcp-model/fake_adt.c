/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A device tree for the files under test: the functions of src/adt.h that they call, over the
 * nodes they look at, as the M4 has them (R37, adt-1.bin: SiliconNT docs/findings/0078).
 * adt_getprop_copy() is as rust/src/adt.rs has it: an error unless the property is exactly as
 * long as what it is copied to.
 */

#include <string.h>

#include "adt.h"
#include "machine.h"
#include "utils.h"

void *adt;

#define MAX_NODES 24
#define MAX_PROPS 10
#define MAX_REGS  2

struct prop {
    struct adt_property p;
    u8 value[256];
};

static struct node {
    char path[64];
    struct prop props[MAX_PROPS];
    u64 reg[MAX_REGS][2];
    int nregs;
} nodes[MAX_NODES];
static int n_nodes;

#define OFFSET(index) (0x100 * ((index) + 1))

static struct node *node_at(int offset)
{
    int index = offset / 0x100 - 1;

    if (offset < 0x100 || offset % 0x100 || index >= n_nodes) {
        fprintf(stdout, "FAIL: a node of the device tree that is not there, %d\n", offset);
        abort();
    }
    return &nodes[index];
}

static struct node *find(const char *path)
{
    for (int i = 0; i < n_nodes; i++)
        if (!strcmp(nodes[i].path, path))
            return &nodes[i];
    return NULL;
}

static struct node *make(const char *path)
{
    struct node *n = find(path);

    if (n)
        return n;
    if (n_nodes == MAX_NODES)
        abort();
    n = &nodes[n_nodes++];
    memset(n, 0, sizeof(*n));
    strcpy(n->path, path);
    return n;
}

void fake_adt_prop(const char *path, const char *name, const void *value, u32 len)
{
    struct node *n = make(path);
    struct prop *slot = NULL;

    for (int i = 0; i < MAX_PROPS; i++) {
        struct prop *p = &n->props[i];
        if (!strcmp(p->p.name, name)) {
            slot = p;
            break;
        }
        if (!p->p.name[0] && !slot)
            slot = p;
    }
    if (!slot || len > sizeof(slot->value))
        abort();
    memset(slot, 0, sizeof(*slot));
    if (!value)
        return;
    strcpy(slot->p.name, name);
    slot->p.size = len;
    memcpy(slot->p.value, value, len);
}

void fake_adt_str(const char *path, const char *name, const char *value)
{
    fake_adt_prop(path, name, value, strlen(value) + 1);
}

void fake_adt_reg(const char *path, int idx, u64 addr, u64 size)
{
    struct node *n = make(path);

    n->reg[idx][0] = addr;
    n->reg[idx][1] = size;
    if (n->nregs <= idx)
        n->nregs = idx + 1;
}

void fake_adt_reset(void)
{
    static const struct adt_segment_ranges segments[] = {
        {0x100002a8000, 0x0, 0x10007770000, 0x843000, 1},
        {0x103dfcd8000, 0x843000, 0x10007fb4000, 0x32c000, 0},
        {0x10005274000, 0xb6f000, 0x10005274000, 0x26000, 0xa},
        {0x103e5c1c000, ~0UL, 0x10000b70000, 0x1000000, 2},
        {0x103e58b8000, ~0UL, 0x10001ba0000, 0x364000, 2},
        {0x103e57f8000, ~0UL, 0x10001f04000, 0xc0000, 2},
        {0x103e004c000, ~0UL, 0x10001fc4000, 0x57ac000, 2},
    };
    static const char compatible[] = "J604AP\0Mac16,1\0AppleARM";
    static const u32 one = 1, sid_dcp = 23, sid_disp = 0;
    static const u64 vm_base = 0x10000000000;
    static const u64 pt_dcp[2] = {0x103ffeec000, 0x103fff1c000};
    static const u64 pt_disp[2] = {0x103fff1c000, 0x103fff3c000};
    static const u64 l2_dcp[2] = {0x103ffeec000, 2};
    static const u64 l2_disp[2] = {0x103fff1c000, 2};

    n_nodes = 0;
    memset(nodes, 0, sizeof(nodes));

    fake_adt_prop("/", "compatible", compatible, sizeof(compatible));
    fake_adt_str("/chosen", "firmware-version", "mBoot-20457.41.3");
    fake_adt_str("/chosen", "system-firmware-version", "mBoot-20457.41.3");
    fake_adt_prop("/product", "partially-occluded-display", &one, 4);
    fake_adt_reg("/vram", 0, FB_BASE, 0x57ac000);
    make("/arm-io");
    make("/arm-io/disp0");
    fake_adt_reg("/arm-io/dcp", 0, 0x30ae00000, 0x100000);
    fake_adt_str("/arm-io/dcp", "compatible", "iop,ascwrap-v6");
    fake_adt_prop("/arm-io/dcp/iop-dcp-nub", "segment-ranges", segments, sizeof(segments));
    fake_adt_reg("/arm-io/dart-dcp", 0, 0x30a340000, 0x4000);
    fake_adt_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    fake_adt_prop("/arm-io/dart-dcp", "vm-base", &vm_base, 8);
    fake_adt_prop("/arm-io/dart-dcp", "pt-region-23", pt_dcp, 16);
    fake_adt_prop("/arm-io/dart-dcp", "l2-tt-23", l2_dcp, 16);
    fake_adt_prop("/arm-io/dart-dcp/mapper-dcp", "reg", &sid_dcp, 4);
    fake_adt_reg("/arm-io/dart-disp0", 0, 0x30a300000, 0x4000);
    fake_adt_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    fake_adt_prop("/arm-io/dart-disp0", "vm-base", &vm_base, 8);
    fake_adt_prop("/arm-io/dart-disp0", "pt-region-0", pt_disp, 16);
    fake_adt_prop("/arm-io/dart-disp0", "l2-tt-0", l2_disp, 16);
    fake_adt_prop("/arm-io/dart-disp0/mapper-disp0", "reg", &sid_disp, 4);
}

/* ---- src/adt.h ---- */

int adt_path_offset(const void *dt, const char *path)
{
    char full[64];
    struct node *n;

    UNUSED(dt);
    snprintf(full, sizeof(full), "%s%s", path[0] == '/' ? "" : "/", path);
    n = find(full);
    return n ? OFFSET((int)(n - nodes)) : -1;
}

/* Every node on the way after the root, then a 0 */
int adt_path_offset_trace(const void *dt, const char *path, int *offsets)
{
    char part[64];
    int offset = adt_path_offset(dt, path);

    if (offset < 0)
        return offset;
    strcpy(part, node_at(offset)->path);
    for (size_t i = 1; i <= strlen(node_at(offset)->path); i++) {
        if (part[i] && part[i] != '/')
            continue;
        char was = part[i];
        part[i] = 0;
        *offsets++ = adt_path_offset(dt, part);
        part[i] = was;
    }
    *offsets = 0;
    return offset;
}

int adt_first_child_offset(const void *dt, int offset)
{
    const char *path = node_at(offset)->path;
    size_t len = strlen(path);

    UNUSED(dt);
    for (int i = 0; i < n_nodes; i++)
        if (!strncmp(nodes[i].path, path, len) && nodes[i].path[len] == '/' &&
            !strchr(nodes[i].path + len + 1, '/'))
            return OFFSET(i);
    return -1;
}

const struct adt_property *adt_get_property(const void *dt, int offset, const char *name)
{
    struct node *n = node_at(offset);

    UNUSED(dt);
    for (int i = 0; i < MAX_PROPS; i++)
        if (n->props[i].p.name[0] && !strcmp(n->props[i].p.name, name))
            return &n->props[i].p;
    return NULL;
}

const void *adt_getprop(const void *dt, int offset, const char *name, u32 *lenp)
{
    const struct adt_property *p = adt_get_property(dt, offset, name);

    if (p && lenp)
        *lenp = p->size;
    return p ? p->value : NULL;
}

int adt_getprop_copy(const void *dt, int offset, const char *name, void *out, size_t len)
{
    const struct adt_property *p = adt_get_property(dt, offset, name);

    if (!p || p->size != len)
        return -1;
    memcpy(out, p->value, len);
    return len;
}

int adt_setprop(void *dt, int offset, const char *name, void *value, size_t len)
{
    struct adt_property *p = (struct adt_property *)adt_get_property(dt, offset, name);

    if (!p || p->size != len)
        return -1;
    memcpy(p->value, value, len);
    return len;
}

int adt_get_reg(const void *dt, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    int last = -1;

    UNUSED(dt);
    while (*path)
        last = *path++;
    struct node *n = node_at(last);
    if (strcmp(prop, "reg") || idx >= n->nregs)
        return -1;
    if (addr)
        *addr = n->reg[idx][0];
    if (size)
        *size = n->reg[idx][1];
    return 0;
}

bool adt_is_compatible(const void *dt, int offset, const char *compat)
{
    u32 len;
    /* the root is given as 0 */
    const char *list = adt_getprop(dt, offset ? offset : OFFSET(0), "compatible", &len);

    for (const char *s = list; list && s < list + len; s += strlen(s) + 1)
        if (!strcmp(s, compat))
            return true;
    return false;
}
