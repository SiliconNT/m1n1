/* SPDX-License-Identifier: GPL-2.0-or-later */
/* See machine.h */

#include <setjmp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

#include "machine.h"
#include "dart.h"
#include "dcp.h"
#include "fb.h"
#include "firmware.h"
#include "malloc.h"
#include "memory.h"
#include "pmgr.h"
#include "r44.h"
#include "sart.h"
#include "smc.h"
#include "utils.h"
#include "xnuboot.h"

#include "dcp/dptx_phy.h"
#include "libfdt/libfdt.h"

#undef printf
#undef malloc
#undef calloc
#undef memalign
#undef free

struct machine m;
bool transcript, verbose;

const char *const step_names[STEPS] = {
    [NEVER] = "never",
    [AT_WAKE] = "wake",
    [AT_HELLO_ACK] = "hello-ack",
    [AT_POWER] = "power",
    [AT_INIT] = "init",
    [AT_GETBUF] = "getbuf",
    [AT_START] = "start",
    [AT_ANNOUNCE] = "announce",
    [AT_SETTLE] = "settle",
    [AT_OPEN] = "open",
    [AT_HPD] = "hpd",
    [AT_SURFACE] = "surface",
    [AT_SHUTDOWN] = "shutdown",
    [AT_AP_QUIESCE] = "ap-quiesce",
    [AT_IOP_QUIESCE] = "iop-quiesce",
};

const char *const way_names[WAYS] = {
    [ANSWERS] = "answers",
    [SILENT] = "silent",
    [MUTE] = "mute",
    [CRASHES] = "crashes",
    [FLOODS_OTHER] = "floods-other",
    [FLOODS_OWN] = "floods-own",
    [FLOODS_LOGS] = "floods-logs",
    [FULL] = "full",
};

/* ---- what the machine is at for now: not looked at by the cases ---- */

enum ep_state { EP_CLOSED, EP_INIT, EP_BUFFER, EP_RINGS, EP_STARTED };

static struct {
    enum way on; /* what it does by now, if not ANSWERS */
    struct {
        u64 msg0, ep, ready;
    } q[256];
    int q_head, q_tail;
    u64 send0;
    int ioreports;
    bool logs_asked;
    u64 syslog_dva, ioreport_dva, ring_dva;
    enum ep_state state;
    u32 tx_rptr, rx_wptr;
    u32 announced;
    bool opened;
    u32 slow;
    u64 settle_at;
} dcp;

/* ---- time ---- */

#define HANG_USEC 30000000

static jmp_buf machine;
static u64 now, started_at;

static void ring_send_later(void);
static void settle_step(void);

static void advance(u64 us)
{
    now += us;
    settle_step();
    ring_send_later();
    if (now - started_at > HANG_USEC)
        longjmp(machine, HUNG + 1);
}

u64 machine_now(void)
{
    return now;
}

void udelay(u32 d)
{
    advance(d);
}

u64 timeout_calculate(u32 usec)
{
    return now + usec;
}

bool timeout_expired(u64 timeout)
{
    advance(1);
    return now > timeout;
}

u64 get_ticks(void)
{
    return now * 24;
}

u64 ticks_to_msecs(u64 ticks)
{
    return ticks / 24000;
}

void flush_and_reboot(void)
{
    longjmp(machine, PANICKED + 1);
}

enum outcome machine_run(void (*fn)(void))
{
    int how = setjmp(machine);

    if (how)
        return how - 1;
    started_at = now;
    fn();
    return RETURNED;
}

/* ---- what is said ---- */

static char log_buf[1 << 16];
static size_t log_len;

static void trace(const char *fmt, ...)
{
    va_list ap;

    if (!transcript)
        return;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

int debug_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (verbose)
        fputs(line, stdout);
    trace("says %s", line);
    /* A flood of the same two lines is kept once */
    static char last[2][512];
    if (!strcmp(line, last[0]) || !strcmp(line, last[1]))
        return n;
    strcpy(last[1], last[0]);
    strcpy(last[0], line);
    /* the last 64 KB */
    for (const char *p = line; *p; p++) {
        if (log_len == sizeof(log_buf) - 1) {
            memmove(log_buf, log_buf + sizeof(log_buf) / 2, sizeof(log_buf) / 2);
            log_len -= sizeof(log_buf) / 2;
        }
        log_buf[log_len++] = *p;
    }
    log_buf[log_len] = 0;
    return n;
}

bool printed(const char *text)
{
    return strstr(log_buf, text) != NULL;
}

static void stray(const char *what, u64 addr)
{
    if (verbose || !m.stray)
        fprintf(stdout, "stray: %s 0x%lx\n", what, (unsigned long)addr);
    m.stray++;
}

void model_sysop(const char *op)
{
    UNUSED(op);
}

void hexdump(const void *d, size_t len)
{
    for (size_t i = 0; i < len; i++)
        debug_printf("%02x%s", ((const u8 *)d)[i], i % 16 == 15 || i == len - 1 ? "\n" : " ");
}

/* ---- memory, where the M4 has it ---- */

#define HEAP_BASE 0x10006880000UL
#define HEAP_SIZE 0x4000000UL
#define PT_BASE   0x103ffeec000UL
#define PT_SIZE   0x50000UL
#define L1_DCP    0x103ffeec000UL
#define L1_DISP   0x103fff1c000UL
/* __DATA of DCP: its own buffer for crash logs is in there */
#define DATA_BASE    0x103dfcd8000UL
#define DATA_SIZE    0x32c000UL
#define CRASHLOG     0x103dff7d000UL
#define CRASHLOG_DVA 0x8259000UL
#define FB_SIZE      (FB_STRIDE * FB_HEIGHT)
#define FB_PAGES     1450

