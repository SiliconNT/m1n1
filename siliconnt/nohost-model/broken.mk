# SPDX-License-Identifier: GPL-2.0-or-later
#
# One thing wrong at a time in the copies of the files under test, as a sed expression. The
# test of the file must fail on each: that is what shows that it looks. Every change must
# change something (cmp in the Makefile), or the source has moved on.

# src/hv/hv_power.c, against power_test.c
BROKEN_HV_POWER := no-offw off1-without-flag flag-not-set wrong-keep reset-flag-kept \
	stage-not-written no-wait-after-off1 no-wait-after-reset reset-stops-on-pmu not-primary \
	wrong-byte-15
BREAK_no-offw := s/if (smc_write_u32(smc, SMC_KEY_MBSE, MBSE_SHUTDOWN_WARNING)) {/if (0) {/
BREAK_off1-without-flag := s/^    if (ret < 0) {$$/    if (0) {/
BREAK_flag-not-set := s/state = (state . PMU_OFF_KEEP) | PMU_SHUTDOWN_FLAG;/state = state \& PMU_OFF_KEEP;/
BREAK_wrong-keep := s/BIT(0) | BIT(1) | BIT(2) | BIT(5)/0xff/
BREAK_reset-flag-kept := s/state &= ~PMU_SHUTDOWN_FLAG;/state \&= 0xff;/
BREAK_stage-not-written := s/pmu_write(pmu, PMU_BOOT_STAGE, 0) < 0 || //
BREAK_no-wait-after-off1 := /udelay(OFF_WAIT_USEC);/d
BREAK_no-wait-after-reset := /udelay(RESET_WAIT_USEC);/d
BREAK_reset-stops-on-pmu := s/printf("HV: power: restarting all the same\\n");/return;/
BREAK_not-primary := s/primary != 1/primary != 0/
BREAK_wrong-byte-15 := s/PMU_BOOT_STATE    15 /PMU_BOOT_STATE    14 /

# src/smc.c, against power_test.c
BROKEN_SMC := smc-no-timeout notification-as-shmem
BREAK_smc-no-timeout := s/        if (timeout_expired(timeout)) {/        if (0) {/
BREAK_notification-as-shmem := s/if (result == SMC_NOTIFICATION) {/if (smc->shmem \&\& result == SMC_NOTIFICATION) {/

# src/rtkit.c, against power_test.c
BROKEN_RTKIT := rtkit-no-drain rtkit-hello-first rtkit-mgmt-only rtkit-no-power-timeout
BREAK_rtkit-no-drain := s/while (asc_recv(rtk->asc, &msg) && !timeout_expired(timeout))/while (0)/
BREAK_rtkit-hello-first := s/rtkit_printf("expected HELLO, dropping a management message of type 0x%02x\\n", msgtype);/return false;/
BREAK_rtkit-mgmt-only := s/rtkit_printf("dropping a message for endpoint 0x%x while booting: %lx\\n", msg->msg1,/return false; (void)(msg->msg1,/
BREAK_rtkit-no-power-timeout := s/else if (rtk->iop_power != RTKIT_POWER_ON && timeout_expired(timeout)) {/else if (0) {/

# src/hv/hv_stop.c, against stop_test.c
BROKEN_HV_STOP := always-host never-host no-reset no-off any-reason no-replay no-console \
	not-masked returns no-registers
BREAK_always-host := s/return iodev_get_usage(uartproxy_iodev) . USAGE_UARTPROXY;/return true;/
BREAK_never-host := s/return iodev_get_usage(uartproxy_iodev) . USAGE_UARTPROXY;/return false;/
BREAK_no-reset := s/^        hv_power_reset();$$/        ;/
BREAK_no-off := s/^        hv_power_off();$$/        ;/
BREAK_any-reason := s/if (reason == START_HV && type == HV_GUEST_RESET)/if (type == HV_GUEST_RESET)/
BREAK_no-replay := /iodev_reset_rp(IODEV_FB);/d
BREAK_no-console := /iodev_set_usage(IODEV_FB, USAGE_CONSOLE);/d
BREAK_not-masked := /msr daifset/d
BREAK_returns := s/^    hv_stop_halt();$$/    return;/
BREAK_no-registers := s/^    if (ctx) {$$/    if (0) {/

# src/fb.c, against fb_test.c
BROKEN_FB := fb-mallocs fb-copies fb-logo fb-frees-screen fb-not-cleared fb-any-base
BREAK_fb-mallocs := s/    if (unbuffered) {/    if (0) {/
BREAK_fb-copies := /^void fb_update/,/^}/s/if (fb.ptr != fb.hwptr)/if (1)/
BREAK_fb-logo := s/if (!unbuffered && !orig_logo.ptr) {/if (!orig_logo.ptr) {/
BREAK_fb-frees-screen := /^void fb_shutdown/,/^}/s/if (fb.ptr != fb.hwptr)/if (1)/
BREAK_fb-not-cleared := s/fb_setup(true, true);/fb_setup(false, true);/
BREAK_fb-any-base := s/if (!fb.hwptr || fb.hwptr != (void \*)cur_boot_args.video.base)/if (!fb.hwptr)/
