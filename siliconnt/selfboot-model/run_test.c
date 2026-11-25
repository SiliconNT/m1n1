/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for src/selfboot_run.c, with the real src/selfboot.c,
 * src/selfboot_files.c and src/hv/hv_layout.c under it.
 *
 * The real files are compiled for the build machine. Everything of m1n1 they call is stood in
 * for here, and every call is written down in the order it came: the SSD and its two files,
 * the heap, the registers of the CPU, the hypervisor (its initialization, its stage 2
 * mappings, execution tracking, the entry into the guest), the consoles and the caches. The
 * device tree functions are those of model.c, over a tree this file makes.
 *
 * Guest memory is real memory at the addresses of the M4: two windows of 48 MB, mapped where
 * guest RAM starts in the two cases, which is as far as files of a few pages reach.
 *
 * What is expected comes from a run on the M4 with the host doing the loading (R21, the
 * output of run_firmware.py): the boot arguments, the ranges of /arm-io, the carveouts, the
 * value of HCR_EL2 and the stage 2 mappings the host made ("PT[...]" lines). Three things are
 * meant to differ from that run and are checked as they are meant: MDCR_EL2 has no TDE and
 * MDSCR_EL1 is 0; the registers the host kept to itself (PMGR, CPU start, RVBAR) are mapped
 * as the devices they are; execution tracking is turned on after the memory is mapped.
 *
 * The table of AIC lines and SPIs is made by the real hv_aicmap() of hv_layout.c, which has
 * its own test in aicmap_test.c. What is checked here is where it goes, that the firmware is
 * told so, and that it is what the host's loader gets from the same function (P_HV_AICMAP of
 * src/proxy.c calls nothing else): both loaders hand the firmware the same bytes.
 */
#include <setjmp.h>
#include <sys/mman.h>

#include "aic.h"
#include "cpu_regs.h"
#include "hv.h"
#include "iodev.h"
#include "mcc.h"
#include "model.h"
#include "selfboot.h"
#include "utils.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the machine ------------------------------------------------------------------------- */

/* Where the heap of m1n1 ends when it is asked to start the firmware: not a whole page */
#define HEAP_TOP 0x1000898f040UL
/*
 * 80 MB above that, up to the next 16 KB: where guest RAM starts when m1n1 loads by itself.
 * What m1n1 keeps for itself on 16 GB of memory: 32 MB, 16 GB / 512, 16 MB.
 */
#define SELF_PHYS_BASE 0x1000d990000UL
#define WINDOW         (48UL << 20)
#define FILL           0xee

#define PART_UUID "8a3c2d1e-5b6f-4a70-9c1d-2e3f4a5b6c7d"

#define IMAGE_SIZE 0x10000UL
#define DISK_SIZE  0x3000UL
#define ADT_SIZE   0x84000UL

#define UART_BASE 0x3ad200000UL /* "Virtual UART at 0x3ad200000" */
#define UART_IRQ  0x3c9
/* The phandle of AIC on the M4, and the lines of a device of the tree besides the UART */
#define AIC_PHANDLE 0xd5
#define DEV_IRQS    {1505, 5, UART_IRQ, 0x70305452}
/* ...which make this table: the lines in order, each once, without what is no line */
static const u32 table_lines[] = {5, UART_IRQ, 1505};
/* hv_map_vuart() of src/hv/hv_vuart.c takes the first 4 KB of the UART */
#define VUART_SIZE 0x1000UL
/* What its reg says: all of it, which the guest does not have without a host */
#define UART_SIZE 0x4000UL

/* HCR_EL2 as hv_init() of this tree leaves it, and as it was when the firmware ran in R21 */
#define HCR_AFTER_HV_INIT 0x00032480002031UL
#define HCR_R21           0x80032480202039UL
#define SCTLR_EL12_R21    0x30d50980UL
/* Whatever it is on the M4; bit 12 is not set in it */
#define ACTLR_EL12_BEFORE 0x0000000000000c40UL
/*
 * proxyclient/m1n1/sysreg.py: class HACR, the seven traps HV.init() sets (TRAP_CTRR 14,
 * TRAP_IPI 16, TRAP_SERROR_INFO 48, TRAP_EHID 49, TRAP_HID 50, TRAP_ACC 52, TRAP_PM 57),
 * class ACTLR (EnMDSB 12), class MDCR (TDE 8, TDA 9, TDOSA 10, TDRA 11)
 */
#define HACR_OF_THE_HOST 0x0217000000014000UL
#define ACTLR_ENMDSB     0x1000UL
#define MDCR_WANTED      0xe00UL

struct boot_args cur_boot_args;
u64 ram_base, mem_size_actual;
/* The AIC of the M4: one die of 2048 lines, room for two of 4096 */
struct aic *aic;
static struct aic m4_aic;
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[4];

/* ---- the device tree ---------------------------------------------------------------------- */

static u8 tree[ADT_SIZE], tree_before[ADT_SIZE];
static int spare_entries;
static u32 arm_io_cells;
static bool with_ranges;
/* A device the M4 does not have: where it is and its size, none if that is 0 */
static u64 one_more_device[2];
/* Whether /arm-io has an AIC; a device with this many lines more, 0 to n - 1 */
static bool with_aic;
static u32 many_lines;
/* Whether /arm-io has uart0 */
static bool with_uart;