static u8 page_tables[PT_SIZE];

static void map_at(u64 addr, size_t size)
{
    void *p = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);

    if (p != (void *)addr) {
        fprintf(stdout, "no memory at 0x%lx\n", (unsigned long)addr);
        exit(1);
    }
}

static void pt_map(u64 l1, u64 *next_l2, u64 iova, u64 pa, u64 size)
{
    for (u64 off = 0; off < size; off += SZ_16K) {
        u64 *top = (u64 *)l1 + ((iova + off) >> 25 & 0x7ff);
        if (!(*top & 1)) {
            *top = FIELD_PREP(GENMASK(39, 10), *next_l2 >> 14) | 1;
            *next_l2 += SZ_16K;
        }
        u64 *l2 = (u64 *)(FIELD_GET(GENMASK(39, 10), *top) << 14);
        l2[(iova + off) >> 14 & 0x7ff] = FIELD_PREP(GENMASK(39, 10), (pa + off) >> 14) |
                                         FIELD_PREP(GENMASK(51, 40), 0xfffUL) | 1;
    }
}

/* What a DART makes of an address: 0 for nothing */
static u64 translate(u64 l1, u64 dva)
{
    u64 top = ((u64 *)l1)[dva >> 25 & 0x7ff];

    if ((dva >> 36) || !(top & 1))
        return 0;
    u64 pte = ((u64 *)(FIELD_GET(GENMASK(39, 10), top) << 14))[dva >> 14 & 0x7ff];
    if (!(pte & 1))
        return 0;
    return (FIELD_GET(GENMASK(39, 10), pte) << 14) + (dva & 0x3fff);
}

static void memory_reset(void)
{
    u64 next;

    map_at(HEAP_BASE, HEAP_SIZE);
    map_at(PT_BASE, PT_SIZE);
    map_at(DATA_BASE, DATA_SIZE);
    map_at(FB_BASE, ALIGN_UP(FB_SIZE, SZ_16K));
    memset((void *)FB_BASE, 0x5a, FB_SIZE);

    next = L1_DCP + SZ_16K;
    pt_map(L1_DCP, &next, 0x7770000, 0x100002a8000, 0x844000);
    pt_map(L1_DCP, &next, 0x7fb4000, DATA_BASE, DATA_SIZE);
    /* Not where the device tree says: SiliconNT docs/findings/0083 */
    pt_map(L1_DCP, &next, 0x5274000, 0x103e28c8000, 0x28000);
    pt_map(L1_DCP, &next, 0xb70000, 0x103e5c1c000, 0x1000000);
    pt_map(L1_DCP, &next, 0x1ba0000, 0x103e58b8000, 0x364000);
    pt_map(L1_DCP, &next, 0x1f04000, 0x103e57f8000, 0xc0000);
    pt_map(L1_DCP, &next, 0x1fc4000, FB_BASE, 0x57ac000);
    next = L1_DISP + SZ_16K;
    pt_map(L1_DISP, &next, FB_DVA, FB_BASE, FB_PAGES * SZ_16K);
    memcpy(page_tables, (void *)PT_BASE, PT_SIZE);

    /* A crash log: one line of text */
    u32 *log = (u32 *)CRASHLOG;
    log[0] = 'CLHE';
    log[8] = 'Cstr';
    log[11] = 64;
    strcpy((char *)&log[13], "ASSERT!afk_messenger_common.c:246");
    log[8 + 16] = 'CLHE';
}

void machine_empty_l2(void)
{
    u64 l2 = L1_DCP + 0x2c000; /* the last of the region for them */

    memset((void *)l2, 0, SZ_16K);
    ((u64 *)L1_DCP)[(0x10000000 >> 25) + 1] = FIELD_PREP(GENMASK(39, 10), l2 >> 14) | 1;
    memcpy(page_tables, (void *)PT_BASE, PT_SIZE);
}

void machine_unmap_segment(void)
{
    for (u64 iova = 0x7fb4000; iova < 0x7fb4000 + DATA_SIZE; iova += SZ_16K) {
        u64 top = ((u64 *)L1_DCP)[iova >> 25];
        ((u64 *)(FIELD_GET(GENMASK(39, 10), top) << 14))[iova >> 14 & 0x7ff] = 0;
    }
    memcpy(page_tables, (void *)PT_BASE, PT_SIZE);
}

void machine_unmap_fb(void)
{
    u64 top = ((u64 *)L1_DISP)[FB_DVA >> 25];

    ((u64 *)(FIELD_GET(GENMASK(39, 10), top) << 14))[FB_DVA >> 14 & 0x7ff] = 0;
    memcpy(page_tables, (void *)PT_BASE, PT_SIZE);
}

void machine_crashlog(const u32 *words, int n)
{
    memset((void *)(CRASHLOG + 32), 0x11, 0x10000 - 32);
    memcpy((void *)(CRASHLOG + 32), words, 4 * n);
}

bool machine_page_tables_untouched(void)
{
    return !memcmp(page_tables, (void *)PT_BASE, PT_SIZE);
}

/* ---- the heap: what was freed is never given out again, and is not what it was ---- */

static struct block {
    u64 addr;
    size_t size;
    bool freed, dma;
} blocks[4096];
static int n_blocks;
static u64 heap_top;

