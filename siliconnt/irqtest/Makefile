# SPDX-License-Identifier: GPL-2.0-or-later
#
# EL1 test guests for src/hv/hv_vgic.c, src/hv/hv_wfi.c, src/hv/hv_psci.c, and src/hv/hv_pmu.c
# with src/hv/hv_xtrack.c and src/hv/hv_xinsn.c.
#
#   make               build/irqtest.bin, build/sgitest.bin, build/gictest.bin,
#                      build/wfitest.bin, build/pscitest.bin, build/pmutest.bin,
#                      build/xitest.bin and build/aictest.bin, the raw images that
#                      run_irqtest.py and run_sgitest.py load under the hypervisor.
#                      aictest needs AIC, which QEMU does not have: it only runs on the
#                      machine (run_sgitest.py --aic-line)
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
#   make qemu-pmu-el2  pmutest.bin under an EL2 of its own that links the real execution
#                      tracking and PMU code (xtrack-qemu.c), and plays a device that writes
#                      into the guest's code
#   make qemu-pmu-el2-nosyn  ...where ESR_EL2 never describes an access, so that every read
#                      and store that EL2 carries out is taken apart from the instruction
#   make qemu-xi       xitest.bin on QEMU with no EL2: checks the guest itself
#   make qemu-xi-el2   xitest.bin under that EL2: every load and store that EL2 takes apart
#                      against the same instruction where EL2 has no part
#   make qemu-xi-el2-nosyn  ...where ESR_EL2 never describes an access
#   make qemu-xi-broken     ...against hv_xtrack.c with one thing wrong in it at a time
#                      (../xtrack-broken.mk): xitest, or pmutest for QEMU_BROKEN_PMU, must
#                      fail on each of QEMU_BROKEN
#   make build/stoptest.bin  a guest that only does what the hypervisor stops it for, with
#                      the header of a firmware image: for run_firmware.py --loader m1n1
#                      [--no-host] (R37)
#   make qemu-nohost   stoptest.bin under an EL2 of its own that links the real code for a
#                      guest that has to be stopped (nohost-qemu.c), with a host and without
#                      one, for hvc #1, SYSTEM_OFF and SYSTEM_RESET
#   make qemu-nohost-broken ...against hv_stop.c with one thing wrong in it at a time
#                      (../nohost-model/broken.mk): it must fail on each of QEMU_NOHOST_BROKEN
#   make anstest       build/anstest.bin: the disk controller core of the SiliconNT drivers
#                      repository (ANS_CORE) in a guest that run_anstest.py loads (R38). Not
#                      part of "all": it needs that repository
#   make qemu-ans      anstest.bin on QEMU, with RAM where the controller would be: the
#                      guest's self-test of its own checks passes, the controller cannot come
#                      up there, and the guest says so, having written no register but RUN,
#                      the mailbox's and the power manager's (check_ans_qemu.sh)
#   make qemu-ans-broken  ...against anstest.c with one of its checks broken at a time
#                      (ans-broken.mk): the self-test has to find each

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

.PHONY: anstest qemu-ans qemu-ans-broken all qemu qemu-sgi qemu-sgi-smp qemu-gic qemu-gic-smp qemu-wfi qemu-wfi-smp qemu-psci qemu-psci-reset qemu-psci-strict qemu-pmu qemu-pmu-strict qemu-pmu-el2 qemu-pmu-el2-nosyn qemu-xi qemu-xi-el2 qemu-xi-el2-nosyn qemu-xi-broken qemu-nohost qemu-nohost-broken clean
.SECONDARY:

all: build/irqtest.bin build/sgitest.bin build/gictest.bin build/wfitest.bin build/pscitest.bin build/pmutest.bin build/xitest.bin build/aictest.bin

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

# build/xitest-qemu-<argument for the image header>.elf, on one CPU
build/xitest-qemu-%.o: sgitest-qemu.S sgitest.h build/xitest.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=1 -DGUEST_ARG=$* -DIMAGE='"build/xitest.bin"' -o $@ $<

build/xitest-qemu-%.elf: build/xitest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

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

# A guest under an EL2 that is the real src/hv/hv_xtrack.c, src/hv/hv_xinsn.c and
# src/hv/hv_pmu.c without their #include lines, and stand-ins for the rest of the hypervisor
# (xtrack-qemu.c)
build/hv_xpage.h: ../../src/hv/hv.h
	@mkdir -p build
	grep -E '^#define HV_X(PAGE|TRACK)_' $< > $@

build/hv_xtrack.inc: ../../src/hv/hv_xtrack.c
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