/* The ranges of /arm-io are those of R21 (r21_mmio of model.c) */
static void make_tree(void)
{
    u8 *p = tree;
    char name[32];
    u64 ranges[R21_MMIO_COUNT + 1][3];
    u64 uart_reg[2] = {UART_BASE - R21_BUS_OFFSET, UART_SIZE};
    unsigned n_ranges = R21_MMIO_COUNT;

    for (unsigned i = 0; i < R21_MMIO_COUNT; i++) {
        ranges[i][0] = r21_mmio[i][0] - R21_BUS_OFFSET;
        ranges[i][1] = r21_mmio[i][0];
        ranges[i][2] = r21_mmio[i][1] - r21_mmio[i][0];
    }
    if (one_more_device[1]) {
        ranges[n_ranges][0] = one_more_device[0] - R21_BUS_OFFSET;
        ranges[n_ranges][1] = one_more_device[0];
        ranges[n_ranges++][2] = one_more_device[1];
    }

    memset(tree, 0, sizeof(tree));
    p = put_node(p, 3, 2);
    p = put_name(p, "device-tree");
    p = put_prop(p, "#address-cells", &(u32){2}, 4);
    p = put_prop(p, "#size-cells", &(u32){2}, 4);

    p = put_node(p, 1, 1);
    p = put_name(p, "chosen");
    p = put_node(p, 5 + spare_entries, 0);
    p = put_name(p, "memory-map");
    p = put_range(p, "BootArgs", 0x100061ac000, 0x4000);
    p = put_range(p, "DeviceTree", 0x10005490000, 0x84000);
    for (int i = 0; i < spare_entries; i++) {
        snprintf(name, sizeof(name), "MemoryMapReserved-%d", i);
        p = put_range(p, name, 0, 0);
    }
    p = put_range(p, "SEPFW", 0x10005a6c000, 0x5d0000);
    p = put_range(p, "TrustCache", 0x10006138000, 0x74000);

    p = put_node(p, with_ranges ? 4 : 3, 1 + with_uart + with_aic + !!many_lines);
    p = put_name(p, "arm-io");
    p = put_prop(p, "#address-cells", &arm_io_cells, 4);
    p = put_prop(p, "#size-cells", &(u32){2}, 4);
    if (with_ranges)
        p = put_prop(p, "ranges", ranges, n_ranges * sizeof(ranges[0]));
    if (with_aic) {
        p = put_node(p, 2, 0);
        p = put_name(p, "aic");
        p = put_prop(p, "AAPL,phandle", &(u32){AIC_PHANDLE}, 4);
    }
    if (with_uart) {
        p = put_node(p, 4, 0);
        p = put_name(p, "uart0");
        p = put_prop(p, "reg", uart_reg, sizeof(uart_reg));
        p = put_prop(p, "interrupt-parent", &(u32){AIC_PHANDLE}, 4);
        p = put_prop(p, "interrupts", &(u32){UART_IRQ}, 4);
    }
    p = put_node(p, 3, 0);
    p = put_name(p, "dev");
    p = put_prop(p, "interrupt-parent", &(u32){AIC_PHANDLE}, 4);
    p = put_prop(p, "interrupts", (u32[])DEV_IRQS, sizeof((u32[])DEV_IRQS));
    if (many_lines) {
        static u32 lines[2048];

        for (u32 i = 0; i < many_lines; i++)
            lines[i] = i;
        p = put_node(p, 3, 0);
        p = put_name(p, "many");
        p = put_prop(p, "interrupt-parent", &(u32){AIC_PHANDLE}, 4);
        p = put_prop(p, "interrupts", lines, 4 * many_lines);
    }

    adt = tree;
    memcpy(tree_before, tree, sizeof(tree));
}

/* ---- guest memory ------------------------------------------------------------------------- */

static void map_window(u64 base)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    /* Darwin treats base as a hint. Accept only the requested address. */
    void *p = mmap((void *)base, WINDOW, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p != (void *)base) {
        printf("run_test: no memory at 0x%lx for the guest\n", base);
        exit(2);
    }
}

/* Where things go above a start of guest RAM, worked out here a second time */
struct where {
    u64 phys_base, adt, image, bootargs, aicmap, disk, top;
};

static struct where where(u64 phys_base)
{
    struct where w = {.phys_base = phys_base};

    w.adt = phys_base + (32UL << 20);
    w.image = (w.adt + ADT_SIZE + 0x1fffff) & ~0x1fffffUL;
    w.bootargs = w.image + IMAGE_SIZE;
    w.aicmap = w.bootargs + 0x4000;
    w.disk = (w.aicmap + 0x4000 + 0x1fffff) & ~0x1fffffUL;
    w.top = (w.disk + DISK_SIZE + 0x3fff) & ~0x3fffUL;
    return w;
}

/* ---- the two files ------------------------------------------------------------------------ */

static u8 image_file[IMAGE_SIZE], disk_file[DISK_SIZE];

static void make_files(void)
{
    u64 size = IMAGE_SIZE;

    for (u64 i = 0; i < IMAGE_SIZE; i++)
        image_file[i] = i * 7 + (i >> 8);
    memcpy(image_file, &(u32){0x10000001}, 4);     /* adr x1, . */
    memcpy(image_file + 4, &(u32){0x14001fff}, 4); /* b . + 0x7ffc */
    memcpy(image_file + 0x10, &size, 8);
    memcpy(image_file + 0x38, "ARM\x64", 4);

    for (u64 i = 0; i < DISK_SIZE; i++)
        disk_file[i] = i * 13 + (i >> 7);
    memcpy(disk_file + 512, "EFI PART", 8);
}

/* ---- what the code under test did, in order ----------------------------------------------- */

enum ev {
    EV_HEAP_LIMIT, /* a: the limit */
    EV_NVME_INIT,
    EV_READ_FILE, /* a: which file (0 image, 1 disk, 2 another), b: dest, c: max_size */
    EV_NVME_SHUTDOWN,
    EV_HV_INIT,
    EV_WRITE_HCR,  /* a: the value */
    EV_WRITE_HACR, /* a: the value */
    EV_MSR,        /* a: the register, b: the value */
    EV_MAP_HW,     /* a: from, b: to, c: size */
    EV_UNMAP,      /* a: from, c: size */
    EV_MAP_VUART,  /* a: base, b: irq, c: iodev */
    EV_XTRACK,
    EV_CTRR,
    EV_CPUS,
    EV_IODEV_USAGE, /* a: iodev, b: usage */
    EV_FB_SHUTDOWN, /* a: restore_logo */
    EV_DC_CVAU,     /* a: address, c: length */
    EV_IC_IVAU,     /* a: address, c: length */
    EV_HV_START,    /* a: entry, b: regs[0], c: regs[1] | regs[2] | regs[3] */
};

static struct event {
    enum ev ev;
    u64 a, b, c;
} events[256];
static int n_events;

static void note(enum ev ev, u64 a, u64 b, u64 c)
{
    if (n_events == COUNT(events)) {
        printf("run_test: more events than there is room for\n");
        exit(2);
    }
    events[n_events++] = (struct event){ev, a, b, c};
}

static int count(enum ev ev)
{
    int n = 0;
    for (int i = 0; i < n_events; i++)
        n += events[i].ev == ev;
    return n;
}

/* The place of the first and of the last event of a kind; -1 if there is none */
static int first(enum ev ev)
{
    for (int i = 0; i < n_events; i++)
        if (events[i].ev == ev)
            return i;
    return -1;
}

static int last(enum ev ev)
{
    for (int i = n_events - 1; i >= 0; i--)
        if (events[i].ev == ev)
            return i;
    return -1;
}

static const struct event *the(enum ev ev)
{
    static const struct event none = {0, NOTHING, NOTHING, NOTHING};
    return count(ev) == 1 ? &events[first(ev)] : &none;
}

/* ---- the stand-ins ------------------------------------------------------------------------ */

/* What can be made to go wrong */
static bool nvme_fails;
static const char *missing_file;
static int map_hw_fails_at; /* the how-manieth call of hv_map_hw() fails; -1: none */
static u64 sctlr_el12;
static bool xtrack_fails;
/* The guest has some of the memory that is read-only on the other cores (CTRR) */
static bool ctrr_fails;
static bool cpu_missing;
/*
 * What hv_start() does. With a host the guest runs until the host leaves it, and hv_start()
 * returns. Without one it runs for good, and hv_start() does not return: here it goes back to
 * where the test called (from_the_ssd(), loaded()), which says ENTERED. Or the hypervisor finds
 * something that keeps it from entering the guest, and returns.
 */
static enum { GUEST_LEFT, GUEST_STAYS, NOT_ENTERED } hv_start_does;
static jmp_buf in_the_guest;
#define ENTERED 1000

