/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-side model test for the display when m1n1 starts (src/display.c and what is under it, see
 * the Makefile), on the machine of machine.c. Each case says what is different from an M4 as it
 * was in SiliconNT's lab run R44c, runs what m1n1_main() does, and looks at what came of it:
 *
 *  - an internal display of 10 bits per color, with OS firmware that this was verified with, has
 *    its format changed, with the messages R44c was done with, byte for byte;
 *  - anything else is left as it is, and DCP is not touched;
 *  - whatever DCP does, or does not do, m1n1 goes on in time with the display as it was, nothing is
 *    sent that DCP's firmware asserts on, and nothing DCP uses is taken away while it is awake.
 *
 * Every case runs in a process of its own: display.c keeps what it did, as m1n1 does.
 *
 *   dcp-model                     every case
 *   dcp-model <case>              one, with what is printed
 *   dcp-model --same-cases        the names of those that must be as they were (Makefile)
 *   dcp-model --transcript <case> one of those, with everything that happens
 */

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "display.h"
#include "fb.h"
#include "firmware.h"
#include "machine.h"
#include "r44.h"
#include "utils.h"
#include "xnuboot.h"

#undef printf

static int failures, checks;
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

extern u32 chip_id;

/* ---- running it ---- */

/* However it goes wrong, the machine starts this much later at most */
#define LIMIT_USEC 15000000

static struct boot_args before;
static int init_ret;

/* As m1n1_main() has it */
static void boot(void)
{
    firmware_init();
    init_ret = display_init();
    display_shutdown(DCP_SLEEP_IF_EXTERNAL);
}

static void run(void)
{
    before = cur_boot_args;
    u64 start = machine_now();
    CHECK_EQ(machine_run(boot), RETURNED);
    CHECK(machine_now() - start <= LIMIT_USEC);
}

static const struct boot_args *boot_args_in_memory(void)
{
    return (const struct boot_args *)boot_args_addr;
}

static bool screen_is(u8 value)
{
    const u8 *fb = (const u8 *)FB_BASE;

    for (size_t i = 0; i < FB_STRIDE * FB_HEIGHT; i++)
        if (fb[i] != value)
            return false;
    return true;
}

/* Whatever happened */
static void check_sane(void)
{
    machine_check_buffers();
    CHECK_EQ(m.stray, 0);
    CHECK_EQ(m.heap_errors, 0);
    CHECK_EQ(m.asserts, 0);
    CHECK_EQ(m.faults, 0);
    CHECK_EQ(m.out_of_band, 0);
    CHECK_EQ(m.before_open, 0);
    /* RBEP_SHUTDOWN at most, and what was sent before the crash was seen */
    CHECK(m.after_crash <= 2);
    if (m.iop_power == 0x10) {
        CHECK(machine_page_tables_untouched());
        /* A ring of an endpoint that did not start is left allocated */
        CHECK(machine_dma_buffers() <= 1);
    }
}

static void check_left_alone(void)
{
    check_sane();
    CHECK_EQ(init_ret, 0);
    CHECK(!memcmp(&cur_boot_args, &before, sizeof(before)));
    CHECK(!memcmp(boot_args_in_memory(), &before, sizeof(before)));
    CHECK_EQ(m.fb_clears, 0);
    CHECK_EQ(m.fb_reinits, 0);
    CHECK(screen_is(0x5a));
    CHECK(printed("display: Display is already initialized (3024x1964)"));
}

static void check_changed(void)
{
    struct boot_args want = before;

    want.video.depth = FB_DEPTH_FLAG_RETINA | 32;
    check_sane();
    CHECK_EQ(init_ret, 1);
    CHECK_EQ(m.surfaces, 1);
    CHECK_EQ(m.surface_fmt, 1); /* BGRA */
    CHECK(!memcmp(&cur_boot_args, &want, sizeof(want)));
    CHECK(!memcmp(boot_args_in_memory(), &want, sizeof(want)));
    CHECK_EQ(m.fb_clears, 1);
    CHECK_EQ(m.surfaces_at_clear, 1);
    CHECK_EQ(m.fb_reinits, 1);
    CHECK_EQ(m.depth_at_reinit, want.video.depth);
    CHECK(screen_is(0));
}

/* The endpoint is closed, the host and DCP are quiesced, and what DCP had is back */
static void check_put_away(void)
{
    CHECK_EQ(m.shutdowns, 1);
    CHECK_EQ(m.ap_power, 0x10);
    CHECK_EQ(m.iop_power, 0x10);
    CHECK(!m.awake);
    CHECK_EQ(machine_dma_buffers(), 0);
}

/* P_DISPLAY_START_DCP, or hv_init() with an external display */
static int start_ret;
static void start_dcp(void)
{
    start_ret = display_start_dcp();
    display_shutdown(DCP_QUIESCED);
}

/* Asked once more in the same boot (P_DISPLAY_INIT), after something went wrong */
static void check_not_again(void)
{
    int n_ap = m.n_ap;

    machine_recover();
    CHECK_EQ(machine_run(boot), RETURNED);
    CHECK_EQ(machine_run(start_dcp), RETURNED);
    CHECK_EQ(start_ret, -1);
    CHECK_EQ(m.n_ap, n_ap);
    CHECK_EQ(m.asserts, 0);
    machine_check_buffers();
    CHECK_EQ(m.faults, 0);
    CHECK_EQ(m.heap_errors, 0);
}

