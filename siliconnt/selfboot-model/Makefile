# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/selfboot.c (test.c): where the firmware image, the boot
# arguments and the boot disk go in guest memory, what is checked of the two files, the boot
# arguments and the device tree the firmware is handed; and for src/selfboot_run.c
# (run_test.c): reading the two files, setting up the hypervisor and entering the firmware;
# and for src/hv/hv_layout.c (layout_test.c, and run_test.c has it under selfboot_run.c):
# where the device tree says the devices of the machine are, and whether the machine has
# something where the guest finds its GIC.
# The real files, compiled for the build machine against stand-in headers, with device trees
# that the tests make.
#
#   make        build them and run them; then check_adt.py reads the device tree test.c wrote
#               with the parser of the proxy client (needs the Python of the proxy client)
#   make run    only the one for src/selfboot_run.c
#   make layout only the one for src/hv/hv_layout.c
#   make broken the last two against src/hv/hv_layout.c and src/selfboot_run.c with one thing
#               wrong in them at a time: they must fail on each (broken.sh)

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -I. -Ibuild

.PHONY: all run layout broken clean

all: build/model run layout broken
	build/model build/adt-out.bin
	$(PYTHON) check_adt.py build/adt-out.bin

run: build/run-model
	build/run-model

layout: build/layout-model
	build/layout-model

broken: build/layout-model build/run-model
	bash broken.sh

# Copies, so that their #include "..." lines find the stand-ins in this directory
build/selfboot.c: $(SRC)/selfboot.c
	@mkdir -p build
	cp $< $@

build/selfboot.h: $(SRC)/selfboot.h
	@mkdir -p build
	cp $< $@

build/adt.h: $(SRC)/adt.h
	@mkdir -p build
	cp $< $@

build/xnuboot.h: $(SRC)/xnuboot.h
	@mkdir -p build
	cp $< $@

build/model: test.c model.c $(wildcard *.h) build/selfboot.c build/selfboot.h build/adt.h build/xnuboot.h
	$(CC) $(CFLAGS) -o $@ test.c model.c build/selfboot.c

build/selfboot_run.c: $(SRC)/selfboot_run.c
	@mkdir -p build
	cp $< $@

build/mcc.h: $(SRC)/mcc.h
	@mkdir -p build
	cp $< $@

build/arm_cpu_regs.h: $(SRC)/arm_cpu_regs.h
	@mkdir -p build
	cp $< $@

# selfboot_run.c includes "../build/build_cfg.h": from build/, that is this one
build/build_cfg.h:
	@mkdir -p build
	echo "#define CHAINLOADING" > $@

build/selfboot_files.c: $(SRC)/selfboot_files.c
	@mkdir -p build
	cp $< $@

build/hv_layout.c: $(SRC)/hv/hv_layout.c
	@mkdir -p build
	cp $< $@

# What the stand-in hv.h of this directory has of the real one, as it is there
build/hv_layout.h: $(SRC)/hv/hv.h
	@mkdir -p build
	grep -E '^#define HV_VGIC_(DIST|REDIST)_(BASE|SIZE) ' $< > $@
	sed -n '/^struct hv_io_ranges {/,/^};/p' $< >> $@

# The stand-in for smp.h: how many cores m1n1 has room for
build/smp.h: $(SRC)/smp_asm.h
	@mkdir -p build
	grep '^#define MAX_CPUS ' $< > $@

LAYOUT_COPIES := build/hv_layout.c build/hv_layout.h build/smp.h build/adt.h build/xnuboot.h

build/layout-model: layout_test.c model.c $(wildcard *.h) $(LAYOUT_COPIES)
	$(CC) $(CFLAGS) -o $@ layout_test.c model.c build/hv_layout.c

RUN_COPIES := build/selfboot_files.c build/selfboot.c build/selfboot_run.c build/selfboot.h build/adt.h build/xnuboot.h
RUN_COPIES += build/mcc.h build/arm_cpu_regs.h build/build_cfg.h $(LAYOUT_COPIES)

build/run-model: run_test.c model.c $(wildcard *.h) $(RUN_COPIES)
	$(CC) $(CFLAGS) -o $@ run_test.c model.c build/selfboot.c build/selfboot_files.c build/selfboot_run.c build/hv_layout.c

clean:
	rm -rf build
