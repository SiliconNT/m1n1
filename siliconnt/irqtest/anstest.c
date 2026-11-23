// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * EL1 test guest for the disk controller core of the SiliconNT drivers repository (ans/core,
 * ANS_CORE in the Makefile): the core as the Windows miniport will use it, on the machine's
 * internal disk controller (ANS), read only, without Windows. SiliconNT plan 0009 group 4,
 * lab run R38.
 *
 * run_anstest.py fills in the parameter block below: where the four register ranges of the
 * controller are (worked out from the device tree the way the firmware does it for the ACPI
 * node, SiliconNT findings/0062), the SART version, whether the I/O queue registers are to be
 * written (nvme-secure-bar), the SPIs of the controller's line and of the mailbox's "message
 * received" line, where the guest's memory for the device is, and how it is to be driven:
 *
 *   polling     ans_isr() and ans_mbox_service() in a loop; the SPIs are never enabled
 *   interrupts  ans_isr() from the handler of the two SPIs, ans_mbox_service() afterwards
 *               outside it, as the miniport's DPC will
 *
 * Either way the guest
 *
 *   1. checks that the controller was handed over stopped (ans_probe()); with interrupts it
 *      then enables both SPIs and waits OBSERVE_MS: neither may arrive
 *   2. brings the controller up (ans_start()); that identifies the controller and namespace 1
 *   3. reads, one block at a time into consecutive slots, the blocks that SiliconNT's
 *      tools/m1n1_nvme_probe.py reads for its digest, in its order: LBA 0, LBA 1 (the GPT
 *      header, whose CRC-32 is checked), the partition entries (CRC-32 checked), the first
 *      block of every partition in the order of their start, DIGEST_BLOCKS blocks at the
 *      start of the third partition. run_anstest.py hashes the slots as that script does
 *   4. reads the first 63 of those last blocks again with one command, into pages scattered
 *      in reverse order with a guard page next to each, the first piece starting in the middle
 *      of a page; compares with step 3 and checks the guard pages
 *   5. reads burst_mib MiB from the start of the third partition with every tag in flight:
 *      once timed, once with a CRC-32 of each read
 *   6. brings the controller down (ans_stop()); with interrupts it then waits OBSERVE_MS
 *      again with the SPIs enabled: neither may arrive
 *
 * Every register access of the core comes through the functions of ans_env.h in this file,
 * which check it against the size of its range. A write to one of the two submission
 * doorbells is held until the command it rings has been put into the command log and
 * checked against this file's own whitelist (env_cmd_allowed()); a command that is not on
 * it, or that finds the log full, is not rung. run_anstest.py checks the log once more with
 * a third whitelist after the guest has stopped.
 *
 * The guest runs on the boot CPU only, with the MMU off: every data access is to Device
 * memory, uncached, so memory the controller reads and writes needs no cache maintenance,
 * and the address the guest uses is the physical address (the hypervisor maps RAM one to one,
 * SiliconNT findings/0057 conclusion 12). It prints on the hypervisor's virtual UART; lines
 * start with "HVLOG: " so that they come back on the proxy console. At the end it fills in
 * the result block below, which run_anstest.py reads, and calls PSCI SYSTEM_OFF. Entry
 * points, vectors and the image header are those of sgitest (sgitest-head.S).
 */

#include "ans_core.h"
#include "gic.h"
#include "sgitest.h"

typedef long s64;

void uart_putc(int c);
extern char image_start[];

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        u64 _v;                                                                                    \
        asm volatile("mrs %0, " #r : "=r"(_v));                                                    \
        _v;                                                                                        \
    })
#define write_sysreg(r, v) asm volatile("msr " #r ", %0" : : "r"((u64)(v)))
#define isb()              asm volatile("isb")
#define irq_mask()         asm volatile("msr daifset, #2" ::: "memory")
#define irq_unmask()       asm volatile("msr daifclr, #2" ::: "memory")

/* ---- what run_anstest.py writes and reads; keep in step with it ---- */

#define MODE_POLLING   1
#define MODE_INTERRUPT 2

#define FLAG_TRACE_WRITES 1 /* print every register write outside ANS_READY */

struct anstest_params {
    char magic[8];           /* "SNTANS01" */
    u64 version;             /* 1 */
    u64 mode;                /* MODE_* */
    u64 base[ANS_RGN_COUNT]; /* NVMe, coprocessor and mailbox, SART, power manager */
    u64 size[ANS_RGN_COUNT];
    u64 spi[2]; /* the controller's line, the mailbox's "message received" */
    u64 sart_version;
    u64 ioq_registers; /* nvme-secure-bar */
    u64 pool_off;      /* memory for the device, from image_start */
    u64 pool_size;
    u64 burst_mib;
    u64 flags; /* FLAG_* */
};

struct anstest_params params __attribute__((aligned(16))) = {
    .magic = {'S', 'N', 'T', 'A', 'N', 'S', '0', '1'},
};

enum outcome {
    OUT_NONE = 0,     /* the guest did not get to the end */
    OUT_OK,           /* up, read right, down cleanly, nothing counted */
    OUT_PRECONDITION, /* stopped before touching the controller */
    OUT_PROBE_FAILED, /* the controller was not handed over stopped: not taken over */
    OUT_START_FAILED, /* ans_start() failed; the core brought the controller down again */
    OUT_CRASHED,      /* the coprocessor crashed */
    OUT_WRONG,        /* up, but something on the way did not hold */
};