void *model_memalign(size_t align, size_t size)
{
    heap_top = ALIGN_UP(heap_top, align);
    if (n_blocks == (int)ARRAY_SIZE(blocks) || heap_top + size > HEAP_BASE + HEAP_SIZE) {
        fprintf(stdout, "the heap is full\n");
        exit(1);
    }
    blocks[n_blocks++] = (struct block){heap_top, size, false, align >= SZ_16K};
    void *p = (void *)heap_top;
    memset(p, 0xaa, size);
    heap_top += size ? size : 1;
    return p;
}

void *model_malloc(size_t size)
{
    return model_memalign(16, size);
}

void *model_calloc(size_t n, size_t size)
{
    void *p = model_memalign(16, n * size);

    memset(p, 0, n * size);
    return p;
}

void model_free(void *p)
{
    if (!p)
        return;
    for (int i = 0; i < n_blocks; i++) {
        if (blocks[i].addr != (u64)p)
            continue;
        if (blocks[i].freed) {
            m.heap_errors++;
            fprintf(stdout, "freed twice: %p\n", p);
        }
        blocks[i].freed = true;
        memset(p, 0xdd, blocks[i].size);
        return;
    }
    m.heap_errors++;
    fprintf(stdout, "freed, and not of the heap: %p\n", p);
}

static bool allocated(u64 addr)
{
    for (int i = 0; i < n_blocks; i++)
        if (!blocks[i].freed && addr >= blocks[i].addr && addr < blocks[i].addr + blocks[i].size)
            return true;
    return false;
}

int machine_dma_buffers(void)
{
    int n = 0;

    for (int i = 0; i < n_blocks; i++)
        n += blocks[i].dma && !blocks[i].freed;
    return n;
}

/* As src/utils.c has it, with the boot arguments of the M4 (R44c) */
bool is_heap(void *addr)
{
    u64 p = (u64)addr;

    return p > 0x10006870000 && p < 0x10002e70000 + 0x3d87a0000;
}

/* ---- the rest of m1n1 ---- */

u32 chip_id = 0x8132, board_id = 0x22;
bool is_mac = true;
struct boot_args cur_boot_args;
static struct boot_args boot_args_memory;
u64 boot_args_addr;

void fb_clear_direct(void)
{
    trace("fb_clear_direct\n");
    m.fb_clears++;
    m.surfaces_at_clear = m.surfaces;
    memset((void *)FB_BASE, 0, FB_SIZE);
}

void fb_reinit(void)
{
    trace("fb_reinit, depth 0x%lx\n", (unsigned long)cur_boot_args.video.depth);
    m.fb_reinits++;
    m.depth_at_reinit = cur_boot_args.video.depth;
}

void mmu_map_framebuffer(u64 addr, size_t size)
{
    trace("mmu_map_framebuffer 0x%lx 0x%lx\n", (unsigned long)addr, (unsigned long)size);
}

u64 top_of_memory_alloc(size_t size)
{
    trace("top_of_memory_alloc 0x%lx\n", (unsigned long)size);
    return (u64)model_memalign(SZ_16K, size);
}

int pmgr_reset(int die, const char *name)
{
    trace("pmgr_reset %d %s\n", die, name);
    m.resets++;
    if (!m.pmgr_resets)
        return -1;
    m.awake = false;
    m.dead = true;
    return 0;
}

int pmgr_adt_power_enable(const char *path)
{
    trace("pmgr_adt_power_enable %s\n", path);
    return 0;
}

/* Linked, and not for the displays of the cases */
#define NOT_HERE(...)                                                                              \
    {                                                                                              \
        stray("a function", 0);                                                                    \
        return 0;                                                                                  \
    }
#define NOT_HERE_VOID(...)                                                                         \
    {                                                                                              \
        stray("a function", 0);                                                                    \
    }
bool sart_add_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE();
bool sart_remove_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE();
smc_dev_t *smc_init(void) NOT_HERE();
int smc_write_u32(smc_dev_t *smc, u32 key, u32 value) NOT_HERE();
void smc_shutdown(smc_dev_t *smc) NOT_HERE_VOID();
dptx_phy_t *dptx_phy_init(const char *phy_path, u32 dcp_index) NOT_HERE();
dcp_dpav_if_t *dcp_dpav_init(dcp_dev_t *dev) NOT_HERE();
dcp_dptx_if_t *dcp_dptx_init(dcp_dev_t *dev, u32 num_dptxports) NOT_HERE();
int dcp_dptx_connect(dcp_dptx_if_t *dptx, dptx_phy_t *phy, u32 die, u32 port) NOT_HERE();
int fdt_setprop(void *fdt, int node, const char *name, const void *val, int len) NOT_HERE();
int fdt_node_offset_by_phandle(const void *fdt, uint32_t phandle) NOT_HERE();
int fdt_node_check_compatible(const void *fdt, int node, const char *compatible) NOT_HERE();
const char *fdt_get_name(const void *fdt, int node, int *lenp) NOT_HERE();
u64 dt_get_address(void *dt, int node) NOT_HERE();

int dcp_dpav_shutdown(dcp_dpav_if_t *dpav)
{
    if (dpav)
        stray("a function", 0);
    return 0;
}

int dcp_dptx_shutdown(dcp_dptx_if_t *dptx)
{
    if (dptx)
        stray("a function", 0);
    return 0;
}

int dcp_system_shutdown(dcp_system_if_t *system)
{
    if (system)
        stray("a function", 0);
    return 0;
}

/* ---- messages ---- */

