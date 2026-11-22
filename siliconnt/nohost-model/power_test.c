// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_power.c: restarting and powering off the machine without
 * a host (SiliconNT docs/findings/0067 and 0069). The files under test are the real ones,
 * hv_power.c with smc.c, rtkit.c, asc.c, spmi.c and wdt.c under it, compiled for the build
 * machine against the stand-in headers of this directory. This file plays the hardware:
 *
 *  - the SMC: an RTKit coprocessor behind the mailbox of asc.c, already running as iBoot
 *    leaves it. It answers the wake-up, the endpoint map and SMC_INITIALIZE, and takes keys.
 *    Told MBSE = off1, it powers the machine off if the shutdown flag of the PMU is set, and
 *    restarts it if not ("needs PMU bit set to stay on", Linux drivers/power/reset/
 *    macsmc-reboot.c);
 *  - three SPMI buses (src/spmi.c, the first generation of registers, as the M4 has them),
 *    with the primary PMU on one of them at address 0xe and its scratchpad at 0xf800;
 *  - the watchdog: written as wdt_reboot() writes it, it restarts the machine once its 24 MHz
 *    count gets to the alarm.
 *
 * Time is the test's: it passes when the code waits (udelay(), timeout_expired()), and the
 * machine restarts or powers off when it has passed far enough. Each case says what the
 * hardware does, and checks what was written where, in what order, how the call ended
 * (the machine restarted, powered off, or hv_power_*() returned) and when.
 *
 * Every case is also a way for the hardware to fail: a coprocessor that does not answer, a
 * mailbox that holds messages from before, a PMU that cannot be written. The code must
 * neither hang nor do what it must not (off1 without the shutdown flag).
 */

#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fake_adt.h"
#include "hv.h"
#include "utils.h"
#include "wdt.h"

/* ---- checks ---- */

static int checks, failures;
static const char *scenario = "";

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, scenario, #cond);                  \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        u64 _a = (a), _b = (b);                                                                    \
        checks++;                                                                                  \
        if (_a != _b) {                                                                            \
            failures++;                                                                            \
            printf("FAIL %s:%d [%s]: %s is 0x%lx, not 0x%lx\n", __FILE__, __LINE__, scenario, #a,  \
                   (unsigned long)_a, (unsigned long)_b);                                          \
        }                                                                                          \
    } while (0)

#undef printf
#define say(...) fprintf(stdout, __VA_ARGS__)

/* ---- where the devices are ---- */

#define SMC_BASE     0x2c8000000UL /* reg 0 of /arm-io/smc: the coprocessor */
#define SMC_MBOX     (SMC_BASE + 0x8000)
#define SMC_SIZE     0x88000UL
#define SPMI_A1_BASE 0x2c9100000UL
#define SPMI0_BASE   0x2c9200000UL
#define SPMI1_BASE   0x2c9300000UL
#define WDT_BASE     0x2c9400000UL
#define WDT_SECOND   0x2c9408000UL

#define PMU_ADDR   0x0e
#define SCRATCH    0xf800
#define BOOT_STAGE (SCRATCH + 1)
#define BOOT_STATE (SCRATCH + 15)
#define SHUTDOWN   0x08
#define KEY_MBSE   0x4d425345
#define MBSE_OFFW  0x6f666677
#define MBSE_OFF1  0x6f666631

/* ---- time, and how the machine ends ---- */

enum outcome { RETURNED, RESTARTED, POWERED_OFF, M1N1_RESTARTED, HUNG };
static const char *const outcome_names[] = {"returned", "restarted", "powered off",
                                            "m1n1 restarted", "hung (not ended after 10 s)"};

/* What has not ended this long after it was called does not end: the longest case takes 3 s */
#define HANG_USEC 10000000

static jmp_buf machine;
static u64 now;          /* microseconds */
static u64 started_at;   /* when the case called the code */
static u64 restart_at;   /* 0: nothing pending */
static u64 power_off_at; /* 0: nothing pending */
static u64 ended_at;

static void advance(u64 us)
{
    now += us;
    if (restart_at && now >= restart_at) {
        ended_at = restart_at;
        longjmp(machine, RESTARTED);
    }
    if (power_off_at && now >= power_off_at) {
        ended_at = power_off_at;
        longjmp(machine, POWERED_OFF);
    }
    if (now - started_at > HANG_USEC) {
        ended_at = now;
        longjmp(machine, HUNG);
    }
}

void udelay(u32 d)
{
    advance(d);
}

u64 timeout_calculate(u32 usec)
{
    return now + usec;
}

/* Looking at the time takes a microsecond: a loop that only waits does get to its end */
bool timeout_expired(u64 timeout)
{
    advance(1);
    return now > timeout;
}

void flush_and_reboot(void)
{
    ended_at = now;
    longjmp(machine, M1N1_RESTARTED);
}

static int stray;
static char stray_what[160];

static void stray_access(const char *what, u64 addr)
{
    if (!stray++)
        snprintf(stray_what, sizeof(stray_what), "%s 0x%lx", what, (unsigned long)addr);
}

void model_sysop(const char *op)
{
    if (op[0] != 'd' && op[0] != 'i')
        stray_access("an instruction", 0);
}

/* ---- what the code printed ---- */

static char log_buf[1 << 16];
static size_t log_len;
/* How many lines: what floods the console's 8 KB pushes the reason out of it (hv_stop.c) */
static int printed_lines;

int debug_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (getenv("POWER_TEST_VERBOSE"))
        fputs(line, stdout);
    /* the last 64 KB */
    for (const char *p = line; *p; p++) {
        if (log_len == sizeof(log_buf) - 1) {
            memmove(log_buf, log_buf + sizeof(log_buf) / 2, sizeof(log_buf) / 2);
            log_len -= sizeof(log_buf) / 2;
        }
        log_buf[log_len++] = *p;
        printed_lines += *p == '\n';
    }
    log_buf[log_len] = 0;
    return n;
}

