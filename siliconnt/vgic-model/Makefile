# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model test for src/hv/hv_vgic.c and src/hv/hv_wfi.c. It compiles the real files
# for the build machine against stand-in headers and plays the hardware and the guest around
# them; see test.c.
#
#   make        build it and run it
#   make tsan   the same under ThreadSanitizer, with fewer rounds

CC := gcc
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all tsan clean

all: build/model
	build/model $(ROUNDS)

tsan: build/model-tsan
	build/model-tsan 3000

# Copies, so that their #include "..." lines find the stand-ins in this directory
build/hv_vgic.c: $(SRC)/hv/hv_vgic.c
	@mkdir -p build
	cp $< $@

build/hv_wfi.c: $(SRC)/hv/hv_wfi.c
	@mkdir -p build
	cp $< $@

build/arm_cpu_regs.h: $(SRC)/arm_cpu_regs.h
	@mkdir -p build
	cp $< $@

build/model: test.c $(wildcard *.h) build/hv_vgic.c build/hv_wfi.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -o $@ test.c build/hv_vgic.c build/hv_wfi.c

build/model-tsan: test.c $(wildcard *.h) build/hv_vgic.c build/hv_wfi.c build/arm_cpu_regs.h
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o $@ test.c build/hv_vgic.c build/hv_wfi.c

clean:
	rm -rf build
