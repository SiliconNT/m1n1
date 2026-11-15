# SPDX-License-Identifier: GPL-2.0-or-later
#
# EL1 test guests for src/hv/hv_vgic.c, src/hv/hv_wfi.c and src/hv/hv_psci.c.
#
#   make               build/irqtest.bin, build/sgitest.bin, build/gictest.bin,
#                      build/wfitest.bin and build/pscitest.bin, the raw images that
#                      run_irqtest.py and run_sgitest.py load under the hypervisor
#   make qemu          run irqtest.bin on QEMU's virt machine (GICv3) to check the guest itself
#   make qemu-sgi      the same for sgitest.bin, on one CPU
#   make qemu-sgi-smp  ...and on QEMU_SMP_CPUS CPUs
#   make qemu-gic      the same for gictest.bin, on one CPU
#   make qemu-gic-smp  ...and on QEMU_SMP_CPUS CPUs
#   make qemu-wfi      the same for wfitest.bin, on one CPU
#   make qemu-wfi-smp  ...and on QEMU_SMP_CPUS CPUs
#   make qemu-psci     the same for pscitest.bin, which ends with SYSTEM_OFF
#   make qemu-psci-reset   ...and with SYSTEM_RESET
#   make qemu-psci-strict  pscitest.bin expecting the hypervisor's answers, which QEMU's
#                      PSCI does not give: shows that those checks can fail

ARCH ?= aarch64-linux-gnu-
QEMU ?= qemu-system-aarch64
QEMU_SMP_CPUS ?= 3

CC := $(ARCH)gcc
LD := $(ARCH)ld
OBJCOPY := $(ARCH)objcopy

QEMU_FLAGS := -M virt,gic-version=3 -cpu cortex-a72 -m 64 -nodefaults \
	-display none -monitor none -serial stdio

# sgitest.c, gictest.c, wfitest.c and pscitest.c run with the MMU off (unaligned accesses fault) at an address
# they do not know
SGI_CFLAGS := -O1 -Wall -Werror -ffreestanding -fno-builtin -nostdinc \
	-mgeneral-regs-only -mstrict-align -mcmodel=tiny -fno-pic -fno-pie \
	-fno-jump-tables -fno-stack-protector -fno-asynchronous-unwind-tables

.PHONY: all qemu qemu-sgi qemu-sgi-smp qemu-gic qemu-gic-smp qemu-wfi qemu-wfi-smp \n	qemu-psci qemu-psci-reset qemu-psci-strict qemu-pmu qemu-pmu-strict clean
.SECONDARY:

all: build/irqtest.bin build/sgitest.bin build/gictest.bin build/wfitest.bin build/pscitest.bin build/pmutest.bin

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

build/sgitest.o build/gictest.o build/wfitest.o build/pscitest.o build/pmutest.o: build/%.o: %.c sgitest.h gic.h
	@mkdir -p build
	$(CC) -c $(SGI_CFLAGS) -o $@ $<

build/sgitest.elf build/gictest.elf build/wfitest.elf build/pscitest.elf build/pmutest.elf: build/%.elf: build/sgitest-head.o build/%.o sgitest.ld
	$(LD) -T sgitest.ld --build-id=none --no-warn-rwx-segments -o $@ build/sgitest-head.o build/$*.o

build/sgitest.bin build/gictest.bin build/wfitest.bin build/pscitest.bin build/pmutest.bin: build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@

# build/<image>-qemu-<cpus>.elf
build/sgitest-qemu-%.o: sgitest-qemu.S sgitest.h build/sgitest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=$* -DIMAGE='"build/sgitest.bin"' -o $@ $<

build/gictest-qemu-%.o: sgitest-qemu.S sgitest.h build/gictest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=$* -DIMAGE='"build/gictest.bin"' -o $@ $<

build/wfitest-qemu-%.o: sgitest-qemu.S sgitest.h build/wfitest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=$* -DIMAGE='"build/wfitest.bin"' -o $@ $<

# build/pscitest-qemu-<argument for the image header>.elf, on one CPU
build/pscitest-qemu-%.o: sgitest-qemu.S sgitest.h build/pscitest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=1 -DGUEST_ARG=$* -DIMAGE='"build/pscitest.bin"' -o $@ $<

# build/pmutest-qemu-<argument for the image header>.elf, on one CPU
build/pmutest-qemu-%.o: sgitest-qemu.S sgitest.h build/pmutest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=1 -DGUEST_ARG=$* -DIMAGE='"build/pmutest.bin"' -o $@ $<

build/pmutest-qemu-%.elf: build/pmutest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

build/pscitest-qemu-%.elf: build/pscitest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

build/sgitest-qemu-%.elf: build/sgitest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

build/gictest-qemu-%.elf: build/gictest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

build/wfitest-qemu-%.elf: build/wfitest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

# The guests spin after they have printed DONE, so stop QEMU after a while
qemu: build/qemu-shim.elf
	-timeout 15 $(QEMU) $(QEMU_FLAGS) -kernel $<

qemu-sgi: build/sgitest-qemu-1.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -kernel $<

qemu-sgi-smp: build/sgitest-qemu-$(QEMU_SMP_CPUS).elf
	-timeout 60 $(QEMU) $(QEMU_FLAGS) -smp $(QEMU_SMP_CPUS) -kernel $<

qemu-gic: build/gictest-qemu-1.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -kernel $<

qemu-gic-smp: build/gictest-qemu-$(QEMU_SMP_CPUS).elf
	-timeout 40 $(QEMU) $(QEMU_FLAGS) -smp $(QEMU_SMP_CPUS) -kernel $<

qemu-wfi: build/wfitest-qemu-1.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -kernel $<

qemu-wfi-smp: build/wfitest-qemu-$(QEMU_SMP_CPUS).elf
	-timeout 60 $(QEMU) $(QEMU_FLAGS) -smp $(QEMU_SMP_CPUS) -kernel $<

# pscitest ends with a call that makes QEMU exit; with -no-reboot a reset does too
qemu-psci: build/pscitest-qemu-0x0.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-psci-reset: build/pscitest-qemu-0x1.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-psci-strict: build/pscitest-qemu-0x100.elf
	-timeout 20 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

# pmutest ends with SYSTEM_OFF. QEMU has real PMUv3 registers, so nothing is replaced there:
# the strict run shows that the checks for replaced code can fail
qemu-pmu: build/pmutest-qemu-0x0.elf
	-timeout 30 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-pmu-strict: build/pmutest-qemu-0x100.elf
	-timeout 30 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

clean:
	rm -rf build