static void check_dcp_untouched(void)
{
    CHECK_EQ(m.n_ap, 0);
    CHECK(!printed("dart:"));
    CHECK(!printed("dcp"));
    CHECK(!printed("display: Internal display"));
}

/* ---- as it was done ---- */

static void check_sent(int i, const u8 *want, size_t len)
{
    CHECK(i < m.n_sent);
    CHECK_EQ(m.sent[i].len, len);
    CHECK(!memcmp(m.sent[i].data, want, len));
    /* "IOP ", and no channel and no type in the ring */
    CHECK_EQ(m.sent[i].hdr[0], 0x20504f49);
    CHECK_EQ(m.sent[i].hdr[1], len);
    CHECK_EQ(m.sent[i].hdr[2], 0);
    CHECK_EQ(m.sent[i].hdr[3], 0);
}

static void check_recorded(void)
{
    run();
    check_changed();
    check_put_away();

    CHECK_EQ(m.n_ap, ARRAY_SIZE(r44c_ap));
    for (size_t i = 0; i < ARRAY_SIZE(r44c_ap) && (int)i < m.n_ap; i++) {
        if (m.ap[i].ep != r44c_ap[i].ep || m.ap[i].msg != r44c_ap[i].msg) {
            printf("message %zu: %02x %016lx, not %02x %016lx\n", i, m.ap[i].ep,
                   (unsigned long)m.ap[i].msg, r44c_ap[i].ep, (unsigned long)r44c_ap[i].msg);
            CHECK(!"the messages of R44c, in order");
            break;
        }
    }
    CHECK_EQ(m.n_sent, 3);
    check_sent(0, r44c_open, sizeof(r44c_open));
    check_sent(1, r44c_get_hpd, sizeof(r44c_get_hpd));
    check_sent(2, r44c_set_surface, sizeof(r44c_set_surface));

    /* 2.5 seconds before the service is opened, and then the surface at once */
    CHECK(m.opened_at - m.announced_at >= 2500000);
    CHECK(m.opened_at - m.announced_at <= 2600000);
    CHECK(m.surface_at - m.opened_at <= 10000);
    /* Before the endpoint is closed, the surface has been on the screen */
    CHECK(m.shutdown_at - m.surface_at >= 150000);
    CHECK(machine_now() <= 3000000);

    /* Once more: there is nothing to do */
    int n_ap = m.n_ap;
    CHECK_EQ(machine_run(boot), RETURNED);
    CHECK_EQ(init_ret, 0);
    CHECK_EQ(m.n_ap, n_ap);
    CHECK_EQ(m.fb_clears, 1);
}

static void r44c(void)
{
    check_recorded();
}

static void r44b(void)
{
    memset(&m.announce, 0, sizeof(m.announce));
    entry_add(&m.announce, r44b_announce, sizeof(r44b_announce));
    check_recorded();
}

/* ---- not for this display, or not with this firmware ---- */

static void not_for_it(void)
{
    run();
    check_left_alone();
    check_dcp_untouched();
}

static void depth_32(void)
{
    cur_boot_args.video.depth = FB_DEPTH_FLAG_RETINA | 32;
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));
    not_for_it();
}

static void depth_24(void)
{
    cur_boot_args.video.depth = FB_DEPTH_FLAG_RETINA | 24;
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));
    not_for_it();
}

static void firmware_is(const char *version)
{
    fake_adt_str("/chosen", "firmware-version", version);
    not_for_it();
}

static void fw_27_0(void)
{
    firmware_is("mBoot-20457.1.29");
}

static void fw_27_3(void)
{
    firmware_is("mBoot-20457.60.65.0.1");
}

static void fw_older_patch(void)
{
    firmware_is("mBoot-20457.41.2");
}

static void fw_longer(void)
{
    firmware_is("mBoot-20457.41.3.1");
}

static void fw_more_digits(void)
{
    firmware_is("mBoot-20457.41.30");
}

static void fw_shorter(void)
{
    firmware_is("mBoot-20457.41");
}

static void fw_iboot(void)
{
    firmware_is("iBoot-20457.41.3");
}

static void fw_empty(void)
{
    firmware_is("");
}

static void fw_none(void)
{
    fake_adt_prop("/chosen", "firmware-version", NULL, 0);
    not_for_it();
}

/* The system firmware is not what counts */
static void fw_system_only(void)
{
    fake_adt_str("/chosen", "system-firmware-version", "mBoot-20457.41.3");
    firmware_is("mBoot-18000.161.10");
}

/* The other way round */
static void fw_os_only(void)
{
    fake_adt_str("/chosen", "system-firmware-version", "mBoot-18000.161.10");
    check_recorded();
}

/* One that display_configure() is not for */
static void external_not_mac(void)
{
    u32 one = 1;

    is_mac = false;
    fake_adt_prop("/arm-io/disp0", "external", &one, 4);
    not_for_it();
}