static bool printed(const char *text)
{
    return strstr(log_buf, text) != NULL;
}

/* ---- the heap and the rest of m1n1 ---- */

static int allocations;

void *model_malloc(size_t size)
{
    allocations++;
    return malloc(size);
}

void *model_calloc(size_t n, size_t size)
{
    allocations++;
    return calloc(n, size);
}

void *model_memalign(size_t align, size_t size)
{
    allocations++;
    return aligned_alloc(align, (size + align - 1) / align * align);
}

void model_free(void *p)
{
    free(p);
}

bool is_heap(void *addr)
{
    UNUSED(addr);
    return false;
}

void spin_lock(spinlock_t *lock)
{
    UNUSED(lock);
}

void spin_unlock(spinlock_t *lock)
{
    UNUSED(lock);
}

/* rtkit.c is linked whole: what it would use for coprocessors behind an IOMMU */
#include "dart.h"
#include "iova.h"
#include "sart.h"
#define NOT_HERE(...)                                                                              \
    {                                                                                              \
        stray_access("an IOMMU function", 0);                                                      \
        return 0;                                                                                  \
    }
void *dart_translate(dart_dev_t *dart, uintptr_t iova) NOT_HERE();
int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len) NOT_HERE();
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len)
{
    stray_access("an IOMMU function", 0);
}
u64 iova_alloc(iova_domain_t *iovad, size_t sz) NOT_HERE();
void iova_free(iova_domain_t *iovad, u64 iova, size_t sz)
{
    stray_access("an IOMMU function", 0);
}
bool sart_add_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE();
bool sart_remove_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE();

/* fake_adt.c: m1n1 would copy a property past the end of where it goes */
void fake_adt_overrun(const char *name, u32 len, u32 room)
{
    say("FAIL [%s]: m1n1 would copy %u bytes of %s to where %u go\n", scenario, len, name, room);
    failures++;
}

/* ---- what happened, in order ---- */

enum ev_kind {
    EV_KEY,      /* a: key, b: value */
    EV_PMU_W,    /* a: register, b: value */
    EV_PMU_R,    /* a: register */
    EV_WDT_W,    /* a: offset, b: value */
    EV_SMC_WAKE, /* the coprocessor's start bit */
};

static struct ev {
    enum ev_kind kind;
    u64 a, b, t;
} evs[256];
static int n_evs;

static void note(enum ev_kind kind, u64 a, u64 b)
{
    if (n_evs < (int)ARRAY_SIZE(evs))
        evs[n_evs++] = (struct ev){kind, a, b, now};
}

static int count(enum ev_kind kind)
{
    int n = 0;
    for (int i = 0; i < n_evs; i++)
        n += evs[i].kind == kind;
    return n;
}

/* The place of the first event of the kind with this a, or -1 */
static int find(enum ev_kind kind, u64 a)
{
    for (int i = 0; i < n_evs; i++)
        if (evs[i].kind == kind && evs[i].a == a)
            return i;
    return -1;
}

/* The place of the last event of the kind with this a, or -1 */
static int find_last(enum ev_kind kind, u64 a)
{
    for (int i = n_evs - 1; i >= 0; i--)
        if (evs[i].kind == kind && evs[i].a == a)
            return i;
    return -1;
}

static int find_key(u64 value)
{
    for (int i = 0; i < n_evs; i++)
        if (evs[i].kind == EV_KEY && evs[i].a == KEY_MBSE && evs[i].b == value)
            return i;
    return -1;
}

/* ---- the SMC ---- */

/* rtkit.c and smc.c: the fields of the messages */
#define MGMT_TYPE      GENMASK(59, 52)
#define T_HELLO        1
#define T_HELLO_ACK    2
#define T_START_EP     5
#define T_IOP_PWR      6
#define T_IOP_PWR_ACK  7
#define T_EPMAP        8
#define T_AP_PWR       0xb
#define T_BUFFER       1 /* on a system endpoint */
#define EPMAP_DONE     BIT(51)
#define SMC_EP         0x20
#define SMC_INITIALIZE 0x17
#define SMC_WRITE_KEY  0x11
#define SMC_NOTIFY     0x18

#define MBOX_A2I_CONTROL 0x110
#define MBOX_I2A_CONTROL 0x114
#define MBOX_A2I_SEND0   0x800
#define MBOX_A2I_SEND1   0x808
#define MBOX_I2A_RECV0   0x830
#define MBOX_I2A_RECV1   0x838
#define MBOX_FULL        BIT(16)
#define MBOX_EMPTY       BIT(17)
#define CPU_CONTROL      0x44

#define LATENCY 30 /* microseconds before an answer is there to be read */

/*
 * The SMC's own memory: the shared memory of SMC_INITIALIZE, and RTKit's buffers. Where the
 * M4's is, by its device tree (region-base of iop-smc-nub, 0x38de00000, 1 MB): an RTKit
 * buffer request has 42 bits for the address, and memory of this process is higher (main())
 */
#define SRAM_BASE 0x38de00000UL
#define SRAM_SIZE 0x100000UL
static u8 *sram;
#define SHMEM (sram + 0x1000)

static struct smc {
    /* What the case makes it do */
    bool no_hello, no_power_ack, no_init_reply, a2i_full;
    bool notify_before_init, notify_always, crash_on_start;
    /* After the wake-up: the power state before HELLO (and not after the endpoint map), a
     * notification, or a management message of no use; the power state in the middle of the
     * endpoint map */
    bool ack_before_hello, notify_before_hello, odd_before_hello, ack_during_epmap;
    /* From when on the mailbox is never empty again, with notifications */
    enum { FLOOD_NEVER, FLOOD_ALWAYS, FLOOD_AFTER_EPMAP, FLOOD_AFTER_START, FLOOD_AFTER_KEY } flood;
    u8 *shmem_at;                 /* the address it gives for the shared memory; NULL: SHMEM */
    u32 fail_value, silent_value; /* MBSE writes of this value are refused, or not answered */
    bool off1_does_nothing;
    u64 off_delay;
    /* What it is doing */
    struct {
        u64 msg0, ep, ready;
    } q[64];
    int q_head, q_tail;
    u64 send0;
    u64 started;
    bool shmem_given, flooding;
    u8 *shmem;
} smc;

