/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * See selfboot.h. Each step here says where it has its counterpart in
 * siliconnt/firmware/run_firmware.py or proxyclient/m1n1/hv/__init__.py (HV), and where it
 * differs from it. Only what the M4 (T8132) needs is here.
 *
 * Everything that can fail for a reason outside m1n1 (the SSD, the two files, the layout)
 * comes before the hypervisor is initialized, and m1n1 keeps the screen and its consoles
 * until nothing can fail any more.
 *
 * What is the same for every guest, where the devices of the machine are and whether the
 * guest can have its GIC where it is, is src/hv/hv_layout.c.
 */

#include "../build/build_cfg.h"

#include "selfboot.h"
#include "adt.h"
#include "cpu_regs.h"
#include "fb.h"
#include "heapblock.h"
#include "hv.h"
#include "iodev.h"
#include "mcc.h"
#include "memory.h"
#include "nvme.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

#define ARM_IO "/arm-io"
#define UART   "/arm-io/uart0"

/*
 * What HV.init() puts into HACR_EL2, by the names of class HACR of
 * proxyclient/m1n1/sysreg.py: TRAP_CTRR, TRAP_IPI, TRAP_SERROR_INFO, TRAP_EHID, TRAP_HID,
 * TRAP_ACC, TRAP_PM
 */
#define HACR_TRAPS (BIT(14) | BIT(16) | BIT(48) | BIT(49) | BIT(50) | BIT(52) | BIT(57))
/* class ACTLR of the same file */
#define ACTLR_EnMDSB BIT(12)

/* No host: not a port that usb_iodev_vuart_setup() of src/usb.c takes */
#define NO_HOST IODEV_NUM

/* The pages of stage 2 in this tree (src/hv/hv_vm.c) */
#define STAGE2_PAGE 0x1000UL

/* What follows hv_init() in HV.init(), as far as the M4 has it */
static void selfboot_regs(void)
{
    hv_write_hcr((mrs(HCR_EL2) & ~(HCR_TIDCP | HCR_TVM)) | HCR_TACR | HCR_FMO | HCR_IMO |
                 HCR_TTLBOS);
    hv_write_hacr(HACR_TRAPS);

    /*
     * The guest's accesses to the debug registers come to EL2, where hv_dbg.c and hv_exc.c
     * answer them. The host also sets TDE, which sends the guest's debug exceptions to EL2,
     * BRK among them (Arm DDI 0487 M.d, D2.2), and hands each one back to the guest itself
     * (HV.handle_brk(), HV._lower()). Without TDE they go to the guest directly.
     */
    msr(MDCR_EL2, MDCR_TDRA | MDCR_TDOSA | MDCR_TDA);
    /*
     * The host sets MDE for breakpoints of its own. Here nothing is to raise a breakpoint or
     * watchpoint exception, whatever the registers of the CPU hold (D24.3.20, MDE).
     */
    msr(SYS_MDSCR_EL1, 0);

    /*
     * The host does this on every machine; the guest's own accesses go through hv_exc.c.
     * The AMX and key registers HV.init() also sets are not touched: the M4 does not have
     * Apple's registers unlocked (features_m4, src/chickens.c).
     */
    msr(SYS_ACTLR_EL12, mrs(SYS_ACTLR_EL12) | ACTLR_EnMDSB);
    sysop("isb");
}

static int map_hw(const char *what, u64 base, u64 size)
{
    if (hv_map_hw(base, base, size) < 0) {
        printf("selfboot: %s, 0x%lx .. 0x%lx, cannot be mapped for the guest\n", what, base,
               base + size);
        return -1;
    }
    return 0;
}

/*
 * The stage 2 mappings HV.pt_update() makes from what HV.init(), HV._prepare_load() and
 * HV.map_vuart() ask for. The registers the host keeps to itself in HV.map_essential()
 * (power states of the port it talks over, the start of the other cores, their reset
 * vectors) are the devices they are here: there is no host to answer for them.
 */
