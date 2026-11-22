// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for src/hv/hv_stop.c: what the hypervisor does when it has to stop its
 * guest, with a host and without one (SiliconNT docs/findings/0068 and 0069). The file under
 * test is the real one, with the real src/iodev.c under it for the consoles and their ring
 * buffer. This file plays
 *
 *  - the ports: the UART and the USB ports, with or without the proxy's usage, as the three
 *    ways a guest is started leave them;
 *  - the screen: fb_init_unbuffered() and fb_set_active() of src/fb.c, and a console that
 *    keeps the text it is given (fb.c itself has fb_test.c);
 *  - hv_power_reset() and hv_power_off() of hv_power.c (which has power_test.c): the machine
 *    restarts or powers off, or they return;
 *  - the core: "msr daifset" and "wfe", the last of which is where the core stays.
 *
 * printf() is m1n1's: into the ring buffer of iodev.c, and to the consoles that are there.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"
#include "hv.h"
#include "iodev.h"
#include "uartproxy.h"
#include "utils.h"

static int checks, failures;
static const char *scenario = "";

#undef printf
#define say(...) fprintf(stdout, __VA_ARGS__)

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            say("FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, scenario, #cond);                     \
        }                                                                                          \
    } while (0)

/* ---- the core ---- */

enum end { RETURNED, HALTED, RESTARTED, POWERED_OFF, M1N1_RESTARTED };
static const char *const end_names[] = {"returned", "halted", "restarted", "powered off",
                                        "m1n1 restarted"};

static jmp_buf core;
static bool masked;
/* Calls of hv_power_*() made with everything masked, and screens set up without */
static int power_masked, screen_unmasked;
static int wfes;

void model_sysop(const char *op)
{
    if (!strcmp(op, "msr daifset, #0xf")) {
        masked = true;
    } else if (!strcmp(op, "wfe")) {
        /* The core waits here; the test has seen enough of it after a few */
        if (++wfes == 3)
            longjmp(core, HALTED);
    }
}

void flush_and_reboot(void)
{
    longjmp(core, M1N1_RESTARTED);
}

void spin_lock(spinlock_t *lock)
{
    UNUSED(lock);
}

void spin_unlock(spinlock_t *lock)
{
    UNUSED(lock);
}

/* No device registers here */
u32 read32(u64 addr)
{
    UNUSED(addr);
    return 0;
}
void write32(u64 addr, u32 data)
{
    UNUSED(addr);
    UNUSED(data);
}
u64 read64(u64 addr)
{
    UNUSED(addr);
    return 0;
}
void write64(u64 addr, u64 data)
{
    UNUSED(addr);
    UNUSED(data);
}

/* ---- printing, as src/utils.c does it ---- */

int debug_printf(const char *fmt, ...)
{
    va_list args;
    char buffer[512];
    int i;

    va_start(args, fmt);
    i = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    iodev_console_write(buffer, min(i, (int)(sizeof(buffer) - 1)));
    return i;
}

/* ---- the ports ---- */

struct port {
    char text[64 * 1024];
    size_t len;
};

static struct port uart_out, usb_out, screen;

static ssize_t port_write(struct port *p, const void *buf, size_t len)
{
    size_t n = min(len, sizeof(p->text) - 1 - p->len);
    memcpy(p->text + p->len, buf, n);
    p->len += n;
    p->text[p->len] = 0;
    return len;
}

static bool can_write(void *opaque)
{
    UNUSED(opaque);
    return true;
}

static ssize_t uart_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    return port_write(&uart_out, buf, len);
}

static ssize_t usb_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    return port_write(&usb_out, buf, len);
}

static const struct iodev_ops uart_ops = {.can_write = can_write, .write = uart_write};
static const struct iodev_ops usb_ops = {.can_write = can_write, .write = usb_write};
static const struct iodev_ops none_ops = {0};

struct iodev iodev_uart = {.ops = &uart_ops};
struct iodev iodev_dockchannel_uart = {.ops = &none_ops};
struct iodev iodev_usb_vuart = {.ops = &none_ops};
struct iodev iodev_log = {.ops = &none_ops};
static struct iodev usb0 = {.ops = &usb_ops};

iodev_id_t uartproxy_iodev;

/* ---- the screen, as fb.c has it ---- */

static bool screen_there, screen_initialized, screen_active;
static int screen_inits;

static bool fb_can_write(void *opaque)
{
    UNUSED(opaque);
    return screen_initialized && screen_active;
}