/*
 * An external display, with OS firmware that this is verified with: it goes where it went, which
 * is display_configure(), and gets no further than it did, the rings not being of the V1. (What
 * was there before went on with a ring it had not set up: --same has nothing to compare with.)
 */
static void external(void)
{
    u32 one = 1;

    fake_adt_prop("/arm-io/disp0", "external", &one, 4);
    run();
    check_sane();
    CHECK(printed("AFK: ring buffer size mismatch"));
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.surfaces, 0);
    CHECK_EQ(m.shutdowns, 0);
    CHECK(!memcmp(&cur_boot_args, &before, sizeof(before)));
    CHECK(m.iop_power == 0x10 || m.iop_power == 1);
    check_not_again();
}

/* An internal display that display_configure() is for (Sonoma): the same */
static void not_retina(void)
{
    cur_boot_args.video.depth = 30;
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));
    run();
    check_sane();
    CHECK(printed("AFK: ring buffer size mismatch"));
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.surfaces, 0);
    CHECK_EQ(m.shutdowns, 0);
    CHECK_EQ(cur_boot_args.video.depth, 30);
    /* As for Windows: nothing else that is asked of DCP gets to it either */
    check_not_again();
}

/* It worked, and then DCP is started the V1 way, twice */
static void start_after(void)
{
    run();
    check_changed();
    check_put_away();
    CHECK_EQ(machine_run(start_dcp), RETURNED);
    CHECK_EQ(start_ret, -1);
    CHECK_EQ(m.asserts, 0);
    check_not_again();
}

/* Not as iBoot left it on the M4: nothing of it is changed, and DCP is not woken */
static void segment_unmapped(void)
{
    machine_unmap_segment();
    run();
    check_left_alone();
    CHECK_EQ(m.n_ap, 0);
    CHECK_EQ(m.resets, 0);
    CHECK(machine_page_tables_untouched());
    check_not_again();
}

static void fb_unmapped(void)
{
    machine_unmap_fb();
    run();
    check_left_alone();
    CHECK_EQ(m.inits, 0);
    CHECK_EQ(m.surfaces, 0);
    CHECK(machine_page_tables_untouched());
    check_not_again();
}

/* ---- it stops somewhere on the way ---- */

static enum step sweep_from;
static enum way sweep_way;

/* How long it may take, in milliseconds, when DCP is silent from there on */
static const int silent_msec[STEPS] = {
    [AT_WAKE] = 1100,       [AT_HELLO_ACK] = 1100,   [AT_POWER] = 1100,     [AT_INIT] = 6200,
    [AT_GETBUF] = 6200,     [AT_START] = 6200,       [AT_ANNOUNCE] = 11200, [AT_SETTLE] = 13800,
    [AT_OPEN] = 13800,      [AT_HPD] = 13800,        [AT_SURFACE] = 13800,  [AT_SHUTDOWN] = 8900,
    [AT_AP_QUIESCE] = 3900, [AT_IOP_QUIESCE] = 3900,
};

static void sweep(void)
{
    m.way = sweep_way;
    m.from = sweep_from;
    run();
    if (sweep_way == SILENT)
        CHECK(machine_now() <= 1000UL * silent_msec[sweep_from]);
    if (sweep_from >= AT_SHUTDOWN) {
        check_changed();
        check_not_again();
        return;
    }
    check_left_alone();
    CHECK_EQ(m.surfaces, 0);
    /* It said that it has started, and has not crashed: it is closed, and only then */
    bool started = sweep_from > AT_START && sweep_way != CRASHES && sweep_way != FULL;
    if (sweep_from == AT_ANNOUNCE && sweep_way >= FLOODS_OTHER && sweep_way <= FLOODS_LOGS)
        started = false;
    CHECK_EQ(m.shutdowns, started);
    if (sweep_way == MUTE) {
        CHECK_EQ(m.iop_power, 0x10);
        CHECK(!m.awake);
    }
    if (sweep_from >= AT_INIT && sweep_way != FULL)
        CHECK_EQ(m.inits, 1);
    check_not_again();
}

/* ---- it is slow, and in time ---- */

static const struct {
    enum step at;
    u32 usec;
} slow_steps[] = {
    {AT_WAKE, 900000},      {AT_HELLO_ACK, 400000},  {AT_POWER, 900000},
    {AT_INIT, 2000000},     {AT_GETBUF, 2000000},    {AT_START, 4500000},
    {AT_ANNOUNCE, 4500000}, {AT_HPD, 4500000},       {AT_SURFACE, 4500000},
    {AT_SHUTDOWN, 4500000}, {AT_AP_QUIESCE, 900000}, {AT_IOP_QUIESCE, 900000},
};
static size_t slow_index;

static void slow(void)
{
    m.slow_at = slow_steps[slow_index].at;
    m.slow_usec = slow_steps[slow_index].usec;
    run();
    check_changed();
    check_put_away();
    CHECK_EQ(m.n_ap, ARRAY_SIZE(r44c_ap));
    CHECK(machine_now() >= m.slow_usec);
}

/* ---- replies ---- */