/* What each port is used for; the proxy's port is the first USB port */
static iodev_usage_t usages[IODEV_NUM], usages_before[IODEV_NUM];

static bool nvme_up;
static u64 hcr_el2, actlr_el12;
static int map_hw_calls, stray;
/* Where the heap of m1n1 ends: HEAP_TOP, unless a test says otherwise */
static u64 heap_top;

void *heapblock_alloc(size_t size)
{
    /* Asking for nothing says where the heap ends; nothing may be taken from it here */
    stray += size != 0;
    return (void *)heap_top;
}

void heapblock_set_limit(void *limit)
{
    note(EV_HEAP_LIMIT, (u64)limit, 0, 0);
}

bool nvme_init(void)
{
    note(EV_NVME_INIT, 0, 0, 0);
    nvme_up = !nvme_fails;
    return nvme_up;
}

void nvme_shutdown(void)
{
    note(EV_NVME_SHUTDOWN, 0, 0, 0);
    stray += !nvme_up;
    nvme_up = false;
}

/* rust/src/readfile.rs */
int rust_read_file(const char *part_uuid, const char *path, void *dest, size_t max_size,
                   size_t *size)
{
    const u8 *file = NULL;
    size_t len = 0;
    int which = 2;

    if (!strcmp(path, "siliconnt/firmware.fd")) {
        which = 0;
        file = image_file;
        len = IMAGE_SIZE;
    } else if (!strcmp(path, "siliconnt/bootdisk.img")) {
        which = 1;
        file = disk_file;
        len = DISK_SIZE;
    }
    note(EV_READ_FILE, which, (u64)dest, max_size);
    /* The controller has to be running, and the partition is the one of the variable */
    stray += !nvme_up || strcmp(part_uuid, PART_UUID);
    if (!file || (missing_file && !strcmp(path, missing_file)) || len > max_size)
        return -1;
    memcpy(dest, file, len);
    *size = len;
    return 0;
}

u64 mock_mrs(int reg)
{
    if (reg == HCR_EL2)
        return hcr_el2;
    if (reg == SYS_SCTLR_EL12)
        return sctlr_el12;
    if (reg == SYS_ACTLR_EL12)
        return actlr_el12;
    stray++;
    printf("run_test: register 0x%x was read\n", reg);
    return 0;
}

void mock_msr(int reg, u64 val)
{
    note(EV_MSR, reg, val, 0);
    if (reg == SYS_ACTLR_EL12)
        actlr_el12 = val;
}

void hv_init(void)
{
    note(EV_HV_INIT, 0, 0, 0);
    hcr_el2 = HCR_AFTER_HV_INIT;
}

/* src/hv/hv.c, which has its own test in siliconnt/vgic-model/cpus_test.c */
bool hv_cpus_ready(void)
{
    note(EV_CPUS, 0, 0, 0);
    return !cpu_missing;
}

void hv_write_hcr(u64 val)
{
    note(EV_WRITE_HCR, val, 0, 0);
    hcr_el2 = val;
}

void hv_write_hacr(u64 val)
{
    note(EV_WRITE_HACR, val, 0, 0);
}

int hv_map_hw(u64 from, u64 to, u64 size)
{
    note(EV_MAP_HW, from, to, size);
    /* src/hv/hv_vm.c: a mapping of the hardware is whole 4 KB pages */
    if ((from | size) & 0xfff)
        return -1;
    return map_hw_calls++ == map_hw_fails_at ? -1 : 0;
}

int hv_unmap(u64 from, u64 size)
{
    note(EV_UNMAP, from, 0, size);
    return 0;
}

void hv_map_vuart(u64 base, int irq, iodev_id_t iodev)
{
    note(EV_MAP_VUART, base, irq, iodev);
}

int hv_pt_xtrack(void)
{
    note(EV_XTRACK, 0, 0, 0);
    return xtrack_fails ? -1 : 0;
}

/* src/hv/hv_vm.c, which has its own test in siliconnt/vgic-model/pt_test.c */
int hv_pt_check_ctrr(void)
{
    note(EV_CTRR, 0, 0, 0);
    return ctrr_fails ? -1 : 0;
}

void hv_start(void *entry, u64 regs[4])
{
    note(EV_HV_START, (u64)entry, regs[0], regs[1] | regs[2] | regs[3]);
    if (hv_start_does == GUEST_STAYS)
        longjmp(in_the_guest, 1);
}

void iodev_set_usage(iodev_id_t id, iodev_usage_t usage)
{
    note(EV_IODEV_USAGE, id, usage, 0);
    usages[id] = usage;
}

iodev_usage_t iodev_get_usage(iodev_id_t id)
{
    return usages[id];
}

/* m1n1 loads by itself, or a host has loaded: what the call returns, or ENTERED */
static int from_the_ssd(void)
{
    if (setjmp(in_the_guest))
        return ENTERED;
    return selfboot_run(PART_UUID);
}

static int loaded(u64 phys_base, u64 image_size, u64 disk_size, int host_iodev)
{
    if (setjmp(in_the_guest))
        return ENTERED;
    return selfboot_run_loaded(phys_base, image_size, disk_size, host_iodev);
}

void fb_shutdown(bool restore_logo)
{
    note(EV_FB_SHUTDOWN, restore_logo, 0, 0);
}

void dc_cvau_range(void *addr, size_t length)
{
    note(EV_DC_CVAU, (u64)addr, 0, length);
}

void ic_ivau_range(void *addr, size_t length)
{
    note(EV_IC_IVAU, (u64)addr, 0, length);
}

/* The machine as m1n1 finds it: the M4 of R21, nothing going wrong */
static void reset(void)
{
    n_events = 0;
    stray = 0;
    nvme_fails = false;
    missing_file = NULL;
    map_hw_fails_at = -1;
    map_hw_calls = 0;
    xtrack_fails = false;
    ctrr_fails = false;
    cpu_missing = false;
    heap_top = HEAP_TOP;
    hv_start_does = GUEST_STAYS;
    /* The proxy on the first USB port; a console on the UART, the screen, the second port */
    memset(usages, 0, sizeof(usages));
    usages[IODEV_UART] = usages[IODEV_FB] = usages[IODEV_USB0 + 1] = 1;
    usages[IODEV_USB0] = 3;
    memcpy(usages_before, usages, sizeof(usages));
    sctlr_el12 = SCTLR_EL12_R21;
    nvme_up = false;
    hcr_el2 = 0x5555555555555555UL; /* nobody is to build on what it is before hv_init() */
    actlr_el12 = ACTLR_EL12_BEFORE;

    r21_ba(&cur_boot_args);
    /* mmu_add_default_mappings() of src/memory.c */
    ram_base = cur_boot_args.phys_base & ~0xffffffffUL;
    mem_size_actual = cur_boot_args.rv3.mem_size_actual;
    /* "Unmapping TZ carveouts..." of R21 */
    mcc_carveouts[0] = (struct mcc_carveout){0x103db610000, 0x103dd680000 - 0x103db610000};
    mcc_carveouts[1] = (struct mcc_carveout){0x103e6da4000, 0x103ffda4000 - 0x103e6da4000};
    mcc_carveouts[2] = (struct mcc_carveout){0, 0};
    mcc_carveout_count = 2;

    spare_entries = 3;
    arm_io_cells = 2;
    with_ranges = true;
    one_more_device[0] = one_more_device[1] = 0;
    with_aic = true;
    many_lines = 0;
    with_uart = true;
    make_tree();

    memset(&m4_aic, 0, sizeof(m4_aic));
    m4_aic.version = 3;
    m4_aic.nr_die = 1;
    m4_aic.max_die = 2;
    m4_aic.nr_irq = 2048;
    m4_aic.max_irq = 4096;
    aic = &m4_aic;
    make_files();

    memset((void *)SELF_PHYS_BASE, FILL, WINDOW);
    memset((void *)R21_PHYS_BASE, FILL, WINDOW);
}