/* All 64 bits, so that run_anstest.py can read it as a list (RESULT_FIELDS there) */
struct anstest_result {
    char magic[8]; /* "SNTANSR1" */
    u64 finished;  /* 1 once the guest has filled in the rest */
    u64 outcome;
    u64 exception; /* vector + 1 when an unexpected exception ended the guest */
    s64 probe;
    s64 start;
    u64 state_started;
    u64 state_end;
    u64 blocks;
    u64 block_size;
    u64 mdts_bytes;
    u64 msgs_ignored;
    u64 msgs_refused;
    u64 msgs_lost;
    u64 nvmmu_inval_failed;
    u64 stray_completions;
    u64 readback_addr;
    u64 readback_blocks;
    u64 gpt_checks; /* GPT_* that held */
    u64 partitions;
    u64 scatter_equal; /* 1: step 4 read what step 3 read */
    u64 guards_intact; /* 1: no guard page and no unused slot was written */
    u64 burst_reads;
    u64 burst_bytes;
    u64 burst_ticks;
    u64 burst_crc;   /* CRC-32 of the CRC-32s of the reads, in LBA order */
    u64 burst_first; /* 1: the first read of the burst has the CRC-32 of step 4 */
    u64 read_errors;
    u64 read_timeouts;
    u64 cmdlog_addr;
    u64 cmdlog_count;
    u64 cmdlog_full; /* doorbells not rung because the log was full */
    u64 env_refused; /* doorbells not rung because the command was not on the whitelist */
    u64 env_bad_access;
    u64 iosq_check; /* 0: no I/O queue registers; 1: 0x1200 holds the queue made; 2: not */
    u64 irq_ctrl;
    u64 irq_mbox;
    u64 irq_other;
    u64 irq_spurious;
    u64 irq_before_start;
    u64 irq_after_stop;
    u64 storms; /* bit 0 controller, bit 1 mailbox: SPI disabled, its line stayed high */
    u64 cntfrq;
    u64 start_ticks;
    u64 stop_ticks;
    u64 dma_allocs;
    u64 dma_frees;
    u64 dma_bytes;
};

struct anstest_result result __attribute__((aligned(16))) = {
    .magic = {'S', 'N', 'T', 'A', 'N', 'S', 'R', '1'},
};

#define GPT_PROTECTIVE_MBR 1
#define GPT_HEADER         2
#define GPT_HEADER_CRC     4
#define GPT_ENTRIES_CRC    8
#define GPT_ALL            15

/* ---- the layout of the memory for the device, from the start of the pool ---- */

#define PAGE           4096UL
#define DMA_ARENA      (32UL << 20) /* for ans_env_dma_alloc() */
#define READBACK_SLOTS 128UL
#define SCATTER_PAGES  130UL /* the pieces in the odd pages 1..127, the rest guards */
#define CMDLOG_MAX     4096UL
#define CMDLOG_WORDS   10UL /* meta, the 64 bytes of the command, the time */
#define CRC_SLOTS      1024UL

#define IO_DEPTH      ANS_MAX_IO_DEPTH
#define CHUNK_BLOCKS  63U /* findings/0071 conclusion 8: 252 KiB a read */
#define DIGEST_BLOCKS 64U /* DIGEST_BLOCKS of m1n1_nvme_probe.py */
#define MAX_PARTS     16

#define OBSERVE_MS      50
#define READ_TIMEOUT_MS 2000
#define STORM_LIMIT     100000 /* interrupts in a row for which ans_isr() found nothing */

#define GUARD 0xa5a5a5a5a5a5a5a5UL

static u64 pool, pool_end;
static u64 arena, arena_next;
static u64 readback, scatter, cmdlog, crcs, burst_buf;
static u32 block_size, chunk_bytes;

/* ---- output ---- */

static void put(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_num(u64 v, unsigned base, int width, char pad)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width && n < (int)sizeof(buf))
        buf[n++] = pad;
    while (n)
        uart_putc(buf[--n]);
}

/*
 * The conversions the core uses (ans_env.h: %s %d %u %x %02x %08x %llu %llx %016llx).
 * 32-bit values arrive as int, 64-bit ones as long long.
 */
static void vprint(const char *fmt, __builtin_va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, longs = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        switch (*fmt) {
            case 's': {
                const char *s = __builtin_va_arg(ap, const char *);
                put(s ? s : "(null)");
                break;
            }
            case 'd': {
                s64 v = longs ? __builtin_va_arg(ap, long long) : __builtin_va_arg(ap, int);
                if (v < 0) {
                    uart_putc('-');
                    v = -v;
                }
                put_num((u64)v, 10, width, pad);
                break;
            }
            case 'u':
            case 'x': {
                u64 v = longs ? __builtin_va_arg(ap, unsigned long long)
                              : __builtin_va_arg(ap, unsigned int);
                put_num(v, *fmt == 'u' ? 10 : 16, width, pad);
                break;
            }
            case '%':
                uart_putc('%');
                break;
            default:
                put("<bad format>");
                return;
        }
    }
}

static void say(const char *fmt, ...)
{
    __builtin_va_list ap;

    put("HVLOG: anstest: ");
    __builtin_va_start(ap, fmt);
    vprint(fmt, ap);
    __builtin_va_end(ap);
    uart_putc('\n');
}

#define ULL(v) ((unsigned long long)(v))

/* ---- time ---- */

static u64 ticks_per_ms, ticks_per_us;

