# SPDX-License-Identifier: GPL-2.0-or-later
#
# EL1 test guest for src/hv/hv_vgic.c.
#
#   make        build/irqtest.bin, the raw image that run_irqtest.py loads under the hypervisor
#   make qemu   run the same image on QEMU's virt machine (GICv3) to check the guest itself

ARCH ?= aarch64-linux-gnu-
QEMU ?= qemu-system-aarch64

CC := $(ARCH)gcc
LD := $(ARCH)ld
OBJCOPY := $(ARCH)objcopy

.PHONY: all qemu clean

all: build/irqtest.bin

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

# The guest spins after it has printed DONE, so stop QEMU after a while
qemu: build/qemu-shim.elf
	-timeout 15 $(QEMU) -M virt,gic-version=3 -cpu cortex-a72 -m 64 -nodefaults \
		-display none -monitor none -serial stdio -kernel $<

clean:
	rm -rf build
