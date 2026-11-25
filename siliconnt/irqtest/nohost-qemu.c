// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Runs stoptest.bin on QEMU's "virt" machine under an EL2 of its own that is the real code of
 * the hypervisor for a guest it has to stop, with a host and without one (SiliconNT
 * docs/findings/0068, 0069): `make qemu-nohost`. Real, compiled for the target from the tree
 * with m1n1's headers: src/hv/hv_psci.c, src/hv/hv_stop.c and src/hv/hv_power.c, with
 * src/smc.c, src/rtkit.c, src/asc.c, src/spmi.c and src/wdt.c under them, src/iodev.c for the
 * consoles, src/vsprintf.c and src/string.c. This file stands in for the rest:
 *
 *  - _hv_exc_proxy() of hv_exc.c, as far as it goes before it would wait for the host: it
 *    asks hv_stop.c first; past that, the host is told, which here ends the run;
 *  - the exception entry: an hvc of the guest becomes the struct exc_info that hv_exc_sync()
 *    makes, for hv_psci_hvc();
 *  - the cores: this one, in the guest, and another that waits (src/smp.c, hv.c);
 *  - the consoles: the UART, which a host listens on or not (HOST), and the screen of
 *    fb_init_unbuffered() (src/fb.c, which has siliconnt/nohost-model/fb_test.c), which
 *    prints what it is given to the serial port of QEMU, every line after "SCREEN> ";
 *  - the device tree: siliconnt/nohost-model/fake_adt.c, with the SMC, the SPMI buses and the
 *    watchdog in memory that nothing answers in: it reads as all ones, as if no device were
 *    there. So on QEMU the SMC never takes a message and the PMU never answers, and the
 *    watchdog never goes off: power off and restart do not happen, and what shows is that
 *    they give up in time and the machine stops showing why. What they do with hardware
 *    that answers is siliconnt/nohost-model/power_test.c.
 *
 * What m1n1 prints goes to its consoles as it would (debug_printf() of src/utils.c), and also
 * to QEMU's serial port after "LOG> ", whether a console takes it or not.
 *
 * HOST: whether a host listens on the port of the proxy. STOP_ARG: what the guest does
 * (stoptest.S): 0 hvc #1, 1 SYSTEM_OFF, 2 SYSTEM_RESET, 3 CPU_ON of a CPU that is not there.
 * Without a host the core stays in EL2, and QEMU runs until the Makefile stops it.
 */

#include <stdarg.h>

#include "adt.h"
#include "dart.h"
#include "exception.h"
#include "fb.h"
#include "hv.h"
#include "iodev.h"
#include "iova.h"
#include "malloc.h"
#include "sart.h"
#include "smp.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"
#include "vsprintf.h"
#include "wdt.h"

#include "../nohost-model/fake_adt.h"

#ifndef HOST
#define HOST 0
#endif
#ifndef STOP_ARG
#define STOP_ARG 0
#endif

/* stoptest.S */
#define HDR_STOP_ARG  0x30
#define STOP_RETURNED 0x5155

#define PL011_BASE 0x09000000UL
/* Where the devices of the device tree are: memory of QEMU above this EL2 and the guest */
#define FAKE_SMC   0x43000000UL
#define FAKE_SPMI  0x43100000UL
#define FAKE_WDT   0x43110000UL
#define FAKE_WDT_2 0x43114000UL

/* xtrack-qemu-head.S */
struct frame {
    u64 regs[31];
    u64 elr;
};
u64 shim_smc(u64 x0, u64 x1, u64 x2, u64 x3);
extern u8 guest[], guest_end[];

/* ---- QEMU's serial port ---- */

static void shim_putc(char c)
{
    *(volatile u32 *)PL011_BASE = c;
}

static void shim_puts(const char *s)
{
    while (*s)
        shim_putc(*s++);
}

/* Text after a prefix at the start of each line; at_start says where the last one ended */
static void shim_lines(const char *prefix, bool *at_start, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (*at_start)
            shim_puts(prefix);
        shim_putc(s[i]);
        *at_start = s[i] == '\n';
    }
}

static void __attribute__((noreturn)) shim_off(void)
{
    shim_puts("nohost-qemu: end\n");
    shim_smc(0x84000008, 0, 0, 0); /* PSCI SYSTEM_OFF of QEMU */
    for (;;)
        ;
}

/* ---- what m1n1 has in src/utils.c ---- */

int debug_printf(const char *fmt, ...)
{
    static bool at_start = true;
    va_list args;
    char buffer[512];
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    shim_lines("LOG> ", &at_start, buffer, min(i, (int)(sizeof(buffer) - 1)));
    iodev_console_write(buffer, min(i, (int)(sizeof(buffer) - 1)));
    return i;
}