static u64 now(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

static void spin_us(u64 us)
{
    u64 end = now() + us * ticks_per_us;

    while (now() < end)
        ;
}

/* ---- memory: Device memory to the guest, so aligned accesses only (rd8() and the like of
 * gic.h are single loads) ---- */

static void fill64(u64 addr, u64 bytes, u64 val)
{
    for (u64 off = 0; off < bytes; off += 8)
        *(volatile u64 *)(addr + off) = val;
}

static int all64(u64 addr, u64 bytes, u64 val)
{
    for (u64 off = 0; off < bytes; off += 8)
        if (*(volatile u64 *)(addr + off) != val)
            return 0;
    return 1;
}

/* GCC may call these for structure copies and initialisers; byte by byte, never DC ZVA */
void *memset(void *s, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);

void *memset(void *s, int c, unsigned long n)
{
    volatile unsigned char *p = s;

    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    volatile unsigned char *dp = d;
    const volatile unsigned char *sp = s;

    while (n--)
        *dp++ = *sp++;
    return d;
}

/* CRC-32 as the GPT has it (IEEE, reflected), with the CRC32 instructions */
static u32 crc32_update(u32 crc, u64 addr, u64 len)
{
    while (len && (addr & 7)) {
        u32 b = rd8(addr++);
        asm("crc32b %w0, %w0, %w1" : "+r"(crc) : "r"(b));
        len--;
    }
    for (; len >= 8; addr += 8, len -= 8) {
        u64 x = rd64(addr);
        asm("crc32x %w0, %w0, %x1" : "+r"(crc) : "r"(x));
    }
    while (len--) {
        u32 b = rd8(addr++);
        asm("crc32b %w0, %w0, %w1" : "+r"(crc) : "r"(b));
    }
    return crc;
}

static u32 crc32(u64 addr, u64 len)
{
    return ~crc32_update(~0U, addr, len);
}

/* ---- the environment of the core (ans_env.h) ---- */

struct ans_env {
    u64 doorbells; /* rung */
};

static struct ans_env env;
static struct ans_core core;
static int interrupts;

/* A switch, not a table of pointers: the image runs where it was not linked */
static const char *rgn_name(enum ans_region rgn)
{
    switch (rgn) {
        case ANS_RGN_NVME:
            return "nvme";
        case ANS_RGN_ASC:
            return "asc";
        case ANS_RGN_SART:
            return "sart";
        case ANS_RGN_PMGR:
            return "pmgr";
        default:
            return "?";
    }
}

#define NVME_ASQ            0x28
#define NVME_IOQ_CMDS       0x1200
#define NVME_DB_LINEAR_ASQ  0x2490c
#define NVME_DB_LINEAR_IOSQ 0x24910

enum queue { Q_ADMIN, Q_IO };

static u64 asq, iosq, ioq_cmds;

static int access_ok(enum ans_region rgn, uint32_t off, uint32_t width)
{
    if ((unsigned)rgn < ANS_RGN_COUNT && !(off & (width - 1)) &&
        (u64)off + width <= params.size[rgn] &&
        !(rgn == ANS_RGN_NVME && off < 0x2400c && off + width > 0x24008))
        return 1;
    result.env_bad_access++;
    say("env: refused a %u-bit access to range %u at +%x", width * 8, (unsigned)rgn, off);
    return 0;
}

static int tracing(void)
{
    return (params.flags & FLAG_TRACE_WRITES) && ans_get_state(&core) != ANS_READY;
}

/*
 * This file's own whitelist of NVMe commands (findings/0071 conclusion 22), written apart
 * from the core's: create and delete I/O queue 1 on completion queue 1, identify the
 * controller or namespace 1, read namespace 1. No fused commands, no SGLs.
 */
static int env_cmd_allowed(enum queue q, u32 opcode, u32 flags, u32 nsid, u32 cdw10, u32 cdw11)
{
    if (flags)
        return 0;
    if (q == Q_IO)
        return opcode == 0x02 && nsid == 1;
    switch (opcode) {
        case 0x05: /* create I/O completion queue */
            return nsid == 0 && (cdw10 & 0xffff) == 1;
        case 0x01: /* create I/O submission queue */
            return nsid == 0 && (cdw10 & 0xffff) == 1 && (cdw11 >> 16) == 1;
        case 0x06: /* identify */
            return (cdw10 == 1 && nsid == 0) || (cdw10 == 0 && nsid == 1);
        case 0x00: /* delete I/O submission queue */
        case 0x04: /* delete I/O completion queue */
            return nsid == 0 && cdw10 == 1;
        default:
            return 0;
    }
}

/* Puts the command that a doorbell write rings into the log and checks it; 0: do not ring */
static int doorbell(enum queue q, u32 tag)
{
    u64 base = q == Q_ADMIN ? asq : iosq;
    const char *qname = q == Q_ADMIN ? "admin" : "I/O";

    if (!base || tag >= 64) {
        result.env_refused++;
        say("env: NOT RUNG: %s doorbell, tag %u, submission queue at %llx", qname, tag, ULL(base));
        return 0;
    }
    if (result.cmdlog_count >= CMDLOG_MAX) {
        result.cmdlog_full++;
        say("env: NOT RUNG: %s doorbell, tag %u: the command log is full", qname, tag);
        return 0;
    }

    u64 sqe = base + (u64)tag * 64;
    u64 w0 = rd64(sqe), w5 = rd64(sqe + 40);
    u32 opcode = w0 & 0xff, flags = (w0 >> 8) & 0xff, nsid = (u32)(w0 >> 32);
    u32 cdw10 = (u32)w5, cdw11 = (u32)(w5 >> 32);
    int allowed = env_cmd_allowed(q, opcode, flags, nsid, cdw10, cdw11);

    volatile u64 *log = (volatile u64 *)(cmdlog + result.cmdlog_count * CMDLOG_WORDS * 8);
    log[0] = (u64)q | ((u64)tag << 8) | ((u64)allowed << 16);
    for (int i = 0; i < 8; i++)
        log[1 + i] = rd64(sqe + 8 * i);
    log[9] = now();
    result.cmdlog_count++;

    if (!allowed) {
        result.env_refused++;
        say("env: NOT RUNG: %s command opcode %02x flags %x nsid %u cdw10 %x cdw11 %x: not on "
            "the whitelist",
            qname, opcode, flags, nsid, cdw10, cdw11);
        return 0;
    }
    if (q == Q_ADMIN) {
        say("env: admin command opcode %02x nsid %u cdw10 %x cdw11 %x prp1 %llx", opcode, nsid,
            cdw10, cdw11, ULL(rd64(sqe + 24)));
        if (opcode == 0x01)
            iosq = rd64(sqe + 24); /* PRP1 of create I/O submission queue */
    }
    env.doorbells++;
    return 1;
}

uint32_t ans_env_read32(struct ans_env *e, enum ans_region rgn, uint32_t off)
{
    (void)e;
    if (!access_ok(rgn, off, 4))
        return 0xffffffffu;
    return *(volatile uint32_t *)(params.base[rgn] + off);
}

void ans_env_write32(struct ans_env *e, enum ans_region rgn, uint32_t off, uint32_t value)
{
    (void)e;
    if (!access_ok(rgn, off, 4))
        return;
    if (rgn == ANS_RGN_NVME) {
        if ((off == NVME_DB_LINEAR_ASQ && !doorbell(Q_ADMIN, value)) ||
            (off == NVME_DB_LINEAR_IOSQ && !doorbell(Q_IO, value)))
            return;
        if (off == NVME_ASQ)
            asq = (asq & ~0xffffffffUL) | value;
        else if (off == NVME_ASQ + 4)
            asq = (asq & 0xffffffffUL) | ((u64)value << 32);
        else if (off == NVME_IOQ_CMDS)
            ioq_cmds = (ioq_cmds & ~0xffffffffUL) | value;
        else if (off == NVME_IOQ_CMDS + 4)
            ioq_cmds = (ioq_cmds & 0xffffffffUL) | ((u64)value << 32);
    }
    if (tracing())
        say("W32 %s+%x = %08x", rgn_name(rgn), off, value);
    *(volatile uint32_t *)(params.base[rgn] + off) = value;
}

uint64_t ans_env_read64(struct ans_env *e, enum ans_region rgn, uint32_t off)
{
    (void)e;
    if (!access_ok(rgn, off, 8))
        return ~0ull;
    return *(volatile uint64_t *)(params.base[rgn] + off);
}

void ans_env_write64(struct ans_env *e, enum ans_region rgn, uint32_t off, uint64_t value)
{
    (void)e;
    if (!access_ok(rgn, off, 8))
        return;
    /* the queue registers and the doorbells are followed 32 bits at a time only */
    if (rgn == ANS_RGN_NVME && ((off + 8 > NVME_ASQ && off < NVME_ASQ + 8) ||
                                (off + 8 > NVME_IOQ_CMDS && off < NVME_IOQ_CMDS + 8) ||
                                (off + 8 > NVME_DB_LINEAR_ASQ && off < NVME_DB_LINEAR_IOSQ + 4))) {
        result.env_bad_access++;
        say("env: refused a 64-bit write to nvme+%x", off);
        return;
    }
    if (tracing())
        say("W64 %s+%x = %016llx", rgn_name(rgn), off, ULL(value));
    *(volatile uint64_t *)(params.base[rgn] + off) = value;
}

bool ans_env_dma_alloc(struct ans_env *e, size_t size, struct ans_dma *dma)
{
    u64 start = (arena_next + 0x3fff) & ~0x3fffUL;

    (void)e;
    if (!size || start + size > arena + DMA_ARENA) {
        say("env: no device memory left for %llu bytes", ULL(size));
        return false;
    }
    fill64(start, (size + 7) & ~7UL, 0);
    arena_next = start + size;
    dma->va = (void *)start;
    dma->dva = start; /* the MMU is off and the hypervisor maps RAM one to one */
    dma->size = size;
    dma->env_priv = 0;
    result.dma_allocs++;
    result.dma_bytes += size;
    if (tracing())
        say("dma: %llx bytes at %llx", ULL(size), ULL(start));
    return true;
}

void ans_env_dma_free(struct ans_env *e, struct ans_dma *dma)
{
    (void)e;
    if (!dma->va || dma->dva < arena || dma->dva + dma->size > arena + DMA_ARENA) {
        result.env_bad_access++;
        say("env: freeing device memory that was not given out: %llx", ULL(dma->dva));
        return;
    }
    /* Not given out again: each guest brings the controller up once */
    result.dma_frees++;
}

void ans_env_udelay(struct ans_env *e, uint32_t usec)
{
    (void)e;
    spin_us(usec);
}

uint32_t ans_env_wait(struct ans_env *e, uint32_t usec)
{
    (void)e;
    /* With interrupts, this is where the handler gets in: the core is consistent here */
    if (interrupts)
        irq_unmask();
    spin_us(usec);
    if (interrupts)
        irq_mask();
    return usec;
}

void ans_env_wmb(struct ans_env *e)
{
    (void)e;
    asm volatile("dmb oshst" ::: "memory");
}

void ans_env_rmb(struct ans_env *e)
{
    (void)e;
    asm volatile("dmb oshld" ::: "memory");
}

void ans_env_log(struct ans_env *e, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)e;
    put("HVLOG: anstest: core: ");
    __builtin_va_start(ap, fmt);
    vprint(fmt, ap);
    __builtin_va_end(ap);
    uart_putc('\n');
}