static void fails(void)
{
    run();
    check_left_alone();
    CHECK_EQ(m.surfaces, 0);
    check_put_away();
    check_not_again();
}

static void works(void)
{
    run();
    check_changed();
    check_put_away();
}

static void hpd_is(u8 hpd, u32 timing, u32 color)
{
    u32 body[5] = {3, 20, 0x0007fe00 | hpd, timing, color};

    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 0, 0, 0, body, sizeof(body));
}

static void hpd_made(void)
{
    hpd_is(1, 6, 1);
    works();
}

static void hpd_not_connected(void)
{
    hpd_is(0, 6, 1);
    fails();
}

static void hpd_no_timing(void)
{
    hpd_is(1, 0, 1);
    fails();
}

static void hpd_no_color(void)
{
    hpd_is(1, 6, 0);
    fails();
}

static void hpd_status(void)
{
    u32 body[5] = {3, 20, 1, 6, 1};

    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 0, 0, 0xe00002c7, body, sizeof(body));
    fails();
}

static void hpd_status_1(void)
{
    u32 body[5] = {3, 20, 1, 6, 1};

    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 0, 0, 1, body, sizeof(body));
    fails();
}

static void hpd_short(void)
{
    u32 body[5] = {3, 20, 1, 6, 1};

    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 0, 0, 0, body, 16);
    fails();
}

static void hpd_empty(void)
{
    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 0, 0, 0, "", 0);
    fails();
}

static void hpd_out_of_band(void)
{
    u32 body[5] = {3, 20, 1, 6, 1};

    m.hpd.n = 0;
    entry_reply(&m.hpd, 3, 1, 0, 0, body, sizeof(body));
    fails();
}

/* What is not the reply is passed over */
static void hpd_after(void (*first)(void))
{
    m.hpd.n = 0;
    first();
    entry_add(&m.hpd, r44c_get_hpd_reply, sizeof(r44c_get_hpd_reply));
    works();
}

static void first_wrong_tag(void)
{
    u32 body[5] = {3, 20, 0, 0, 0};
    entry_reply(&m.hpd, 3, 0, 7, 0, body, sizeof(body));
}

static void first_wrong_iface(void)
{
    u32 body[5] = {3, 20, 0, 0, 0};
    entry_reply(&m.hpd, 4, 0, 0, 0, body, sizeof(body));
}

static void first_report(void)
{
    entry_v2(&m.hpd, 3, 0xc0, 0, 0, "12345678", 8);
}

static void first_command(void)
{
    entry_v2(&m.hpd, 3, 0xc0, 1, 0, "\0\0\0\0\0\0\0\0", 8);
}

/* A message of the endpoint itself, shorter than the others: behind it is an old end of a service
 */
static void first_internal(void)
{
    u8 data[16] = {1, 0, 0, 0, 8, 0, 0, 0, 2};
    entry_add(&m.hpd, data, sizeof(data));
    memcpy(m.hpd.e[0].tail, (u8[]){0x14, 0, 1}, 3);
}

static void first_internal_iface_3(void)
{
    first_internal();
    m.hpd.e[0].data[2] = 3;
}

/* Behind it is what a header could be */
static void first_no_command_header(void)
{
    entry_v2(&m.hpd, 3, 0xc0, 2, 0, "", 0);
    memset(m.hpd.e[0].tail, 0, sizeof(m.hpd.e[0].tail));
}

static void first_category_3(void)
{
    entry_v2(&m.hpd, 3, 0xc0, 3, 0, "\0\0\0\0\0\0\0\0", 8);
}

static void hpd_after_wrong_tag(void)
{
    hpd_after(first_wrong_tag);
}

static void hpd_after_wrong_iface(void)
{
    hpd_after(first_wrong_iface);
}

static void hpd_after_report(void)
{
    hpd_after(first_report);
}

static void hpd_after_command(void)
{
    hpd_after(first_command);
}

static void hpd_after_internal(void)
{
    hpd_after(first_internal);
}

static void hpd_after_internal_iface_3(void)
{
    hpd_after(first_internal_iface_3);
}

/* And then the reply, which says that nothing is connected */
static void hpd_after_no_header(void)
{
    u32 body[5] = {3, 20, 0, 6, 1};

    m.hpd.n = 0;
    first_no_command_header();
    entry_reply(&m.hpd, 3, 0, 0, 0, body, sizeof(body));
    fails();
}

static void hpd_after_category_3(void)
{
    hpd_after(first_category_3);
}

static void hpd_only_wrong_tag(void)
{
    m.hpd.n = 0;
    first_wrong_tag();
    fails();
}

/* The first part of a longer one */
static void hpd_split(void)
{
    u8 data[sizeof(r44c_get_hpd_reply)];

    memcpy(data, r44c_get_hpd_reply, sizeof(data));
    data[4] += 0x40;
    m.hpd.n = 0;
    entry_add(&m.hpd, data, sizeof(data));
    fails();
}

/* The service is taken back instead: not waited for any longer */
static void hpd_terminated(void)
{
    m.hpd.n = 0;
    entry_v2(&m.hpd, 3, 0x14, 0, 1, "", 0);
    fails();
    CHECK(m.shutdown_at - m.opened_at < 1000000);
}