static int selfboot_map(const struct selfboot_layout *l, iodev_id_t host)
{
    u64 ram_base = l->phys_base & ~0xffffffffUL;
    struct hv_io_ranges ranges;
    u64 base, size, uart;
    int path[8];
    u32 irq;

    /* Every range of /arm-io, where the CPU has it */
    if (hv_io_ranges(&ranges) < 0)
        return -1;
    while (hv_io_ranges_next(&ranges, &base, &size))
        if (map_hw("a range of " ARM_IO, base, size) < 0)
            return -1;

    /*
     * Memory: what lies below iBoot's, and from the start of guest RAM to the end of all
     * there is. In between are m1n1 and its heap.
     */
    if (map_hw("the memory below iBoot's", ram_base, cur_boot_args.phys_base - ram_base) < 0 ||
        map_hw("guest RAM", l->phys_base, ram_base + mem_size_actual - l->phys_base) < 0)
        return -1;
    for (size_t i = 0; i < mcc_carveout_count; i++)
        if (hv_unmap(mcc_carveouts[i].base, mcc_carveouts[i].size) < 0) {
            printf("selfboot: the carveout at 0x%lx cannot be taken from the guest\n",
                   mcc_carveouts[i].base);
            return -1;
        }

    /*
     * Without a host the guest has no UART: the UART is taken out again from the range it
     * lies in, and the guest's device tree does not name it (selfboot_enter()). SiliconNT
     * docs/DESIGN.md, "开发模式": the UART is for a debugger, on the host.
     */
    if (host == NO_HOST) {
        if (adt_path_offset_trace(adt, UART, path) < 0)
            return 0;
        if (adt_get_reg(adt, path, "reg", 0, &uart, &size) < 0 ||
            hv_unmap(uart, ALIGN_UP(size, STAGE2_PAGE)) < 0) {
            printf("selfboot: " UART " cannot be taken from the guest\n");
            return -1;
        }
        return 0;
    }

    /* The virtual UART in place of the real one, after the range it lies in */
    if (adt_path_offset_trace(adt, UART, path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &uart, NULL) < 0 ||
        ADT_GETPROP(adt, adt_path_offset(adt, UART), "interrupts", &irq) < 0) {
        printf("selfboot: no " UART " with an address and an interrupt in the device tree\n");
        return -1;
    }
    hv_map_vuart(uart, irq, host);
    return 0;
}

static void print_layout(const struct selfboot_layout *l)
{
    printf("selfboot: guest RAM 0x%lx .. 0x%lx\n", l->phys_base, l->mem_top);
    printf("  device tree        0x%lx .. 0x%lx\n", l->adt_base, l->adt_base + l->adt_size);
    printf("  firmware image     0x%lx .. 0x%lx\n", l->image_base, l->image_base + l->image_size);
    printf("  boot arguments     0x%lx .. 0x%lx\n", l->bootargs,
           l->bootargs + SELFBOOT_BOOTARGS_SIZE);
    printf("  AIC to GIC table   0x%lx .. 0x%lx\n", l->aicmap, l->aicmap + SELFBOOT_AICMAP_SIZE);
    printf("  boot disk          0x%lx .. 0x%lx\n", l->disk_base, l->disk_base + l->disk_size);
    printf("  firmware work area 0x%lx .. 0x%lx\n", l->uefi_base, l->mem_top);
}

/*
 * The guest has not been entered after the ports were taken. Without a host they are as they
 * were again: with P_SELFBOOT_RUN and no host, the port the command came over is the proxy's
 * once more, and a host can connect to m1n1 again. With a host nothing changes: its port was
 * never taken. Returns -1.
 */
static int selfboot_give_back(const iodev_usage_t *usage, iodev_id_t host)
{
    if (host == NO_HOST)
        for (int i = 0; i < IODEV_MAX; i++)
            iodev_set_usage(i, usage[i]);
    return -1;
}

/*
 * The two files are in guest memory and the layout is complete. host: the port a host talks
 * to m1n1 over, or NO_HOST.
 */