/* ---- reads ---- */

static volatile u32 tag_done[IO_DEPTH];
static volatile int tag_status[IO_DEPTH];
static volatile u32 mbox_pending;

static void read_done(void *ctx, uint16_t tag, int status)
{
    (void)ctx;
    if (tag < IO_DEPTH) {
        tag_status[tag] = status;
        tag_done[tag] = 1;
    }
}

/*
 * What the miniport's interrupt handler and DPC will do, in this guest's two ways. The
 * reading loops call it with the interrupt masked; it returns with it masked.
 */
static void service(void)
{
    if (!interrupts) {
        ans_isr(&core, read_done, 0);
        ans_mbox_service(&core);
        return;
    }
    irq_unmask();
    spin_us(5);
    irq_mask();
    if (mbox_pending) {
        mbox_pending = 0;
        ans_mbox_service(&core);
    }
}

/* After a read that never completed: the DMA is stopped first (findings/0071 conclusion 28) */
static void read_timed_out(const char *what)
{
    result.read_timeouts++;
    say("FAIL %s did not complete within %u ms: halting the controller", what, READ_TIMEOUT_MS);
    ans_halt(&core);
}

/* One read with tag 0; 0 if it completed without an error */
static int read_sync(u64 lba, u32 blocks, const struct ans_segment *segs, u32 nsegs)
{
    int r;

    tag_done[0] = 0;
    r = ans_read(&core, 0, lba, blocks, segs, nsegs);
    if (r) {
        result.read_errors++;
        say("FAIL ans_read(LBA %llu, %u blocks): %d", ULL(lba), blocks, r);
        return r;
    }

    u64 deadline = now() + READ_TIMEOUT_MS * ticks_per_ms;
    while (!tag_done[0]) {
        service();
        if (ans_get_state(&core) != ANS_READY) {
            result.read_errors++;
            say("FAIL the core stopped during the read of LBA %llu", ULL(lba));
            return -100;
        }
        if (now() > deadline) {
            read_timed_out("a read");
            return -101;
        }
    }
    if (tag_status[0]) {
        result.read_errors++;
        say("FAIL the read of LBA %llu completed with status %x", ULL(lba), tag_status[0]);
        return -102;
    }
    return 0;
}

