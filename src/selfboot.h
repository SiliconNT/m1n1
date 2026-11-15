/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef SELFBOOT_H
#define SELFBOOT_H

#include "types.h"
#include "xnuboot.h"

/*
 * Starting the SiliconNT firmware from files of the internal SSD, without a host: the part
 * that is arithmetic and checks. It says where things go in guest memory and what the
 * firmware is handed, the way siliconnt/firmware/run_firmware.py does it on the host:
 *
 *   phys_base             start of guest RAM
 *   phys_base + 32 MB     Apple device tree, with the boot disk in /chosen/memory-map
 *   next 2 MB boundary    the firmware image, entered at its first byte
 *   after the image       Apple boot arguments, one 16 KB page
 *   next 2 MB boundary    the boot disk image
 *   ...
 *   mem_top - 128 MB      the firmware's own work area
 *   mem_top               end of guest RAM
 *
 * Every function that returns an int returns 0, or -1 after printing what is wrong.
 */

#define SELFBOOT_PAGE 0x4000UL
/* The guest's own data starts this far above the start of guest RAM */
#define SELFBOOT_GUEST_GAP (32UL << 20)
/* A multiple of every alignment the modules of the image ask for */
#define SELFBOOT_IMAGE_ALIGN   0x200000UL
#define SELFBOOT_BOOTARGS_SIZE SELFBOOT_PAGE
#define SELFBOOT_DISK_ALIGN    0x200000UL
/* The firmware's pages: it reserves exactly the pages the disk lies in */
#define SELFBOOT_DISK_PAGE 0x1000UL
/* PcdSystemMemoryUefiRegionSize of the firmware: it keeps the top of RAM for itself */
#define SELFBOOT_UEFI_REGION 0x8000000UL

struct selfboot_layout {
    u64 phys_base; /* guest RAM */
    u64 mem_top;
    u64 adt_base;
    u64 adt_size;
    u64 image_base;
    u64 image_size;
    u64 bootargs;
    u64 disk_base;
    u64 disk_size;
    u64 top; /* top_of_kernel_data: where what was put here ends */
    u64 uefi_base;
};

/*
 * Guest RAM from phys_base to the end of the memory `ba` describes. Sets everything that
 * does not depend on the two files, and checks, before anything is written, that the
 * framebuffer and what iBoot listed in /chosen/memory-map of the device tree keep clear of
 * where the device tree, the files and the firmware's work area go.
 */
int selfboot_layout_init(struct selfboot_layout *l, u64 phys_base, const struct boot_args *ba);

/* The most bytes the firmware image may have: the size of the buffer to read it into */
u64 selfboot_image_room(const struct selfboot_layout *l);
/* The image, read to image_base, has this size: sets bootargs and disk_base */
int selfboot_layout_image(struct selfboot_layout *l, u64 size);

/* The same for the boot disk: valid after selfboot_layout_image() */
u64 selfboot_disk_room(const struct selfboot_layout *l);
int selfboot_layout_disk(struct selfboot_layout *l, u64 size);

/* What the entry code of the firmware relies on in the header of the image */
int selfboot_check_image(const void *image, u64 size);
/* What the firmware needs of the boot disk to register it and to find a partition */
int selfboot_check_disk(const void *disk, u64 size);

/* The boot arguments of the guest: those of m1n1, with its memory and its device tree */
void selfboot_boot_args(struct boot_args *out, const struct boot_args *cur,
                        const struct selfboot_layout *l);

/*
 * The device tree of the guest, written to `dest` (adt_base): a copy of m1n1's, `size` bytes,
 * with DeviceTree, BootArgs and RAMDisk in /chosen/memory-map. m1n1's own is not changed.
 */
int selfboot_adt(void *dest, u32 size, const struct selfboot_layout *l);

#endif
