# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/hv/hv_vgic.c and src/hv/hv_wfi.c (test.c) and for
# src/hv/hv_psci.c (psci_test.c), src/hv/hv_pmu.c (pmu_test.c), the page tables of
# src/hv/hv_vm.c (pt_test.c) and src/hv/hv_xtrack.c (xtrack_test.c). They compile the real files for the build machine against
# stand-in headers and play the hardware, the guest and the host around them.
#
#   make        build them and run them
#   make pt     only the page table one
#   make xtrack only the execution tracking one
#   make dbg    only the one for the debug registers, src/hv/hv_dbg.c (dbg_test.c)
#   make psci   only the PSCI one, and test_hv_events.py: that the host has a handler for
#               every event the hypervisor sends (needs the Python of the proxy client)
#   make tsan   test.c under ThreadSanitizer, with fewer rounds

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all psci pmu pt xtrack dbg tsan clean

all: build/model psci pmu pt xtrack dbg
	build/model $(ROUNDS)

# The breakpoint and watchpoint registers of the guest, src/hv/hv_dbg.c
dbg: build/dbg-model
	build/dbg-model

build/hv_dbg.c: $(SRC)/hv/hv_dbg.c
	@mkdir -p build
	cp $< $@

build/dbg-model: dbg_test.c $(wildcard *.h) build/hv_dbg.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -o $@ dbg_test.c build/hv_dbg.c

# The stage 2 page tables of src/hv/hv_vm.c
pt: build/pt-model
	build/pt-model

# The page table half of hv_vm.c, everything before the MMIO emulation, which is ARM
# assembly in places. Without its #include lines: pt_test.c stands in for those
build/hv_vm_pt.inc: $(SRC)/hv/hv_vm.c
	@mkdir -p build
	grep -q '^#define CHECK_RN' $<
	sed -n '1,/^#define CHECK_RN/p' $< | sed -e '$$d' -e '/^#include /d' > $@

build/hv_xpage.h: $(SRC)/hv/hv.h
	@mkdir -p build
	grep -E '^#define HV_X(PAGE|TRACK)_' $< > $@

# Execution tracking, src/hv/hv_xtrack.c, with the real hv_pmu.c and page states of its own
xtrack: build/xtrack-model
	build/xtrack-model

build/hv_xtrack.inc: $(SRC)/hv/hv_xtrack.c
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

build/xtrack-model: xtrack_test.c $(wildcard *.h) build/hv_xtrack.inc build/hv_xpage.h build/hv_pmu.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -o $@ xtrack_test.c build/hv_pmu.c

build/pt-model: pt_test.c $(wildcard *.h) build/hv_vm_pt.inc build/hv_xpage.h build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -o $@ pt_test.c

# make pmu PMU_FILES="a.efi b.exe" also counts the accesses in those files
pmu: build/pmu-model
	build/pmu-model $(PMU_FILES)

build/hv_pmu.c: $(SRC)/hv/hv_pmu.c
	@mkdir -p build
	cp $< $@

build/pmu-model: pmu_test.c $(wildcard *.h) build/hv_pmu.c
	$(CC) $(CFLAGS) -o $@ pmu_test.c build/hv_pmu.c

psci: build/psci-model
	build/psci-model
	$(PYTHON) test_hv_events.py

tsan: build/model-tsan
	build/model-tsan 3000

# Copies, so that their #include "..." lines find the stand-ins in this directory
build/hv_vgic.c: $(SRC)/hv/hv_vgic.c
	@mkdir -p build
	cp $< $@

build/hv_wfi.c: $(SRC)/hv/hv_wfi.c
	@mkdir -p build
	cp $< $@

build/hv_psci.c: $(SRC)/hv/hv_psci.c
	@mkdir -p build
	cp $< $@

build/arm_cpu_regs.h: $(SRC)/arm_cpu_regs.h
	@mkdir -p build
	cp $< $@

build/psci-model: psci_test.c $(wildcard *.h) build/hv_psci.c
	$(CC) $(CFLAGS) -o $@ psci_test.c build/hv_psci.c

build/model: test.c $(wildcard *.h) build/hv_vgic.c build/hv_wfi.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -o $@ test.c build/hv_vgic.c build/hv_wfi.c

build/model-tsan: test.c $(wildcard *.h) build/hv_vgic.c build/hv_wfi.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o $@ test.c build/hv_vgic.c build/hv_wfi.c

clean:
	rm -rf build