static void to_ap(u64 ep, u64 msg0)
{
    if (smc.q_tail - smc.q_head == (int)ARRAY_SIZE(smc.q)) {
        stray_access("the SMC's mailbox overflowing", 0);
        return;
    }
    smc.q[smc.q_tail % ARRAY_SIZE(smc.q)].msg0 = msg0;
    smc.q[smc.q_tail % ARRAY_SIZE(smc.q)].ep = ep;
    smc.q[smc.q_tail % ARRAY_SIZE(smc.q)].ready = now + LATENCY;
    smc.q_tail++;
}

static u64 mgmt(u64 type, u64 rest)
{
    return FIELD_PREP(MGMT_TYPE, type) | rest;
}

static u64 notification(void)
{
    return 0x1234567800000000UL | SMC_NOTIFY;
}

static void smc_key(u64 msg0)
{
    u32 id = (msg0 >> 12) & 0xf, size = (msg0 >> 16) & 0xff, key = msg0 >> 32, value;

    memcpy(&value, smc.shmem, sizeof(value));
    note(EV_KEY, key, value);
    if (smc.flood == FLOOD_AFTER_KEY)
        smc.flooding = true;
    if (size != 4)
        stray_access("a key written with another size", size);
    if (key == KEY_MBSE && value == smc.fail_value) {
        to_ap(SMC_EP, 0x84 | id << 12);
        return;
    }
    if (key == KEY_MBSE && value == smc.silent_value)
        return;
    to_ap(SMC_EP, id << 12 | (u64)size << 16);
    if (key == KEY_MBSE && value == MBSE_OFF1 && !smc.off1_does_nothing) {
        extern u8 pmu_mem[];
        /* As the SMC's firmware has it: without the flag of the PMU, off1 restarts */
        if (pmu_mem[BOOT_STATE] & SHUTDOWN)
            power_off_at = now + smc.off_delay;
        else
            restart_at = now + smc.off_delay;
    }
}

/* A message from the AP */
static void smc_receive(u64 msg0, u64 ep)
{
    if (ep == 0) {
        switch (FIELD_GET(MGMT_TYPE, msg0)) {
            case T_IOP_PWR:
                /* One that was running says first how it was, or has a notification out */
                if (smc.ack_before_hello)
                    to_ap(0, mgmt(T_IOP_PWR_ACK, 0x20));
                if (smc.notify_before_hello)
                    to_ap(SMC_EP, notification());
                if (smc.odd_before_hello)
                    to_ap(0, mgmt(0x3f, 0));
                if ((msg0 & 0xffff) == 0x220 && !smc.no_hello)
                    to_ap(0, mgmt(T_HELLO, 12UL << 16 | 11));
                break;
            case T_HELLO_ACK:
                /* crashlog, syslog, ioreport; then the SMC's endpoint, 0x20 */
                to_ap(0, mgmt(T_EPMAP, BIT(1) | BIT(2) | BIT(4)));
                if (smc.ack_during_epmap)
                    to_ap(0, mgmt(T_IOP_PWR_ACK, 0x20));
                to_ap(0, mgmt(T_EPMAP, EPMAP_DONE | 1UL << 32 | BIT(0)));
                break;
            case T_EPMAP:
                /* Said already, when it says it once */
                if ((msg0 & EPMAP_DONE) && !smc.no_power_ack && !smc.ack_before_hello &&
                    !smc.ack_during_epmap)
                    to_ap(0, mgmt(T_IOP_PWR_ACK, 0x20));
                if ((msg0 & EPMAP_DONE) && smc.flood == FLOOD_AFTER_EPMAP)
                    smc.flooding = true;
                break;
            case T_START_EP: {
                u64 which = (msg0 >> 32) & 0xff;
                smc.started |= BIT(which >= 0x20 ? 63 : which);
                if (which == SMC_EP && smc.flood == FLOOD_AFTER_START)
                    smc.flooding = true;
                if (which == 1 || which == 2 || which == 4)
                    to_ap(which, mgmt(T_BUFFER, 1UL << 44 | (u64)(sram + 0x1000 * which)));
                if (which == 1 && smc.crash_on_start)
                    to_ap(1, mgmt(T_BUFFER, 1UL << 44 | (u64)(sram + 0x1000)));
                break;
            }
            case T_AP_PWR:
                to_ap(0, mgmt(T_AP_PWR, msg0 & 0xffff));
                break;
            default:
                stray_access("a management message", msg0);
        }
        return;
    }
    if (ep != SMC_EP) {
        stray_access("a message for a system endpoint", msg0);
        return;
    }
    if (!(smc.started & BIT(63)))
        stray_access("a message before endpoint 0x20 was started", msg0);
    if (smc.notify_always)
        to_ap(SMC_EP, notification());
    switch (msg0 & 0xff) {
        case SMC_INITIALIZE:
            if (smc.notify_before_init)
                to_ap(SMC_EP, notification());
            if (!smc.no_init_reply) {
                smc.shmem = smc.shmem_at ? smc.shmem_at : SHMEM;
                to_ap(SMC_EP, (u64)smc.shmem);
                smc.shmem_given = true;
            }
            break;
        case SMC_WRITE_KEY:
            if (!smc.shmem_given)
                stray_access("a key before the shared memory was given", msg0);
            smc_key(msg0);
            break;
        default:
            stray_access("an SMC command", msg0);
    }
}

static bool smc_has_message(void)
{
    return smc.flooding ||
           (smc.q_head != smc.q_tail && smc.q[smc.q_head % ARRAY_SIZE(smc.q)].ready <= now);
}