int snprintf(char *buffer, size_t size, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, size, fmt, args);
    va_end(args);
    return i;
}

void udelay(u32 d)
{
    u64 delay = ((u64)d) * mrs(CNTFRQ_EL0) / 1000000;
    u64 val = mrs(CNTPCT_EL0);
    while ((mrs(CNTPCT_EL0) - val) < delay)
        ;
    sysop("isb");
}

u64 timeout_calculate(u32 usec)
{
    u64 delay = ((u64)usec) * mrs(CNTFRQ_EL0) / 1000000;
    return mrs(CNTPCT_EL0) + delay;
}

bool timeout_expired(u64 timeout)
{
    bool expired = mrs(CNTPCT_EL0) > timeout;
    sysop("isb");
    return expired;
}

/* One core, and no MMU in this EL2: the consoles take no lock (mmu_active()) */
void spin_lock(spinlock_t *lock)
{
    UNUSED(lock);
}

void spin_unlock(spinlock_t *lock)
{
    UNUSED(lock);
}

int boot_cpu_idx = 0;
u64 boot_cpu_mpidr;

void flush_and_reboot(void)
{
    shim_puts("nohost-qemu: m1n1 restarts (flush_and_reboot)\n");
    shim_off();
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function)
{
    UNUSED(line);
    UNUSED(function);
    shim_puts("nohost-qemu: assertion failed: ");
    shim_puts(assertion);
    shim_puts(" in ");
    shim_puts(file);
    shim_puts("\n");
    flush_and_reboot();
}

/* A heap that is not given back: power off takes a few small things from it */
static u8 heap[0x40000] ALIGNED(0x4000);
static size_t heap_used;

void *memalign(size_t align, size_t size)
{
    size_t at = ALIGN_UP(heap_used, align);
    if (at + size > sizeof(heap)) {
        shim_puts("nohost-qemu: out of heap\n");
        shim_off();
    }
    heap_used = at + size;
    return heap + at;
}

void *malloc(size_t size)
{
    return memalign(16, size);
}

void *calloc(size_t n, size_t size)
{
    void *p = malloc(n * size);
    memset(p, 0, n * size);
    return p;
}

void free(void *p)
{
    UNUSED(p);
}

bool is_heap(void *addr)
{
    UNUSED(addr);
    return false;
}

/* rtkit.c is linked whole: what it would use for coprocessors behind an IOMMU */
#define NOT_HERE                                                                                   \
    {                                                                                              \
        shim_puts("nohost-qemu: an IOMMU function was called\n");                                  \
        shim_off();                                                                                \
    }
void *dart_translate(dart_dev_t *dart, uintptr_t iova) NOT_HERE;
int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len) NOT_HERE;
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len) NOT_HERE;
u64 iova_alloc(iova_domain_t *iovad, size_t sz) NOT_HERE;
void iova_free(iova_domain_t *iovad, u64 iova, size_t sz) NOT_HERE;
bool sart_add_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE;
bool sart_remove_allowed_region(sart_dev_t *sart, void *paddr, size_t sz) NOT_HERE;

void fake_adt_overrun(const char *name, u32 len, u32 room)
{
    UNUSED(len);
    UNUSED(room);
    shim_puts("nohost-qemu: m1n1 would copy past the end of where ");
    shim_puts(name);
    shim_puts(" goes\n");
    shim_off();
}

/* ---- the consoles ---- */

static bool always(void *opaque)
{
    UNUSED(opaque);
    return true;
}

static ssize_t uart_write(void *opaque, const void *buf, size_t len)
{
    static bool at_start = true;
    UNUSED(opaque);
    shim_lines("TTY> ", &at_start, buf, len);
    return len;
}

static const struct iodev_ops uart_ops = {.can_write = always, .write = uart_write};
static const struct iodev_ops no_ops = {0};

struct iodev iodev_uart = {.ops = &uart_ops};
struct iodev iodev_dockchannel_uart = {.ops = &no_ops};
struct iodev iodev_usb_vuart = {.ops = &no_ops};
struct iodev iodev_log = {.ops = &no_ops};

iodev_id_t uartproxy_iodev;

/* The screen, as src/fb.c has it, with QEMU's serial port for the pixels */
static bool screen_initialized, screen_active;

static bool screen_can_write(void *opaque)
{
    UNUSED(opaque);
    return screen_initialized && screen_active;
}

static ssize_t screen_write(void *opaque, const void *buf, size_t len)
{
    static bool at_start = true;
    UNUSED(opaque);
    shim_lines("SCREEN> ", &at_start, buf, len);
    return len;
}