void entry_add(struct entries *list, const void *data, u32 len)
{
    struct entry *e = &list->e[list->n++];

    if (list->n > ENTRIES_MAX || len > ENTRY_MAX)
        abort();
    memset(e, 0, sizeof(*e));
    memset(e->tail, 0xaa, sizeof(e->tail));
    memcpy(e->data, data, len);
    e->len = len;
}

void entry_v2(struct entries *list, u16 iface, u8 type, u8 category, u8 flags, const void *body,
              u32 len)
{
    u8 data[ENTRY_MAX] = {0};
    u32 total = 16 + len;

    /* What is not looked at is what was left there */
    data[0] = 0x20 + list->n;
    data[1] = 0xd8;
    memcpy(data + 2, &iface, 2);
    memcpy(data + 4, &total, 4);
    data[16] = type;
    data[17] = category;
    data[18] = flags;
    memset(data + 19, 0xff, 5);
    memcpy(data + 24, body, len);
    entry_add(list, data, 24 + len);
}

void entry_reply(struct entries *list, u16 iface, u8 flags, u8 tag, u32 status, const void *body,
                 u32 len)
{
    u8 data[ENTRY_MAX] = {flags, tag, 0x5a, 0x5a};

    memcpy(data + 4, &status, 4);
    memcpy(data + 8, body, len);
    entry_v2(list, iface, 0xc0, 2, 0, data, 8 + len);
}

/* ---- DCP ---- */

#define MGMT_TYPE      GENMASK(59, 52)
#define T_HELLO_ACK    2
#define T_START_EP     5
#define T_IOP_PWR      6
#define T_EPMAP        8
#define T_AP_PWR       0xb
#define T_BUFFER       1
#define RBEP_TYPE      GENMASK(63, 48)
#define POWER_QUIESCED 0x10
#define POWER_SLEEP    0x01
#define LATENCY        30
#define QE_MAGIC       0x20504f49
#define EP_LOGS        2
#define EP_IOREPORT    4
#define EP_IBOOT       0x23
#define V1_CHANNEL     0

static void to_ap(u64 ep, u64 msg0)
{
    if (m.dead || dcp.on != ANSWERS)
        return;
    if (dcp.q_tail - dcp.q_head == (int)ARRAY_SIZE(dcp.q)) {
        stray("the mailbox overflowing", 0);
        return;
    }
    dcp.q[dcp.q_tail % ARRAY_SIZE(dcp.q)].msg0 = msg0;
    dcp.q[dcp.q_tail % ARRAY_SIZE(dcp.q)].ep = ep;
    dcp.q[dcp.q_tail % ARRAY_SIZE(dcp.q)].ready = now + LATENCY + dcp.slow;
    dcp.q_tail++;
}

static void crash(void)
{
    to_ap(1, 0x0010410008259000);
    m.crashes++;
    m.dead = true;
    m.awake = false;
}

static void firmware_assert(const char *what)
{
    fprintf(stdout, "DCP asserts: %s\n", what);
    m.asserts++;
    crash();
}

/* Whether it answers at that step, and what becomes of it there */
static bool at(enum step step)
{
    if (m.dead)
        return false;
    dcp.slow = m.slow_at == step ? m.slow_usec : 0;
    if (m.way != ANSWERS && m.from == step) {
        if (m.way == CRASHES) {
            crash();
            return false;
        }
        dcp.on = m.way;
    }
    return dcp.on == ANSWERS || (dcp.on == MUTE && (step < AT_INIT || step > AT_SHUTDOWN));
}

/* Nothing the host does leads to it, only time */
static void settle_step(void)
{
    if (!dcp.settle_at || now < dcp.settle_at)
        return;
    dcp.settle_at = 0;
    if (at(AT_SETTLE) && m.shutdown_ack_unasked) {
        dcp.state = EP_CLOSED;
        to_ap(EP_IBOOT, 0x00c1000000000000);
    }
}

/* A mute endpoint: the rest of DCP still speaks */
static void to_ap_mgmt(u64 ep, u64 msg0)
{
    enum way was = dcp.on;

    if (dcp.on == MUTE)
        dcp.on = ANSWERS;
    to_ap(ep, msg0);
    dcp.on = was;
}

/* What it uses while it is awake is there */
static void check_buffers(void)
{
    u64 dvas[3] = {dcp.syslog_dva, dcp.ioreport_dva, dcp.state >= EP_BUFFER ? dcp.ring_dva : 0};

    if (!m.awake || m.dead)
        return;
    for (int i = 0; i < 3; i++) {
        u64 pa = dvas[i] ? translate(L1_DCP, dvas[i]) : 1;
        if (pa == 1 || (pa && allocated(pa)))
            continue;
        if (!m.faults++)
            fprintf(stdout, "DCP is awake, and its buffer at 0x%lx is gone\n",
                    (unsigned long)dvas[i]);
    }
}

void machine_check_buffers(void)
{
    check_buffers();
}

static u8 *ring(int which, u32 off)
{
    u64 dva = dcp.ring_dva + (which ? m.ring_off : 0) + off;
    u64 pa = translate(L1_DCP, dva);

    if (!pa || !allocated(pa)) {
        static u8 nowhere[0x8000];
        if (!m.faults++)
            fprintf(stdout, "DCP is awake, and its ring at 0x%lx is gone\n", (unsigned long)dva);
        return nowhere;
    }
    return (u8 *)pa;
}

static u32 ring_get(int which, u32 off)
{
    u32 v;

    memcpy(&v, ring(which, off), 4);
    return v;
}