/* Another one is */
static void hpd_other_terminated(void)
{
    m.hpd.n = 0;
    entry_v2(&m.hpd, 4, 0x14, 0, 1, "", 0);
    entry_add(&m.hpd, r44c_get_hpd_reply, sizeof(r44c_get_hpd_reply));
    works();
}

static void surface_reply_is(u8 flags, u8 tag, u32 status, u32 len)
{
    u32 body[2] = {1, 8};

    m.surface.n = 0;
    entry_reply(&m.surface, 3, flags, tag, status, body, len);
}

/* What was sent may have been done: whether it was cannot be known, and the display stays as it is
 */
static void surface_fails(void)
{
    run();
    check_left_alone();
    CHECK_EQ(m.surfaces, 1);
    check_put_away();
    check_not_again();
}

static void surface_status(void)
{
    surface_reply_is(0, 1, 0xe00002bc, 0);
    surface_fails();
}

static void surface_status_1(void)
{
    surface_reply_is(0, 1, 1, 0);
    surface_fails();
}

static void surface_out_of_band(void)
{
    surface_reply_is(1, 1, 0, 0);
    surface_fails();
}

/* The reply to the command before, once more */
static void surface_old_tag(void)
{
    surface_reply_is(0, 0, 0, 0);
    surface_fails();
}

static void surface_with_content(void)
{
    surface_reply_is(0, 1, 0, 8);
    works();
}

/* ---- announcements ---- */

static void announce_name(const char *name, u16 iface, u8 flags, u32 len)
{
    char body[32 + 60] = {0};

    strcpy(body, name);
    entry_v2(&m.announce, iface, 0x11, 0, flags, body, len);
}

static void announce_other_first(void)
{
    m.announce.n = 0;
    announce_name("dcpav-service-epic", 2, 1, 92);
    entry_add(&m.announce, r44c_announce, sizeof(r44c_announce));
    works();
}

static void announce_report_first(void)
{
    m.announce.n = 0;
    entry_v2(&m.announce, 3, 0x20, 0, 1, "12345678", 8);
    entry_add(&m.announce, r44c_announce, sizeof(r44c_announce));
    works();
}

static void announce_twice(void)
{
    entry_add(&m.announce, r44c_announce, sizeof(r44c_announce));
    works();
}

static void announce_only_name(void)
{
    m.announce.n = 0;
    announce_name("disp0-service", 3, 1, 32);
    works();
}

static void announce_other(void)
{
    m.announce.n = 0;
    announce_name("dcpav-service-epic", 3, 1, 92);
    fails();
}

static void announce_longer_name(void)
{
    m.announce.n = 0;
    announce_name("disp0-service-epic", 3, 1, 92);
    fails();
}

static void announce_no_room(void)
{
    m.announce.n = 0;
    announce_name("disp0-service", 3, 1, 31);
    fails();
}

/* A report of the service's own, not of the interface */
static void announce_not_internal(void)
{
    m.announce.n = 0;
    announce_name("disp0-service", 3, 0, 92);
    fails();
}

/* No report, but for what is in it */
static void announce_category(void)
{
    m.announce.e[0].data[17] = 2;
    fails();
}

/* More of them than m1n1 keeps */
static void announce_many(void)
{
    m.announce.n = 0;
    for (int i = 0; i < ENTRIES_MAX; i++)
        announce_name("dcpav-service-epic", 4 + i, 1, 92);
    m.after_announce = m.announce;
    fails();
}

/* Taken back before it is opened */
static void announce_terminated(void)
{
    entry_v2(&m.after_announce, 3, 0x14, 0, 1, "", 0);
    fails();
    CHECK_EQ(m.opened_at, 0);
    CHECK_EQ(m.n_sent, 0);
}

static void announce_split(void)
{
    m.announce.e[0].data[4] += 0x40;
    fails();
}

/* ---- entries that are none, and rings ---- */

static void bad_magic(void)
{
    m.announce.e[0].bad_magic = 0x20504f4a;
    fails();
}

static void bad_size(u32 size)
{
    m.announce.e[0].is_bad_size = true;
    m.announce.e[0].bad_size = size;
    fails();
}

static void bad_size_ring(void)
{
    bad_size(0x3e80);
}

static void bad_size_huge(void)
{
    bad_size(0xfffffff0);
}

/* Goes past the end by the header only */
static void bad_size_end(void)
{
    bad_size(0x3e71);
}

static void bad_size_small(void)
{
    bad_size(23);
}

static void bad_size_0(void)
{
    bad_size(0);
}

static void ring_wraps(void)
{
    m.first_ptr = 0x3e00;
    works();
}

static void ring_wraps_later(void)
{
    m.first_ptr = 0x3d00;
    works();
}

/* The endpoint never starts: it is not closed, and what it was given stays */
static void never_starts(void)
{
    run();
    check_left_alone();
    CHECK_EQ(m.inits, 1);
    CHECK_EQ(m.shutdowns, 0);
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.iop_power, 0x10);
    CHECK(!m.awake);
    check_not_again();
}

