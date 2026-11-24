# SPDX-License-Identifier: GPL-2.0-or-later
#
# One thing wrong at a time in the copies of the files under test, as a sed expression. The test
# must fail on each: that is what shows that it looks. Every change must change something (cmp in
# the Makefile), or the source has moved on.

BROKEN_afk := v2-ring-always block-0x40 read-pointer write-pointer ring-data block-0xc0 \
	block-cut-off ring-anywhere pointer-anywhere early-data old-wrap any-size \
	no-length no-header external-report no-category short-announcement any-service \
	any-interface no-wait-timeout no-start-timeout no-shutdown-timeout not-started-kept \
	not-shut-down-freed not-shut-down-silent any-tag reply-of-any-interface any-category \
	no-command-header any-status any-band longer-reply never-terminated open-terminated \
	not-passed sequence tag-of-0 not-opened v1-as-v2 command-type not-internal open-v1
BREAK_v2-ring-always := s/(!epic->afk->v2 && block != (1 << BLOCK_SHIFT))/0/
BREAK_block-0x40 := s/    rb->block = block;/    rb->block = 0x40;/
BREAK_read-pointer := s/rb->rptr = (void \*)hdr + block;/rb->rptr = (void *)hdr + 0x40;/
BREAK_write-pointer := s/rb->wptr = (void \*)hdr + 2 \* block;/rb->wptr = (void *)hdr + 0x80;/
BREAK_ring-data := s/rb->buf = (void \*)hdr + 3 \* block;/rb->buf = (void *)hdr + 0xc0;/
BREAK_block-0xc0 := s/(block & (block - 1)) || //
BREAK_block-cut-off := s/size % block ||/0 ||/
BREAK_ring-anywhere := s/if (!epic->buf.bfr || base + size > epic->buf.sz) {/if (0) {/
BREAK_pointer-anywhere := s/if (\*rb->rptr >= bufsz || .*) {/if (0) {/
BREAK_early-data := s/if (!rb->ready) {/if (0) {/
BREAK_old-wrap := s/if (hdr->size > rb->bufsz - rptr - sizeof(\*hdr)) {/if (rptr + hdr->size > rb->bufsz) {/
BREAK_any-size := s/if (hdr->size > rb->bufsz - sizeof(\*hdr)) {/if (0) {/
BREAK_no-length := s/ || msg->len != qe->size - offsetof(struct afk_v2_msg, timestamp))/)/
BREAK_no-header := s/if (qe->size < sizeof(\*msg) || /if (/
BREAK_external-report := s/ && (msg->flags & V2_INTERNAL) && / \&\& /
BREAK_no-category := s/msg->category == V2_REPORT && //
BREAK_short-announcement := s/qe->size >= sizeof(\*msg) + sizeof(struct epic_announce))/1)/
BREAK_any-service := s/ops = afk_match_service(epic, ((struct epic_announce .*/ops = epic->ops;/
BREAK_any-interface := s/service->channel = msg->iface;/service->channel = 0;/
BREAK_no-wait-timeout := s/    while (!timeout_expired(timeout)) {$$/    while (1) {/
BREAK_no-start-timeout := s/while (!epic->started && !timeout_expired(timeout)) {/while (!epic->started) {/
BREAK_no-shutdown-timeout := s/while (epic->started && !timeout_expired(timeout)) {/while (epic->started) {/
BREAK_not-started-kept := s/    if (!epic->started) {/    if (0) {/
BREAK_not-shut-down-freed := s/    if (epic->started) {/    if (epic->started = false) {/
BREAK_not-shut-down-silent := /did not shut down/,/}/s/ret = -1;/ret = 0;/
BREAK_any-tag := s/cmd->tag != tag) {/0) {/
BREAK_reply-of-any-interface := s/msg->iface != channel || //
BREAK_any-category := s/msg->category != V2_REPLY || //
BREAK_no-command-header := s/qe->size < hdrsize ||/0 ||/
BREAK_any-status := s/if (cmd->value || /if (/
BREAK_any-band := s/if (cmd->value || (cmd->flags & V2_CMD_OOB)) {/if (cmd->value) {/
BREAK_longer-reply := s/V2_MAX_REPLY 0x1000/V2_MAX_REPLY 0x4000/
BREAK_never-terminated := s/                service->enabled = false;/                ;/
BREAK_open-terminated := s/    if (!afk_epic_find_service(epic, channel)) {/    if (0) {/
BREAK_not-passed := /^static void afk_v2_pass/,/^}/s/afk_epic_rx_ack(epic);//
BREAK_sequence := s/msg->seq = epic->seq++;/msg->seq = 0;/
BREAK_tag-of-0 := s/u8 tag = service->seq++;/u8 tag = 0;/
BREAK_not-opened := s/    return afk_v2_send(epic, &msg, sizeof(msg));/    return 1;/
BREAK_v1-as-v2 := /^int afk_epic_command/,/^}/s/if (epic->afk->v2)/if (0)/
BREAK_command-type := s/    msg->type = type;/    msg->type = 0xc1;/
BREAK_not-internal := s/        .flags = V2_INTERNAL,/        .flags = 0,/
BREAK_open-v1 := /^int afk_epic_open/,/^}/s/if (!epic->afk->v2) {/if (0) {/

BROKEN_dcp_iboot := surface-164 v2-not-set shutdown-ok
BREAK_surface-164 := s/iboot->v2 ? sizeof(\*cmd) : sizeof(\*layer)/sizeof(*layer)/
BREAK_v2-not-set := /afk_epic_set_v2(dcp->afk);/s/afk_epic_set_v2(dcp->afk)//
BREAK_shutdown-ok := s/int ret = afk_epic_shutdown_ep(iboot->epic);/int ret = 0 * afk_epic_shutdown_ep(iboot->epic);/

BROKEN_display := any-firmware any-depth external-too no-settle no-swap-wait w30r not-in-memory \
	not-retina depth-24 not-cleared not-reinit any-hpd hpd-failed no-open v1-way \
	again-after-failure again-after-shutdown again-after-quiesce stride swap-failed \
	start-again height
BREAK_any-firmware := s/firmware_dcp_set_surface_verified() && //
BREAK_any-depth := s/(cur_boot_args.video.depth & FB_DEPTH_MASK) == 30 &&/1 \&\&/
BREAK_external-too := s/if (!display_is_external && (cur_boot_args/if ((cur_boot_args/
BREAK_no-settle := s/DISPLAY_SETTLE_USEC 2500000/DISPLAY_SETTLE_USEC 2000000/
BREAK_no-swap-wait := /^static int display_set_bgra/,/^}/s/mdelay(150);/mdelay(50);/
BREAK_w30r := s/FMT_BGRA);/FMT_w30r);/
BREAK_not-in-memory := /^static int display_set_bgra/,/^}/{/memcpy(/d}
BREAK_not-retina := s/(video->depth & ~FB_DEPTH_MASK) | 32/32/
BREAK_depth-24 := s/(video->depth & ~FB_DEPTH_MASK) | 32/(video->depth \& ~FB_DEPTH_MASK) | 24/
BREAK_not-cleared := /^static int display_set_bgra/,/^}/{/fb_clear_direct();/d}
BREAK_not-reinit := /^static int display_set_bgra/,/^}/{/fb_reinit();/d}
BREAK_any-hpd := s/if (!hpd || !timing_cnt || !color_cnt) {/if (0) {/
BREAK_hpd-failed := /^static int display_set_bgra/,/^}/s/if (hpd < 0) {/if (0) {/
BREAK_no-open := s/if ((ret = dcp_ib_open(iboot, DISPLAY_SETTLE_USEC)) < 0) {/if (0) {/
BREAK_v1-way := s/display_do_start_dcp(true)/display_do_start_dcp(false)/
BREAK_again-after-failure := /leaving the display as it is/,/}/s/dcp_abandoned = true;/;/
BREAK_again-after-shutdown := s/int ret = dcp_ib_shutdown(iboot);/int ret = 0 * dcp_ib_shutdown(iboot);/
BREAK_again-after-quiesce := s/ret |= dcp_shutdown(dcp, display_is_external);/dcp_shutdown(dcp, display_is_external);/
BREAK_stride := s/ret = display_swap(fb_dva, video->stride, /ret = display_swap(fb_dva, video->width * 8, /
BREAK_swap-failed := /FMT_BGRA);/,/return ret;/s/return ret;/;/
BREAK_start-again := s/    if (dcp_abandoned) {/    if (0) {/
BREAK_height := s/video->width, video->height, FMT_BGRA/video->width, video->height - 64, FMT_BGRA/

BROKEN_firmware := prefix system-firmware no-version
BREAK_prefix := s/!strcmp(os_firmware.iboot, dcp_set_surface_verified\[i\])/!strncmp(os_firmware.iboot, dcp_set_surface_verified[i], 16)/
BREAK_system-firmware := s/if (os_firmware.iboot && !strcmp(os_firmware.iboot, /if (!strcmp(system_firmware.iboot, /
BREAK_no-version := s/if (os_firmware.iboot && /if (/

BROKEN_dcp := not-quiet-freed
BREAK_not-quiet-freed := s/} else if (!rtkit_quiesce(dcp->rtkit)) {/} else if (!rtkit_quiesce(dcp->rtkit) \&\& 0) {/

BROKEN_rtkit := logs-forever ap-forever iop-forever log-no-length log-no-end
BREAK_logs-forever := s/while (!timeout_expired(timeout) && asc_recv(rtk->asc, &asc_msg)) {/while (asc_recv(rtk->asc, \&asc_msg)) {/
BREAK_ap-forever := /while (rtk->ap_power != RTKIT_POWER_QUIESCED) {/,/^    }/s/if (timeout_expired(timeout)) {/if (0) {/
BREAK_iop-forever := /while (rtk->iop_power != target) {/,/^    }/s/if (timeout_expired(timeout)) {/if (0) {/
BREAK_log-no-length := s/if (p->len < sizeof(\*p))/if (0)/
BREAK_log-no-end := s/while ((void \*)(p + 1) <= end && /while (/

BROKEN_dart := frees-iboots
BREAK_frees-iboots := s/    if (!is_heap(l2))/    if (0)/