build/hv_pmu.inc: ../../src/hv/hv_pmu.c
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

build/hv_xinsn.inc: ../../src/hv/hv_xinsn.c
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

build/hv_xinsn_h.inc: ../../src/hv/hv_xinsn.h
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

XTRACK_INCS := build/hv_xpage.h build/hv_xtrack.inc build/hv_pmu.inc build/hv_xinsn.inc build/hv_xinsn_h.inc
EL2_GUESTS := pmutest xitest

$(EL2_GUESTS:%=build/%-el2-head.o): build/%-el2-head.o: xtrack-qemu-head.S build/%.bin
	$(CC) -c -Wall -Werror -DIMAGE='"build/$*.bin"' -o $@ $<

build/xtrack-qemu.o: xtrack-qemu.c $(XTRACK_INCS)
	$(CC) -c $(SGI_CFLAGS) -Ibuild -o $@ $<

build/xtrack-qemu-nosyn.o: xtrack-qemu.c $(XTRACK_INCS)
	$(CC) -c $(SGI_CFLAGS) -Ibuild -DNO_SYNDROME -o $@ $<

$(EL2_GUESTS:%=build/%-el2-qemu.elf): build/%-el2-qemu.elf: build/%-el2-head.o build/xtrack-qemu.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $^

$(EL2_GUESTS:%=build/%-el2-qemu-nosyn.elf): build/%-el2-qemu-nosyn.elf: build/%-el2-head.o build/xtrack-qemu-nosyn.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $^

# The same EL2 with one thing wrong in hv_xtrack.c (../xtrack-broken.mk). Each must fail on
# QEMU: in what the guest finds, or in what this EL2 checks as it runs (xtrack-qemu.c: where
# stage 2 points, and no shadow page found out of date that no device wrote). xitest runs
# them, told to end at its first failure. DC IVAC and a device that writes into code are in
# pmutest alone, which runs 16 and 23.
# Three cannot show on QEMU, which has no caches: 19 leaves lines of the guest's page not
# clean, 20 a shadow page, and 21 leaves the instruction caches as they are. Those are for the
# model (../vgic-model, make xtrack-broken).
include ../xtrack-broken.mk
QEMU_BROKEN := $(filter-out 19 20 21,$(BROKEN))
QEMU_BROKEN_PMU := 16 23
broken_guest = $(if $(filter $(1),$(QEMU_BROKEN_PMU)),pmutest,xitest)

build/hv_xtrack-broken-%.inc: build/hv_xtrack.inc ../xtrack-broken.mk
	sed -e '$(BREAK_$*)' $< > $@
	! cmp -s $< $@

# ARG_STRICT and ARG_DEVICE for pmutest, ARG_STOP for xitest
build/xtrack-qemu-broken-%.o: xtrack-qemu.c $(XTRACK_INCS) build/hv_xtrack-broken-%.inc
	$(CC) -c $(SGI_CFLAGS) -Wno-error -Ibuild -DXTRACK_INC='"hv_xtrack-broken-$*.inc"' -DGUEST_ARG=0x700 -o $@ $<

build/xitest-el2-broken-%.elf: build/xitest-el2-head.o build/xtrack-qemu-broken-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $^

build/pmutest-el2-broken-%.elf: build/pmutest-el2-head.o build/xtrack-qemu-broken-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $^

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
# the strict run shows that the checks for replaced code can fail (2 of the 38). Neither has
# ARG_DEVICE: the device that writes into code is for the EL2 of xtrack-qemu.c to play
qemu-pmu: build/pmutest-qemu-0x0.elf
	-timeout 30 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-pmu-strict: build/pmutest-qemu-0x100.elf
	-timeout 30 $(QEMU) $(QEMU_FLAGS) -smp 1 -no-reboot -kernel $<

# QEMU enters these at EL2. The guest ends with SYSTEM_OFF, which its EL2 passes on to QEMU
QEMU_EL2_FLAGS := -M virt,gic-version=3,virtualization=on -cpu cortex-a72 -m 64 -nodefaults -display none -monitor none -serial stdio
# xitest has the atomic instructions of Armv8.1, which that core has not
QEMU_XI_FLAGS := -M virt,gic-version=3 -cpu neoverse-n1 -m 64 -nodefaults -display none -monitor none -serial stdio
QEMU_XI_EL2_FLAGS := -M virt,gic-version=3,virtualization=on -cpu neoverse-n1 -m 64 -nodefaults -display none -monitor none -serial stdio

