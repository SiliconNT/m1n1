# SPDX-License-Identifier: GPL-2.0-or-later
#
# EL1 test guests for src/hv/hv_vgic.c, src/hv/hv_wfi.c, src/hv/hv_psci.c, and src/hv/hv_pmu.c
# with src/hv/hv_xtrack.c and src/hv/hv_xinsn.c.
#
#   make               build/irqtest.bin, build/sgitest.bin, build/gictest.bin,
#                      build/wfitest.bin, build/pscitest.bin, build/pmutest.bin,
#                      build/xitest.bin and build/aictest.bin, the raw images that
#                      run_irqtest.py and run_sgitest.py load under the hypervisor.
#                      aictest needs AIC: run_sgitest.py --aic-line
#   make build/stoptest.bin  a guest that only does what the hypervisor stops it for, with
#                      the header of a firmware image: for run_firmware.py --loader m1n1
#                      [--no-host] (R37)
#   make anstest       build/anstest.bin: the disk controller core of the SiliconNT drivers
#                      repository (ANS_CORE) in a guest that run_anstest.py loads (R38). Not
#                      part of "all": it needs that repository

ARCH ?= aarch64-linux-gnu-

CC := $(ARCH)gcc
LD := $(ARCH)ld
OBJCOPY := $(ARCH)objcopy

# sgitest.c, gictest.c, wfitest.c and pscitest.c run with the MMU off (unaligned accesses fault) at an address
# they do not know
SGI_CFLAGS := -O1 -Wall -Werror -ffreestanding -fno-builtin -nostdinc \
	-mgeneral-regs-only -mstrict-align -mcmodel=tiny -fno-pic -fno-pie \
	-fno-jump-tables -fno-stack-protector -fno-asynchronous-unwind-tables

.PHONY: anstest all clean
.SECONDARY:

all: build/irqtest.bin build/sgitest.bin build/gictest.bin build/wfitest.bin build/pscitest.bin build/pmutest.bin build/xitest.bin build/aictest.bin

build/irqtest.o: irqtest.S
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/irqtest.elf: build/irqtest.o
	$(LD) -Ttext=0 -e 0 --build-id=none -o $@ $<

build/irqtest.bin: build/irqtest.elf
	$(OBJCOPY) -O binary $< $@

build/sgitest-head.o: sgitest-head.S sgitest.h
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/sgitest.o build/gictest.o build/wfitest.o build/pscitest.o build/pmutest.o build/aictest.o: build/%.o: %.c sgitest.h gic.h
	@mkdir -p build
	$(CC) -c $(SGI_CFLAGS) -o $@ $<

build/sgitest.elf build/gictest.elf build/wfitest.elf build/pscitest.elf build/pmutest.elf build/aictest.elf: build/%.elf: build/sgitest-head.o build/%.o sgitest.ld
	$(LD) -T sgitest.ld --build-id=none --no-warn-rwx-segments -o $@ build/sgitest-head.o build/$*.o

build/sgitest.bin build/gictest.bin build/wfitest.bin build/pscitest.bin build/pmutest.bin build/aictest.bin: build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@

# xitest: its instructions are assembled (LSE and RCPC among them), and nothing of it that is
# written to shares a page with what is executed (xitest.ld)
build/xitest.o: xitest.c sgitest.h gic.h xitest.h
	@mkdir -p build
	$(CC) -c $(SGI_CFLAGS) -o $@ $<

build/xitest-insns.o: xitest-insns.S xitest.h
	@mkdir -p build
	$(CC) -c -Wall -Werror -march=armv8.3-a -o $@ $<

build/xitest.elf: build/sgitest-head.o build/xitest.o build/xitest-insns.o xitest.ld
	$(LD) -T xitest.ld --build-id=none --no-warn-rwx-segments -o $@ build/sgitest-head.o build/xitest.o build/xitest-insns.o

build/xitest.bin: build/xitest.elf
	$(OBJCOPY) -O binary $< $@

# stoptest: a guest that the hypervisor stops

build/stoptest.o: stoptest.S
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/stoptest.elf: build/stoptest.o
	$(LD) -Ttext=0 -e 0 --build-id=none -o $@ $<

build/stoptest.bin: build/stoptest.elf
	$(OBJCOPY) -O binary $< $@

# anstest: the core of the drivers repository is compiled from there, as it is; nothing of it
# is copied here. Its headers come with the compiler's own stdbool.h, stddef.h and stdint.h
ANS_CORE ?= /mnt/c/src/siliconnt-drivers/ans/core
ANS_CORE_SRCS := asc nvme pmgr rtkit sart
ANS_CFLAGS := $(SGI_CFLAGS) -isystem $(shell $(CC) -print-file-name=include) -I$(ANS_CORE)
ANS_CORE_OBJS := $(ANS_CORE_SRCS:%=build/anscore-%.o)

anstest: build/anstest.bin

build/anscore-%.o: $(ANS_CORE)/%.c $(wildcard $(ANS_CORE)/*.h)
	@mkdir -p build
	$(CC) -c $(ANS_CFLAGS) -o $@ $<

# The CRC32 instructions for the GPT
build/anstest.o: anstest.c sgitest.h gic.h $(wildcard $(ANS_CORE)/*.h)
	@mkdir -p build
	$(CC) -c $(ANS_CFLAGS) -march=armv8-a+crc -o $@ $<

# The image runs where it was not linked: no absolute address may be stored in it
build/anstest.elf: build/sgitest-head.o build/anstest.o $(ANS_CORE_OBJS) sgitest.ld
	! $(ARCH)objdump -r build/anstest.o $(ANS_CORE_OBJS) | grep -E "R_AARCH64_ABS(64|32)"
	$(LD) -T sgitest.ld --build-id=none --no-warn-rwx-segments -o $@ \
		build/sgitest-head.o build/anstest.o $(ANS_CORE_OBJS)

build/anstest.bin: build/anstest.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf build