static void ring_put(int which, u32 off, u32 v)
{
    memcpy(ring(which, off), &v, 4);
}

static u32 ring_room(void)
{
    return m.rb_size - 3 * m.block;
}

/* An entry for the host */
static void ring_send(const struct entry *e, u32 channel, u32 type)
{
    u32 room = ring_room(), data = 3 * m.block;
    u32 hdr[4] = {e->bad_magic ? e->bad_magic : QE_MAGIC, e->is_bad_size ? e->bad_size : e->len,
                  channel, type};
    u32 was = dcp.rx_wptr, where = was;

    for (int i = 0; i < 4; i++)
        ring_put(1, data + where + 4 * i, hdr[i]);
    /* What does not fit to the end is at the beginning, with its header once more */
    if (where + 16 + e->len > room) {
        where = 0;
        for (int i = 0; i < 4; i++)
            ring_put(1, data + where + 4 * i, hdr[i]);
    }
    for (u32 i = 0; i < e->len; i++)
        *ring(1, data + where + 16 + i) = e->data[i];
    for (u32 i = 0; i < sizeof(e->tail) && where + 16 + e->len + i < room; i++)
        *ring(1, data + where + 16 + e->len + i) = e->tail[i];
    dcp.rx_wptr = ALIGN_UP(where + 16 + e->len, m.block) % room;
    ring_put(1, 2 * m.block, dcp.rx_wptr);
    if (!m.no_bell)
        to_ap(EP_IBOOT, 0x0085000000000000UL | (u64)dcp.rx_wptr << 16 | was);
}

/* A little later, after what it sends at once */
static struct {
    const struct entries *list;
    u64 when;
} later[4];

static void ring_send_all(const struct entries *list)
{
    for (size_t i = 0; i < ARRAY_SIZE(later); i++) {
        if (later[i].list)
            continue;
        later[i].list = list;
        later[i].when = now + 2 * LATENCY + dcp.slow;
        return;
    }
    stray("too much at once", 0);
}

static void ring_send_later(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(later); i++) {
        const struct entries *list = later[i].list;

        if (!list || later[i].when > now)
            continue;
        later[i].list = NULL;
        for (int j = 0; j < list->n && dcp.on == ANSWERS && !m.dead; j++)
            ring_send(&list->e[j], 0, 0);
    }
}

static void set_surface(const u8 *layer)
{
    m.surfaces++;
    m.surface_at = now;
    memcpy(&m.surface_fmt, layer + 132 + 4 * 4, 4);
}

static void v2_receive(const struct entry *e)
{
    u16 iface;
    u32 total, op;

    memcpy(&iface, e->data + 2, 2);
    memcpy(&total, e->data + 4, 4);
    if (e->len < 24 || total != e->len - 8) {
        firmware_assert("a message that is not one of the V2");
        return;
    }
    if (iface != 3) {
        firmware_assert("a message for another interface");
        return;
    }
    if (e->data[17] == 0 && e->data[18] == 1 && e->data[16] == 0x12) {
        m.opened_at = now;
        dcp.opened = true;
        at(AT_OPEN);
        return;
    }
    if (e->data[17] != 1 || e->len < 32 + 16) {
        firmware_assert("neither an opening nor a command");
        return;
    }
    if (!dcp.opened)
        m.before_open++;
    if (e->data[24] & 1) {
        m.out_of_band++;
        firmware_assert("a command with a buffer for the reply, which has no content");
        return;
    }
    memcpy(&op, e->data + 32, 4);
    switch (op) {
        case 3:
            if (at(AT_HPD))
                ring_send_all(&m.hpd);
            break;
        case 1:
            if (at(AT_SURFACE)) {
                to_ap(EP_LOGS, 0x0050000000000003);
                set_surface(e->data + 48);
                ring_send_all(&m.surface);
            }
            break;
        default:
            firmware_assert("a command of no use");
    }
}

/* The V1: commands are out of band, as src/afk.c has them */
struct v1_msg {
    u8 version;
    u16 seq;
    u8 pad;
    u32 unk;
    u64 timestamp;
    u32 length;
    u8 sub_version;
    u8 category;
    u16 type;
    u64 sub_timestamp;
    u16 sub_seq;
    u8 sub_unk;
    u8 flags;
    u32 inline_len;
    u32 retcode;
    u64 rxbuf, txbuf;
    u32 rxlen, txlen;
    u8 rxcookie, txcookie;
} PACKED;

static void v1_receive(const struct entry *e)
{
    struct v1_msg msg;
    u32 *tx, *rx, rx_len = 8;
    struct entry reply = {.len = sizeof(msg)};

    memcpy(&msg, e->data, sizeof(msg));
    if (e->len != sizeof(msg) || msg.category != 0x30 || msg.type != 0xc0) {
        firmware_assert("not a command of the V1");
        return;
    }
    tx = (u32 *)translate(L1_DCP, msg.txbuf);
    rx = (u32 *)translate(L1_DCP, msg.rxbuf);
    if (!tx || !allocated((u64)tx) || !rx || !allocated((u64)rx)) {
        m.faults++;
        return;
    }
    trace("dcp command %u, %u bytes:", tx[0], msg.txlen);
    for (u32 i = 0; i < msg.txlen; i++)
        trace(" %02x", ((u8 *)tx)[i]);
    trace("\n");
    if (m.v1_no_replies)
        return;
    rx[0] = tx[0];
    switch (tx[0]) {
        case 3: /* connected, 2 timing modes, 2 color modes */
            rx[2] = 1;
            rx[3] = 2;
            rx[4] = 2;
            rx_len = 20;
            break;
        case 4: /* 1920 x 1080 and 3024 x 1964, 60 Hz */
            memcpy(&rx[2], (u32[]){2, 1, 1920, 1080, 60 << 16, 0, 0, 1, 3024, 1964, 60 << 16, 0, 0},
                   13 * 4);
            rx_len = 8 + 13 * 4;
            break;
        case 5: /* 30 and 24 bits per pixel */
            memcpy(&rx[2], (u32[]){2, 1, 1, 1, 1, 30, 0, 1, 1, 1, 1, 24, 0}, 13 * 4);
            rx_len = 8 + 13 * 4;
            break;
        case 1:
            set_surface((u8 *)&tx[4]);
            break;
    }
    rx[1] = rx_len;
    msg.category = 0x20;
    msg.rxlen = rx_len;
    msg.retcode = 0;
    memcpy(reply.data, &msg, sizeof(msg));
    ring_send(&reply, V1_CHANNEL, 4);
}