static void ring_length(int which, u32 bufsz)
{
    m.bufsz[which] = bufsz;
    never_starts();
}

static void ring_length_tx_0x3e81(void)
{
    ring_length(0, 0x3e81);
}

static void ring_length_rx_0x3e81(void)
{
    ring_length(1, 0x3e81);
}

static void ring_length_whole(void)
{
    ring_length(1, 0x4000);
}

static void ring_length_more(void)
{
    ring_length(1, 0x4100);
}

static void ring_length_huge(void)
{
    ring_length(1, 0xffffff80);
}

/* Three blocks of 0x20, of 0xc0, of 1 */
static void ring_block_0x20(void)
{
    ring_length(1, 0x4000 - 3 * 0x20);
}

static void ring_block_0xc0(void)
{
    ring_length(1, 0x4000 - 3 * 0xc0);
}

/* No power of two, and they fit */
static void ring_block_0xc0_fits(void)
{
    m.block = 0xc0;
    m.rb_size = m.ring_off = 0x3000;
    never_starts();
}

/* The last block is cut off */
static void ring_size(void)
{
    m.rb_size = 0x3fc0;
    never_starts();
}

static void ring_block_1(void)
{
    ring_length(1, 0x4000 - 3);
}

/* Nothing but a header */
static void ring_block_all(void)
{
    m.rb_size = 0xc000;
    m.ring_off = 0;
    ring_length(0, 1);
}

static void ring_block_0x100(void)
{
    m.block = 0x100;
    works();
}

/* What iBoot has: the messages of the V2 in a ring of the V1 */
static void ring_block_0x40(void)
{
    m.block = 0x40;
    works();
}

static void ring_wrong_tag(void)
{
    m.ring_tag = 0xbeef;
    never_starts();
}

static void ring_outside(void)
{
    m.ring_off = 0x8000;
    never_starts();
}

static void ring_before_buffer(void)
{
    m.rings_before_buffer = true;
    never_starts();
}

static void ring_only_tx(void)
{
    m.only_tx_ring = true;
    never_starts();
}

/* A pointer that is not into the ring */
static void ring_pointer(void)
{
    m.first_ptr = 0x10000;
    never_starts();
}

/* It says that it has started, and there are no rings */
static void start_ack_before_rings(void)
{
    m.start_ack_before_rings = true;
    run();
    check_left_alone();
    CHECK_EQ(m.shutdowns, 0);
    CHECK_EQ(m.n_sent, 0);
    check_not_again();
}

/* It says that it is closed, and nobody has asked for that: nothing more is sent to it */
static void shutdown_ack_unasked(void)
{
    m.shutdown_ack_unasked = true;
    run();
    check_left_alone();
    CHECK_EQ(m.shutdowns, 0);
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.iop_power, 0x10);
    check_not_again();
}

/* The ring is looked at, whatever is said of it */
static void no_bell(void)
{
    m.no_bell = true;
    works();
}

/* An entry before there are rings: passed over */
static void recv_before_rings(void)
{
    m.recv_before_rings = true;
    works();
}

/* The V1, where the V2 is verified: not this way */
static void v1_there(void)
{
    machine_v1();
    run();
    check_left_alone();
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.surfaces, 0);
    check_put_away();
    check_not_again();
}

/* Started the V1 way before (P_DISPLAY_START_DCP), and not closed */
static void start(void)
{
    firmware_init();
    CHECK_EQ(display_start_dcp(), 0);
}

static void v1_started(void)
{
    machine_v1();
    CHECK_EQ(machine_run(start), RETURNED);
    run();
    check_left_alone();
    CHECK_EQ(m.n_sent, 0);
    CHECK_EQ(m.surfaces, 0);
    check_put_away();
}

/* An external display that does not go to sleep: what DCP has stays, unless DCP is reset */
static void v1_external_awake(bool resets)
{
    u32 one = 1;

    machine_v1();
    fake_adt_str("/chosen", "firmware-version", "iBoot-8422.141.2");
    fake_adt_prop("/arm-io/disp0", "external", &one, 4);
    m.way = SILENT;
    m.from = AT_AP_QUIESCE;
    m.pmgr_resets = resets;
    run();
    check_sane();
    CHECK_EQ(m.surfaces, 1);
    CHECK_EQ(m.resets, 1);
    CHECK(resets == (machine_dma_buffers() == 0));
    if (!resets)
        check_not_again();
}

static void v1_external_awake_not_reset(void)
{
    v1_external_awake(false);
}

static void v1_external_awake_reset(void)
{
    v1_external_awake(true);
}

/* A table that is not of m1n1's heap is not given back to it */
static void empty_l2(void)
{
    machine_empty_l2();
    works();
}

static void crash_with(const u32 *log, int n)
{
    machine_crashlog(log, n);
    m.way = CRASHES;
    m.from = AT_SURFACE;
    run();
    check_left_alone();
    CHECK(printed("IOP crashed!"));
    check_not_again();
}

/* An entry with no length, or with one that is too short */
static void crashlog_length_0(void)
{
    crash_with((u32[]){'Crg8', 0, 0, 0}, 4);
}

