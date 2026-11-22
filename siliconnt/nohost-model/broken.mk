# SPDX-License-Identifier: GPL-2.0-or-later
#
# One thing wrong at a time in the copies of the files under test, as a sed expression. The
# test of the file must fail on each: that is what shows that it looks. Every change must
# change something (cmp in the Makefile), or the source has moved on.

# src/hv/hv_power.c, against power_test.c
BROKEN_HV_POWER := no-offw off1-without-flag flag-not-set wrong-keep reset-flag-kept \
	stage-not-written no-wait-after-off1 no-wait-after-reset reset-stops-on-pmu not-primary \
	wrong-byte-15 pmu-no-compat pmu-first-of-two pmu-compat-prefix pmu-compat-unbounded \
	flag-left-set restore-unread restore-silent
BREAK_no-offw := s/if (smc_write_u32(smc, SMC_KEY_MBSE, MBSE_SHUTDOWN_WARNING)) {/if (0) {/
BREAK_off1-without-flag := s/if (pmu_mark(&pmu, true, &was) < 0) {/if (pmu_mark(\&pmu, true, \&was) < 0 \&\& 0) {/
BREAK_flag-not-set := s/state = (state . PMU_OFF_KEEP) | PMU_SHUTDOWN_FLAG;/state = state \& PMU_OFF_KEEP;/
BREAK_wrong-keep := s/BIT(0) | BIT(1) | BIT(2) | BIT(5)/0xff/
BREAK_reset-flag-kept := s/state &= ~PMU_SHUTDOWN_FLAG;/state \&= 0xff;/
BREAK_stage-not-written := s/pmu_write(pmu, PMU_BOOT_STAGE, 0) < 0 || //
BREAK_no-wait-after-off1 := /udelay(OFF_WAIT_USEC);/d
BREAK_no-wait-after-reset := /udelay(RESET_WAIT_USEC);/d
BREAK_reset-stops-on-pmu := s/printf("HV: power: restarting all the same\\n");/return;/
BREAK_not-primary := s/primary != 1/primary != 0/
BREAK_wrong-byte-15 := s/PMU_BOOT_STATE    15 /PMU_BOOT_STATE    14 /
BREAK_pmu-no-compat := s/^                !pmu_compatible(dev))$$/                0)/
BREAK_pmu-first-of-two := s/if (count != 1) {/if (count < 1) {/
BREAK_pmu-compat-prefix := s/strlen(pmus\[i\]) + 1)/strlen(pmus[i]))/
BREAK_pmu-compat-unbounded := s/len > strlen(pmus\[i\]) && //
BREAK_flag-left-set := s/if (was >= 0 && pmu_write(&pmu, PMU_BOOT_STATE, was) < 0)/if (0)/
BREAK_restore-unread := s/if (was >= 0 && pmu_write(&pmu,/if (pmu_write(\&pmu,/
BREAK_restore-silent := s/if (was >= 0 && pmu_write(&pmu, PMU_BOOT_STATE, was) < 0)/if (was >= 0 \&\& pmu_write(\&pmu, PMU_BOOT_STATE, was) < 0 \&\& 0)/

# src/smc.c, against power_test.c
BROKEN_SMC := smc-no-timeout notification-as-shmem notifications-printed \
	smc-timeout-only-when-idle shmem-not-checked shmem-end-inclusive shmem-size-ignored \
	shmem-small-region
BREAK_smc-no-timeout := s/        if (timeout_expired(timeout)) {/        if (0) {/
BREAK_notification-as-shmem := s/if (result == SMC_NOTIFICATION) {/if (smc->shmem \&\& result == SMC_NOTIFICATION) {/
BREAK_notifications-printed := s/        smc->notifications++;/        printf("SMC: Notification: 0x%lx\\n", msg);/
BREAK_smc-timeout-only-when-idle := s/        if (timeout_expired(timeout)) {/        if (!rtkit_can_recv(smc->rtkit) \&\& timeout_expired(timeout)) {/
BREAK_shmem-not-checked := s/    if (!smc_shmem_ok(smc))/    if (0)/
BREAK_shmem-end-inclusive := s/shmem - base > size - SMC_SHMEM_SIZE/shmem - base >= size - SMC_SHMEM_SIZE/
BREAK_shmem-size-ignored := s/shmem - base > size - SMC_SHMEM_SIZE/shmem - base >= size/
BREAK_shmem-small-region := s/size < SMC_SHMEM_SIZE || //

# src/rtkit.c, against power_test.c
BROKEN_RTKIT := rtkit-no-drain rtkit-hello-first rtkit-mgmt-only rtkit-no-power-timeout \
	early-ack-dropped epmap-ack-fails drops-printed drain-drops-printed \
	power-timeout-only-when-idle
BREAK_rtkit-no-drain := s/while (asc_recv(rtk->asc, &msg) && !timeout_expired(timeout))/while (0)/
BREAK_rtkit-hello-first := /did not receive HELLO/,/while (msgtype != MGMT_MSG_HELLO);/s/(\*dropped)++;/return false;/
BREAK_rtkit-mgmt-only := /^static bool rtkit_recv_mgmt/,/^}/s/(\*dropped)++;/return false;/
BREAK_rtkit-no-power-timeout := s/        if (timeout_expired(timeout)) {/        if (0) {/
BREAK_early-ack-dropped := s/if (msgtype != MGMT_MSG_HELLO && !rtkit_power_ack(rtk, &msg))/if (msgtype != MGMT_MSG_HELLO)/
BREAK_epmap-ack-fails := /        if (rtkit_power_ack(rtk, &msg))$$/,+1d
BREAK_drops-printed := /^static bool rtkit_recv_mgmt/,/^}/s/(\*dropped)++;/rtkit_printf("dropping %lx\\n", msg->msg0);/
BREAK_drain-drops-printed := /while (asc_recv(rtk->asc, &msg) && !timeout_expired(timeout))/,+1s/(\*dropped)++;/rtkit_printf("dropping %lx\\n", msg.msg0);/
BREAK_power-timeout-only-when-idle := s/        if (timeout_expired(timeout)) {/        if (!asc_can_recv(rtk->asc) \&\& timeout_expired(timeout)) {/

# src/hv/hv_stop.c, against stop_test.c
BROKEN_HV_STOP := always-host never-host no-reset no-off any-reason no-replay no-console \
	not-masked returns no-registers masked-late masked-early
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
BREAK_masked-late := /msr daifset/d;s/^    hv_stop_show();$$/    hv_stop_show(); sysop("msr daifset, \#0xf");/
BREAK_masked-early := /msr daifset/d;s/^    if (reason == START_HV && type == HV_GUEST_RESET)$$/    sysop("msr daifset, \#0xf"); if (reason == START_HV \&\& type == HV_GUEST_RESET)/

# src/fb.c, against fb_test.c
BROKEN_FB := fb-mallocs fb-copies fb-logo fb-frees-screen fb-not-cleared fb-any-base
BREAK_fb-mallocs := s/    if (unbuffered) {/    if (0) {/
BREAK_fb-copies := /^void fb_update/,/^}/s/if (fb.ptr != fb.hwptr)/if (1)/
BREAK_fb-logo := s/if (!unbuffered && !orig_logo.ptr) {/if (!orig_logo.ptr) {/
BREAK_fb-frees-screen := /^void fb_shutdown/,/^}/s/if (fb.ptr != fb.hwptr)/if (1)/
BREAK_fb-not-cleared := s/fb_setup(true, true);/fb_setup(false, true);/
BREAK_fb-any-base := s/if (!fb.hwptr || fb.hwptr != (void \*)cur_boot_args.video.base)/if (!fb.hwptr)/
