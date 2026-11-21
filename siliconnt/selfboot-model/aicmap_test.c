/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for hv_aicmap() of src/hv/hv_layout.c: the table of AIC lines and the
 * SPIs they are, made from the device tree, which the hypervisor forwards the lines by and
 * the firmware is handed.
 *
 * The real file is compiled for the build machine. The device tree functions are those of
 * model.c, over trees this file makes, in the format of Apple's; AIC is the structure
 * aic_init() of src/aic.c fills in, with the numbers m1n1 printed on the M4 (SiliconNT
 * docs/findings/0011: "AIC3 with 1/2 dies, 2048/4096 IRQs").
 *
 * The tree that has to come out right is made after the M4's (SiliconNT docs/findings/0058,
 * evidence 1, and 0060): lines that several nodes share, a controller of its own (GPIO) below
 * AIC whose children are not AIC's, a node with "interrupts" and no interrupt-parent, lines in
 * the last node of the tree, and the two shapes of an "interrupt-parent" with more than one
 * phandle: clpc's, one for each value, two of them another controller's; and that of the SPMI
 * controllers, three phandles for many values, the last one for all that follow.
 */
#include <unistd.h>

#include "aic.h"
#include "hv.h"
#include "model.h"
#include "utils.h"

u64 ram_base, mem_size_actual;
struct aic *aic;

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define AIC_PHANDLE  0xd5
#define GPIO_PHANDLE 0x40
/* The two other controllers of the M4 that clpc and nub-spmi0 have values for */
#define OTHER_PHANDLE 0x112
#define SPMI_PHANDLE  0x143

static struct aic m4_aic;

/* The AIC of the M4: one die of 2048 lines, room for two of 4096 */
static void m4(void)
{
    memset(&m4_aic, 0, sizeof(m4_aic));
    m4_aic.version = 3;
    m4_aic.nr_die = 1;
    m4_aic.max_die = 2;
    m4_aic.nr_irq = 2048;
    m4_aic.max_irq = 4096;
    aic = &m4_aic;
}

/* ---- the device tree ---------------------------------------------------------------------- */

static u8 tree[0x10000];
static u8 *at;

static void begin(void)
{
    memset(tree, 0, sizeof(tree));
    at = tree;
    adt = tree;
}

/* Properties of a node beyond its name */
struct props {
    u32 phandle;         /* AAPL,phandle; 0: none */
    u32 parent;          /* interrupt-parent, one phandle; 0: none, or parents has it */
    const u32 *parents;  /* ...or these: interrupt-parent with more than one */
    u32 parent_bytes;    /* their length */
    bool flagged_parent; /* the top bit of the length of "interrupt-parent" set */
    const u32 *irqs;     /* interrupts */
    u32 irq_bytes;       /* their length */
    bool flagged;        /* the top bit of the length of "interrupts" set */
};

/* A node: its children are the next `children` nodes made, with theirs */
static void node(const char *name, u32 children, struct props pr)
{
    u32 nprops = 1 + !!pr.phandle + !!(pr.parent || pr.parents) + !!pr.irqs;

    at = put_node(at, nprops, children);
    at = put_name(at, name);
    if (pr.phandle)
        at = put_prop(at, "AAPL,phandle", &pr.phandle, 4);
    /* A placeholder goes last: nothing can be stepped over it (model.c) */
    if (pr.flagged_parent && pr.irqs)
        at = put_prop(at, "interrupts", pr.irqs, pr.irq_bytes);
    if (pr.parents || pr.parent) {
        struct adt_property *prop = (struct adt_property *)at;

        if (pr.parents)
            at = put_prop(at, "interrupt-parent", pr.parents, pr.parent_bytes);
        else
            at = put_prop(at, "interrupt-parent", &pr.parent, 4);
        if (pr.flagged_parent)
            prop->size |= 0x80000000;
    }
    if (pr.irqs && !pr.flagged_parent) {
        struct adt_property *prop = (struct adt_property *)at;

        at = put_prop(at, "interrupts", pr.irqs, pr.irq_bytes);
        if (pr.flagged)
            prop->size |= 0x80000000;
    }
}

#define IRQS(...)                                                                                  \
    .irqs = (const u32[]){__VA_ARGS__}, .irq_bytes = sizeof((const u32[]){__VA_ARGS__})
#define ON_AIC(...) ((struct props){.parent = AIC_PHANDLE, IRQS(__VA_ARGS__)})
#define PARENTS(...)                                                                               \
    .parents = (const u32[]){__VA_ARGS__}, .parent_bytes = sizeof((const u32[]){__VA_ARGS__})

