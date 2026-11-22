/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * What the hypervisor does when it has to stop its guest and there is no host to tell
 * (SiliconNT docs/DESIGN.md, "停下客户机"; docs/findings/0068 and 0069).
 *
 * Everything that stops the guest for good ends up in uartproxy_run(), which tells the host
 * why and waits for its answer: _hv_exc_proxy() of hv_exc.c, for the guest's exceptions and
 * PSCI calls and for the hypervisor's own events, and hv_do_panic() of hv_wdt.c. Without a
 * host that wait never ends. Both call hv_stop_if_no_host() first. With a host it returns,
 * and everything goes on as before; without one the guest does not run again:
 *
 *  - PSCI SYSTEM_RESET restarts the machine, and SYSTEM_OFF powers it off (hv_power.c);
 *  - everything else, and a restart or power off that did not happen, leaves the machine
 *    showing what m1n1 printed last and the registers of the guest on the screen, with every
 *    core in EL2, until its owner holds the power button. It does not restart by itself:
 *    what stops a guest once usually stops it every time, and the machine would restart over
 *    and over with nobody seeing why.
 *
 * m1n1 failing itself (an exception in EL2, a failed assertion) does not come here: m1n1
 * restarts then, host or not, as upstream has it.
 *
 * Tested on the build machine by siliconnt/nohost-model (stop_test.c), and on QEMU by
 * siliconnt/irqtest (nohost-qemu.c).
 */

#include "hv.h"
#include "fb.h"
#include "iodev.h"
#include "uartproxy.h"
#include "utils.h"

/*
 * A host listens on the port the proxy talks over when it has loaded the guest, with its own
 * loader or with P_SELFBOOT_RUN (src/proxy.c). Not when m1n1 has started the guest by itself,
 * and not when the host said with P_SELFBOOT_RUN that it is not staying: selfboot_enter() of
 * src/selfboot_run.c takes the proxy's usage off every port then, and uartproxy_iodev is the
 * UART, or the USB port the command came over.
 *
 * So it is settled before the guest runs, and plugging the cable in or out afterwards changes
 * nothing. hv_tick() asks the same, for whether a byte on that port is a host that wants the
 * guest stopped.
 */
bool hv_has_host(void)
{
    return iodev_get_usage(uartproxy_iodev) & USAGE_UARTPROXY;
}

/* What the host would have been told, in words */
static const char *hv_stop_what(uartproxy_boot_reason_t reason, u32 type)
{
    if (reason == START_EXCEPTION_LOWER) {
        switch (type) {
            case EXC_SYNC:
                return "a synchronous exception of the guest that the hypervisor did not handle";
            case EXC_IRQ:
                return "an interrupt that the hypervisor did not handle";
            case EXC_FIQ:
                return "a FIQ that the hypervisor did not handle";
            case EXC_SERROR:
                return "an SError while the guest ran";
        }
    } else if (reason == START_HV) {
        switch (type) {
            case HV_GUEST_RESET:
                return "the guest called PSCI SYSTEM_RESET";
            case HV_GUEST_OFF:
                return "the guest called PSCI SYSTEM_OFF";
            case HV_GUEST_HVC:
                return "the guest made a call that the hypervisor cannot carry out";
            case HV_PANIC:
                return "the hypervisor cannot go on";
        }
    }
    return "an event for a host";
}

/*
 * The screen, which the guest has had, is m1n1's console again, and what m1n1 printed last
 * comes back on it: the console keeps the last 8 KB in con_buf (src/iodev.c), also while no
 * console takes them. The reason for stopping is among them: it was printed where the guest
 * was stopped, before this.
 */
static void hv_stop_show(void)
{
    if (!fb_init_unbuffered()) {
        printf("HV: there is no screen to show this on\n");
        return;
    }
    iodev_reset_rp(IODEV_FB);
    iodev_set_usage(IODEV_FB, USAGE_CONSOLE);
    fb_set_active(true);
}

/* At most 77 characters to a line: the console of the M4 has 78 columns */
static void hv_stop_print(const struct exc_info *ctx)
{
    if (ctx) {
        printf("HV: the guest's registers, on CPU %ld (MPIDR 0x%lx):\n", ctx->cpu_id, ctx->mpidr);
        for (int i = 0; i < 28; i += 4)
            printf("x%d-x%d: %016lx %016lx %016lx %016lx\n", i, i + 3, ctx->regs[i],
                   ctx->regs[i + 1], ctx->regs[i + 2], ctx->regs[i + 3]);
        printf("x28-x30: %016lx %016lx %016lx\n", ctx->regs[28], ctx->regs[29], ctx->regs[30]);
        printf("PC   0x%016lx (0x%lx)  SPSR 0x%lx\n", ctx->elr, ctx->elr_phys, ctx->spsr);
        printf("FAR  0x%016lx (0x%lx)  ESR  0x%lx\n", ctx->far, ctx->far_phys, ctx->esr);
        printf("SP_EL0 0x%016lx  SP_EL1 0x%016lx\n", ctx->sp[0], ctx->sp[1]);
    }
    printf("HV: the guest has stopped. Hold the power button to turn the machine off.\n");
}

static void __attribute__((noreturn)) hv_stop_halt(void)
{
    /* Not even an SError is taken from here on: m1n1 restarts on one (exc_serr()) */
    sysop("msr daifset, #0xf");
    for (;;)
        sysop("wfe");
}

void hv_stop_if_no_host(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type)
{
    if (hv_has_host())
        return;

    printf("HV: %s\n", hv_stop_what(reason, type));
    printf("HV: there is no host: the guest does not run again\n");
    /* Each returns only if the machine is still running */
    if (reason == START_HV && type == HV_GUEST_RESET)
        hv_power_reset();
    else if (reason == START_HV && type == HV_GUEST_OFF)
        hv_power_off();

    hv_stop_show();
    hv_stop_print(ctx);
    hv_stop_halt();
}