/* What the host has put into its ring */
static void ring_receive(void)
{
    u32 room = ring_room(), data = 3 * m.block;
    u32 wptr = ring_get(0, 2 * m.block);

    if (wptr % m.block || wptr >= room) {
        firmware_assert("a write pointer that is none");
        return;
    }
    while (dcp.tx_rptr != wptr && !m.dead) {
        struct entry e = {.len = ring_get(0, data + dcp.tx_rptr + 4)};
        u32 from = dcp.tx_rptr;

        if (ring_get(0, data + dcp.tx_rptr) != QE_MAGIC || e.len > ENTRY_MAX) {
            firmware_assert("an entry that is none");
            return;
        }
        if (from + 16 + e.len > room) {
            /* At the beginning, with its header once more */
            from = 0;
            if (ring_get(0, data) != QE_MAGIC || ring_get(0, data + 4) != e.len) {
                firmware_assert("an entry whose header is not at the beginning of the ring");
                return;
            }
        }
        for (int i = 0; i < 4; i++)
            e.hdr[i] = ring_get(0, data + from + 4 * i);
        for (u32 i = 0; i < e.len; i++)
            e.data[i] = *ring(0, data + from + 16 + i);
        trace("dcp ring, channel %u, type %u:", ring_get(0, data + from + 8),
              ring_get(0, data + from + 12));
        for (u32 i = 0; i < e.len; i++)
            trace(" %02x", e.data[i]);
        trace("\n");
        dcp.tx_rptr = ALIGN_UP(from + 16 + e.len, m.block) % room;
        ring_put(0, m.block, dcp.tx_rptr);
        if (m.n_sent < SENT_MAX)
            m.sent[m.n_sent++] = e;
        if (m.v1)
            v1_receive(&e);
        else
            v2_receive(&e);
    }
}

static u64 ring_init(u64 type, u32 off)
{
    u16 tag = m.ring_tag ? m.ring_tag : 0xcafe;

    return type << 48 | (u64)(off >> 6) << 32 | (u64)(m.rb_size >> 6) << 16 | tag;
}

static void ep_receive(u64 msg0)
{
    switch (FIELD_GET(RBEP_TYPE, msg0)) {
        case 0x80:
            m.inits++;
            if (dcp.state != EP_CLOSED) {
                firmware_assert("an endpoint initialized, and not closed");
                return;
            }
            dcp.state = EP_INIT;
            if (!dcp.logs_asked) {
                to_ap_mgmt(EP_LOGS, 0x008000008000003f);
                to_ap_mgmt(EP_LOGS, 0x0010300000000000);
                dcp.logs_asked = true;
            }
            if (!at(AT_INIT))
                return;
            to_ap(EP_IBOOT, 0x00a0000000000100);
            if (m.start_ack_before_rings)
                to_ap(EP_IBOOT, 0x0086000000000000);
            if (m.recv_before_rings)
                to_ap(EP_IBOOT, 0x0085000001000000);
            if (m.rings_before_buffer)
                to_ap(EP_IBOOT, ring_init(0x8a, 0));
            to_ap(EP_IBOOT, 0x0089000000000000 | (u64)(2 * 0x4000 >> 6) << 16 | 0xcafe);
            break;
        case 0xa1:
            dcp.ring_dva = msg0 & GENMASK(47, 0);
            dcp.state = EP_BUFFER;
            if (!at(AT_GETBUF))
                return;
            for (int i = 8; i < 13; i++)
                to_ap(r44c_dcp[14 + i].ep, r44c_dcp[14 + i].msg);
            to_ap_mgmt(0, 0x00b0000000000020);
            for (int i = 0; i < 2; i++) {
                if (i && m.ring_off >= 0x8000)
                    continue;
                ring_put(i, 0, m.bufsz[i] ? m.bufsz[i] : ring_room());
                ring_put(i, 4, m.v1 ? 0x00010001 : 0x00070006);
                ring_put(i, m.block, m.first_ptr);
                ring_put(i, 2 * m.block, m.first_ptr);
            }
            dcp.tx_rptr = dcp.rx_wptr = m.first_ptr;
            dcp.state = EP_RINGS;
            to_ap(EP_IBOOT, ring_init(0x8a, 0));
            if (!m.only_tx_ring)
                to_ap(EP_IBOOT, ring_init(0x8b, m.ring_off));
            break;
        case 0xa3:
            if (dcp.state != EP_RINGS) {
                firmware_assert("an endpoint started, and its rings are not there");
                return;
            }
            if (!at(AT_START))
                return;
            dcp.state = EP_STARTED;
            to_ap(EP_IBOOT, 0x0086000000000000);
            if (!at(AT_ANNOUNCE))
                return;
            m.announced_at = now;
            if (!m.v1)
                dcp.settle_at = now + 1000000;
            ring_send_all(&m.announce);
            if (!m.v1)
                to_ap(EP_LOGS, 0x0050000000000002);
            ring_send_all(&m.after_announce);
            break;
        case 0xa2:
            if (dcp.state != EP_STARTED) {
                firmware_assert("an entry, and the endpoint is not started");
                return;
            }
            ring_receive();
            break;
        case 0xc0:
            m.shutdowns++;
            m.shutdown_at = now;
            if (dcp.state != EP_STARTED) {
                firmware_assert("an endpoint shut down, and not started");
                return;
            }
            if (!at(AT_SHUTDOWN))
                return;
            dcp.state = EP_CLOSED;
            dcp.opened = false;
            to_ap(EP_IBOOT, 0x00c1000000000000);
            break;
        default:
            firmware_assert("a message of no known type");
    }
}