/* After the M4's: what the table of it has to be is below */
static void m4_tree(void)
{
    begin();
    node("device-tree", 3, (struct props){0});
    /* Interrupts, but no interrupt-parent: not AIC's */
    node("orphan", 0, (struct props){IRQS(7)});
    node("arm-io", 11, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    node("wdt", 0, ON_AIC(0, 1));
    /* As on the M4: a phandle for each value; the last two are another controller's */
    node(
        "clpc", 0,
        (struct props){PARENTS(AIC_PHANDLE, AIC_PHANDLE, AIC_PHANDLE, OTHER_PHANDLE, OTHER_PHANDLE),
                       IRQS(894, 0, 892, 0x70305452, 0x70305472)});
    node("ppm", 0, ON_AIC(0));
    /* As on the M4: its own, AIC's, and its own for all the values that follow */
    node("nub-spmi0", 0,
         (struct props){.phandle = SPMI_PHANDLE,
                        PARENTS(SPMI_PHANDLE, AIC_PHANDLE, SPMI_PHANDLE),
                        IRQS(0x100, 0x201, 0x119, 0x104, 0x105, 0x108, 0x106)});
    /* A controller of its own, with a line of AIC; its children's interrupts are its */
    node("gpio0", 1,
         (struct props){.phandle = GPIO_PHANDLE, .parent = AIC_PHANDLE, IRQS(190, 191)});
    node("button", 0, (struct props){.parent = GPIO_PHANDLE, IRQS(5)});
    node("dart-dcp", 0, ON_AIC(678));
    node("dart-disp0", 0, ON_AIC(678));
    node("uart0", 0, ON_AIC(969));
    node("apcie", 0, ON_AIC(1505));
    /* Six bytes: one whole value */
    node("odd", 0, (struct props){.parent = AIC_PHANDLE, IRQS(33, 34), .irq_bytes = 6});
    node("cpus", 1, (struct props){0});
    /* The last node of the tree: 2047 is the last line, 2048 and 4095 are on no line */
    node("cpu0", 0, ON_AIC(2047, 2048, 4095));
}

/* ...and 2048 and 4095 left out */
static const u32 m4_lines[] = {0, 1, 33, 190, 191, 513, 678, 892, 894, 969, 1505, 2047};

/* ---- what the code under test says -------------------------------------------------------- */

static char said[8192];
static struct hv_aicmap map;

/* Fills the table with 0xee first; keeps what was printed meanwhile, and prints it too */
static int make(void)
{
    FILE *tmp = tmpfile();
    int out, ret;
    size_t n;

    memset(&map, 0xee, sizeof(map));
    fflush(stdout);
    out = dup(STDOUT_FILENO);
    if (!tmp || out < 0 || dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        fprintf(stderr, "aicmap_test: cannot keep what is printed\n");
        exit(2);
    }
    ret = hv_aicmap(&map);
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

/* The table is these lines, and nothing past its size was written */
static bool table_is(int ret, const u32 *lines, u32 count)
{
    const u8 *bytes = (const u8 *)&map;

    if (ret != (int)(8 + 4 * count) || map.version != 1 || map.count != count)
        return false;
    for (u32 i = 0; i < count; i++)
        if (map.line[i] != lines[i])
            return false;
    for (size_t i = ret; i < sizeof(map); i++)
        if (bytes[i] != 0xee)
            return false;
    return true;
}

static bool untouched(void)
{
    const u8 *bytes = (const u8 *)&map;

    for (size_t i = 0; i < sizeof(map); i++)
        if (bytes[i] != 0xee)
            return false;
    return true;
}

/* ---- the tests ---------------------------------------------------------------------------- */

static void test_the_format(void)
{
    /* What the firmware reads: version 1, a count, the lines; the most is 988, SPIs 32-1019 */
    CHECK_EQ(HV_AICMAP_VERSION, 1);
    CHECK_EQ(HV_AICMAP_FIRST_SPI, 32);
    CHECK_EQ(HV_AICMAP_MAX, 988);
    CHECK_EQ(offsetof(struct hv_aicmap, version), 0);
    CHECK_EQ(offsetof(struct hv_aicmap, count), 4);
    CHECK_EQ(offsetof(struct hv_aicmap, line), 8);
    CHECK_EQ(sizeof(struct hv_aicmap), 8 + 4 * 988);
    /* It fits into the page the loaders keep for it */
    CHECK(sizeof(struct hv_aicmap) <= 0x4000);
}

static void test_the_m4(void)
{
    int ret;

    m4();
    m4_tree();
    ret = make();
    CHECK(table_is(ret, m4_lines, COUNT(m4_lines)));
    CHECK(says("12 AIC lines are SPIs 32 .. 43: AIC 0 .. 2047; 2 values of \"interrupts\" are not "
               "lines of this AIC"));

    /* Again: the same, nothing left over from the first time */
    CHECK(table_is(make(), m4_lines, COUNT(m4_lines)));
}

static void test_two_dies(void)
{
    static const u32 lines[] = {3, 4096 + 3, 4096 + 2047};

    m4();
    m4_aic.nr_die = 2;
    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 2, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    /* die * max_irq + line: the second die starts at 4096; a third there is not */
    node("dev", 0, ON_AIC(4096 + 3, 3, 4096 + 2047, 4096 + 2048, 2 * 4096, 2 * 4096 + 1));
    CHECK(table_is(make(), lines, COUNT(lines)));
    CHECK(says("3 values"));
}

/* A tree whose one device has `n` lines, 0 to n - 1, each twice */
static void many(u32 n)
{
    static u32 irqs[2 * 1000];

    for (u32 i = 0; i < n; i++)
        irqs[2 * i] = irqs[2 * i + 1] = n - 1 - i;
    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 2, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    node("dev", 0, (struct props){.parent = AIC_PHANDLE, .irqs = irqs, .irq_bytes = 8 * n});
}

static void test_how_many(void)
{
    static u32 lines[989];
    int ret;

    for (u32 i = 0; i < COUNT(lines); i++)
        lines[i] = i;
    m4();

    /* As many as there are SPIs */
    many(988);
    ret = make();
    CHECK_EQ(ret, 8 + 4 * 988);
    CHECK(table_is(ret, lines, 988));
    CHECK(says("988 AIC lines are SPIs 32 .. 1019"));

    /* One more: no table, and it says how many there are */
    many(989);
    CHECK_EQ(make(), -1);
    CHECK(untouched());
    CHECK(says("989 AIC lines"));
    CHECK(says("988 SPIs"));

    /* None: a table all the same */
    many(0);
    CHECK(table_is(make(), lines, 0));
    CHECK(says("no AIC lines"));
}

static void test_what_is_not_aics(void)
{
    static const u32 lines[] = {10, 11, 15, 16, 20};

    m4();
    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 9, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    node("dev", 0, ON_AIC(10));
    /* AIC's first, then another's, which goes on for the values that follow */
    node("pair", 0, (struct props){PARENTS(AIC_PHANDLE, OTHER_PHANDLE), IRQS(11, 12, 13)});
    /* Another's first, then AIC's for the rest */
    node("later", 0, (struct props){PARENTS(OTHER_PHANDLE, AIC_PHANDLE), IRQS(14, 15, 16)});
    /* More phandles than values */
    node("few", 0, (struct props){PARENTS(AIC_PHANDLE, AIC_PHANDLE, OTHER_PHANDLE), IRQS(20)});
    /* AIC's, but no line of this AIC: left out, and counted */
    node("bad", 0, (struct props){PARENTS(AIC_PHANDLE, OTHER_PHANDLE), IRQS(5000, 21)});
    /* Less than one phandle: no interrupt-parent */
    node("half", 0, (struct props){PARENTS(AIC_PHANDLE), .parent_bytes = 2, IRQS(17)});
    /* Another controller's */
    node("other", 0, (struct props){.parent = GPIO_PHANDLE, IRQS(19)});
    /* AIC's, without interrupts */
    node("quiet", 0, (struct props){.parent = AIC_PHANDLE});
    CHECK(table_is(make(), lines, COUNT(lines)));
    CHECK(says("5 AIC lines are SPIs 32 .. 36: AIC 10 .. 20; 1 values"));
}

/*
 * Properties that are only placeholders, the top bit of their length set. m1n1's device tree
 * code steps over a property by its length as it is (model.c), so such a property can only be
 * the last one of the tree; there, its value is not to be read as lines.
 */
static void test_placeholders(void)
{
    static const u32 lines[] = {10};

    m4();
    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 3, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    node("dev", 0, ON_AIC(10));
    node("sep", 0, (struct props){.parent = AIC_PHANDLE, IRQS(77), .flagged = true});
    CHECK(table_is(make(), lines, COUNT(lines)));

    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 3, (struct props){0});
    node("aic", 0, (struct props){.phandle = AIC_PHANDLE});
    node("dev", 0, ON_AIC(10));
    node("placeholder", 0, (struct props){.parent = AIC_PHANDLE, .flagged_parent = true, IRQS(18)});
    CHECK(table_is(make(), lines, COUNT(lines)));
}

static void test_no_table(void)
{
    /* No AIC: aic_init() found none, or not one it knows */
    m4();
    m4_tree();
    aic = NULL;
    CHECK_EQ(make(), -1);
    CHECK(untouched());
    CHECK(says("no AIC"));

    /* No node of AIC, or one without a phandle */
    m4();
    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 1, (struct props){0});
    node("dev", 0, ON_AIC(10));
    CHECK_EQ(make(), -1);
    CHECK(untouched());
    CHECK(says("/arm-io/aic"));

    begin();
    node("device-tree", 1, (struct props){0});
    node("arm-io", 2, (struct props){0});
    node("aic", 0, (struct props){0});
    node("dev", 0, ON_AIC(10));
    CHECK_EQ(make(), -1);
    CHECK(untouched());
}

int main(void)
{
    test_the_format();
    test_the_m4();
    test_two_dies();
    test_how_many();
    test_what_is_not_aics();
    test_placeholders();
    test_no_table();

    printf("aicmap_test: %d checks, %d failed\n", checks, failed);
    if (!failed)
        printf("ALL OK\n");
    return failed ? 1 : 0;
}