/* Reads one block into the next slot of the readback area; its address, or 0 */
static u64 read_block(u64 lba)
{
    if (result.readback_blocks >= READBACK_SLOTS) {
        result.read_errors++;
        say("FAIL more blocks than readback slots");
        return 0;
    }
    u64 slot = readback + result.readback_blocks * PAGE;
    struct ans_segment seg = {slot, PAGE};

    if (read_sync(lba, 1, &seg, 1))
        return 0;
    result.readback_blocks++;
    return slot;
}

struct part {
    u64 start, end;
    u32 type; /* the first 32 bits of the type GUID */
};

static const char *first_block_kind(u64 blk)
{
    static const char fat32[8] = {'F', 'A', 'T', '3', '2', ' ', ' ', ' '};
    int is_fat32 = 1;

    if (rd32(blk + 32) == 0x4253584e) /* "NXSB" */
        return "NXSB";
    for (int i = 0; i < 8; i++)
        is_fat32 &= rd8(blk + 82 + i) == (u8)fat32[i];
    return is_fat32 ? "FAT32" : "neither NXSB nor FAT32";
}

/* Step 3 up to the partitions. The first LBA of the third partition by start, 0 if none; ~0 */
static u64 read_tables(void)
{
    struct part parts[MAX_PARTS];
    u32 nparts = 0;
    u64 blk;

    if (!(blk = read_block(0)))
        return ~0UL;
    if (rd8(blk + 510) == 0x55 && rd8(blk + 511) == 0xaa && rd8(blk + 450) == 0xee)
        result.gpt_checks |= GPT_PROTECTIVE_MBR;
    else
        say("FAIL LBA 0 is not a protective MBR");

    if (!(blk = read_block(1)))
        return ~0UL;
    u32 hdr_size = rd32(blk + 12), hdr_crc = rd32(blk + 16);
    u64 my_lba = rd64(blk + 24), ent_lba = rd64(blk + 72);
    u32 n_ent = rd32(blk + 80), ent_size = rd32(blk + 84), ent_crc = rd32(blk + 88);
    if (rd64(blk) != 0x5452415020494645UL /* "EFI PART" */ || hdr_size < 92 || hdr_size > PAGE ||
        my_lba != 1 || ent_size < 128 || (ent_size & 7) || n_ent > 1024) {
        say("FAIL LBA 1 is not a GPT header this guest takes (size %u, %u entries of %u bytes)",
            hdr_size, n_ent, ent_size);
        return ~0UL;
    }
    result.gpt_checks |= GPT_HEADER;
    u32 zero = 0;
    u32 crc = crc32_update(~0U, blk, 16);
    crc = crc32_update(crc, (u64)&zero, 4);
    crc = ~crc32_update(crc, blk + 20, hdr_size - 20);
    if (crc == hdr_crc)
        result.gpt_checks |= GPT_HEADER_CRC;
    else
        say("FAIL the CRC-32 of the GPT header is %08x, the header says %08x", crc, hdr_crc);

    u64 table_bytes = (u64)n_ent * ent_size;
    u64 table_blocks = (table_bytes + PAGE - 1) / PAGE;
    u64 table = readback + result.readback_blocks * PAGE;
    for (u64 i = 0; i < table_blocks; i++)
        if (!read_block(ent_lba + i))
            return ~0UL;
    crc = crc32(table, table_bytes);
    if (crc == ent_crc)
        result.gpt_checks |= GPT_ENTRIES_CRC;
    else
        say("FAIL the CRC-32 of the partition entries is %08x, the header says %08x", crc, ent_crc);

    for (u32 i = 0; i < n_ent; i++) {
        u64 ent = table + (u64)i * ent_size;
        if (!rd64(ent) && !rd64(ent + 8))
            continue;
        if (nparts == MAX_PARTS) {
            say("FAIL more than %u partitions", MAX_PARTS);
            return ~0UL;
        }
        struct part p = {rd64(ent + 32), rd64(ent + 40), rd32(ent)};
        u32 j = nparts++;
        while (j && parts[j - 1].start > p.start) {
            parts[j] = parts[j - 1];
            j--;
        }
        parts[j] = p;
    }
    result.partitions = nparts;
    say("GPT: %u entries of %u bytes from LBA %llu, %u in use; by start:", n_ent, ent_size,
        ULL(ent_lba), nparts);
    for (u32 i = 0; i < nparts; i++) {
        if (!(blk = read_block(parts[i].start)))
            return ~0UL;
        say("  type %08x LBA %llu..%llu, first block: %s", parts[i].type, ULL(parts[i].start),
            ULL(parts[i].end), first_block_kind(blk));
    }
    return nparts > 2 ? parts[2].start : 0;
}

static u32 scatter_crc;

