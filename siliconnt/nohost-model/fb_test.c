// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Host-side model test for fb_init_unbuffered() of src/fb.c: the framebuffer console once
 * more after fb_shutdown(), drawn on the framebuffer itself, for the hypervisor to show why it
 * has stopped its guest when there is no host (src/hv/hv_stop.c). The file under test is the
 * real fb.c; the framebuffer is memory of the test, as big as the M4's (3024 x 1964, 30 bits,
 * the retina font: R30's "fb init" and "fb console" lines), and the fonts are made up.
 *
 * What it is to do: take nothing from the heap and map nothing (the heap may have no room left
 * by then: m1n1 panics when it runs out, src/dlmalloc/malloc_config.h), and draw what the
 * console with a copy in the heap draws, pixel for pixel, scrolling included.
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"
#include "iodev.h"
#include "malloc.h"
#include "utils.h"
#include "xnuboot.h"

/* The heap of the C library, for the stand-ins below */
#undef malloc
#undef calloc
#undef free

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

/* ---- what fb.c needs of m1n1 ---- */

#define WIDTH  3024
#define HEIGHT 1964
#define STRIDE (WIDTH * 4)
#define SIZE   ((size_t)STRIDE * HEIGHT)

struct boot_args cur_boot_args;
u32 chip_id = 0x8132;
static u32 *hw;

u8 _binary_build_font_bin_start[95 * 8 * 16];
u8 _binary_build_font_retina_bin_start[95 * 16 * 32];
u8 _binary_build_bootlogo_48_bin_start[48 * 48 * 4];
u8 _binary_build_bootlogo_128_bin_start[128 * 128 * 4];
u8 _binary_build_bootlogo_256_bin_start[256 * 256 * 4];

static int mallocs, frees, mappings, copies, stray;

void *model_malloc(size_t size)
{
    mallocs++;
    return malloc(size);
}

void *model_calloc(size_t n, size_t size)
{
    mallocs++;
    return calloc(n, size);
}

void *model_memalign(size_t align, size_t size)
{
    mallocs++;
    return aligned_alloc(align, (size + align - 1) / align * align);
}

void model_free(void *p)
{
    /* The framebuffer is not the heap's */
    if (p && (u32 *)p == hw) {
        stray++;
        return;
    }
    frees++;
    free(p);
}

void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attribute_index, u64 perms)
{
    UNUSED(from);
    UNUSED(to);
    UNUSED(size);
    UNUSED(attribute_index);
    UNUSED(perms);
    mappings++;
}

void memcpy128(void *dst, void *src, size_t size)
{
    copies++;
    memcpy(dst, src, size & ~15UL);
}

void memset32(void *dst, u32 value, size_t size)
{
    u32 *p = dst;
    for (size_t i = 0; i < size / 4; i++)
        p[i] = value;
}

void memcpy32(void *dst, void *src, size_t size)
{
    memmove(dst, src, size & ~3UL);
}

void memset64(void *dst, u64 value, size_t size)
{
    u64 *p = dst;
    for (size_t i = 0; i < size / 8; i++)
        p[i] = value;
}

bool payload_logo(void **custom_128, void **custom_256)
{
    UNUSED(custom_128);
    UNUSED(custom_256);
    return false;
}

void iodev_console_kick(void)
{
}

int debug_printf(const char *fmt, ...)
{
    UNUSED(fmt);
    return 0;
}

void flush_and_reboot(void)
{
    say("FAIL [%s]: m1n1 restarted\n", scenario);
    exit(1);
}

/* ---- the test ---- */

static void glyphs(u8 *font, int w, int h)
{
    for (int c = 0; c < 95; c++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                font[(c * h + y) * w + x] = (c * 37 + x * 11 + y * 5) & 0xff;
}

/* What the guest left on the screen */
static void guest_picture(void)
{
    for (size_t i = 0; i < SIZE / 4; i++)
        hw[i] = 0x12345678 ^ (u32)(i * 2654435761u);
}

static bool all(u32 value)
{
    for (size_t i = 0; i < SIZE / 4; i++)
        if (hw[i] != value)
            return false;
    return true;
}

/* Some lines, a long one that wraps, and enough of them for the console to scroll */
static void text(void)
{
    char line[200];

    for (int i = 0; i < 90; i++) {
        snprintf(line, sizeof(line), "HV: line %d of what m1n1 printed%s\n", i,
                 i % 7 ? ""
                       : ", and this one is long enough to go past the 78 columns of the console "
                         "and on to the next row");
        fb_console_write(line, strlen(line));
    }
}

/* A broken version of fb.c may draw anywhere: that is a failure too */
static void crashed(int sig)
{
    say("FAIL [%s]: the test crashed (signal %d)\n", scenario, sig);
    exit(1);
}

int main(void)
{
    u32 *picture;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGSEGV, crashed);
    signal(SIGBUS, crashed);
    hw = aligned_alloc(0x4000, SIZE);
    picture = malloc(SIZE);
    glyphs(_binary_build_font_bin_start, 8, 16);
    glyphs(_binary_build_font_retina_bin_start, 16, 32);
    cur_boot_args.video.base = (u64)hw;
    cur_boot_args.video.stride = STRIDE;
    cur_boot_args.video.width = WIDTH;
    cur_boot_args.video.height = HEIGHT;
    cur_boot_args.video.depth = 30 | FB_DEPTH_FLAG_RETINA;
    cur_boot_args.video.display = 1;

    /* m1n1 has had no console: nothing is mapped for one, and nothing is drawn */
    scenario = "before any console";
    guest_picture();
    memcpy(picture, hw, SIZE);
    CHECK(!fb_init_unbuffered());
    CHECK(!memcmp(picture, hw, SIZE));

    /* As m1n1 starts (main.c), and as selfboot_enter() gives the screen to the guest */
    scenario = "the console of when m1n1 starts";
    fb_init(false);
    fb_set_active(true);
    text();
    fb_shutdown(true);

    /* The guest draws; the hypervisor stops it, and the console is there again */
    scenario = "the console without a copy in the heap";
    guest_picture();
    mallocs = frees = mappings = copies = stray = 0;
    CHECK(fb_init_unbuffered());
    CHECK(mallocs == 0 && mappings == 0);
    /* The screen is cleared: nothing of the guest's picture is left */
    CHECK(all(0));
    fb_set_active(true);
    text();
    CHECK(mallocs == 0 && mappings == 0 && copies == 0);
    CHECK(!all(0));
    memcpy(picture, hw, SIZE);
    fb_shutdown(false);
    CHECK(frees == 0 && stray == 0);

    /* The same with the copy in the heap, from the same picture of the guest */
    scenario = "the same with a copy in the heap";
    guest_picture();
    fb_init(true);
    CHECK(mappings == 1);
    fb_set_active(true);
    text();
    CHECK(!memcmp(picture, hw, SIZE));
    fb_shutdown(false);
    CHECK(stray == 0);

    /* The framebuffer has moved since m1n1 started: nothing is mapped there, nothing drawn */
    scenario = "a framebuffer somewhere else";
    u32 *elsewhere = aligned_alloc(0x4000, SIZE);
    memset(elsewhere, 0x5a, SIZE);
    cur_boot_args.video.base = (u64)elsewhere;
    CHECK(!fb_init_unbuffered());
    for (size_t i = 0; i < SIZE / 4; i++)
        if (elsewhere[i] != 0x5a5a5a5a) {
            CHECK(!"something was drawn there");
            break;
        }

    say("fb_test: %d checks, %d failed\n", checks, failures);
    if (failures)
        return 1;
    say("ALL OK\n");
    return 0;
}