/* ---- what stage 2 looks like in the end ---------------------------------------------------- */

enum kind { NONE, HW, VUART };

struct seg {
    u64 start, end;
    enum kind kind;
};

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

/* The mapping calls, each over what the ones before it left; neighbours of a kind as one */
static int final_map(struct seg *out, int max)
{
    static u64 bounds[2 * COUNT(events)];
    int n = 0, segs = 0;

    for (int i = 0; i < n_events; i++) {
        const struct event *e = &events[i];
        if (e->ev == EV_MAP_HW || e->ev == EV_UNMAP) {
            bounds[n++] = e->a;
            bounds[n++] = e->a + e->c;
        } else if (e->ev == EV_MAP_VUART) {
            bounds[n++] = e->a;
            bounds[n++] = e->a + VUART_SIZE;
        }
    }
    qsort(bounds, n, sizeof(bounds[0]), cmp_u64);

    for (int b = 0; b + 1 < n; b++) {
        enum kind kind = NONE;

        if (bounds[b] == bounds[b + 1])
            continue;
        for (int i = 0; i < n_events; i++) {
            const struct event *e = &events[i];
            u64 size = e->ev == EV_MAP_VUART ? VUART_SIZE : e->c;

            if (e->ev != EV_MAP_HW && e->ev != EV_UNMAP && e->ev != EV_MAP_VUART)
                continue;
            if (bounds[b] < e->a || bounds[b] >= e->a + size)
                continue;
            kind = e->ev == EV_MAP_HW ? HW : e->ev == EV_UNMAP ? NONE : VUART;
        }
        if (kind == NONE)
            continue;
        if (segs && out[segs - 1].end == bounds[b] && out[segs - 1].kind == kind) {
            out[segs - 1].end = bounds[b + 1];
        } else if (segs < max) {
            out[segs++] = (struct seg){bounds[b], bounds[b + 1], kind};
        }
    }
    return segs;
}

/*
 * The "PT[...]" lines of R21, with guest RAM starting at phys_base. Where the host had the
 * registers it answers itself (RVBAR, CPU STATE HACK, PMGR HACK, CPU_START) there is the
 * device; of the UART the hypervisor takes the 4 KB that hv_map_vuart() maps. Without a host
 * the guest has no UART at all: all of its registers are taken out (SiliconNT docs/DESIGN.md,
 * "开发模式").
 */
static void check_map(u64 phys_base, bool host)
{
    struct seg want[] = {
        {0x200000000, UART_BASE, HW},
        {UART_BASE, UART_BASE + VUART_SIZE, VUART},
        {UART_BASE + VUART_SIZE, 0x5a0000000, HW},
        {0x800000000, 0xae0000000, HW},
        {0xb80000000, 0xbe0000000, HW},
        {0xc00000000, 0xe80000000, HW},
        {0x1000000000, 0x1280000000, HW},
        {0x1400000000, 0x1680000000, HW},
        {0x1c50000000, 0x1c90000000, HW},
        {0x1cb0000000, 0x1cc0000000, HW},
        /* RAM-LOW, RAM-HIGH without the two carveouts */
        {0x10000000000, 0x10002e70000, HW},
        {phys_base, 0x103db610000, HW},
        {0x103dd680000, 0x103e6da4000, HW},
        {0x103ffda4000, 0x10400000000, HW},
    };
    size_t n_want = COUNT(want);
    struct seg got[32];
    int n = final_map(got, COUNT(got));

    if (!host) {
        want[1] = (struct seg){UART_BASE + UART_SIZE, 0x5a0000000, HW};
        memmove(&want[2], &want[3], (n_want - 3) * sizeof(want[0]));
        n_want--;
    }
    CHECK_EQ(n, n_want);
    for (int i = 0; i < n && i < (int)n_want; i++) {
        CHECK_EQ(got[i].start, want[i].start);
        CHECK_EQ(got[i].end, want[i].end);
        CHECK_EQ(got[i].kind, want[i].kind);
    }
    /* Every address of the guest is the same address of the machine */
    for (int i = 0; i < n_events; i++)
        if (events[i].ev == EV_MAP_HW)
            CHECK_EQ(events[i].b, events[i].a);
    /* m1n1 and its heap, from where iBoot's memory starts to where the guest's does, are not
     * the guest's to see */
    for (int i = 0; i < n; i++)
        CHECK(got[i].end <= cur_boot_args.phys_base || got[i].start >= phys_base);
}

/* ---- what the firmware is handed ------------------------------------------------------------ */

static bool guest_range_is(const struct where *w, const char *name, u64 base, u64 size)
{
    void *m1n1_tree = adt;
    const struct adt_property *p;
    u64 want[2] = {base, size};
    int node;

    /* Not a tree at all: nothing to walk */
    if (memcmp((void *)w->adt, tree, 0x40))
        return false;
    adt = (void *)w->adt;
    node = adt_path_offset(adt, "/chosen/memory-map");
    p = node < 0 ? NULL : adt_get_property(adt, node, name);
    adt = m1n1_tree;
    return p && p->size == sizeof(want) && !memcmp(p->value, want, sizeof(want));
}

static bool all_bytes(u64 start, u64 end, u8 value)
{
    for (const u8 *p = (const u8 *)start; p < (const u8 *)end; p++)
        if (*p != value)
            return false;
    return true;
}

/*
 * /arm-io/uart0 in the guest's tree: there with a host. Without one its name is zeroes, of the
 * length it had, so that the firmware, which finds the node by its path, does not, and the
 * rest of the tree is m1n1's (SiliconNT docs/DESIGN.md, "开发模式")
 */
static void check_uart_in_tree(const struct where *w, bool host)
{
    int node = adt_path_offset(adt, "/arm-io/uart0");
    const struct adt_property *name = adt_get_property(adt, node, "name");
    size_t at = (const u8 *)name->value - tree;
    const u8 *guest = (const u8 *)w->adt;
    void *m1n1_tree = adt;
    int found;

    adt = (void *)w->adt;
    found = adt_path_offset(adt, "/arm-io/uart0");
    adt = m1n1_tree;
    CHECK(host ? found == node : found < 0);
    for (size_t i = at; i < at + name->size; i++)
        CHECK_EQ(guest[i], host ? tree[i] : 0);
    CHECK(!memcmp(guest + at + name->size, tree + at + name->size, 0x4000));
}