static void crashlog_length_8(void)
{
    crash_with((u32[]){'Crg8', 0, 0, 8}, 4);
}

/* No end to it, or the last entry goes past it */
static void crashlog_no_end(void)
{
    crash_with((u32[]){'Crg8', 0, 0, 0x10}, 4);
}

static void crashlog_long(void)
{
    crash_with((u32[]){'Crg8', 0, 0, 0xfffffff0}, 4);
}

/* ---- the same as before: transcripts (Makefile) ---- */

static void same_boot(void)
{
    before = cur_boot_args;
    machine_run(boot);
    printf("returned %d, depth 0x%lx, %s\n", init_ret, (unsigned long)cur_boot_args.video.depth,
           memcmp(&cur_boot_args, boot_args_in_memory(), sizeof(before)) ? "not written back"
                                                                         : "as in memory");
    printf("dma buffers left: %d, page tables %s\n", machine_dma_buffers(),
           machine_page_tables_untouched() ? "as they were" : "changed");
    printf("surfaces %d, format %u\n", m.surfaces, m.surface_fmt);
}

static void v1_firmware(void)
{
    machine_v1();
    fake_adt_str("/chosen", "firmware-version", "iBoot-8422.141.2");
    fake_adt_str("/chosen", "system-firmware-version", "iBoot-8422.141.2");
}

static void same_unlisted(void)
{
    fake_adt_str("/chosen", "firmware-version", "mBoot-20457.60.65.0.1");
    same_boot();
}

static void same_unlisted_known(void)
{
    fake_adt_str("/chosen", "firmware-version", "mBoot-20457.1.29");
    same_boot();
}

static void same_no_version(void)
{
    fake_adt_prop("/chosen", "firmware-version", NULL, 0);
    same_boot();
}

static void same_8_bit(void)
{
    cur_boot_args.video.depth = FB_DEPTH_FLAG_RETINA | 32;
    same_boot();
}

/* display_configure(), because iBoot did not say Retina */
static void same_v1_internal(void)
{
    v1_firmware();
    cur_boot_args.video.depth = 30;
    same_boot();
}

static void same_v1_external(void)
{
    u32 one = 1;

    v1_firmware();
    fake_adt_prop("/arm-io/disp0", "external", &one, 4);
    same_boot();
    /* As hv_init() has it */
    printf("start: %d\n", display_start_dcp());
    display_shutdown(DCP_QUIESCED);
}

static void same_v1_dummy(void)
{
    v1_firmware();
    cur_boot_args.video.width = 640;
    cur_boot_args.video.height = 1136;
    same_boot();
}

static void same_v1_sequoia(void)
{
    v1_firmware();
    chip_id = 0x8112;
    fake_adt_prop("/product", "partially-occluded-display", NULL, 0);
    fake_adt_str("/chosen", "system-firmware-version", "iBoot-11881.1.1");
    same_boot();
}

static void configure(void)
{
    firmware_init();
    printf("configured: %d\n", display_configure("1920x1080@60,retina"));
    display_shutdown(DCP_QUIESCED);
}

static void same_v1_configure(void)
{
    v1_firmware();
    same_boot();
    machine_run(configure);
    printf("depth 0x%lx\n", (unsigned long)cur_boot_args.video.depth);
}

static void same_external_known_new(void)
{
    u32 one = 1;

    fake_adt_str("/chosen", "firmware-version", "iBoot-13822.1.2");
    fake_adt_prop("/arm-io/disp0", "external", &one, 4);
    same_boot();
}

/* DCP does not wake up, says nothing of its endpoints, or crashes early: as it was */
static void same_v1_silent(void)
{
    v1_firmware();
    cur_boot_args.video.depth = 30;
    m.way = SILENT;
    m.from = AT_HELLO_ACK;
    same_boot();
}

static void same_v1_crash(void)
{
    v1_firmware();
    cur_boot_args.video.depth = 30;
    m.way = CRASHES;
    m.from = AT_WAKE;
    same_boot();
}

/* ---- all of them ---- */

struct test_case {
    const char *name;
    void (*fn)(void);
};

