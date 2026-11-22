# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for what the hypervisor does without a host (SiliconNT
# docs/findings/0068, 0069): src/hv/hv_power.c with src/smc.c, src/rtkit.c, src/asc.c,
# src/spmi.c and src/wdt.c under it (power_test.c), src/hv/hv_stop.c with src/iodev.c
# (stop_test.c), and fb_init_unbuffered() of src/fb.c (fb_test.c). They compile the real
# files for the build machine against stand-in headers and play the hardware around them.
#
#   make         build them and run them, and the broken ones
#   make power   only the one for restarting and powering off
#   make stop    only the one for stopping the guest
#   make fb      only the one for the framebuffer console
#   make broken  the three against the real files with one thing wrong in them at a time
#                (broken.mk): each must fail, with a failed check or by hanging

CC := gcc
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -Wno-multichar -I. -Ibuild
# How long a test may take before it is taken to hang: the longest takes a few seconds
TIMEOUT := 20

.PHONY: all power stop fb broken clean

all: power stop fb broken

power: build/power-model
	build/power-model

stop: build/stop-model
	build/stop-model

fb: build/fb-model
	build/fb-model

# Copies, so that their #include "..." lines find the stand-ins of this directory
COPIES := hv/hv_power.c hv/hv_stop.c smc.c rtkit.c asc.c spmi.c iodev.c fb.c \
	smc.h asc.h rtkit.h spmi.h wdt.h adt.h dart.h iova.h sart.h iodev.h uartproxy.h fb.h \
	xnuboot.h payload.h
$(foreach f,$(COPIES),$(eval build/$(notdir $(f)): $(SRC)/$(f) ; @mkdir -p build && cp $$< $$@))

# wdt.c keeps where the watchdog is from when m1n1 starts: the test makes it forget, for a
# machine where m1n1 found none
build/wdt.c: $(SRC)/wdt.c
	@mkdir -p build
	cp $< $@
	echo 'void wdt_forget(void) { wdt_base = 0; }' >> $@

# What the stand-in hv.h has of the real one, as it is there, after what it needs
build/hv_nohost.h: $(SRC)/hv/hv.h build/exc_info.h
	@mkdir -p build
	printf '#include "exc_info.h"\n#include "uartproxy.h"\n' > $@
	sed -n '/^typedef enum _hv_entry_type {/,/^} hv_entry_type;/p' $< >> $@
	grep -E '^(bool hv_has_host|void hv_stop_if_no_host|void hv_power_(reset|off))\(' $< >> $@
	test $$(grep -c '^\(bool\|void\) hv_' $@) = 4

build/exc_info.h: $(SRC)/exception.h
	@mkdir -p build
	printf '#include "types.h"\n' > $@
	sed -n '/^struct exc_info {/,/^};/p' $< >> $@

HEADERS := $(wildcard *.h) $(addprefix build/,$(notdir $(filter %.h,$(COPIES)))) \
	build/hv_nohost.h build/exc_info.h
POWER_FILES := build/smc.c build/rtkit.c build/asc.c build/spmi.c build/wdt.c
STOP_FILES := build/iodev.c

build/power-model: power_test.c fake_adt.c build/hv_power.c $(POWER_FILES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ power_test.c fake_adt.c build/hv_power.c $(POWER_FILES)

build/stop-model: stop_test.c build/hv_stop.c $(STOP_FILES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ stop_test.c build/hv_stop.c $(STOP_FILES)

build/fb-model: fb_test.c build/fb.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ fb_test.c build/fb.c

# ---- one thing wrong at a time ----

include broken.mk

# $(1): the copy that is broken, $(2): the test, $(3): the other files of that test
define BROKEN_RULES
build/$(1)-broken-%.c: build/$(1).c broken.mk
	sed -e '$$(BREAK_$$*)' $$< > $$@
	! cmp -s $$< $$@

build/$(2)-broken-$(1)-%: $(2)_test.c build/$(1)-broken-%.c $(3) $$(HEADERS)
	$$(CC) $$(CFLAGS) -Wno-error -o $$@ $(2)_test.c build/$(1)-broken-$$*.c $(3)
endef

$(eval $(call BROKEN_RULES,hv_power,power,fake_adt.c $(POWER_FILES)))
$(eval $(call BROKEN_RULES,smc,power,fake_adt.c build/hv_power.c build/rtkit.c build/asc.c build/spmi.c build/wdt.c))
$(eval $(call BROKEN_RULES,rtkit,power,fake_adt.c build/hv_power.c build/smc.c build/asc.c build/spmi.c build/wdt.c))
$(eval $(call BROKEN_RULES,hv_stop,stop,$(STOP_FILES)))
$(eval $(call BROKEN_RULES,fb,fb,))

BROKEN_BINS := $(BROKEN_HV_POWER:%=build/power-broken-hv_power-%) \
	$(BROKEN_SMC:%=build/power-broken-smc-%) $(BROKEN_RTKIT:%=build/power-broken-rtkit-%) \
	$(BROKEN_HV_STOP:%=build/stop-broken-hv_stop-%) $(BROKEN_FB:%=build/fb-broken-fb-%)

# Each fails a check, or hangs where the real one has a timeout
broken: $(BROKEN_BINS)
	@for b in $(BROKEN_BINS); do \
		timeout $(TIMEOUT) $$b > $$b.log 2>&1; ret=$$?; \
		if [ $$ret = 0 ]; then echo "$$b: the test passes and must not"; exit 1; fi; \
		if [ $$ret = 124 ]; then echo "$$(basename $$b): hangs"; continue; fi; \
		grep -q -m1 '^FAIL' $$b.log || { echo "$$b: ended without a failed check"; exit 1; }; \
		echo "$$(basename $$b): $$(grep -m1 '^FAIL' $$b.log)"; \
	done
	@echo "broken: all $(words $(BROKEN_BINS)) failed, as they must"

clean:
	rm -rf build