/* Guest memory when the guest is entered */
static void check_memory(const struct where *w)
{
    const struct boot_args *ba = (const struct boot_args *)w->bootargs;
    struct boot_args expect;

    /* The two files, where the layout says, and nothing around them */
    CHECK(!memcmp((void *)w->image, image_file, IMAGE_SIZE));
    CHECK(!memcmp((void *)w->disk, disk_file, DISK_SIZE));

    /* The boot arguments: m1n1's, with the guest's memory and device tree; the rest of the
     * page is 0 */
    memcpy(&expect, &cur_boot_args, sizeof(expect));
    expect.phys_base = w->phys_base;
    expect.mem_size = R21_MEM_TOP - w->phys_base;
    expect.top_of_kernel_data = w->top;
    expect.virt_base = ba->virt_base;
    expect.devtree = ba->devtree;
    CHECK(!memcmp(ba, &expect, sizeof(expect)));
    CHECK_EQ((u64)ba->devtree - ba->virt_base + ba->phys_base, w->adt);
    CHECK(all_bytes(w->bootargs + sizeof(*ba), w->bootargs + 0x4000, 0));

    /* The table: version 1, the count, the lines; what P_HV_AICMAP writes, byte for byte */
    const u32 *table = (const u32 *)w->aicmap;
    u32 table_size = 8 + sizeof(table_lines);
    static struct hv_aicmap of_the_host;

    CHECK_EQ(table[0], 1);
    CHECK_EQ(table[1], COUNT(table_lines));
    CHECK(!memcmp(table + 2, table_lines, sizeof(table_lines)));
    memset(&of_the_host, 0, sizeof(of_the_host));
    CHECK_EQ(hv_aicmap(&of_the_host), table_size);
    CHECK(!memcmp(table, &of_the_host, table_size));

    /* The device tree: m1n1's, with four entries of /chosen/memory-map changed */
    CHECK(guest_range_is(w, "DeviceTree", w->adt, ADT_SIZE));
    CHECK(guest_range_is(w, "BootArgs", w->bootargs, 0x4000));
    CHECK(guest_range_is(w, "RAMDisk", w->disk, DISK_SIZE));
    CHECK(guest_range_is(w, "AICToGIC", w->aicmap, table_size));
    /*
     * What the firmware checks of the entry before it reads the table (AppleGetAicToGic() of
     * AppleDTLib.c, siliconnt-firmware aic-to-gic 8b3fe9d): the size is that of the table,
     * 8 + 4 * its count; the address is the start of a 4 KB page; all of it lies in the
     * memory the boot arguments describe; the lines go up strictly
     */
    CHECK_EQ(table_size, 8 + 4 * table[1]);
    CHECK_EQ(w->aicmap % 0x1000, 0);
    CHECK(w->aicmap >= ba->phys_base && w->aicmap + table_size <= ba->phys_base + ba->mem_size);
    for (u32 i = 1; i < table[1] && i < 988; i++)
        CHECK(table[2 + i] > table[2 + i - 1]);
    CHECK(guest_range_is(w, "SEPFW", 0x10005a6c000, 0x5d0000));
    CHECK(!memcmp((void *)w->adt, tree, 0x40));
    CHECK(!memcmp(tree, tree_before, sizeof(tree)));

    /* Nothing else of the window was written: not the 32 MB below the device tree, not the
     * gaps, not what follows the disk */
    CHECK(all_bytes(w->phys_base, w->adt, FILL));
    CHECK(all_bytes(w->adt + ADT_SIZE, w->image, FILL));
    CHECK(all_bytes(w->aicmap + table_size, w->disk, FILL));
    CHECK(all_bytes(w->disk + DISK_SIZE, w->phys_base + WINDOW, FILL));
}

/* The registers HV.init() of the host sets after hv_init(), as far as the M4 has them */
static void check_registers(void)
{
    const struct event *hcr = the(EV_WRITE_HCR), *hacr = the(EV_WRITE_HACR);
    bool mdcr = false, mdscr = false, actlr = false;

    /* What the firmware ran under in R21: TACR, FMO and TTLBOS on top of hv_init() */
    CHECK_EQ(hcr->a, HCR_R21);
    CHECK_EQ(hacr->a, HACR_OF_THE_HOST);
    CHECK_EQ(count(EV_MSR), 3);
    for (int i = 0; i < n_events; i++) {
        const struct event *e = &events[i];

        if (e->ev != EV_MSR)
            continue;
        if (e->a == MDCR_EL2) {
            /* The three access traps. Not TDE: a BRK of the guest goes to the guest */
            CHECK_EQ(e->b, MDCR_WANTED);
            mdcr = true;
        } else if (e->a == SYS_MDSCR_EL1) {
            CHECK_EQ(e->b, 0);
            mdscr = true;
        } else if (e->a == SYS_ACTLR_EL12) {
            CHECK_EQ(e->b, ACTLR_EL12_BEFORE | ACTLR_ENMDSB);
            actlr = true;
        } else {
            CHECK_EQ(e->a, NOTHING);
        }
    }
    CHECK(mdcr && mdscr && actlr);
    /* All of them after the hypervisor is initialized, which sets HCR_EL2 itself */
    CHECK(first(EV_HV_INIT) < first(EV_WRITE_HCR) && first(EV_HV_INIT) < first(EV_WRITE_HACR) &&
          first(EV_HV_INIT) < first(EV_MSR));
}

/*
 * The guest was entered, once, as the last thing, with everything in place. host_iodev: the
 * port of the host, or -1 for none
 */