/* Step 4. Page k of the scatter area holds a piece when k is odd and at most 127 */
static void read_scattered(u64 base_lba, u64 compare)
{
    struct ans_segment segs[CHUNK_BLOCKS + 1];
    u32 nsegs = 0;
    u64 left = (u64)CHUNK_BLOCKS * PAGE;

    fill64(scatter, SCATTER_PAGES * PAGE, GUARD);
    /* pages 127, 125, ..., 1; the first piece is the second half of page 127 */
    for (int k = 127; k > 0 && left; k -= 2) {
        u64 addr = scatter + (u64)k * PAGE, len = PAGE;
        if (k == 127) {
            addr += PAGE / 2;
            len = PAGE / 2;
        }
        if (len > left)
            len = left;
        segs[nsegs].addr = addr;
        segs[nsegs].len = (uint32_t)len;
        nsegs++;
        left -= len;
    }
    if (read_sync(base_lba, CHUNK_BLOCKS, segs, nsegs))
        return;

    int equal = 1;
    u64 c = compare;
    u32 crc = ~0U;
    for (u32 i = 0; i < nsegs; i++) {
        for (u64 off = 0; off < segs[i].len; off += 8, c += 8)
            if (rd64(segs[i].addr + off) != rd64(c))
                equal = 0;
        crc = crc32_update(crc, segs[i].addr, segs[i].len);
    }
    scatter_crc = ~crc;
    result.scatter_equal = equal;
    say("step 4: %u blocks in %u pieces, %s the single reads", CHUNK_BLOCKS, nsegs,
        equal ? "the same as" : "DIFFERENT FROM");

    /* the guard pages, the first half of page 127 and what the last piece left of its page */
    int intact = 1;
    for (u64 k = 0; k < SCATTER_PAGES; k++)
        if (!(k & 1) || k > 127)
            intact &= all64(scatter + k * PAGE, PAGE, GUARD);
    intact &= all64(scatter + 127 * PAGE, PAGE / 2, GUARD);
    intact &= all64(segs[nsegs - 1].addr + segs[nsegs - 1].len, PAGE - segs[nsegs - 1].len, GUARD);
    if (!intact) {
        result.guards_intact = 0;
        say("FAIL the controller wrote outside the pieces of the scattered read");
    }
}

/* Step 5, one pass of nreads reads; with_crc: the CRC-32 of each read into crcs */
static int read_burst(u64 lba, u64 nreads, int with_crc)
{
    u64 read_of[IO_DEPTH];
    u8 busy[IO_DEPTH];
    u64 next = 0, done = 0, progress = now();

    for (u32 t = 0; t < IO_DEPTH; t++)
        busy[t] = 0;
    while (done < nreads) {
        for (u32 t = 0; t < IO_DEPTH && next < nreads; t++) {
            if (busy[t])
                continue;
            struct ans_segment seg = {burst_buf + (u64)t * chunk_bytes, chunk_bytes};
            tag_done[t] = 0;
            int r = ans_read(&core, (uint16_t)t, lba + next * CHUNK_BLOCKS, CHUNK_BLOCKS, &seg, 1);
            if (r) {
                result.read_errors++;
                say("FAIL ans_read of the burst, tag %u: %d", t, r);
                return r;
            }
            busy[t] = 1;
            read_of[t] = next++;
        }
        service();
        for (u32 t = 0; t < IO_DEPTH; t++) {
            if (!busy[t] || !tag_done[t])
                continue;
            if (tag_status[t]) {
                result.read_errors++;
                say("FAIL a read of the burst completed with status %x", tag_status[t]);
                return -102;
            }
            if (with_crc)
                *(volatile u32 *)(crcs + 4 * read_of[t]) =
                    crc32(burst_buf + (u64)t * chunk_bytes, chunk_bytes);
            busy[t] = 0;
            done++;
            progress = now();
        }
        if (ans_get_state(&core) != ANS_READY) {
            result.read_errors++;
            say("FAIL the core stopped during the burst");
            return -100;
        }
        if (now() - progress > READ_TIMEOUT_MS * ticks_per_ms) {
            read_timed_out("a read of the burst");
            return -101;
        }
    }
    return 0;
}

static void reads(void)
{
    u64 base_lba = read_tables();
    if (base_lba == ~0UL)
        return;

    u64 container = readback + result.readback_blocks * PAGE;
    for (u32 i = 0; i < DIGEST_BLOCKS; i++)
        if (!read_block(base_lba + i))
            return;
    say("step 3: %llu blocks read one at a time", ULL(result.readback_blocks));

    /* nothing may have been written into the slots that were not used */
    u64 used = result.readback_blocks * PAGE;
    if (!all64(readback + used, READBACK_SLOTS * PAGE - used, GUARD)) {
        result.guards_intact = 0;
        say("FAIL something wrote into the unused readback slots");
    }

    read_scattered(base_lba, container);
    if (!result.scatter_equal)
        return;

    u64 nreads = (params.burst_mib << 20) / chunk_bytes;
    if (nreads > CRC_SLOTS)
        nreads = CRC_SLOTS;
    if (base_lba + nreads * CHUNK_BLOCKS > result.blocks)
        nreads = (result.blocks - base_lba) / CHUNK_BLOCKS;
    if (!nreads) {
        say("FAIL no room for the burst");
        return;
    }

    u64 t0 = now();
    if (read_burst(base_lba, nreads, 0))
        return;
    result.burst_ticks = now() - t0;
    if (read_burst(base_lba, nreads, 1))
        return;
    result.burst_reads = nreads;
    result.burst_bytes = nreads * chunk_bytes;

    u32 crc = crc32(crcs, nreads * 4);
    result.burst_crc = crc;
    result.burst_first = rd32(crcs) == scatter_crc;
    say("step 5: %llu reads of %u bytes, %u in flight: %llu ms, %llu MiB/s; CRC-32 of the "
        "CRC-32s %08x; the first read %s step 4",
        ULL(nreads), chunk_bytes, IO_DEPTH, ULL(result.burst_ticks / ticks_per_ms),
        ULL(result.burst_ticks ? (result.burst_bytes * result.cntfrq / result.burst_ticks) >> 20
                               : 0),
        crc, result.burst_first ? "is the same as" : "DIFFERS FROM");
}

/* ---- interrupts ---- */

static u32 unhandled[2];