qemu-pmu-el2: build/pmutest-el2-qemu.elf
	-timeout 30 $(QEMU) $(QEMU_EL2_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-pmu-el2-nosyn: build/pmutest-el2-qemu-nosyn.elf
	-timeout 30 $(QEMU) $(QEMU_EL2_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-xi: build/xitest-qemu-0x0.elf
	-timeout 300 $(QEMU) $(QEMU_XI_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-xi-el2: build/xitest-el2-qemu.elf
	-timeout 900 $(QEMU) $(QEMU_XI_EL2_FLAGS) -smp 1 -no-reboot -kernel $<

qemu-xi-el2-nosyn: build/xitest-el2-qemu-nosyn.elf
	-timeout 900 $(QEMU) $(QEMU_XI_EL2_FLAGS) -smp 1 -no-reboot -kernel $<

# One variant ($(1)) with its guest ($(2)): run it, and insist that the guest started, or that
# execution tracking stopped it at its first instruction (22 does), and that it did not end
# without a failure
define XI_BROKEN_RUN
timeout 900 $(QEMU) $(QEMU_XI_EL2_FLAGS) -smp 1 -no-reboot -kernel build/$(2)-el2-broken-$(1).elf > build/$(2)-el2-broken-$(1).log 2>&1; \
grep -q -E '$(2): start|execution tracking stops the guest' build/$(2)-el2-broken-$(1).log || { echo "qemu-xi-broken: $(1): the guest did not start"; exit 1; }; \
if grep -q -E 'DONE pass fail = [0-9]+ 0[^0-9]*$$' build/$(2)-el2-broken-$(1).log; then echo "qemu-xi-broken: $(1): the test passes and must not"; exit 1; fi; \
grep -q -E 'FAIL|stops the guest|unexpected exception' build/$(2)-el2-broken-$(1).log || { echo "qemu-xi-broken: $(1): ended without a failed check"; exit 1; }; \
echo "qemu-xi-broken: $(1) $(BREAK_$(1)_IS) ($(2)): $$(grep -m1 -E 'FAIL|stops the guest|unexpected exception' build/$(2)-el2-broken-$(1).log | tr -d '\r')";
endef

qemu-xi-broken: $(foreach n,$(QEMU_BROKEN),build/$(call broken_guest,$(n))-el2-broken-$(n).elf)
	@$(foreach n,$(QEMU_BROKEN),$(call XI_BROKEN_RUN,$(n),$(call broken_guest,$(n))))
	@echo "qemu-xi-broken: all $(words $(QEMU_BROKEN)) failed, as they must"

# ---- a guest that the hypervisor stops, with a host and without one (nohost-qemu.c) ----

build/stoptest.o: stoptest.S
	@mkdir -p build
	$(CC) -c -Wall -Werror -o $@ $<

build/stoptest.elf: build/stoptest.o
	$(LD) -Ttext=0 -e 0 --build-id=none -o $@ $<

build/stoptest.bin: build/stoptest.elf
	$(OBJCOPY) -O binary $< $@

build/stoptest-el2-head.o: xtrack-qemu-head.S build/stoptest.bin
	$(CC) -c -Wall -Werror -DIMAGE='"build/stoptest.bin"' -o $@ $<

# The real files of m1n1 with its headers, as its Makefile compiles them (BASE_CFLAGS), but
# not position independent: this EL2 is linked where it runs
M1N1_CFLAGS := -O2 -Wall -Wundef -Werror=strict-prototypes -fno-common -fno-PIE -fno-pic \
	-Werror=implicit-function-declaration -Werror=implicit-int -Wno-multichar -ffreestanding \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) -isystem ../../sysinc \
	-I../../src -I../../src/hv -fno-stack-protector -mstrict-align \
	-march=armv8.2-a -mgeneral-regs-only
NOHOST_M1N1 := hv/hv_psci.c hv/hv_stop.c hv/hv_power.c smc.c rtkit.c asc.c spmi.c wdt.c \
	iodev.c vsprintf.c string.c
NOHOST_OBJS := $(foreach f,$(NOHOST_M1N1),build/m1n1-$(basename $(notdir $(f))).o) build/fake_adt.o

$(foreach f,$(NOHOST_M1N1),$(eval build/m1n1-$(basename $(notdir $(f))).o: ../../src/$(f) ; \
	@mkdir -p build && $$(CC) -c $$(M1N1_CFLAGS) -o $$@ $$<))

build/fake_adt.o: ../nohost-model/fake_adt.c ../nohost-model/fake_adt.h
	@mkdir -p build
	$(CC) -c $(M1N1_CFLAGS) -Werror -o $@ $<

# build/nohost-qemu-<host or nohost>-<STOP_ARG of stoptest.S>
build/nohost-qemu-host-%.o: nohost-qemu.c ../nohost-model/fake_adt.h
	@mkdir -p build
	$(CC) -c $(M1N1_CFLAGS) -Werror -DHOST=1 -DSTOP_ARG=$* -o $@ $<

build/nohost-qemu-nohost-%.o: nohost-qemu.c ../nohost-model/fake_adt.h
	@mkdir -p build
	$(CC) -c $(M1N1_CFLAGS) -Werror -DHOST=0 -DSTOP_ARG=$* -o $@ $<

build/nohost-qemu-%.elf: build/stoptest-el2-head.o build/nohost-qemu-%.o $(NOHOST_OBJS)
	$(LD) -Ttext=0x40100000 -e _start --build-id=none --no-warn-rwx-segments -o $@ $^

# QEMU ends by itself where a host is told; without one the core stays in EL2, and QEMU is
# stopped after a while. check_nohost_qemu.sh says what each run is to show.
NOHOST_RUNS := host-0 host-1 host-2 nohost-0 nohost-1 nohost-2
define NOHOST_RUN
{ timeout 10 $(QEMU) $(QEMU_XI_EL2_FLAGS) -smp 1 -no-reboot -kernel $(2) > $(3) 2>&1; \
bash check_nohost_qemu.sh $(1) $(3) $$?; }
endef

qemu-nohost: $(NOHOST_RUNS:%=build/nohost-qemu-%.elf) build/stoptest.bin
	@$(foreach c,$(NOHOST_RUNS),$(call NOHOST_RUN,$(c),build/nohost-qemu-$(c).elf,build/nohost-qemu-$(c).log) &&) true
	@echo "qemu-nohost: all $(words $(NOHOST_RUNS)) as they should be"

# hv_stop.c with one thing wrong in it (../nohost-model/broken.mk), in the run that shows it
include ../nohost-model/broken.mk
QEMU_NOHOST_BROKEN := always-host returns no-replay no-off
nohost_broken_run = $(if $(filter no-off,$(1)),nohost-1,nohost-0)

build/hv_stop-broken-%.c: ../../src/hv/hv_stop.c ../nohost-model/broken.mk
	@mkdir -p build
	sed -e '$(BREAK_$*)' $< > $@
	! cmp -s $< $@

build/m1n1-hv_stop-broken-%.o: build/hv_stop-broken-%.c
	$(CC) -c $(M1N1_CFLAGS) -Wno-error -o $@ $<

$(foreach n,$(QEMU_NOHOST_BROKEN),$(eval build/nohost-qemu-broken-$(n).elf: \
	build/stoptest-el2-head.o build/nohost-qemu-$(call nohost_broken_run,$(n)).o \
	build/m1n1-hv_stop-broken-$(n).o $(filter-out build/m1n1-hv_stop.o,$(NOHOST_OBJS)) ; \
	$$(LD) -Ttext=0x40100000 -e _start --build-id=none --no-warn-rwx-segments -o $$@ $$^))

qemu-nohost-broken: $(QEMU_NOHOST_BROKEN:%=build/nohost-qemu-broken-%.elf)
	@$(foreach n,$(QEMU_NOHOST_BROKEN),\
		if ($(call NOHOST_RUN,$(call nohost_broken_run,$(n)),build/nohost-qemu-broken-$(n).elf,build/nohost-qemu-broken-$(n).log)) > build/nohost-qemu-broken-$(n).check; \
		then echo "qemu-nohost-broken: $(n): the run is as it should be, and must not be"; exit 1; \
		else echo "qemu-nohost-broken: $(n): $$(tail -1 build/nohost-qemu-broken-$(n).check)"; fi;)
	@echo "qemu-nohost-broken: all $(words $(QEMU_NOHOST_BROKEN)) failed, as they must"

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

# anstest on QEMU: run_anstest.py --qemu-image fills in the parameters with QEMU's addresses,
# RAM where the controller's four ranges would be, and the image goes behind sgitest-qemu.S.
# build/anstest-qemu-<polling or interrupts>-<refused or silent>: refused, the power manager
# register reads 0, so the reset before the start is refused; silent, it reads "active", the
# coprocessor is started and never answers. The mailbox reads empty in both
PYTHON ?= python3
ANS_QEMU_FLAGS := $(subst -m 64,-m 256,$(QEMU_FLAGS))
ANS_QEMU_ASC := 0x48100000
ANS_QEMU_PMGR := 0x48300538
ANS_QEMU_RUNS := polling-refused polling-silent interrupts-refused interrupts-silent
ans_qemu_mode = $(word 1,$(subst -, ,$(1)))
ans_qemu_pmgr = $(if $(filter %-silent,$(1)),0xf0,0)

build/anstest-qemu-%.bin: build/anstest.bin run_anstest.py
	$(PYTHON) run_anstest.py --mode $(call ans_qemu_mode,$*) --qemu-image $@ $<

build/anstest-qemu-%.o: sgitest-qemu.S sgitest.h build/anstest-qemu-%.bin
	$(CC) -c -Wall -Werror -DNR_CPUS=1 -DIMAGE='"build/anstest-qemu-$*.bin"' -o $@ $<

build/anstest-qemu-%.elf: build/anstest-qemu-%.o
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ $<

# $(1) the run check_ans_qemu.sh judges, $(2) the ELF without .elf (the log goes next to it),
# $(3) what the power manager register reads
define ANS_QEMU_RUN
{ timeout 30 $(QEMU) $(ANS_QEMU_FLAGS) -smp 1 -no-reboot \
	-device loader,addr=$$(($(ANS_QEMU_ASC) + 0x8114)),data=0x20000,data-len=4 \
	-device loader,addr=$(ANS_QEMU_PMGR),data=$(3),data-len=4 \
	-kernel $(2).elf > $(2).log 2>&1; \
bash check_ans_qemu.sh $(1) $(2).log $$?; }
endef

qemu-ans: $(ANS_QEMU_RUNS:%=build/anstest-qemu-%.elf)
	@$(foreach r,$(ANS_QEMU_RUNS),\
		$(call ANS_QEMU_RUN,$(r),build/anstest-qemu-$(r),$(call ans_qemu_pmgr,$(r))) &&) true
	@echo "qemu-ans: all $(words $(ANS_QEMU_RUNS)) as they should be"

# anstest.c with one of its own checks broken at a time (ans-broken.mk), run as polling-silent:
# each run has to fail its check, and because the self-test found the break.
# build/ansbroken-<name>.bin is the image, build/ansbroken-qemu-<name> the run on QEMU
include ans-broken.mk

build/ansbroken-%.c: anstest.c ans-broken.mk
	@mkdir -p build
	sed -e '$(ANS_BREAK_$*)' $< > $@
	! cmp -s $< $@

build/ansbroken-%.o: build/ansbroken-%.c sgitest.h gic.h $(wildcard $(ANS_CORE)/*.h)
	$(CC) -c $(ANS_CFLAGS) -Wno-error -I. -march=armv8-a+crc -o $@ $<

build/ansbroken-%.bin: build/sgitest-head.o build/ansbroken-%.o $(ANS_CORE_OBJS) sgitest.ld
	$(LD) -T sgitest.ld --build-id=none --no-warn-rwx-segments -o build/ansbroken-$*.elf \
		build/sgitest-head.o build/ansbroken-$*.o $(ANS_CORE_OBJS)
	$(OBJCOPY) -O binary build/ansbroken-$*.elf $@

build/ansbroken-qemu-%.elf: build/ansbroken-%.bin run_anstest.py sgitest-qemu.S sgitest.h
	$(PYTHON) run_anstest.py --mode polling --qemu-image build/ansbroken-qemu-$*.bin $<
	$(CC) -c -Wall -Werror -DNR_CPUS=1 -DIMAGE='"build/ansbroken-qemu-$*.bin"' \
		-o build/ansbroken-qemu-$*.o sgitest-qemu.S
	$(LD) -Ttext=0x40100000 -e _start --build-id=none -o $@ build/ansbroken-qemu-$*.o

qemu-ans-broken: $(ANS_BROKEN:%=build/ansbroken-qemu-%.elf)
	@$(foreach n,$(ANS_BROKEN),\
		if ($(call ANS_QEMU_RUN,polling-silent,build/ansbroken-qemu-$(n),0xf0)) \
			> build/ansbroken-qemu-$(n).check; \
		then echo "qemu-ans-broken: $(n): the run is as it should be, and must not be"; exit 1; \
		elif ! grep -q "^HVLOG: anstest: FAIL self-test" build/ansbroken-qemu-$(n).log; \
		then echo "qemu-ans-broken: $(n): the self-test did not find it"; exit 1; \
		else echo "qemu-ans-broken: $(n): $$(grep -m1 "FAIL self-test" \
			build/ansbroken-qemu-$(n).log | sed "s/^HVLOG: anstest: //")"; fi;)
	@echo "qemu-ans-broken: all $(words $(ANS_BROKEN)) failed, as they must"

clean:
	rm -rf build