static int selfboot_enter(struct selfboot_layout *l, iodev_id_t host)
{
    u64 regs[4] = {l->bootargs, 0, 0, 0};
    iodev_usage_t usage[IODEV_MAX];
    int aicmap_size;

    print_layout(l);

    /* run_firmware.py: the entry code runs with the MMU of EL1 off until it has its tables */
    if (mrs(SYS_SCTLR_EL12) & SCTLR_M) {
        printf("selfboot: the MMU of EL1 is on, the firmware expects it off\n");
        return -1;
    }

    /*
     * Not in run_firmware.py: the firmware tells Windows where the GIC is, and the machine
     * must have nothing of its own there. hv_start() asks again, for every guest, but by
     * then the screen is no longer m1n1's to say so on.
     */
    if (!hv_vgic_ranges_free())
        return -1;

    /*
     * The table of AIC lines and SPIs: P_HV_AICMAP, in load_firmware(). hv_init() makes the
     * same for the hypervisor from the same device tree.
     */
    aicmap_size = hv_aicmap((struct hv_aicmap *)l->aicmap);
    if (aicmap_size < 0)
        return -1;
    l->aicmap_size = aicmap_size;

    /* The device tree and the boot arguments: load_firmware() and HV.start() */
    if (selfboot_adt((void *)l->adt_base, cur_boot_args.devtree_size, l) < 0)
        return -1;
    /* Without a host the guest has no UART (selfboot_map()), and the firmware no serial port */
    if (host == NO_HOST && selfboot_adt_unname((void *)l->adt_base, UART) < 0)
        return -1;
    memset((void *)l->bootargs, 0, SELFBOOT_BOOTARGS_SIZE);
    selfboot_boot_args((struct boot_args *)l->bootargs, &cur_boot_args, l);

    /* HV._load_executable_raw() */
    dc_cvau_range((void *)l->image_base, l->image_size);
    ic_ivau_range((void *)l->image_base, l->image_size);

    /*
     * HV.init(). The USB ports nobody talks over stop being m1n1's first: hv_init() gives the
     * interrupts of their port controllers back, for the guest (usb_hpm_restore_irqs() of
     * src/usb.c). It also has every other core come into EL2. What the ports were is kept, for a
     * guest that is not entered without a host (selfboot_give_back())
     */
    for (int i = 0; i < IODEV_MAX; i++)
        usage[i] = iodev_get_usage(i);
    for (int i = IODEV_USB0; i < IODEV_MAX; i++)
        if (i != (int)host)
            iodev_set_usage(i, 0);
    hv_init();

    /*
     * Not in run_firmware.py, as for the GIC above: hv_start() checks, for every guest, but
     * this says which core is missing while the screen is still m1n1's
     */
    if (!hv_cpus_ready())
        return selfboot_give_back(usage, host);

    selfboot_regs();
    if (selfboot_map(l, host) < 0)
        return selfboot_give_back(usage, host);

    /*
     * Execution tracking, when all the memory is mapped: this is where it is taken apart
     * into pages (hv_pt_xtrack() of src/hv/hv_vm.c). run_firmware.py has HV.pt_update() make
     * the mappings first too.
     */
    if (hv_pt_xtrack() < 0) {
        printf("selfboot: the hypervisor refused to track execution\n");
        return selfboot_give_back(usage, host);
    }

    /*
     * Not in run_firmware.py, as for the GIC above: hv_start() checks, for every guest, but
     * this says so while the screen is still m1n1's
     */
    if (hv_pt_check_ctrr() < 0)
        return selfboot_give_back(usage, host);

    /*
     * HV.start(): from here on m1n1 prints to the host only, if there is one, and the screen
     * is the guest's.
     */
    printf("selfboot: entering the firmware at 0x%lx, x0 = 0x%lx\n", l->image_base, l->bootargs);
    for (int i = 0; i < IODEV_USB0; i++)
        if (i != (int)host)
            iodev_set_usage(i, 0);
    fb_shutdown(true);

    hv_start((void *)l->image_base, regs);
    /* Left by the host. Without one the guest is never left: hv_start() did not enter it */
    if (host != NO_HOST)
        return 0;
    printf("selfboot: the hypervisor did not enter the firmware\n");
    return selfboot_give_back(usage, host);
}

#ifdef CHAINLOADING

int selfboot_run(const char *part_uuid)
{
    /* Where ProxyUtils of the host puts its own heap: here guest RAM starts there */
    u64 phys_base = ALIGN_UP((u64)heapblock_alloc(0) + selfboot_m1n1_heap(mem_size_actual),
                             SELFBOOT_PAGE);
    struct selfboot_layout l;
    int ret;

    printf("selfboot: the firmware and the boot disk are on partition %s\n", part_uuid);
    if (selfboot_layout_init(&l, phys_base, &cur_boot_args) < 0)
        return -1;
    heapblock_set_limit((void *)phys_base);

    if (!nvme_init()) {
        printf("selfboot: the SSD controller did not come up\n");
        return -1;
    }
    ret = selfboot_read_files(&l, part_uuid, selfboot_check_disk);
    nvme_shutdown();
    if (ret < 0)
        return -1;

    return selfboot_enter(&l, NO_HOST);
}

#endif

int selfboot_run_loaded(u64 phys_base, u64 image_size, u64 disk_size, int host_iodev)
{
    u64 lowest = (u64)heapblock_alloc(0) + selfboot_m1n1_heap(mem_size_actual);
    struct selfboot_layout l;

    if (phys_base % SELFBOOT_PAGE || phys_base < lowest) {
        printf("selfboot: guest RAM cannot start at 0x%lx: not a page at 0x%lx or above, where "
               "m1n1 has room for its heap\n",
               phys_base, lowest);
        return -1;
    }
    if (selfboot_layout_init(&l, phys_base, &cur_boot_args) < 0 ||
        selfboot_layout_image(&l, image_size) < 0 ||
        selfboot_check_image((void *)l.image_base, image_size) < 0 ||
        selfboot_layout_disk(&l, disk_size) < 0 ||
        selfboot_check_disk((void *)l.disk_base, disk_size) < 0)
        return -1;

    return selfboot_enter(&l, host_iodev == SELFBOOT_NO_HOST ? NO_HOST : host_iodev);
}