static ssize_t fb_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    return port_write(&screen, buf, len);
}

static const struct iodev_ops fb_ops = {.can_write = fb_can_write, .write = fb_write};
struct iodev iodev_fb = {.ops = &fb_ops, .usage = USAGE_CONSOLE};

bool fb_init_unbuffered(void)
{
    screen_inits++;
    screen_unmasked += !masked;
    if (!screen_there)
        return false;
    /* What fb_init() prints, before the console is there */
    debug_printf("fb init: 3024x1964 (30) [s=3024] @0x103e004c000\n");
    screen_initialized = true;
    screen_active = false;
    screen.len = 0;
    screen.text[0] = 0;
    debug_printf("fb console: max rows 57, max cols 78\n");
    return true;
}

void fb_set_active(bool active)
{
    screen_active = active;
    if (active)
        iodev_console_kick();
}

/* ---- restarting and powering off (hv_power.c) ---- */

static bool power_works;
static int resets, offs;

void hv_power_reset(void)
{
    resets++;
    power_masked += masked;
    if (power_works)
        longjmp(core, RESTARTED);
    debug_printf("HV: the watchdog has not restarted the machine within 500 ms\n");
}

void hv_power_off(void)
{
    offs++;
    power_masked += masked;
    if (power_works)
        longjmp(core, POWERED_OFF);
    debug_printf("HV: power: the SMC cannot be used\n");
}

/* ---- a case ---- */

/* What the caller prints before it stops the guest: different in each case */
static char why[64];
static int case_no;

enum how { HOST_USB, HOST_UART, NO_HOST_SELF, NO_HOST_SAID };

/* The ports as each way of starting the guest leaves them (src/selfboot_run.c, uartproxy.c) */
static void start(const char *name, enum how how)
{
    scenario = name;
    snprintf(why, sizeof(why), "HV: the caller says why, case %d\n", ++case_no);
    iodev_register_device(IODEV_USB0, &usb0);
    for (int i = 0; i < IODEV_NUM; i++)
        iodev_set_usage(i, 0);
    switch (how) {
        case HOST_USB: /* a host's loader, or P_SELFBOOT_RUN: the port the command came over */
            uartproxy_iodev = IODEV_USB0;
            iodev_set_usage(IODEV_USB0, USAGE_CONSOLE | USAGE_UARTPROXY);
            break;
        case HOST_UART:
            uartproxy_iodev = IODEV_UART;
            iodev_set_usage(IODEV_UART, USAGE_CONSOLE | USAGE_UARTPROXY);
            break;
        case NO_HOST_SELF: /* m1n1 started the guest by itself: the proxy never ran */
            uartproxy_iodev = IODEV_UART;
            break;
        case NO_HOST_SAID: /* P_SELFBOOT_RUN with no host: selfboot_enter() took the usage off */
            uartproxy_iodev = IODEV_USB0;
            break;
    }
    uart_out.len = usb_out.len = screen.len = 0;
    uart_out.text[0] = usb_out.text[0] = screen.text[0] = 0;
    screen_there = true;
    screen_initialized = screen_active = false;
    screen_inits = 0;
    power_works = false;
    resets = offs = 0;
    masked = false;
    power_masked = screen_unmasked = 0;
    wfes = 0;
}

static struct exc_info ctx;

static void make_ctx(void)
{
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < 31; i++)
        ctx.regs[i] = 0x1000000000000000UL * (i % 16) + 0x111111 * i;
    ctx.elr = 0xfffff80012345678UL;
    ctx.elr_phys = 0x10012345678UL;
    ctx.spsr = 0x3c5;
    ctx.esr = 0x5a000001;
    ctx.far = 0xfffff8009abcdef0UL;
    ctx.far_phys = 0x1009abcdef0UL;
    ctx.sp[0] = 0x7ffe0000;
    ctx.sp[1] = 0xfffff80000123000UL;
    ctx.cpu_id = 0;
    ctx.mpidr = 0x80000000;
}

static enum end stop(struct exc_info *c, uartproxy_boot_reason_t reason, u32 type)
{
    int e = setjmp(core);
    if (e)
        return e;
    hv_stop_if_no_host(c, reason, type);
    return RETURNED;
}

static void check_end(enum end got, enum end want)
{
    checks++;
    if (got != want) {
        failures++;
        say("FAIL [%s]: %s; it should have %s\n", scenario, end_names[got], end_names[want]);
    }
}

static bool on_screen(const char *text)
{
    return strstr(screen.text, text) != NULL;
}