static u64 smc_read(u64 off, bool high)
{
    switch (off) {
        case MBOX_A2I_CONTROL:
            return smc.a2i_full ? MBOX_FULL : 0;
        case MBOX_I2A_CONTROL:
            return smc_has_message() ? 0 : MBOX_EMPTY;
        case MBOX_I2A_RECV0:
            if (smc.flooding)
                return notification();
            return smc_has_message() ? smc.q[smc.q_head % ARRAY_SIZE(smc.q)].msg0 : 0;
        case MBOX_I2A_RECV1:
            if (smc.flooding)
                return SMC_EP;
            if (!smc_has_message())
                return 0;
            return smc.q[smc.q_head++ % ARRAY_SIZE(smc.q)].ep;
    }
    UNUSED(high);
    stray_access("a read of the SMC", SMC_MBOX + off);
    return 0;
}

static void smc_write(u64 off, u64 data)
{
    switch (off) {
        case MBOX_A2I_SEND0:
            smc.send0 = data;
            return;
        case MBOX_A2I_SEND1:
            smc_receive(smc.send0, data);
            return;
    }
    stray_access("a write of the SMC", SMC_MBOX + off);
}

/* ---- the SPMI buses and the PMU ---- */

u8 pmu_mem[0x10000];

static struct spmi {
    /* What the case makes the PMU do */
    int fail_write, fail_read; /* a register that is not written, or not read; -1 none */
    int fail_write_after;      /* ...after this many writes of it that work */
    bool absent, tx_stuck;
    /* What it is doing */
    u32 cmd, words_left, len, data[4], ndata;
    u32 reply[8];
    int r_head, r_tail;
} spmi;

static void spmi_reply(u32 word)
{
    spmi.reply[spmi.r_tail++ % ARRAY_SIZE(spmi.reply)] = word;
}

static void spmi_execute(void)
{
    u32 opc = spmi.cmd & 0xff, addr = (spmi.cmd >> 8) & 0x7f, reg = spmi.cmd >> 16;
    u32 head = opc | addr << 8;

    if (!(spmi.cmd & BIT(15)))
        stray_access("an SPMI command without the active bit", spmi.cmd);
    if (spmi.absent || addr != PMU_ADDR)
        return; /* nobody answers */
    if ((opc & ~7) == 0x30) {
        if ((int)reg == spmi.fail_write && spmi.fail_write_after-- <= 0) {
            spmi_reply(head);
            return;
        }
        for (u32 i = 0; i < spmi.len; i++) {
            u8 val = spmi.data[i / 4] >> (8 * (i % 4));
            pmu_mem[(reg + i) & 0xffff] = val;
            note(EV_PMU_W, reg + i, val);
        }
        spmi_reply(head | BIT(15));
    } else if ((opc & ~7) == 0x38) {
        u32 word = 0;
        note(EV_PMU_R, reg, spmi.len);
        spmi_reply(head | ((int)reg == spmi.fail_read ? 0 : (u32)MASK(spmi.len) << 16));
        for (u32 i = 0; i < spmi.len; i++)
            word |= (u32)pmu_mem[(reg + i) & 0xffff] << (8 * (i % 4));
        spmi_reply(word);
    } else {
        stray_access("an SPMI command other than an extended long read or write", spmi.cmd);
        spmi_reply(head | BIT(15));
    }
}

static u32 spmi_read(u64 off)
{
    switch (off) {
        case 0x0:
            return (spmi.r_head == spmi.r_tail ? BIT(24) : 0) | (spmi.tx_stuck ? 0 : BIT(8));
        case 0x8:
            if (spmi.r_head == spmi.r_tail) {
                stray_access("a read of an empty SPMI reply", off);
                return 0;
            }
            return spmi.reply[spmi.r_head++ % ARRAY_SIZE(spmi.reply)];
    }
    stray_access("a read of SPMI", SPMI0_BASE + off);
    return 0;
}

static void spmi_write(u64 off, u32 data)
{
    if (off != 0x4) {
        stray_access("a write of SPMI", SPMI0_BASE + off);
        return;
    }
    if (spmi.words_left) {
        spmi.data[spmi.ndata++ % 4] = data;
        if (!--spmi.words_left)
            spmi_execute();
        return;
    }
    spmi.cmd = data;
    spmi.len = (data & 7) + 1;
    spmi.ndata = 0;
    if ((data & 0xf8) == 0x30) {
        spmi.words_left = (spmi.len + 3) / 4;
        return;
    }
    spmi_execute();
}

/* ---- the watchdog ---- */

static struct {
    bool dead;
    u32 count, alarm;
} wdt;

static void wdt_write(u64 off, u32 data)
{
    note(EV_WDT_W, off, data);
    switch (off) {
        case 0x10:
            wdt.count = data;
            break;
        case 0x14:
            wdt.alarm = data;
            break;
        case 0x1c:
            /* 24 MHz (t8132.dtsi: clkref), and bit 2 makes it reset the machine */
            if ((data & BIT(2)) && !wdt.dead)
                restart_at = now + (wdt.alarm > wdt.count ? (wdt.alarm - wdt.count) / 24 : 0) + 1;
            break;
    }
}

/* ---- the bus ---- */

static bool in(u64 addr, u64 base, u64 size)
{
    return addr >= base && addr < base + size;
}

/*
 * A read of the mailbox takes a microsecond: a loop that only reads it, and never looks at the
 * time, still lets time pass, and is found to hang (advance())
 */
u32 read32(u64 addr)
{
    if (in(addr, SMC_MBOX, 0x1000)) {
        advance(1);
        return smc_read(addr - SMC_MBOX, false);
    }
    if (addr == SMC_BASE + CPU_CONTROL)
        return 0x10; /* running, as iBoot leaves it */
    if (in(addr, SPMI0_BASE, 0x4000))
        return spmi_read(addr - SPMI0_BASE);
    stray_access("a read", addr);
    return 0;
}