static void check_entry(const struct where *w, int host_iodev)
{
    const struct event *start = the(EV_HV_START), *vuart = the(EV_MAP_VUART);
    bool left[IODEV_MAX] = {false};
    bool host = host_iodev >= 0;

    CHECK_EQ(count(EV_HV_INIT), 1);
    CHECK_EQ(count(EV_HV_START), 1);
    CHECK_EQ(last(EV_HV_START), n_events - 1);
    /* At the first byte of the image, x0 the boot arguments, x1 to x3 0 */
    CHECK_EQ(start->a, w->image);
    CHECK_EQ(start->b, w->bootargs);
    CHECK_EQ(start->c, 0);

    check_registers();
    check_map(w->phys_base, host);
    check_uart_in_tree(w, host);

    if (host) {
        /* The virtual UART: where /arm-io/uart0 is, with its interrupt, for the host's port */
        CHECK_EQ(vuart->a, UART_BASE);
        CHECK_EQ(vuart->b, UART_IRQ);
        CHECK_EQ(vuart->c, host_iodev);
    } else {
        /* None, and all of the UART taken from the guest, once */
        CHECK_EQ(count(EV_MAP_VUART), 0);
        CHECK_EQ(count(EV_UNMAP), 3);
        for (int i = 0; i < n_events; i++)
            if (events[i].ev == EV_UNMAP && events[i].a == UART_BASE)
                CHECK(events[i].c == UART_SIZE && i > last(EV_MAP_HW));
    }

    /* Execution tracking: once, when every mapping is there, so that the memory is taken
     * apart into pages before the guest runs */
    CHECK_EQ(count(EV_XTRACK), 1);
    CHECK(first(EV_XTRACK) > last(EV_MAP_HW) && first(EV_XTRACK) > last(EV_UNMAP) &&
          first(EV_XTRACK) > last(EV_MAP_VUART));
    CHECK(first(EV_MAP_HW) > first(EV_HV_INIT));

    /*
     * The memory that is read-only on the other cores: once every mapping is there, and
     * while the screen is still m1n1's, to say so on (hv_start() asks again)
     */
    CHECK_EQ(count(EV_CTRR), 1);
    CHECK(first(EV_CTRR) > first(EV_XTRACK) && first(EV_CTRR) < first(EV_FB_SHUTDOWN));

    /*
     * The cores that hv_init() has started: as soon as it is known which have come, while the
     * screen is still m1n1's, to say so on (hv_start() asks again)
     */
    CHECK_EQ(count(EV_CPUS), 1);
    CHECK(first(EV_CPUS) > first(EV_HV_INIT) && first(EV_CPUS) < first(EV_MAP_HW) &&
          first(EV_CPUS) < first(EV_FB_SHUTDOWN));

    /* The image is in the instruction cache's view before anything executes it */
    CHECK_EQ(the(EV_DC_CVAU)->a, w->image);
    CHECK_EQ(the(EV_DC_CVAU)->c, IMAGE_SIZE);
    CHECK_EQ(the(EV_IC_IVAU)->a, w->image);
    CHECK_EQ(the(EV_IC_IVAU)->c, IMAGE_SIZE);
    CHECK(first(EV_DC_CVAU) < first(EV_IC_IVAU));

    /* m1n1 gives up the screen and stops printing, except to the host if there is one; only
     * when nothing can fail any more, so that a failure can still be read */
    CHECK_EQ(the(EV_FB_SHUTDOWN)->a, true);
    CHECK(first(EV_FB_SHUTDOWN) > first(EV_XTRACK));
    for (int i = 0; i < n_events; i++) {
        if (events[i].ev != EV_IODEV_USAGE)
            continue;
        /*
         * The USB ports nobody talks over go first, as in HV.init(): hv_init() gives the
         * interrupts of their port controllers back, for the guest, where the port is not
         * m1n1's (usb_hpm_restore_irqs() of src/usb.c)
         */
        if (events[i].a >= IODEV_USB0)
            CHECK(i < first(EV_HV_INIT));
        else
            CHECK(i > first(EV_XTRACK));
        CHECK_EQ(events[i].b, 0);
        CHECK(events[i].a < IODEV_MAX && (int)events[i].a != host_iodev);
        if (events[i].a < IODEV_MAX)
            left[events[i].a] = true;
    }
    for (int i = 0; i < IODEV_MAX; i++)
        CHECK_EQ(left[i], i != host_iodev);

    check_memory(w);
    CHECK_EQ(stray, 0);
}

/* It did not get as far as the hypervisor: m1n1 is as it was, for the proxy to take over */
static void check_not_started(int ret)
{
    CHECK_EQ(ret, -1);
    CHECK_EQ(count(EV_HV_INIT), 0);
    CHECK_EQ(count(EV_HV_START), 0);
    CHECK_EQ(count(EV_WRITE_HCR) + count(EV_WRITE_HACR) + count(EV_MSR), 0);
    CHECK_EQ(count(EV_MAP_HW) + count(EV_UNMAP) + count(EV_MAP_VUART) + count(EV_XTRACK), 0);
    CHECK_EQ(count(EV_CTRR) + count(EV_CPUS), 0);
    CHECK_EQ(count(EV_IODEV_USAGE) + count(EV_FB_SHUTDOWN), 0);
    /* The SSD controller is not left running */
    CHECK_EQ(count(EV_NVME_SHUTDOWN), nvme_fails ? 0 : count(EV_NVME_INIT));
    CHECK(!nvme_up);
    CHECK(!memcmp(tree, tree_before, sizeof(tree)));
    CHECK_EQ(stray, 0);
}

/*
 * The ports after the guest was not entered. With a host: its own as it was, the spare USB
 * ports the guest's (as before), the UART and the screen untouched. Without one every port is
 * what it was, the proxy's port the proxy's again, so that a host can connect.
 */
static void check_ports_given_back(int host_iodev)
{
    for (int i = 0; i < IODEV_NUM; i++) {
        if (host_iodev < 0 || i < IODEV_USB0 || i == host_iodev)
            CHECK_EQ(usages[i], usages_before[i]);
        else
            CHECK_EQ(usages[i], 0);
    }
}

/* The hypervisor was initialized, but the guest never ran, and m1n1 still prints */
static void check_not_entered(int ret, int host_iodev)
{
    CHECK_EQ(ret, -1);
    CHECK_EQ(count(EV_HV_INIT), 1);
    CHECK_EQ(count(EV_HV_START), 0);
    CHECK_EQ(count(EV_CPUS), 1);
    CHECK_EQ(count(EV_XTRACK) + count(EV_CTRR), 0);
    /* The screen as it was; the consoles that are not USB ports are not even touched */
    CHECK_EQ(count(EV_FB_SHUTDOWN), 0);
    if (host_iodev >= 0)
        for (int i = 0; i < n_events; i++)
            CHECK(events[i].ev != EV_IODEV_USAGE || events[i].a >= IODEV_USB0);
    check_ports_given_back(host_iodev);
    CHECK_EQ(stray, 0);
}

static void put_files(const struct where *w);

/* ---- m1n1 loads by itself ------------------------------------------------------------------ */

static void test_from_the_ssd(void)
{
    struct where w = where(SELF_PHYS_BASE);
    const struct event *limit, *read;

    reset();
    /* The guest ran and was left */
    CHECK_EQ(from_the_ssd(), ENTERED);

    /* Guest RAM starts 80 MB above the heap, on a 16 KB page, and the heap ends there */
    limit = the(EV_HEAP_LIMIT);
    CHECK_EQ(limit->a, SELF_PHYS_BASE);

    /* The controller runs while the two files are read, and only then */
    CHECK_EQ(count(EV_NVME_INIT), 1);
    CHECK_EQ(count(EV_NVME_SHUTDOWN), 1);
    CHECK_EQ(count(EV_READ_FILE), 2);
    CHECK(first(EV_HEAP_LIMIT) < first(EV_NVME_INIT));
    CHECK(first(EV_NVME_INIT) < first(EV_READ_FILE));
    CHECK(last(EV_READ_FILE) < first(EV_NVME_SHUTDOWN));
    CHECK(first(EV_NVME_SHUTDOWN) < first(EV_HV_INIT));

    /* The image first, straight to where it goes, with all the room there is for it: up to
     * two pages, for the boot arguments and the table, below the firmware's work area */
    read = &events[first(EV_READ_FILE)];
    CHECK_EQ(read->a, 0);
    CHECK_EQ(read->b, w.image);
    CHECK_EQ(read->c, R21_MEM_TOP - 0x8000000 - w.image - 0x8000);
    read = &events[last(EV_READ_FILE)];
    CHECK_EQ(read->a, 1);
    CHECK_EQ(read->b, w.disk);
    CHECK_EQ(read->c, R21_MEM_TOP - 0x8000000 - w.disk);
    CHECK(last(EV_READ_FILE) < first(EV_DC_CVAU));

    check_entry(&w, -1);
}

