/* SPDX-License-Identifier: GPL-2.0-or-later */
/* See fake_adt.h */

#include <string.h>

#include "fake_adt.h"
#include "adt.h"

void *adt;
struct fake_node fake_adt_nodes[FAKE_NODES];
int fake_adt_misses;

/* Values as the M4 has them (R35, adt-1.bin) */
static const u32 one = 1;
static u64 smc_sram, smc_sram_size;
static const u32 gen3 = 3;
static const u32 wdt_version = 2;
static const u32 pmu_reg[6] = {0x0e, 0x03, 0, 0x04, 0, 0};
static const u32 leg_scrpad = 0xf800;
static const char pmu_compat[] = "pmu,spmi\0pmu,abbey";
/* Of these two only the first entry: what the code looks at */
static const char pmu_aux_compat[] = "pmu,spmi";
static const char hpm_compat[] = "usbc,sn201202x,spmi";
static const char smc_compat[] = "iop,ascwrap-v6";

static void node(int index, const char *name, int parent)
{
    memset(&fake_adt_nodes[index], 0, sizeof(fake_adt_nodes[index]));
    fake_adt_nodes[index].name = name;
    fake_adt_nodes[index].parent = parent;
}

static void reg(int index, u64 base, u64 size)
{
    struct fake_node *n = &fake_adt_nodes[index];
    n->reg[n->nregs][0] = base;
    n->reg[n->nregs][1] = size;
    n->nregs++;
}

void fake_adt_set(int index, const char *name, const void *value, u32 len)
{
    struct fake_prop *slot = NULL;

    for (int i = 0; i < FAKE_ADT_MAX_PROPS; i++) {
        struct fake_prop *p = &fake_adt_nodes[index].props[i];
        if (p->name && !strcmp(p->name, name)) {
            slot = p;
            break;
        }
        if (!p->name && !slot)
            slot = p;
    }
    if (!slot) {
        fake_adt_overrun(name, 0, 0);
        return;
    }
    slot->name = value ? name : NULL;
    slot->value = value;
    slot->len = len;
}

void fake_adt_reset(const struct fake_adt_places *where)
{
    node(FAKE_ROOT, "device-tree", -1);
    node(FAKE_ARM_IO, "arm-io", FAKE_ROOT);
    node(FAKE_SMC, "smc", FAKE_ARM_IO);
    fake_adt_set(FAKE_SMC, "compatible", smc_compat, sizeof(smc_compat));
    reg(FAKE_SMC, where->smc, where->smc_size);
    node(FAKE_SMC_NUB, "iop-smc-nub", FAKE_SMC);
    fake_adt_set(FAKE_SMC_NUB, "no-shutdown", &one, 4);
    smc_sram = where->smc_sram;
    smc_sram_size = where->smc_sram_size;
    fake_adt_set(FAKE_SMC_NUB, "region-base", &smc_sram, 8);
    fake_adt_set(FAKE_SMC_NUB, "region-size", &smc_sram_size, 8);
    node(FAKE_SPMI_A1, "nub-spmi-a1", FAKE_ARM_IO);
    reg(FAKE_SPMI_A1, where->spmi_a1, 0x4000);
    node(FAKE_HPM3, "hpm3", FAKE_SPMI_A1);
    fake_adt_set(FAKE_HPM3, "compatible", hpm_compat, sizeof(hpm_compat));
    node(FAKE_SPMI0, "nub-spmi0", FAKE_ARM_IO);
    fake_adt_set(FAKE_SPMI0, "gen", &gen3, 4);
    reg(FAKE_SPMI0, where->spmi0, 0x4000);
    node(FAKE_PMU_MAIN, "pmu-main", FAKE_SPMI0);
    fake_adt_set(FAKE_PMU_MAIN, "reg", pmu_reg, sizeof(pmu_reg));
    fake_adt_set(FAKE_PMU_MAIN, "is-primary", &one, 4);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", pmu_compat, sizeof(pmu_compat));
    fake_adt_set(FAKE_PMU_MAIN, "info-leg_scrpad", &leg_scrpad, 4);
    node(FAKE_BTM, "btm", FAKE_SPMI0);
    node(FAKE_SPMI1, "nub-spmi1", FAKE_ARM_IO);
    fake_adt_set(FAKE_SPMI1, "gen", &gen3, 4);
    reg(FAKE_SPMI1, where->spmi1, 0x4000);
    node(FAKE_PMU_AUX, "pmu-aux", FAKE_SPMI1);
    fake_adt_set(FAKE_PMU_AUX, "compatible", pmu_aux_compat, sizeof(pmu_aux_compat));
    node(FAKE_WDT, "wdt", FAKE_ARM_IO);
    fake_adt_set(FAKE_WDT, "wdt-version", &wdt_version, 4);
    reg(FAKE_WDT, where->wdt, 0x4000);
    reg(FAKE_WDT, where->wdt + 0xc224, 4); /* made up, not used */
    reg(FAKE_WDT, where->wdt_second, 4);
    fake_adt_misses = 0;
}