void write32(u64 addr, u32 data)
{
    if (addr == SMC_BASE + CPU_CONTROL) {
        note(EV_SMC_WAKE, data, 0);
        return;
    }
    if (in(addr, SPMI0_BASE, 0x4000)) {
        spmi_write(addr - SPMI0_BASE, data);
        return;
    }
    if (in(addr, WDT_BASE, 0x4000)) {
        wdt_write(addr - WDT_BASE, data);
        return;
    }
    if (addr == WDT_SECOND) {
        note(EV_WDT_W, 0x10000, data);
        return;
    }
    stray_access("a write", addr);
}

u64 read64(u64 addr)
{
    if (in(addr, SMC_MBOX, 0x1000)) {
        advance(1);
        return smc_read(addr - SMC_MBOX, true);
    }
    stray_access("a 64-bit read", addr);
    return 0;
}

void write64(u64 addr, u64 data)
{
    if (in(addr, SMC_MBOX, 0x1000)) {
        smc_write(addr - SMC_MBOX, data);
        return;
    }
    stray_access("a 64-bit write", addr);
}

/* ---- a case ---- */

static const struct fake_adt_places places = {
    .smc = SMC_BASE,
    .smc_size = SMC_SIZE,
    .spmi_a1 = SPMI_A1_BASE,
    .spmi0 = SPMI0_BASE,
    .spmi1 = SPMI1_BASE,
    .wdt = WDT_BASE,
    .wdt_second = WDT_SECOND,
    .smc_sram = SRAM_BASE,
    .smc_sram_size = SRAM_SIZE,
};

/* The machine as m1n1 finds it when it starts, and as it is when the guest has run */
static void reset(const char *name, u8 boot_state)
{
    scenario = name;
    memset(&smc, 0, sizeof(smc));
    smc.off_delay = 20000;
    memset(&spmi, 0, sizeof(spmi));
    spmi.fail_write = spmi.fail_read = -1;
    memset(&wdt, 0, sizeof(wdt));
    memset(pmu_mem, 0, sizeof(pmu_mem));
    pmu_mem[BOOT_STAGE] = 0x2f; /* "the last stage of the boot loader" (macsmc-reboot.c) */
    pmu_mem[SCRATCH + 2] = 0x11;
    pmu_mem[BOOT_STATE] = boot_state;
    memset(sram, 0, SRAM_SIZE);
    fake_adt_reset(&places);
    now = started_at = 1000000;
    restart_at = power_off_at = ended_at = 0;
    stray = 0;
    log_len = 0;
    log_buf[0] = 0;
    printed_lines = 0;

    /* src/main.c: m1n1 finds the watchdog when it starts */
    wdt_disable();
    n_evs = 0;
    started_at = now;
}

static enum outcome run(void (*fn)(void))
{
    int o = setjmp(machine);
    if (o) {
        restart_at = power_off_at = 0;
        return o;
    }
    fn();
    ended_at = now;
    return RETURNED;
}

/* How long after the call */
static u64 took(void)
{
    return ended_at - started_at;
}

static void check_outcome(enum outcome got, enum outcome want)
{
    checks++;
    if (got != want) {
        failures++;
        say("FAIL [%s]: the machine %s; it should have %s\n", scenario, outcome_names[got],
            outcome_names[want]);
    }
    checks++;
    if (stray) {
        failures++;
        say("FAIL [%s]: %d accesses that should not be, the first: %s\n", scenario, stray,
            stray_what);
    }
}

/* The SMC was not touched: not woken, no key */
static void check_no_smc(void)
{
    CHECK_EQ(count(EV_SMC_WAKE), 0);
    CHECK_EQ(count(EV_KEY), 0);
}

/* The PMU's scratchpad: only bytes 1 and 15 ever written */
static void check_only_two_bytes(void)
{
    for (int i = 0; i < n_evs; i++)
        if (evs[i].kind == EV_PMU_W)
            CHECK(evs[i].a == BOOT_STAGE || evs[i].a == BOOT_STATE);
}

/* The watchdog: only the three registers of its second timer, as wdt_reboot() has them */
static void check_wdt_rebooted(void)
{
    int at = -1;

    for (int i = 0; i < n_evs; i++)
        if (evs[i].kind == EV_WDT_W) {
            CHECK(evs[i].a == 0x10 || evs[i].a == 0x14 || evs[i].a == 0x1c);
            at = at < 0 ? i : at;
        }
    CHECK_EQ(count(EV_WDT_W), 3);
    CHECK(at >= 0 && evs[at].a == 0x14 && evs[at].b == 0x100000 && evs[at + 1].a == 0x10 &&
          evs[at + 1].b == 0 && evs[at + 2].a == 0x1c && evs[at + 2].b == 4);
}

/* ---- powering off ---- */

/* Powered off as it should: offw, the PMU, off1, and nothing else */
static void check_powered_off(enum outcome o, u8 before)
{
    int offw = find_key(MBSE_OFFW), off1 = find_key(MBSE_OFF1);
    int stage = find(EV_PMU_W, BOOT_STAGE), state = find(EV_PMU_W, BOOT_STATE);
    int read = find(EV_PMU_R, BOOT_STATE);

    check_outcome(o, POWERED_OFF);
    CHECK_EQ(count(EV_KEY), 2);
    CHECK(offw >= 0 && off1 > offw);
    CHECK(stage > offw && read > offw && state > read && off1 > stage && off1 > state);
    CHECK_EQ(count(EV_PMU_W), 2);
    CHECK_EQ(pmu_mem[BOOT_STAGE], 0);
    /* As macOS leaves it: the flag set, bits 0, 1, 2 and 5 as they were, the rest clear */
    CHECK_EQ(pmu_mem[BOOT_STATE], (before & 0x27) | SHUTDOWN);
    CHECK_EQ(pmu_mem[SCRATCH + 2], 0x11);
    CHECK_EQ(count(EV_WDT_W), 0);
    check_only_two_bytes();
}

