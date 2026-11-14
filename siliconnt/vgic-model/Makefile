# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/hv/hv_vgic.c and src/hv/hv_wfi.c (test.c) and for
# src/hv/hv_psci.c (psci_test.c). They compile the real files for the build machine against
# stand-in headers and play the hardware, the guest and the host around them.
#
#   make        build them and run them
#   make psci   only the PSCI one, and test_hv_events.py: that the host has a handler for
#               every event the hypervisor sends (needs the Python of the proxy client)
#   make tsan   test.c under ThreadSanitizer, with fewer rounds

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all psci tsan clean

all: build/model psci
	build/model $(ROUNDS)

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