static int index_of(int offset)
{
    for (int i = 0; i < FAKE_NODES; i++)
        if (FAKE_ADT_OFFSET(i) == offset && fake_adt_nodes[i].name)
            return i;
    fake_adt_misses++;
    return -1;
}

static const struct fake_prop *prop_of(int offset, const char *name)
{
    int i = index_of(offset);

    if (i < 0)
        return NULL;
    for (int p = 0; p < FAKE_ADT_MAX_PROPS; p++)
        if (fake_adt_nodes[i].props[p].name && !strcmp(fake_adt_nodes[i].props[p].name, name))
            return &fake_adt_nodes[i].props[p];
    return NULL;
}

int adt_get_child_count(const void *dt, int offset)
{
    int i = index_of(offset), n = 0;

    for (int c = 0; i >= 0 && c < FAKE_NODES; c++)
        n += fake_adt_nodes[c].name && fake_adt_nodes[c].parent == i;
    return n;
}

int adt_first_child_offset(const void *dt, int offset)
{
    int i = index_of(offset);

    for (int c = 0; i >= 0 && c < FAKE_NODES; c++)
        if (fake_adt_nodes[c].name && fake_adt_nodes[c].parent == i)
            return FAKE_ADT_OFFSET(c);
    return -1;
}

int adt_next_sibling_offset(const void *dt, int offset)
{
    int i = index_of(offset);

    for (int c = i + 1; i >= 0 && c < FAKE_NODES; c++)
        if (fake_adt_nodes[c].name && fake_adt_nodes[c].parent == fake_adt_nodes[i].parent)
            return FAKE_ADT_OFFSET(c);
    return -1;
}

const char *adt_get_name(const void *dt, int offset)
{
    int i = index_of(offset);
    return i < 0 ? NULL : fake_adt_nodes[i].name;
}

/* The nodes on the way, after the root, end with a 0 */
int adt_path_offset_trace(const void *dt, const char *path, int *offsets)
{
    int at = FAKE_ROOT;

    while (*path == '/')
        path++;
    while (*path) {
        size_t len = 0;
        int next = -1;

        while (path[len] && path[len] != '/')
            len++;
        for (int c = 0; c < FAKE_NODES; c++)
            if (fake_adt_nodes[c].name && fake_adt_nodes[c].parent == at &&
                strlen(fake_adt_nodes[c].name) == len &&
                !strncmp(fake_adt_nodes[c].name, path, len))
                next = c;
        if (next < 0)
            return -1;
        at = next;
        if (offsets)
            *offsets++ = FAKE_ADT_OFFSET(at);
        path += len;
        while (*path == '/')
            path++;
    }
    if (offsets)
        *offsets = 0;
    return FAKE_ADT_OFFSET(at);
}

int adt_path_offset(const void *dt, const char *path)
{
    return adt_path_offset_trace(dt, path, NULL);
}

const void *adt_getprop(const void *dt, int offset, const char *name, u32 *lenp)
{
    const struct fake_prop *p = prop_of(offset, name);

    if (!p)
        return NULL;
    if (lenp)
        *lenp = p->len;
    return p->value;
}

int adt_getprop_copy(const void *dt, int offset, const char *name, void *out, size_t len)
{
    const struct fake_prop *p = prop_of(offset, name);

    if (!p)
        return -1;
    if (p->len > len) {
        fake_adt_overrun(name, p->len, len);
        return -1;
    }
    if (p->len != len)
        return -2;
    memcpy(out, p->value, len);
    return len;
}

/* Entry idx of the "reg" of the last node of the path, where the table has it */
int adt_get_reg(const void *dt, int *path, const char *prop, int idx, u64 *paddr, u64 *psize)
{
    int last = -1, i;

    while (*path)
        last = *path++;
    i = index_of(last);
    if (i < 0 || strcmp(prop, "reg") || idx < 0 || idx >= fake_adt_nodes[i].nregs)
        return -1;
    if (paddr)
        *paddr = fake_adt_nodes[i].reg[idx][0];
    if (psize)
        *psize = fake_adt_nodes[i].reg[idx][1];
    return 0;
}