/* Where the case's own line is on the screen: what is before it is from earlier cases */
static const char *this_case(void)
{
    const char *p = strstr(screen.text, why), *next;

    while (p && (next = strstr(p + 1, why)))
        p = next;
    return p;
}

static bool after_why(const char *text)
{
    const char *p = this_case();
    return p && strstr(p, text);
}

/* Every line from the case's own on is short enough for the M4's 78 columns */
static bool lines_fit(void)
{
    const char *p = this_case();
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 77)
            return false;
        p = nl ? nl + 1 : NULL;
    }
    return true;
}

/* ---- with a host ---- */

static const struct {
    uartproxy_boot_reason_t reason;
    u32 type;
    const char *words;
} events[] = {
    {START_EXCEPTION_LOWER, EXC_SYNC,
     "a synchronous exception of the guest that the hypervisor did not handle"},
    {START_EXCEPTION_LOWER, EXC_IRQ, "an interrupt that the hypervisor did not handle"},
    {START_EXCEPTION_LOWER, EXC_FIQ, "a FIQ that the hypervisor did not handle"},
    {START_EXCEPTION_LOWER, EXC_SERROR, "an SError while the guest ran"},
    {START_HV, HV_HOOK_VM, "an event for a host"},
    {START_HV, HV_VTIMER, "an event for a host"},
    {START_HV, HV_USER_INTERRUPT, "an event for a host"},
    {START_HV, HV_WDT_BARK, "an event for a host"},
    {START_HV, HV_CPU_SWITCH, "an event for a host"},
    {START_HV, HV_VIRTIO, "an event for a host"},
    {START_HV, HV_PANIC, "the hypervisor cannot go on"},
    {START_HV, HV_GUEST_RESET, "the guest called PSCI SYSTEM_RESET"},
    {START_HV, HV_GUEST_OFF, "the guest called PSCI SYSTEM_OFF"},
    {START_HV, HV_GUEST_HVC, "the guest made a call that the hypervisor cannot carry out"},
    /* Not events there are, but what they would be is not a restart or a power off */
    {START_EXCEPTION_LOWER, HV_GUEST_RESET, "an event for a host"},
    {START_EXCEPTION_LOWER, HV_GUEST_OFF, "an event for a host"},
    {START_EXCEPTION, HV_GUEST_OFF, "an event for a host"},
};

static void host_tests(void)
{
    for (enum how how = HOST_USB; how <= HOST_UART; how++)
        for (size_t i = 0; i < ARRAY_SIZE(events); i++) {
            start(how == HOST_USB ? "a host on a USB port" : "a host on the UART", how);
            CHECK(hv_has_host());
            make_ctx();
            debug_printf("%s", why);
            size_t printed = usb_out.len + uart_out.len;
            check_end(stop(&ctx, events[i].reason, events[i].type), RETURNED);
            /* Nothing done, nothing printed: the host is told in hv_exc.c */
            CHECK(resets == 0 && offs == 0 && screen_inits == 0 && !masked && wfes == 0);
            CHECK(usb_out.len + uart_out.len == printed);
        }

    /* A port with the proxy's usage is a host; a console alone, or nothing, is not */
    start("which ports are a host", NO_HOST_SAID);
    iodev_set_usage(IODEV_USB0, USAGE_CONSOLE);
    CHECK(!hv_has_host());
    iodev_set_usage(IODEV_USB0, USAGE_UARTPROXY);
    CHECK(hv_has_host());
    uartproxy_iodev = IODEV_UART;
    CHECK(!hv_has_host());
    iodev_set_usage(IODEV_UART, USAGE_CONSOLE);
    CHECK(!hv_has_host());
}

/* ---- without a host ---- */