/* Did not power off, and did not write off1 or restart instead */
static void check_not_off(enum outcome o)
{
    check_outcome(o, RETURNED);
    CHECK_EQ(find_key(MBSE_OFF1), -1);
    CHECK_EQ(count(EV_WDT_W), 0);
    check_only_two_bytes();
}

static void off_tests(void)
{
    static const u8 states[] = {0xda, 0x00, 0xff, 0x08, 0x27, 0xd0};
    enum outcome o;

    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        reset("off", states[i]);
        o = run(hv_power_off);
        check_powered_off(o, states[i]);
        /* Woken, handshaken and told within a fraction of a second, and off 20 ms after off1 */
        CHECK(took() < 100000);
        CHECK(find_key(MBSE_OFF1) >= 0 && ended_at == evs[find_key(MBSE_OFF1)].t + 20000);
    }

    /* What iBoot or something before it left in the mailbox: an old HELLO, a log, a reply */
    reset("off, messages left in the mailbox", 0xda);
    to_ap(0, mgmt(T_HELLO, 12UL << 16 | 11));
    to_ap(2, mgmt(5, 3));
    to_ap(SMC_EP, notification());
    to_ap(SMC_EP, 0x7000);
    for (int i = smc.q_head; i < smc.q_tail; i++)
        smc.q[i].ready = 0;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(printed("dropped 4 messages"));

    /*
     * What comes after the wake-up and before HELLO: the power state the SMC is in, which is
     * kept, as Linux keeps it, and is not asked again; anything else is passed over
     */
    reset("off, the power state before HELLO", 0xda);
    smc.ack_before_hello = true;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(!printed("dropped"));

    reset("off, a notification before HELLO", 0xda);
    smc.notify_before_hello = true;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(printed("dropped 1 messages"));

    reset("off, a management message of no use before HELLO", 0xda);
    smc.odd_before_hello = true;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(printed("dropped 1 messages"));

    /* The power state in the middle of the endpoint map: kept too */
    reset("off, the power state during the endpoint map", 0xda);
    smc.ack_during_epmap = true;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(!printed("dropped"));

    /* An SMC that has notifications on: one before the address of the shared memory */
    reset("off, a notification before the shared memory", 0xda);
    smc.notify_before_init = true;
    check_powered_off(run(hv_power_off), 0xda);
    CHECK(printed("SMC: 1 notifications passed over\n"));

    reset("off, a notification before every answer", 0x00);
    smc.notify_always = true;
    check_powered_off(run(hv_power_off), 0x00);

    /* The machine takes its time to go off: the code still waits */
    reset("off, half a second until it is off", 0xda);
    smc.off_delay = 500000;
    check_powered_off(run(hv_power_off), 0xda);

    /* The coprocessor does not answer: nothing else is touched, and the call does not hang */
    reset("off, no HELLO", 0xda);
    smc.no_hello = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY), 0);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 1000000 && took() < 2100000);
    CHECK(printed("the SMC cannot be used"));

    reset("off, never says it is on", 0xda);
    smc.no_power_ack = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 1000000 && took() < 2100000);

    reset("off, no answer to SMC_INITIALIZE", 0xda);
    smc.no_init_reply = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 500000 && took() < 600000);

    reset("off, nothing can be sent to it", 0xda);
    smc.a2i_full = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() < 1500000);

    /*
     * A mailbox that is never empty again, from before the wake-up, from the endpoint map on,
     * from when endpoint 0x20 is started, or from after offw: every wait still ends in its
     * time, and what is passed over is said in a line, not one for each message (the
     * console's 8 KB would have nothing else left)
     */
    reset("off, a mailbox that is never empty", 0xda);
    smc.flood = FLOOD_ALWAYS;
    smc.flooding = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 2000000 && took() < 3100000);
    CHECK(printed("did not receive HELLO") && printed("dropped"));
    CHECK(printed_lines < 30);

    reset("off, a mailbox never empty from the endpoint map on", 0xda);
    smc.flood = FLOOD_AFTER_EPMAP;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 1000000 && took() < 1200000);
    CHECK(printed("did not say that it is on") && printed("dropped"));
    CHECK(printed_lines < 30);

    reset("off, a mailbox never empty from when endpoint 0x20 is started", 0xda);
    smc.flood = FLOOD_AFTER_START;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 500000 && took() < 700000);
    CHECK(printed("SMC: no answer within 500 ms") && printed("notifications passed over"));
    CHECK(printed_lines < 30);

    reset("off, a mailbox never empty from offw on", 0xda);
    smc.flood = FLOOD_AFTER_KEY;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_KEY), 1);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 500000 && took() < 700000);
    CHECK(printed("did not take MBSE = offw") && printed("notifications passed over"));
    CHECK(printed_lines < 30);

    reset("off, the SMC crashes while it starts", 0xda);
    smc.crash_on_start = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() < 1500000);

    /* offw refused, or not answered: the PMU is not touched, off1 not written */
    reset("off, offw refused", 0xda);
    smc.fail_value = MBSE_OFFW;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(printed("did not take MBSE = offw"));

    reset("off, offw not answered", 0xda);
    smc.silent_value = MBSE_OFFW;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() >= 500000 && took() < 600000);

    /*
     * The shared memory the SMC gives: written to only if all of its 4 KB are in the SMC's own
     * memory (region-base and region-size of iop-smc-nub). Not elsewhere: memory of m1n1's here
     */
    static u8 elsewhere[0x2000] __attribute__((aligned(0x1000)));
    reset("off, the shared memory is not the SMC's", 0xda);
    smc.shmem_at = elsewhere;
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY) + count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(printed("is not in its memory"));
    for (size_t i = 0; i < sizeof(elsewhere); i++)
        if (elsewhere[i]) {
            CHECK(!"written where the SMC said its shared memory is");
            break;
        }

    reset("off, the shared memory at the start of the SMC's memory", 0xda);
    smc.shmem_at = sram;
    check_powered_off(run(hv_power_off), 0xda);

    reset("off, the shared memory in the last 4 KB of the SMC's memory", 0xda);
    smc.shmem_at = sram + SRAM_SIZE - 0x1000;
    check_powered_off(run(hv_power_off), 0xda);

    reset("off, the shared memory running past the end of the SMC's memory", 0xda);
    smc.shmem_at = sram + SRAM_SIZE - 0x800;
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY), 0);
    CHECK(printed("is not in its memory"));

    reset("off, the shared memory below the SMC's memory", 0xda);
    smc.shmem_at = sram - 0x1000;
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY), 0);

    reset("off, no region-base", 0xda);
    fake_adt_set(FAKE_SMC_NUB, "region-base", NULL, 0);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY), 0);
    CHECK(printed("no region-base and region-size"));

    reset("off, a region-size of 32 bits", 0xda);
    fake_adt_set(FAKE_SMC_NUB, "region-size", &(u32){SRAM_SIZE}, 4);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY), 0);

    reset("off, the SMC's memory smaller than the shared memory", 0xda);
    fake_adt_set(FAKE_SMC_NUB, "region-size", &(u64){0x800}, 8);
    smc.shmem_at = sram;
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_KEY), 0);

    /* The PMU: not there, not written, not read. off1 without the flag would restart */
    reset("off, no primary PMU", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "is-primary", NULL, 0);
    o = run(hv_power_off);
    check_not_off(o);
    CHECK(find_key(MBSE_OFFW) >= 0);
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(printed("0 primary PMUs"));

    /*
     * Which node is the primary PMU: is-primary, and compatible with a PMU. A second one that
     * says so leaves it unknown which is meant, and none is written to; a node of another
     * kind that says so is not one
     */
    reset("off, two primary PMUs", 0xda);
    fake_adt_set(FAKE_PMU_AUX, "is-primary", &(u32){1}, 4);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(printed("2 primary PMUs"));

    reset("off, a USB-C controller that says it is primary", 0xda);
    fake_adt_set(FAKE_HPM3, "is-primary", &(u32){1}, 4);
    check_powered_off(run(hv_power_off), 0xda);

    reset("off, a primary PMU of the d2422 kind", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", "pmu,d2422", sizeof("pmu,d2422"));
    check_powered_off(run(hv_power_off), 0xda);

    reset("off, a primary that is not a PMU", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", "pmu,other", sizeof("pmu,other"));
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(printed("0 primary PMUs"));

    reset("off, a primary whose compatible only starts like a PMU's", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", "pmu,spmix", sizeof("pmu,spmix"));
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a primary whose compatible has no end", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", "pmu,spmi", 8);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a primary without compatible", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "compatible", NULL, 0);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, is-primary 0", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "is-primary", &(u32){0}, 4);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a primary PMU on a bus that is not an SPMI bus", 0xda);
    fake_adt_nodes[FAKE_SPMI0].name = "i2c0";
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, no scratchpad", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "info-leg_scrpad", NULL, 0);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a scratchpad that is too long a property", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "info-leg_scrpad", (u32[]){0xf800, 0}, 8);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a scratchpad where byte 15 has no address", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "info-leg_scrpad", &(u32){0xfff8}, 4);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, a PMU address that is not one of SPMI", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "reg", (u32[]){0x1e}, 4);
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    reset("off, no reg", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "reg", NULL, 0);
    check_not_off(run(hv_power_off));

    /*
     * Byte 15 is put back only once it has been read: before, what it was is not known, and
     * nothing is said about it
     */
    reset("off, the PMU does not answer", 0xda);
    spmi.absent = true;
    o = run(hv_power_off);
    check_not_off(o);
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(took() < 1000000);
    CHECK(!printed("may still be set"));

    reset("off, byte 1 not written", 0xda);
    spmi.fail_write = BOOT_STAGE;
    check_not_off(run(hv_power_off));
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK_EQ(find(EV_PMU_W, BOOT_STATE), -1);
    CHECK(!printed("may still be set"));

    reset("off, byte 15 not read", 0xda);
    spmi.fail_read = BOOT_STATE;
    check_not_off(run(hv_power_off));
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK_EQ(find(EV_PMU_W, BOOT_STATE), -1);
    CHECK(!printed("may still be set"));

    /* Not written, and not put back either: whether the flag is set is not known */
    reset("off, byte 15 not written", 0xda);
    spmi.fail_write = BOOT_STATE;
    check_not_off(run(hv_power_off));
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(printed("the shutdown flag of the PMU may still be set"));

    reset("off, the SPMI controller has commands it has not sent", 0xda);
    spmi.tx_stuck = true;
    check_not_off(run(hv_power_off));
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);

    /*
     * off1: refused, not answered, or the machine stays on. Byte 15 had the flag when off1
     * was sent, and is put back as it was afterwards: left set, the next restart could be a
     * power off
     */
    reset("off, off1 refused", 0xda);
    smc.fail_value = MBSE_OFF1;
    o = run(hv_power_off);
    check_outcome(o, RETURNED);
    CHECK_EQ(count(EV_WDT_W), 0);
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(printed("did not take MBSE = off1"));
    CHECK(took() < 100000);
    CHECK_EQ(count(EV_PMU_W), 3);
    CHECK(find(EV_PMU_W, BOOT_STATE) >= 0 && evs[find(EV_PMU_W, BOOT_STATE)].b == 0x0a &&
          find(EV_PMU_W, BOOT_STATE) < find_key(MBSE_OFF1));
    CHECK(find_last(EV_PMU_W, BOOT_STATE) > find_key(MBSE_OFF1));
    CHECK(!printed("may still be set"));
    check_only_two_bytes();

    reset("off, off1 not answered", 0xda);
    smc.silent_value = MBSE_OFF1;
    o = run(hv_power_off);
    check_outcome(o, RETURNED);
    CHECK(took() >= 500000 && took() < 600000);
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(find_last(EV_PMU_W, BOOT_STATE) > find_key(MBSE_OFF1));

    reset("off, off1 answered and the machine stays on", 0xda);
    smc.off1_does_nothing = true;
    o = run(hv_power_off);
    check_outcome(o, RETURNED);
    CHECK_EQ(count(EV_WDT_W), 0);
    CHECK(took() >= 1000000 && took() < 1100000);
    CHECK(printed("still on 1000 ms after MBSE = off1"));
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    /* Not before the wait: the machine may yet go off */
    CHECK(find_last(EV_PMU_W, BOOT_STATE) >= 0 && find_key(MBSE_OFF1) >= 0 &&
          evs[find_last(EV_PMU_W, BOOT_STATE)].t >= evs[find_key(MBSE_OFF1)].t + 1000000);
    check_only_two_bytes();

    /* Set, and then cannot be put back: said, for whoever looks at the screen */
    reset("off, the shutdown flag cannot be put back", 0xda);
    smc.off1_does_nothing = true;
    spmi.fail_write = BOOT_STATE;
    spmi.fail_write_after = 1;
    o = run(hv_power_off);
    check_outcome(o, RETURNED);
    CHECK_EQ(pmu_mem[BOOT_STATE], 0x0a);
    CHECK(printed("the shutdown flag of the PMU may still be set"));
}