static void test_no_ssd(void)
{
    reset();
    nvme_fails = true;
    check_not_started(from_the_ssd());
    CHECK_EQ(count(EV_READ_FILE), 0);
}

static void test_no_files(void)
{
    reset();
    missing_file = "siliconnt/firmware.fd";
    check_not_started(from_the_ssd());
    /* Without the image the disk is not read */
    CHECK_EQ(count(EV_READ_FILE), 1);

    reset();
    missing_file = "siliconnt/bootdisk.img";
    check_not_started(from_the_ssd());
    CHECK_EQ(count(EV_READ_FILE), 2);
}

static void test_files_that_are_not_what_they_should_be(void)
{
    reset();
    image_file[0x38] = 'X';
    check_not_started(from_the_ssd());
    CHECK_EQ(count(EV_READ_FILE), 1);

    reset();
    disk_file[512] = 'X';
    check_not_started(from_the_ssd());
}

static void test_a_machine_that_will_not_do(void)
{
    /* The framebuffer inside guest RAM: nothing is read */
    reset();
    cur_boot_args.video.base = SELF_PHYS_BASE + 0x4000;
    check_not_started(from_the_ssd());
    CHECK_EQ(count(EV_NVME_INIT), 0);

    /* The MMU of EL1 is on: the entry code of the firmware expects it off */
    reset();
    sctlr_el12 = SCTLR_EL12_R21 | 1;
    check_not_started(from_the_ssd());

    /* No spare entry of /chosen/memory-map for the boot disk */
    reset();
    spare_entries = 0;
    make_tree();
    check_not_started(from_the_ssd());

    /*
     * /arm-io without ranges, and with addresses of three cells: where the devices of the
     * machine are is not known, and so not whether one is where the guest finds its GIC
     */
    reset();
    with_ranges = false;
    make_tree();
    check_not_started(from_the_ssd());

    reset();
    arm_io_cells = 3;
    make_tree();
    check_not_started(from_the_ssd());
}

/*
 * No table of AIC lines and SPIs: more lines than SPIs, no AIC, no node of AIC. The firmware
 * is not entered, and that is found out before anything is set up for the guest, while
 * m1n1 still has the screen to say so.
 */
static void check_no_table(const struct where *w, int ret)
{
    check_not_started(ret);
    CHECK(all_bytes(w->phys_base, w->image, FILL));
    CHECK(all_bytes(w->bootargs, w->disk, FILL));
}

static void test_no_table(void)
{
    struct where self = where(SELF_PHYS_BASE), host = where(R21_PHYS_BASE);

    /* Lines 0 to 986, and 1505 of the other device, are as many as there are SPIs */
    reset();
    many_lines = 987;
    make_tree();
    CHECK_EQ(from_the_ssd(), ENTERED);
    CHECK_EQ(((const u32 *)self.aicmap)[1], 988);
    CHECK(guest_range_is(&self, "AICToGIC", self.aicmap, 8 + 4 * 988));

    /* One more */
    reset();
    many_lines = 988;
    make_tree();
    check_no_table(&self, from_the_ssd());

    reset();
    put_files(&host);
    many_lines = 988;
    make_tree();
    check_no_table(&host, loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0));

    reset();
    aic = NULL;
    check_no_table(&self, from_the_ssd());

    reset();
    with_aic = false;
    make_tree();
    check_no_table(&self, from_the_ssd());
}

static void test_mappings_that_fail(void)
{
    /* The first mapping, one in the middle, the last one of memory */
    for (unsigned i = 0; i < 3; i++) {
        static const int at[] = {0, 11, R21_MMIO_COUNT + 1};

        reset();
        map_hw_fails_at = at[i];
        check_not_entered(from_the_ssd(), -1);
        CHECK_EQ(map_hw_calls, at[i] + 1);
    }
}

/* ---- a host has put the two files into memory ----------------------------------------------- */

static void put_files(const struct where *w)
{
    memcpy((void *)w->image, image_file, IMAGE_SIZE);
    memcpy((void *)w->disk, disk_file, DISK_SIZE);
}

static void test_loaded_by_a_host(void)
{
    /* Guest RAM where the host of R21 had it; the host talks over the first USB port */
    struct where w = where(R21_PHYS_BASE);

    reset();
    /* The image is where it was in R21 */
    CHECK_EQ(w.image, 0x1001ac00000);
    put_files(&w);
    /* The guest runs until the host leaves it */
    hv_start_does = GUEST_LEFT;
    CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0), 0);

    /* The SSD is not touched, and the heap is the host's business */
    CHECK_EQ(count(EV_NVME_INIT) + count(EV_NVME_SHUTDOWN) + count(EV_READ_FILE), 0);
    CHECK_EQ(count(EV_HEAP_LIMIT), 0);
    check_entry(&w, IODEV_USB0);
}

/*
 * Without a host, a step between taking the ports and entering the guest fails: a core that did
 * not come into EL2, a mapping, execution tracking, the check of the memory that is read-only on
 * the other cores, or the hypervisor itself (hv_start() returns). m1n1 goes back to the proxy,
 * and every port is what it was: with P_SELFBOOT_RUN the one the command came over is the
 * proxy's again, so that a host can connect again. With a host nothing is given back: its port
 * was never taken, and the spare ones stay the guest's, as before.
 */
