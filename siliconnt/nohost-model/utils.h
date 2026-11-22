/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-in for the m1n1 header of the same name, for the build machine. The registers of
 * devices and of the CPU, time, printing and the few instructions are the test's (model.h).
 */

#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

#include "types.h"

#define ARRAY_SIZE(s)    (sizeof(s) / sizeof((s)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define min(a, b)        (((a) < (b)) ? (a) : (b))
#define max(a, b)        (((a) > (b)) ? (a) : (b))

#define USEC_PER_SEC 1000000L

/* What m1n1 prints goes where src/utils.c sends it: debug_printf() */
int debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#undef printf
#define printf(...) debug_printf(__VA_ARGS__)

/* The registers of the devices the test plays */
u64 read64(u64 addr);
void write64(u64 addr, u64 data);
u32 read32(u64 addr);
void write32(u64 addr, u32 data);

static inline u32 set32(u64 addr, u32 set)
{
    u32 data = read32(addr) | set;
    write32(addr, data);
    return data;
}

static inline u32 clear32(u64 addr, u32 clear)
{
    u32 data = read32(addr) & ~clear;
    write32(addr, data);
    return data;
}

/* An instruction: "wfe", "msr daifset, #0xf", a barrier */
void model_sysop(const char *op);
#define sysop(op) model_sysop(op)

#define dma_mb()  sysop("dmb osh")
#define dma_rmb() sysop("dmb oshld")
#define dma_wmb() sysop("dmb oshst")

/* Time, which passes when the code under test waits */
void udelay(u32 d);
#define mdelay(m) udelay((m) * 1000)
u64 timeout_calculate(u32 usec);
bool timeout_expired(u64 timeout);

/* As src/utils.h has it */
static inline int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    while (--timeout > 0) {
        u32 value = read32(addr) & mask;
        if (value == target)
            return 0;
        udelay(1);
    }

    return -1;
}

typedef struct {
    s64 lock;
    int count;
} spinlock_t;
#define SPINLOCK_INIT       {-1, 0}
#define DECLARE_SPINLOCK(n) spinlock_t n = SPINLOCK_INIT;
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

static inline int is_boot_cpu(void)
{
    return 1;
}

/* Whether a buffer of src/rtkit.c came from the heap: never, here */
bool is_heap(void *addr);

/* m1n1 gives up and restarts: the test is told, and stops there */
void flush_and_reboot(void) __attribute__((noreturn));
#define panic(fmt, ...)                                                                            \
    do {                                                                                           \
        debug_printf(fmt, ##__VA_ARGS__);                                                          \
        flush_and_reboot();                                                                        \
    } while (0)

/* fb.c */
void memcpy128(void *dst, void *src, size_t size);
void memset32(void *dst, u32 value, size_t size);
void memcpy32(void *dst, void *src, size_t size);
void memset64(void *dst, u64 value, size_t size);
extern u32 chip_id;
#define T8012 0x8012

#endif