/* ---- restarting ---- */

static void check_restarted(enum outcome o, u8 before)
{
    int stage = find(EV_PMU_W, BOOT_STAGE), state = find(EV_PMU_W, BOOT_STATE);
    int wdt_first = find(EV_WDT_W, 0x14);

    check_outcome(o, RESTARTED);
    check_no_smc();
    check_wdt_rebooted();
    CHECK(stage >= 0 && state > stage && wdt_first > state);
    CHECK_EQ(pmu_mem[BOOT_STAGE], 0);
    /* The flag clear, the rest as it was */
    CHECK_EQ(pmu_mem[BOOT_STATE], before & ~SHUTDOWN);
    CHECK_EQ(count(EV_PMU_W), 2);
    check_only_two_bytes();
}

static void reset_tests(void)
{
    static const u8 states[] = {0xda, 0x00, 0xff, 0x08, 0xd2};
    enum outcome o;

    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        reset("restart", states[i]);
        o = run(hv_power_reset);
        check_restarted(o, states[i]);
        /* 0x100000 counts of 24 MHz after the watchdog was set: 43.7 ms */
        CHECK(took() > 43000 && took() < 45000);
    }

    /* The PMU in the way: the machine restarts all the same, as soon */
    reset("restart, no primary PMU", 0xda);
    fake_adt_set(FAKE_PMU_MAIN, "is-primary", NULL, 0);
    o = run(hv_power_reset);
    check_outcome(o, RESTARTED);
    check_no_smc();
    check_wdt_rebooted();
    CHECK_EQ(count(EV_PMU_W) + count(EV_PMU_R), 0);
    CHECK(took() < 45000);

    reset("restart, the PMU does not answer", 0xda);
    spmi.absent = true;
    o = run(hv_power_reset);
    check_outcome(o, RESTARTED);
    check_wdt_rebooted();
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);
    CHECK(took() < 100000);

    reset("restart, byte 1 not written", 0xda);
    spmi.fail_write = BOOT_STAGE;
    o = run(hv_power_reset);
    check_outcome(o, RESTARTED);
    check_wdt_rebooted();
    CHECK_EQ(pmu_mem[BOOT_STATE], 0xda);

    reset("restart, byte 15 not read", 0xda);
    spmi.fail_read = BOOT_STATE;
    o = run(hv_power_reset);
    check_outcome(o, RESTARTED);
    check_wdt_rebooted();
    CHECK_EQ(find(EV_PMU_W, BOOT_STATE), -1);
    CHECK_EQ(pmu_mem[BOOT_STAGE], 0);

    reset("restart, byte 15 not written", 0xda);
    spmi.fail_write = BOOT_STATE;
    o = run(hv_power_reset);
    check_outcome(o, RESTARTED);
    check_wdt_rebooted();

    /* The watchdog does not restart the machine: the call returns, after half a second */
    reset("restart, the watchdog does nothing", 0xda);
    wdt.dead = true;
    o = run(hv_power_reset);
    check_outcome(o, RETURNED);
    check_no_smc();
    check_wdt_rebooted();
    CHECK(took() >= 500000 && took() < 510000);
    CHECK(printed("has not restarted the machine within 500 ms"));

    /* m1n1 found no watchdog when it started: nothing is written, the call returns */
    reset("restart, no watchdog", 0xda);
    fake_adt_nodes[FAKE_WDT].name = "wdt-not-here";
    /* again, as at the start, which does not find it this time */
    extern void wdt_forget(void);
    wdt_forget();
    o = run(hv_power_reset);
    check_outcome(o, RETURNED);
    CHECK_EQ(count(EV_WDT_W), 0);
    CHECK(took() >= 500000 && took() < 510000);
}

/* A broken version of the code may write anywhere: that is a failure too */
static void crashed(int sig)
{
    say("FAIL [%s]: the test crashed (signal %d)\n", scenario, sig);
    exit(1);
}

int main(void)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    sram = mmap((void *)SRAM_BASE, SRAM_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (sram != (u8 *)SRAM_BASE) {
        say("power_test: no memory at 0x%lx for the SMC\n", SRAM_BASE);
        return 2;
    }
    signal(SIGSEGV, crashed);
    signal(SIGBUS, crashed);
    signal(SIGABRT, crashed);

    off_tests();
    reset_tests();

    say("power_test: %d checks, %d failed\n", checks, failures);
    if (failures)
        return 1;
    say("ALL OK\n");
    return 0;
}
