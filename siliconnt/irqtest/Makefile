# SPDX-License-Identifier: GPL-2.0-or-later
#
# EL1 test guests for src/hv/hv_vgic.c.
#
#   make               build/irqtest.bin and build/sgitest.bin, the raw images that
#                      run_irqtest.py and run_sgitest.py load under the hypervisor
#   make qemu          run irqtest.bin on QEMU's virt machine (GICv3) to check the guest itself
#   make qemu-sgi      the same for sgitest.bin, on one CPU
#   make qemu-sgi-smp  ...and on QEMU_SMP_CPUS CPUs

ARCH ?= aarch64-linux-gnu-
QEMU ?= qemu-system-aarch64
QEMU_SMP_CPUS ?= 3

CC := $(ARCH)gcc
LD := $(ARCH)ld
OBJCOPY := $(ARCH)objcopy

QEMU_FLAGS := -M virt,gic-version=3 -cpu cortex-a72 -m 64 -nodefaults \
	-display none -monitor none -serial stdio

# sgitest.c runs with the MMU off (unaligned accesses fault) at an address it does not know
SGI_CFLAGS := -O1 -Wall -Werror -ffreestanding -fno-builtin -nostdinc \
	-mgeneral-regs-only -mstrict-align -mcmodel=tiny -fno-pic -fno-pie \
	-fno-jump-tables -fno-stack-protector -fno-asynchronous-unwind-tables

.PHONY: all qemu qemu-sgi qemu-sgi-smp clean

all: build/irqtest.bin build/sgitest.bin

build/irqtest.o: irqtest.S
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/irqtest.elf: build/irqtest.o
	$(LD) -Ttext=0 -e 0 --build-id=none -o $@ $<

build/irqtest.bin: build/irqtest.elf
	$(OBJCOPY) -O binary $< $@

build/qemu-shim.o: qemu-shim.S build/irqtest.bin
	$(CC) -c -Wall -Werror -o $@ $<

build/qemu-shim.elf: build/qemu-shim.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

build/sgitest-head.o: sgitest-head.S sgitest.h
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/sgitest.o: sgitest.c sgitest.h
	@mkdir -p build
	$(CC) -c $(SGI_CFLAGS) -o $@ $<

build/sgitest.elf: build/sgitest-head.o build/sgitest.o sgitest.ld
	$(LD) -T sgitest.ld --build-id=none --no-warn-rwx-segments -o $@ build/sgitest-head.o build/sgitest.o

build/sgitest.bin: build/sgitest.elf
	$(OBJCOPY) -O binary $< $@

build/sgitest-qemu-%.o: sgitest-qemu.S build/sgitest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=$* -o $@ $<

build/sgitest-qemu-%.elf: build/sgitest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

# The guests spin after they have printed DONE, so stop QEMU after a while
qemu: build/qemu-shim.elf
	-timeout 15 $(QEMU) $(QEMU_FLAGS) -kernel $<

qemu-sgi: build/sgitest-qemu-1.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -kernel $<

qemu-sgi-smp: build/sgitest-qemu-$(QEMU_SMP_CPUS).elf
	-timeout 60 $(QEMU) $(QEMU_FLAGS) -smp $(QEMU_SMP_CPUS) -kernel $<

clean:
	rm -rf build