/* A message of the host's */
static void dcp_receive(u64 msg0, u64 ep)
{
    u64 power = msg0 & 0xffff;

    trace("ap %02lx %016lx\n", (unsigned long)ep, (unsigned long)msg0);
    if (m.n_ap < (int)ARRAY_SIZE(m.ap)) {
        m.ap[m.n_ap].ep = ep;
        m.ap[m.n_ap++].msg = msg0;
    }
    m.after_crash += m.dead;
    check_buffers();
    if (m.dead)
        return;

    switch (ep) {
        case 0:
            switch (FIELD_GET(MGMT_TYPE, msg0)) {
                case T_IOP_PWR:
                    if (power == 0x220) {
                        dcp.logs_asked = false;
                        dcp.ioreports = 0;
                        m.iop_power = power;
                        if (at(AT_WAKE))
                            to_ap(0, 0x00100000000c000c);
                        return;
                    }
                    if (!at(AT_IOP_QUIESCE))
                        return;
                    m.iop_power = power;
                    m.awake = false;
                    dcp.syslog_dva = dcp.ioreport_dva = dcp.ring_dva = 0;
                    to_ap_mgmt(0, 0x0070000000000000 | power);
                    return;
                case T_HELLO_ACK:
                    m.awake = true;
                    if (at(AT_HELLO_ACK))
                        to_ap(0, 0x008000000000051f);
                    return;
                case T_EPMAP:
                    if (!(msg0 & BIT(51)))
                        to_ap(0, 0x0088000100803ffd);
                    return;
                case T_START_EP:
                    dcp.slow = 0;
                    /* oslog is the last of the system's */
                    if ((msg0 >> 32 & 0xff) == 8) {
                        to_ap(1, 0x0011010008259000);
                        to_ap(EP_IOREPORT, 0x0010400000000000);
                    }
                    return;
                case T_AP_PWR:
                    m.ap_power = power;
                    if (power == POWER_QUIESCED && at(AT_AP_QUIESCE))
                        to_ap_mgmt(0, 0x00b0000000000010);
                    return;
            }
            break;
        case EP_IOREPORT:
            switch (FIELD_GET(MGMT_TYPE, msg0)) {
                case T_BUFFER:
                    dcp.ioreport_dva = msg0 & GENMASK(41, 0);
                    to_ap(EP_IOREPORT, 0x00c0000000000000);
                    return;
                case 0xc:
                case 8:
                    if (dcp.ioreports++ < 11) {
                        to_ap(EP_IOREPORT, 0x0080000000000000);
                    } else if (at(AT_POWER)) {
                        m.iop_power = 0x20;
                        to_ap(0, 0x0070000000000020);
                    }
                    return;
            }
            break;
        case EP_LOGS:
            switch (FIELD_GET(MGMT_TYPE, msg0)) {
                case T_BUFFER:
                    dcp.syslog_dva = msg0 & GENMASK(41, 0);
                    return;
                case 5:
                    return;
            }
            break;
        case EP_IBOOT:
            ep_receive(msg0);
            return;
    }
    firmware_assert("a message nobody asked for");
}

/* ---- the mailbox ---- */

#define ASC_BASE  0x30ae00000UL
#define ASC_MBOX  (ASC_BASE + 0x8000)
#define DART_DCP  0x30a340000UL
#define DART_DISP 0x30a300000UL

static u64 flood(bool high)
{
    switch (dcp.on) {
        case FLOODS_OTHER:
            return high ? 0x24 : 0x0085000001000000;
        case FLOODS_OWN:
            return high ? EP_IBOOT : 0x0099000000000000;
        case FLOODS_LOGS:
            return high ? EP_LOGS : 0x0050000000000004;
        default:
            return 0;
    }
}

static bool floods(void)
{
    return dcp.on >= FLOODS_OTHER && dcp.on <= FLOODS_LOGS;
}

static bool has_message(void)
{
    return floods() ||
           (dcp.q_head != dcp.q_tail && dcp.q[dcp.q_head % ARRAY_SIZE(dcp.q)].ready <= now);
}