void irq_handler(void)
{
    u64 iar = read_sysreg(ICC_IAR1_EL1);
    u32 intid = iar & 0xffffff;

    if (intid == 1023) {
        result.irq_spurious++;
        return;
    }

    int line = intid == params.spi[0] ? 0 : intid == params.spi[1] ? 1 : -1;
    if (line < 0) {
        result.irq_other++;
    } else {
        if (line == 0)
            result.irq_ctrl++;
        else
            result.irq_mbox++;
        if (ans_isr(&core, read_done, 0)) {
            unhandled[line] = 0;
        } else if (++unhandled[line] >= STORM_LIMIT && !(result.storms & (1U << line))) {
            /* the line stays high and nothing the core does lowers it */
            wr32(hdr_gicd_base + GICD_ICENABLER + 4 * (intid / 32), 1U << (intid % 32));
            result.storms |= 1U << line;
        }
        if (ans_get_state(&core) == ANS_READY)
            mbox_pending = 1;
    }
    write_sysreg(ICC_EOIR1_EL1, iar);
}

u64 sync_handler(u64 esr, u64 elr)
{
    (void)esr;
    (void)elr;
    return 0;
}

/* PSCI SYSTEM_OFF: the hypervisor stops the guest for good and does not come back */
static void system_off(void)
{
    register u64 x0 asm("x0") = 0x84000008UL;

    asm volatile("hvc #0" : "+r"(x0) : : "memory");
}

void bad_exception(u64 vector)
{
    irq_mask();
    result.exception = vector + 1;
    say("EXCEPTION vector %x ESR %08x ELR %016llx FAR %016llx: stopping, the controller is "
        "left as it is",
        (u32)vector, (u32)read_sysreg(esr_el1), ULL(read_sysreg(elr_el1)),
        ULL(read_sysreg(far_el1)));
    asm volatile("dsb sy" ::: "memory");
    system_off();
    for (;;)
        ;
}

void secondary_main(int slot)
{
    (void)slot;
    for (;;)
        ;
}

static u64 mpidr;

/* The GIC as the Windows kernel sets it up (aictest.c); 0, or the check that failed */
static int gic_init(void)
{
    u64 gicr, sre;
    int dist = gic_dist_init();

    if (dist)
        return dist;
    mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    gicr = gic_find_redist(mpidr);
    if (!gicr)
        return 10;

    sre = read_sysreg(ICC_SRE_EL1);
    if (!(sre & 1))
        write_sysreg(ICC_SRE_EL1, sre | 1);
    isb();
    write_sysreg(ICC_IGRPEN1_EL1, 0);
    write_sysreg(ICC_PMR_EL1, 0);
    write_sysreg(ICC_BPR1_EL1, 3);
    gic_local_init(gicr);
    write_sysreg(ICC_CTLR_EL1, read_sysreg(ICC_CTLR_EL1) & ~2UL); // EOImode = 0
    write_sysreg(ICC_IGRPEN1_EL1, 1);
    write_sysreg(ICC_PMR_EL1, 0xf0);
    isb();
    return 0;
}

static void spi_disable(u64 spi)
{
    wr32(hdr_gicd_base + GICD_ICENABLER + 4 * (spi / 32), 1U << (spi % 32));
}

/* Waits with the interrupt unmasked; how many of the two SPIs arrived meanwhile */
static u64 observe(const char *when)
{
    u64 before = result.irq_ctrl + result.irq_mbox;

    irq_unmask();
    spin_us(OBSERVE_MS * 1000);
    irq_mask();
    u64 n = result.irq_ctrl + result.irq_mbox - before;
    say("%s: %llu interrupts of the controller and the mailbox in %u ms", when, ULL(n), OBSERVE_MS);
    return n;
}

/* ---- the run ---- */

static int params_ok(void)
{
    if (params.version != 1 || (params.mode != MODE_POLLING && params.mode != MODE_INTERRUPT)) {
        say("FAIL the loader did not fill in the parameters (run_anstest.py)");
        return 0;
    }
    for (int i = 0; i < ANS_RGN_COUNT; i++) {
        if (!params.base[i] || !params.size[i] || (params.base[i] & 3) ||
            params.size[i] > 1UL << 32) {
            say("FAIL register range %s: %llx+%llx", rgn_name((enum ans_region)i),
                ULL(params.base[i]), ULL(params.size[i]));
            return 0;
        }
    }
    if (params.mode == MODE_INTERRUPT &&
        (params.spi[0] < 32 || params.spi[0] > 1019 || params.spi[1] < 32 || params.spi[1] > 1019 ||
         params.spi[0] == params.spi[1])) {
        say("FAIL SPIs %llu and %llu", ULL(params.spi[0]), ULL(params.spi[1]));
        return 0;
    }
    if (!params.burst_mib) {
        say("FAIL no burst to read");
        return 0;
    }

    pool = ((u64)image_start + params.pool_off + 0x3fff) & ~0x3fffUL;
    pool_end = (u64)image_start + params.pool_off + params.pool_size;
    chunk_bytes = CHUNK_BLOCKS * PAGE;
    arena = arena_next = pool;
    readback = arena + DMA_ARENA;
    scatter = readback + READBACK_SLOTS * PAGE;
    cmdlog = scatter + SCATTER_PAGES * PAGE;
    crcs = cmdlog + CMDLOG_MAX * CMDLOG_WORDS * 8;
    burst_buf = (crcs + CRC_SLOTS * 4 + 0x3fff) & ~0x3fffUL;
    if (!params.pool_off || burst_buf + (u64)IO_DEPTH * chunk_bytes > pool_end) {
        say("FAIL the memory for the device, %llx..%llx, is too small", ULL(pool), ULL(pool_end));
        return 0;
    }
    return 1;
}