static void test_not_entered_without_a_host(void)
{
    struct where w = where(R21_PHYS_BASE);

    for (int i = 0; i < 5; i++) {
        reset();
        put_files(&w);
        cpu_missing = i == 0;
        map_hw_fails_at = i == 1 ? 0 : -1;
        xtrack_fails = i == 2;
        ctrr_fails = i == 3;
        hv_start_does = i == 4 ? NOT_ENTERED : GUEST_STAYS;
        CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, SELFBOOT_NO_HOST), -1);
        CHECK_EQ(count(EV_HV_INIT), 1);
        CHECK_EQ(count(EV_CPUS), 1);
        CHECK_EQ(count(EV_MAP_HW) > 0, i >= 1);
        CHECK_EQ(count(EV_XTRACK), i >= 2);
        CHECK_EQ(count(EV_CTRR), i >= 3);
        CHECK_EQ(count(EV_FB_SHUTDOWN), i == 4);
        CHECK_EQ(count(EV_HV_START), i == 4);
        check_ports_given_back(-1);
        CHECK_EQ(usages[IODEV_USB0], 3);
        CHECK_EQ(stray, 0);
    }

    /* From the SSD, where the hypervisor does not enter the guest */
    reset();
    hv_start_does = NOT_ENTERED;
    CHECK_EQ(from_the_ssd(), -1);
    check_ports_given_back(-1);

    /* With a host: a mapping that fails, and hv_start() returning, are as before */
    for (int i = 0; i < 3; i++) {
        reset();
        put_files(&w);
        xtrack_fails = i == 0;
        ctrr_fails = i == 1;
        cpu_missing = i == 2;
        CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0), -1);
        CHECK_EQ(count(EV_HV_START) + count(EV_FB_SHUTDOWN), 0);
        check_ports_given_back(IODEV_USB0);
        CHECK_EQ(usages[IODEV_USB0 + 1], 0);
    }

    reset();
    put_files(&w);
    hv_start_does = NOT_ENTERED;
    CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0), 0);
}

/*
 * The same, with the host saying that it is not staying (P_SELFBOOT_RUN with no host, R37):
 * the guest is set up as m1n1 sets it up when it has read the two files itself
 */
static void test_loaded_without_a_host(void)
{
    struct where w = where(R21_PHYS_BASE);

    reset();
    put_files(&w);
    CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, SELFBOOT_NO_HOST), ENTERED);
    CHECK_EQ(count(EV_NVME_INIT) + count(EV_NVME_SHUTDOWN) + count(EV_READ_FILE), 0);
    CHECK_EQ(count(EV_HEAP_LIMIT), 0);
    check_entry(&w, -1);
}

/*
 * A machine without /arm-io/uart0. Without a host there is nothing to take from the guest, and
 * it is entered; a host would have no UART to talk to the guest's debugger over, and it is not
 */
static void test_no_uart(void)
{
    struct where w = where(R21_PHYS_BASE);

    reset();
    with_uart = false;
    make_tree();
    CHECK_EQ(from_the_ssd(), ENTERED);
    CHECK_EQ(count(EV_HV_START), 1);
    CHECK_EQ(count(EV_MAP_VUART), 0);
    CHECK_EQ(count(EV_UNMAP), 2);
    CHECK(!memcmp(tree, tree_before, sizeof(tree)));
    CHECK_EQ(stray, 0);

    reset();
    with_uart = false;
    make_tree();
    put_files(&w);
    check_not_entered(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0), IODEV_USB0);
}

static void test_loaded_refuses(void)
{
    struct where w = where(R21_PHYS_BASE);

    /* A start of guest RAM that is not a page, or that is inside m1n1's heap */
    reset();
    put_files(&w);
    check_not_started(loaded(R21_PHYS_BASE + 0x1000, IMAGE_SIZE, DISK_SIZE, 4));
    reset();
    check_not_started(loaded(HEAP_TOP & ~0x3fffUL, IMAGE_SIZE, DISK_SIZE, 4));
    /*
     * One that leaves m1n1 a byte less above its heap than it keeps when it loads by itself,
     * and one that leaves it just as much
     */
    reset();
    put_files(&w);
    heap_top = R21_PHYS_BASE - selfboot_m1n1_heap(mem_size_actual) + 1;
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, 4));
    reset();
    put_files(&w);
    heap_top = R21_PHYS_BASE - selfboot_m1n1_heap(mem_size_actual);
    hv_start_does = GUEST_LEFT;
    CHECK_EQ(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0), 0);
    CHECK_EQ(count(EV_HV_START), 1);

    /* The image is not there; the disk is not there; sizes that are not theirs */
    reset();
    memcpy((void *)w.disk, disk_file, DISK_SIZE);
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, 4));
    reset();
    memcpy((void *)w.image, image_file, IMAGE_SIZE);
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, 4));
    reset();
    put_files(&w);
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE - 0x4000, DISK_SIZE, 4));
    reset();
    put_files(&w);
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE + 0x800, 4));
}

/*
 * The firmware is not entered on a machine that has something of its own where the guest
 * finds the Distributor or the Redistributors of its GIC (src/hv/hv_layout.c, which has its
 * own test in layout_test.c). It is found out before anything is set up for the guest.
 */
static void check_nothing_for_the_guest(const struct where *w)
{
    CHECK(all_bytes(w->phys_base, w->image, FILL));
    CHECK(all_bytes(w->bootargs, w->disk, FILL));
}

static void test_a_machine_with_something_at_the_gic(void)
{
    struct where self = where(SELF_PHYS_BASE), host = where(R21_PHYS_BASE);

    /* A device at the Distributor */
    reset();
    one_more_device[0] = 0x5000000000;
    one_more_device[1] = 0x4000;
    make_tree();
    check_not_started(from_the_ssd());
    check_nothing_for_the_guest(&self);

    /* A device at the Redistributor of the last core there can be */
    reset();
    one_more_device[0] = 0x51003e0000;
    one_more_device[1] = 0x20000;
    make_tree();
    check_not_started(from_the_ssd());
    check_nothing_for_the_guest(&self);

    /* Memory that reaches the Redistributors */
    reset();
    ram_base = 0x5000000000 + 0x100000000;
    check_not_started(from_the_ssd());
    check_nothing_for_the_guest(&self);

    /* The same when a host has done the loading */
    reset();
    put_files(&host);
    one_more_device[0] = 0x5000000000;
    one_more_device[1] = 0x4000;
    make_tree();
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0));
    check_nothing_for_the_guest(&host);

    reset();
    put_files(&host);
    ram_base = 0x5000000000 - 0x100000000;
    mem_size_actual = 0x100004000;
    check_not_started(loaded(R21_PHYS_BASE, IMAGE_SIZE, DISK_SIZE, IODEV_USB0));
    check_nothing_for_the_guest(&host);

    /* A device right after the Redistributors is in nobody's way, and is the guest's */
    reset();
    one_more_device[0] = 0x5100400000;
    one_more_device[1] = 0x4000;
    make_tree();
    CHECK_EQ(from_the_ssd(), ENTERED);
    CHECK_EQ(count(EV_HV_START), 1);
    CHECK_EQ(count(EV_MAP_HW), R21_MMIO_COUNT + 1 + 2);
}

int main(void)
{
    map_window(SELF_PHYS_BASE);
    map_window(R21_PHYS_BASE);

    test_from_the_ssd();
    test_no_ssd();
    test_no_files();
    test_files_that_are_not_what_they_should_be();
    test_a_machine_that_will_not_do();
    test_no_table();
    test_mappings_that_fail();
    test_loaded_by_a_host();
    test_loaded_without_a_host();
    test_not_entered_without_a_host();
    test_no_uart();
    test_loaded_refuses();
    test_a_machine_with_something_at_the_gic();

    printf("run_test: %d checks, %d failed\n", checks, failed);
    if (!failed)
        printf("ALL OK\n");
    return failed ? 1 : 0;
}