static u64 mbox_read(u64 off)
{
    switch (off) {
        case 0x110:
            return dcp.on == FULL ? BIT(16) : 0;
        case 0x114:
            return has_message() ? 0 : BIT(17);
        case 0x830:
            if (floods())
                return flood(false);
            return has_message() ? dcp.q[dcp.q_head % ARRAY_SIZE(dcp.q)].msg0 : 0;
        case 0x838:
            if (floods()) {
                advance(20);
                return flood(true);
            }
            if (!has_message())
                return 0;
            /* Not that there is an entry: the host looks at the ring too, and time decides */
            if (dcp.q[dcp.q_head % ARRAY_SIZE(dcp.q)].msg0 >> 48 != 0x85)
                trace("dcp %02lx %016lx\n", (unsigned long)dcp.q[dcp.q_head % ARRAY_SIZE(dcp.q)].ep,
                      (unsigned long)dcp.q[dcp.q_head % ARRAY_SIZE(dcp.q)].msg0);
            return dcp.q[dcp.q_head++ % ARRAY_SIZE(dcp.q)].ep;
    }
    stray("a read of the mailbox", off);
    return 0;
}

static bool in(u64 addr, u64 base, u64 size)
{
    return addr >= base && addr < base + size;
}

u32 read32(u64 addr)
{
    if (in(addr, ASC_MBOX, 0x1000)) {
        advance(1);
        return mbox_read(addr - ASC_MBOX);
    }
    trace("read32 0x%lx\n", (unsigned long)addr);
    if (addr == ASC_BASE + 0x44)
        return 0x10; /* it runs, as iBoot leaves it */
    if (addr == DART_DCP + 0x200)
        return 0x1ff;
    if (addr == DART_DISP + 0x200)
        return 0x1f9;
    if (addr == DART_DCP + 0x1400 + 4 * 23)
        return L1_DCP >> 14 << 2 | 1;
    if (addr == DART_DISP + 0x1400)
        return L1_DISP >> 14 << 2 | 1;
    if (addr == DART_DCP + 0x700 || addr == DART_DISP + 0x700)
        return 0;
    stray("a read", addr);
    return 0;
}

void write32(u64 addr, u32 data)
{
    trace("write32 0x%lx 0x%x\n", (unsigned long)addr, data);
    /* Started, or stopped when an external display is put to sleep */
    if (addr == ASC_BASE + 0x44 && (data == 0x10 || (data == 0 && m.ap_power == POWER_QUIESCED)))
        return;
    if (addr == DART_DCP + 0xc00 && data == BIT(23))
        return;
    if (addr == DART_DISP + 0xc00 && data == BIT(0))
        return;
    if (addr == DART_DCP + 0x80 && data == (BIT(8) | 23))
        return;
    if (addr == DART_DISP + 0x80 && data == BIT(8))
        return;
    stray("a write", addr);
}

u64 read64(u64 addr)
{
    if (in(addr, ASC_MBOX, 0x1000)) {
        advance(1);
        return mbox_read(addr - ASC_MBOX);
    }
    stray("a 64-bit read", addr);
    return 0;
}

void write64(u64 addr, u64 data)
{
    if (addr == ASC_MBOX + 0x800) {
        dcp.send0 = data;
        return;
    }
    if (addr == ASC_MBOX + 0x808) {
        dcp_receive(dcp.send0, data);
        return;
    }
    stray("a 64-bit write", addr);
}

/* ---- a case ---- */

/* display.c as m1n1 has it when it starts; the cases run in a process each */
void machine_reset(void)
{
    static bool mapped;

    if (!mapped)
        memory_reset();
    mapped = true;
    memset(&m, 0, sizeof(m));
    memset(&dcp, 0, sizeof(dcp));
    memset(&later, 0, sizeof(later));
    n_blocks = 0;
    heap_top = HEAP_BASE;
    fake_adt_reset();

    m.block = 0x80;
    m.rb_size = 0x4000;
    m.ring_off = 0x4000;
    m.iop_power = m.ap_power = POWER_QUIESCED;
    entry_add(&m.announce, r44c_announce, sizeof(r44c_announce));
    entry_add(&m.hpd, r44c_get_hpd_reply, sizeof(r44c_get_hpd_reply));
    entry_add(&m.surface, r44c_set_surface_reply, sizeof(r44c_set_surface_reply));

    memset(&cur_boot_args, 0, sizeof(cur_boot_args));
    cur_boot_args.revision = 3;
    cur_boot_args.version = 2;
    cur_boot_args.phys_base = 0x10002e70000;
    cur_boot_args.mem_size = 0x3d87a0000;
    cur_boot_args.top_of_kernel_data = 0x10006870000;
    cur_boot_args.video.base = FB_BASE;
    cur_boot_args.video.display = 1;
    cur_boot_args.video.stride = FB_STRIDE;
    cur_boot_args.video.width = FB_WIDTH;
    cur_boot_args.video.height = FB_HEIGHT;
    cur_boot_args.video.depth = FB_DEPTH_FLAG_RETINA | 30;
    boot_args_memory = cur_boot_args;
    boot_args_addr = (u64)&boot_args_memory;
}

void machine_v1(void)
{
    /* struct epic_hdr, struct epic_sub_hdr, struct epic_announce */
    u8 announce[16 + 24 + 32] = {0};
    u32 length = 32;

    m.v1 = true;
    m.block = 0x40;
    memset(&m.announce, 0, sizeof(m.announce));
    announce[0] = 2; /* the version, and nothing at offset 4 */
    memcpy(announce + 16, &length, 4);
    announce[20] = 4;
    announce[22] = 0x30;
    strcpy((char *)announce + 16 + 24, "disp0-service");
    entry_add(&m.announce, announce, sizeof(announce));
}

void machine_recover(void)
{
    m.way = ANSWERS;
    dcp.on = ANSWERS;
    dcp.q_head = dcp.q_tail;
}