#define C(name) {#name, name}
static const struct test_case cases[] = {
    C(r44c),
    C(r44b),
    C(depth_32),
    C(depth_24),
    C(fw_27_0),
    C(fw_27_3),
    C(fw_older_patch),
    C(fw_longer),
    C(fw_more_digits),
    C(fw_shorter),
    C(fw_iboot),
    C(fw_empty),
    C(fw_none),
    C(fw_system_only),
    C(fw_os_only),
    C(external_not_mac),
    C(external),
    C(not_retina),
    C(start_after),
    C(segment_unmapped),
    C(fb_unmapped),
    C(hpd_made),
    C(hpd_not_connected),
    C(hpd_no_timing),
    C(hpd_no_color),
    C(hpd_status),
    C(hpd_status_1),
    C(hpd_short),
    C(hpd_empty),
    C(hpd_out_of_band),
    C(hpd_after_wrong_tag),
    C(hpd_after_wrong_iface),
    C(hpd_after_report),
    C(hpd_after_command),
    C(hpd_after_internal),
    C(hpd_after_internal_iface_3),
    C(hpd_after_no_header),
    C(hpd_after_category_3),
    C(hpd_only_wrong_tag),
    C(hpd_split),
    C(hpd_terminated),
    C(hpd_other_terminated),
    C(surface_status),
    C(surface_status_1),
    C(surface_out_of_band),
    C(surface_old_tag),
    C(surface_with_content),
    C(announce_other_first),
    C(announce_report_first),
    C(announce_twice),
    C(announce_only_name),
    C(announce_other),
    C(announce_longer_name),
    C(announce_no_room),
    C(announce_not_internal),
    C(announce_category),
    C(announce_many),
    C(announce_terminated),
    C(announce_split),
    C(bad_magic),
    C(bad_size_ring),
    C(bad_size_huge),
    C(bad_size_end),
    C(bad_size_small),
    C(bad_size_0),
    C(ring_wraps),
    C(ring_wraps_later),
    C(ring_length_tx_0x3e81),
    C(ring_length_rx_0x3e81),
    C(ring_length_whole),
    C(ring_length_more),
    C(ring_length_huge),
    C(ring_block_0x20),
    C(ring_block_0xc0),
    C(ring_block_0xc0_fits),
    C(ring_size),
    C(ring_block_1),
    C(ring_block_all),
    C(ring_block_0x100),
    C(ring_block_0x40),
    C(ring_wrong_tag),
    C(ring_outside),
    C(ring_before_buffer),
    C(ring_only_tx),
    C(ring_pointer),
    C(recv_before_rings),
    C(start_ack_before_rings),
    C(shutdown_ack_unasked),
    C(no_bell),
    C(v1_there),
    C(v1_started),
    C(v1_external_awake_not_reset),
    C(v1_external_awake_reset),
    C(empty_l2),
    C(crashlog_length_0),
    C(crashlog_length_8),
    C(crashlog_no_end),
    C(crashlog_long),
};

static const struct test_case same_cases[] = {
    C(same_unlisted),     C(same_unlisted_known),     C(same_no_version), C(same_8_bit),
    C(same_v1_internal),  C(same_v1_external),        C(same_v1_dummy),   C(same_v1_sequoia),
    C(same_v1_configure), C(same_external_known_new), C(same_v1_silent),  C(same_v1_crash),
};

/* In a process of its own; how many checks failed */
static int run_case(const char *name, void (*fn)(void), int *n_checks)
{
    int fds[2], counts[2] = {1, 0}, status;

    fflush(stdout);
    if (pipe(fds))
        abort();
    pid_t pid = fork();
    if (!pid) {
        scenario = name;
        machine_reset();
        fn();
        fflush(stdout);
        counts[0] = failures;
        counts[1] = checks;
        if (write(fds[1], counts, sizeof(counts)) != sizeof(counts))
            abort();
        _exit(0);
    }
    close(fds[1]);
    if (read(fds[0], counts, sizeof(counts)) != sizeof(counts)) {
        printf("FAIL [%s]: it ended before it was done\n", name);
        counts[0] = 1;
        counts[1] = 1;
    }
    close(fds[0]);
    waitpid(pid, &status, 0);
    *n_checks += counts[1];
    return counts[0];
}

int main(int argc, char **argv)
{
    int bad = 0, n_checks = 0, n = 0;

    if (argc == 2 && !strcmp(argv[1], "--same-cases")) {
        for (size_t i = 0; i < ARRAY_SIZE(same_cases); i++)
            printf("%s\n", same_cases[i].name);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--transcript")) {
        for (size_t i = 0; i < ARRAY_SIZE(same_cases); i++) {
            if (strcmp(argv[2], same_cases[i].name))
                continue;
            machine_reset();
            transcript = true;
            same_cases[i].fn();
            return 0;
        }
        return 2;
    }

    verbose = argc == 2;
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        if (verbose && strcmp(argv[1], cases[i].name))
            continue;
        n++;
        bad += run_case(cases[i].name, cases[i].fn, &n_checks);
    }
    for (slow_index = 0; slow_index < ARRAY_SIZE(slow_steps); slow_index++) {
        char name[64];

        snprintf(name, sizeof(name), "slow-at-%s", step_names[slow_steps[slow_index].at]);
        if (verbose && strcmp(argv[1], name))
            continue;
        n++;
        bad += run_case(name, slow, &n_checks);
    }
    for (sweep_from = AT_WAKE; sweep_from < STEPS; sweep_from++) {
        for (sweep_way = SILENT; sweep_way < WAYS; sweep_way++) {
            char name[64];

            snprintf(name, sizeof(name), "%s-at-%s", way_names[sweep_way], step_names[sweep_from]);
            if (verbose && strcmp(argv[1], name))
                continue;
            /* The rest of DCP is mute only when it is silent */
            if (sweep_way == MUTE && (sweep_from < AT_INIT || sweep_from > AT_SHUTDOWN))
                continue;
            n++;
            bad += run_case(name, sweep, &n_checks);
        }
    }
    if (!n) {
        printf("no such case\n");
        return 2;
    }
    printf("%d cases, %d checks, %d failed\n", n, n_checks, bad);
    return bad ? 1 : 0;
}
