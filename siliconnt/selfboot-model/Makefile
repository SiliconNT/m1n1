# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model test for src/selfboot.c: where the firmware image, the boot arguments and
# the boot disk go in guest memory, what is checked of the two files, the boot arguments and
# the device tree the firmware is handed. The real file, compiled for the build machine
# against stand-in headers, with a device tree that test.c makes.
#
#   make        build it and run it; then check_adt.py reads the device tree it wrote with
#               the parser of the proxy client (needs the Python of the proxy client)

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -I. -Ibuild

.PHONY: all clean

all: build/model
	build/model build/adt-out.bin
	$(PYTHON) check_adt.py build/adt-out.bin

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

build/model: test.c $(wildcard *.h) build/selfboot.c build/selfboot.h build/adt.h build/xnuboot.h
	$(CC) $(CFLAGS) -o $@ test.c build/selfboot.c

clean:
	rm -rf build