static const struct iodev_ops screen_ops = {.can_write = screen_can_write, .write = screen_write};
struct iodev iodev_fb = {.ops = &screen_ops, .usage = USAGE_CONSOLE};

bool fb_init_unbuffered(void)
{
    printf("fb init: the screen of nohost-qemu\n");
    screen_initialized = true;
    screen_active = false;
    printf("fb console: max rows 57, max cols 78\n");
    return true;
}

void fb_set_active(bool active)
{
    screen_active = active;
    if (active)
        iodev_console_kick();
}

/* ---- the hypervisor ---- */

/* _hv_exc_proxy() of hv_exc.c: hv_stop.c first; then the host is told and leaves the guest */
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra)
{
    char line[96];

    UNUSED(extra);
    hv_stop_if_no_host(ctx, reason, type);
    snprintf(line, sizeof(line), "nohost-qemu: the host was told: reason %d, event %d\n", reason,
             type);
    shim_puts(line);
    shim_off();
}

bool hv_started_cpus[MAX_CPUS] = {true};

bool smp_is_alive(int cpu)
{
    return cpu == 1;
}

uint64_t smp_get_mpidr(int cpu)
{
    return cpu;
}

bool hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    UNUSED(cpu);
    UNUSED(entry);
    UNUSED(regs);
    shim_puts("nohost-qemu: hv_start_secondary() was called\n");
    shim_off();
    return false;
}

static const struct fake_adt_places places = {
    .smc = FAKE_SMC,
    .smc_size = 0x88000,
    .spmi_a1 = FAKE_SPMI,
    .spmi0 = FAKE_SPMI + 0x4000,
    .spmi1 = FAKE_SPMI + 0x8000,
    .wdt = FAKE_WDT,
    .wdt_second = FAKE_WDT_2,
};

u64 shim_main(void)
{
    shim_puts(HOST ? "nohost-qemu: start, with a host\n" : "nohost-qemu: start, without a host\n");

    boot_cpu_mpidr = mrs(MPIDR_EL1);

    /* Devices that are not there */
    memset((void *)FAKE_SMC, 0xff, 0x88000);
    memset((void *)FAKE_SPMI, 0xff, 0xc000);
    memset((void *)FAKE_WDT, 0, 0x8000);
    fake_adt_reset(&places);

    /*
     * The port of the proxy: the UART, as m1n1 leaves it when it starts the guest itself, or
     * with a host listening on it
     */
    uartproxy_iodev = IODEV_UART;
    iodev_set_usage(IODEV_UART, HOST ? USAGE_CONSOLE | USAGE_UARTPROXY : 0);
    iodev_set_usage(IODEV_FB, 0);

    /* src/main.c: m1n1 finds the watchdog when it starts */
    wdt_disable();

    /* A 64-bit guest; its hvc comes here (HCR_EL2.HCD is 0) */
    msr(HCR_EL2, BIT(31));
    sysop("isb");

    *(volatile u64 *)(guest + HDR_STOP_ARG) = STOP_ARG;
    return (u64)guest;
}

/* What hv_exc_sync() does with an hvc of the guest */
void shim_sync(struct frame *f)
{
    u64 esr = mrs(ESR_EL2);
    struct exc_info ctx;

    if ((esr >> 26) != 0x16) {
        shim_puts("nohost-qemu: an exception of the guest that is not an hvc\n");
        shim_off();
    }
    if ((esr & 0xffff) == STOP_RETURNED) {
        shim_puts("nohost-qemu: the guest went on after the call\n");
        shim_off();
    }

    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.regs, f->regs, sizeof(f->regs));
    ctx.elr = f->elr;
    ctx.elr_phys = f->elr; /* the guest's MMU is off */
    ctx.spsr = mrs(SPSR_EL2);
    ctx.esr = esr;
    ctx.far = mrs(FAR_EL2);
    ctx.sp[0] = mrs(SP_EL0);
    ctx.sp[1] = mrs(SP_EL1);
    ctx.mpidr = mrs(MPIDR_EL1);
    if (!hv_psci_hvc(&ctx, esr & 0xffff)) {
        shim_puts("nohost-qemu: hv_psci_hvc() did not take the hvc\n");
        shim_off();
    }
    f->regs[0] = ctx.regs[0];
}

void shim_irq(struct frame *f)
{
    UNUSED(f);
    shim_puts("nohost-qemu: an interrupt\n");
    shim_off();
}

void shim_bad(u64 vector)
{
    UNUSED(vector);
    shim_puts("nohost-qemu: an exception of this EL2\n");
    shim_off();
}