static void check_shown(enum how how, const char *words, bool regs)
{
    char line[128];

    /* The screen, once, and what was printed before the stop on it, before what follows */
    CHECK(screen_inits == 1 && screen_initialized && screen_active);
    CHECK(this_case() != NULL);
    snprintf(line, sizeof(line), "\nHV: %s\nHV: there is no host: the guest does not run again\n",
             words);
    CHECK(after_why(line));
    if (regs) {
        CHECK(after_why("HV: the guest's registers, on CPU 0 (MPIDR 0x80000000):\n"));
        CHECK(after_why("x0-x3: 0000000000000000 1000000000111111 2000000000222222 "
                        "3000000000333333\n"));
        CHECK(after_why("x28-x30: c000000001dddddc d000000001eeeeed e000000001fffffe\n"));
        CHECK(after_why("PC   0xfffff80012345678 (0x10012345678)  SPSR 0x3c5\n"));
        CHECK(after_why("FAR  0xfffff8009abcdef0 (0x1009abcdef0)  ESR  0x5a000001\n"));
        CHECK(after_why("SP_EL0 0x000000007ffe0000  SP_EL1 0xfffff80000123000\n"));
    } else {
        CHECK(!after_why("HV: the guest's registers"));
    }
    CHECK(lines_fit());
    /* Last, and then the core stays, with nothing to take it away */
    const char *last =
        "HV: the guest has stopped. Hold the power button to turn the machine off.\n";
    CHECK(screen.len >= strlen(last) && !strcmp(screen.text + screen.len - strlen(last), last));
    CHECK(masked && wfes == 3);
    /* Masked before the screen was set up, and not before a restart or power off was tried */
    CHECK(screen_unmasked == 0 && power_masked == 0);
    /* The ports nobody listens on got nothing */
    CHECK(uart_out.len == 0 && usb_out.len == 0);
    UNUSED(how);
}

static void no_host_tests(void)
{
    for (enum how how = NO_HOST_SELF; how <= NO_HOST_SAID; how++)
        for (size_t i = 0; i < ARRAY_SIZE(events); i++) {
            bool reset = events[i].reason == START_HV && events[i].type == HV_GUEST_RESET;
            bool off = events[i].reason == START_HV && events[i].type == HV_GUEST_OFF;

            start(how == NO_HOST_SELF ? "no host, m1n1 started the guest"
                                      : "no host, the host said it is not staying",
                  how);
            CHECK(!hv_has_host());
            make_ctx();
            debug_printf("%s", why);
            check_end(stop(&ctx, events[i].reason, events[i].type), HALTED);
            CHECK(resets == reset && offs == off);
            check_shown(how, events[i].words, true);
            if (reset)
                CHECK(on_screen("has not restarted the machine within 500 ms\n"));
            if (off)
                CHECK(on_screen("HV: power: the SMC cannot be used\n"));
        }

    /* The machine restarts, or powers off: nothing is shown */
    start("no host, the machine restarts", NO_HOST_SELF);
    power_works = true;
    make_ctx();
    check_end(stop(&ctx, START_HV, HV_GUEST_RESET), RESTARTED);
    CHECK(resets == 1 && offs == 0 && screen_inits == 0 && power_masked == 0);

    start("no host, the machine powers off", NO_HOST_SAID);
    power_works = true;
    make_ctx();
    check_end(stop(&ctx, START_HV, HV_GUEST_OFF), POWERED_OFF);
    CHECK(resets == 0 && offs == 1 && screen_inits == 0 && power_masked == 0);

    /* hv_do_panic(): no registers to show */
    start("no host, the hypervisor cannot go on", NO_HOST_SELF);
    debug_printf("%s", why);
    check_end(stop(NULL, START_HV, HV_PANIC), HALTED);
    check_shown(NO_HOST_SELF, "the hypervisor cannot go on", false);

    /* No screen: the core stays all the same */
    start("no host, no screen", NO_HOST_SELF);
    screen_there = false;
    make_ctx();
    check_end(stop(&ctx, START_HV, HV_GUEST_HVC), HALTED);
    CHECK(screen_inits == 1 && screen.len == 0 && masked && wfes == 3 && !screen_unmasked);

    /*
     * More than the ring buffer holds before the stop: the screen starts with what it still
     * has (the last 8 KB), and has all of what came after
     */
    start("no host, a lot printed before", NO_HOST_SAID);
    for (int i = 0; i < 400; i++)
        debug_printf("HV: line %03d of what was printed before the stop\n", i);
    debug_printf("%s", why);
    make_ctx();
    check_end(stop(&ctx, START_EXCEPTION_LOWER, EXC_SYNC), HALTED);
    check_shown(NO_HOST_SAID,
                "a synchronous exception of the guest that the hypervisor did not "
                "handle",
                true);
    CHECK(!on_screen("HV: line 000 of"));
    CHECK(on_screen("HV: line 300 of what was printed before the stop\n"));
    CHECK(on_screen("HV: line 399 of what was printed before the stop\n"));
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    host_tests();
    no_host_tests();

    say("stop_test: %d checks, %d failed\n", checks, failures);
    if (failures)
        return 1;
    say("ALL OK\n");
    return 0;
}