static void run(void)
{
    struct ans_config config = {
        .ioq_registers = params.ioq_registers != 0,
        .sart_version = (uint32_t)params.sart_version,
        .io_depth = IO_DEPTH,
        .max_transfer = CHUNK_BLOCKS * PAGE,
    };
    u64 t0;
    int r;

    r = ans_probe(&core, &env);
    result.probe = r;
    if (r) {
        say("FAIL ans_probe: %d: the controller is not taken over", r);
        result.outcome = OUT_PROBE_FAILED;
        return;
    }

    if (interrupts) {
        gic_enable_spi((u32)params.spi[0], 0x80, mpidr, 0);
        gic_enable_spi((u32)params.spi[1], 0x80, mpidr, 0);
        result.irq_before_start = observe("before ans_start");
    }

    t0 = now();
    r = ans_start(&core, &env, &config);
    result.start_ticks = now() - t0;
    result.start = r;
    result.state_started = ans_get_state(&core);
    if (r) {
        say("FAIL ans_start: %d after %llu ms; the core is %llu", r,
            ULL(result.start_ticks / ticks_per_ms), ULL(result.state_started));
        result.state_end = result.state_started;
        result.outcome = result.state_started == ANS_CRASHED ? OUT_CRASHED : OUT_START_FAILED;
        if (interrupts)
            result.irq_after_stop = observe("after the failed start");
        return;
    }

    const struct ans_info *info = ans_get_info(&core);
    result.blocks = info->blocks;
    result.block_size = block_size = info->block_size;
    result.mdts_bytes = info->mdts_bytes;
    say("up in %llu ms: model '%s', firmware '%s', %llu blocks of %u bytes, MDTS %u bytes",
        ULL(result.start_ticks / ticks_per_ms), info->model, info->firmware, ULL(info->blocks),
        info->block_size, info->mdts_bytes);

    if (block_size == PAGE) {
        fill64(readback, READBACK_SLOTS * PAGE, GUARD);
        result.guards_intact = 1;
        reads();
    } else {
        result.read_errors++;
        say("FAIL blocks of %u bytes: this guest reads blocks of 4096", block_size);
    }

    t0 = now();
    ans_stop(&core);
    result.stop_ticks = now() - t0;
    result.state_end = ans_get_state(&core);
    say("down in %llu ms; the core is %llu", ULL(result.stop_ticks / ticks_per_ms),
        ULL(result.state_end));
    if (interrupts)
        result.irq_after_stop = observe("after ans_stop");
}

static void finish(void)
{
    const struct ans_stats *st = ans_get_stats(&core);

    if (interrupts) {
        spi_disable(params.spi[0]);
        spi_disable(params.spi[1]);
    }
    irq_mask();

    result.msgs_ignored = st->msgs_ignored;
    result.msgs_refused = st->msgs_refused;
    result.msgs_lost = st->msgs_lost;
    result.nvmmu_inval_failed = st->nvmmu_inval_failed;
    result.stray_completions = st->stray_completions;
    result.readback_addr = readback;
    result.cmdlog_addr = cmdlog;
    if (params.ioq_registers && iosq)
        result.iosq_check = ioq_cmds == iosq ? 1 : 2;

    if (result.outcome == OUT_NONE) {
        int wrong = result.state_end != ANS_OFF || result.gpt_checks != GPT_ALL ||
                    !result.scatter_equal || !result.guards_intact || !result.burst_reads ||
                    !result.burst_first || result.read_errors || result.read_timeouts ||
                    result.msgs_ignored || result.msgs_refused || result.msgs_lost ||
                    result.nvmmu_inval_failed || result.stray_completions || result.env_refused ||
                    result.cmdlog_full || result.env_bad_access ||
                    (params.ioq_registers && result.iosq_check != 1);
        if (interrupts)
            wrong |= result.irq_before_start || result.irq_after_stop || result.storms ||
                     !result.irq_ctrl || result.irq_other;
        if (result.state_end == ANS_CRASHED)
            result.outcome = OUT_CRASHED;
        else
            result.outcome = wrong ? OUT_WRONG : OUT_OK;
    }

    say("counts: ignored %llu refused %llu lost %llu nvmmu %llu stray %llu; commands %llu, "
        "not rung %llu, log full %llu, bad accesses %llu",
        ULL(result.msgs_ignored), ULL(result.msgs_refused), ULL(result.msgs_lost),
        ULL(result.nvmmu_inval_failed), ULL(result.stray_completions), ULL(result.cmdlog_count),
        ULL(result.env_refused), ULL(result.cmdlog_full), ULL(result.env_bad_access));
    if (interrupts)
        say("interrupts: controller %llu, mailbox %llu, other %llu, spurious %llu, storms %llx",
            ULL(result.irq_ctrl), ULL(result.irq_mbox), ULL(result.irq_other),
            ULL(result.irq_spurious), ULL(result.storms));
    result.finished = 1;
    asm volatile("dsb sy" ::: "memory");
    say("DONE outcome %llu", ULL(result.outcome));
    system_off();
}

void boot_main(void)
{
    u64 sctlr = read_sysreg(sctlr_el1);
    int gic;

    asm volatile("msr daifclr, #4"); /* SError on: a refused access is to show */
    result.cntfrq = read_sysreg(cntfrq_el0);
    ticks_per_ms = result.cntfrq / 1000;
    ticks_per_us = result.cntfrq / 1000000;
    interrupts = params.mode == MODE_INTERRUPT;
    say("start, %s", interrupts ? "interrupts" : "polling");

    if (sctlr & 5) {
        say("FAIL SCTLR_EL1 is %llx: the MMU or the data cache is on", ULL(sctlr));
        result.outcome = OUT_PRECONDITION;
    } else if (!params_ok()) {
        result.outcome = OUT_PRECONDITION;
    } else if (interrupts && (gic = gic_init())) {
        say("FAIL the GIC is not usable, check %d", gic);
        result.outcome = OUT_PRECONDITION;
    } else {
        say("nvme %llx+%llx asc %llx+%llx sart %llx+%llx pmgr %llx+%llx, SART version %llu, "
            "I/O queue registers %llu, SPIs %llu %llu, memory %llx..%llx",
            ULL(params.base[0]), ULL(params.size[0]), ULL(params.base[1]), ULL(params.size[1]),
            ULL(params.base[2]), ULL(params.size[2]), ULL(params.base[3]), ULL(params.size[3]),
            ULL(params.sart_version), ULL(params.ioq_registers), ULL(params.spi[0]),
            ULL(params.spi[1]), ULL(pool), ULL(pool_end));
        run();
    }
    finish();
    for (;;)
        ;
}
