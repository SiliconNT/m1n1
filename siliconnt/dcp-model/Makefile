# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model test for the display: src/display.c with src/dcp.c, src/dcp_iboot.c, src/afk.c,
# src/dcp/parser.c, src/rtkit.c, src/asc.c, src/dart.c, src/iova.c and src/firmware.c under it.
# The real files, compiled for the build machine against stand-in headers (this directory's, and
# those of nohost-model), on a machine that machine.c plays.
#
#   make         build it and run it, and the two below
#   make run     only run it
#   make same    what must not have changed since BASE, in both: every register written, every
#                message, everything printed (transcripts of both are compared)
#   make broken  the test against the real files with one thing wrong in them at a time
#                (broken.mk): each must fail, with a failed check or by hanging (with -j, or it
#                takes ten minutes)

CC := gcc
SRC := ../../src
# The commit before the display was taught to change its format
BASE := 4205170a
CFLAGS := -O2 -g -Wall -Werror -Wno-multichar -Wno-stringop-truncation -Wno-unused-function \
	-include stdlib.h -I. -I../nohost-model
# How long a run may take before it is taken to hang: one takes a few seconds
TIMEOUT := 120

.PHONY: all run same broken
.SECONDARY:

all: run same broken

run: build/dcp-model
	build/dcp-model

FILES := display dcp dcp_iboot afk rtkit asc dart iova firmware dcp/parser
COPIES := $(FILES:%=%.c) display.h dcp.h dcp_iboot.h afk.h rtkit.h asc.h dart.h iova.h sart.h \
	firmware.h adt.h soc.h fb.h pmgr.h smc.h xnuboot.h devicetree.h \
	dcp/dpav_ep.h dcp/dptx_port_ep.h dcp/system_ep.h dcp/dptx_phy.h dcp/parser.h

# $(1): the directory under build/ that a tree of copies goes to. Copies, so that their
# #include "..." lines find the stand-ins.
define TREE_RULES
build/$(1)/src/types.h: types.h
	@mkdir -p $$(@D)
	echo '#include "../../../types.h"' > $$@
build/$(1)/src/utils.h: utils.h
	@mkdir -p $$(@D)
	echo '#include "../../../utils.h"' > $$@
build/$(1)/config.h: config.h
	@mkdir -p $$(@D)
	echo '#include "../../config.h"' > $$@
build/$(1)/build/build_cfg.h:
	@mkdir -p $$(@D)
	touch $$@
$(1)_HEADERS := $$(addprefix build/$(1)/src/,types.h utils.h $$(filter %.h,$$(COPIES))) \
	build/$(1)/config.h build/$(1)/build/build_cfg.h
endef

$(eval $(call TREE_RULES,new))
$(eval $(call TREE_RULES,base))
$(foreach f,$(COPIES),$(eval build/new/src/$(f): $(SRC)/$(f) ; @mkdir -p $$(@D) && cp $$< $$@))
$(foreach f,$(COPIES),$(eval build/base/src/$(f): ; @mkdir -p $$(@D) && git show $(BASE):src/$(f) > $$@))

MODEL := machine fake_adt dcp_test
MODEL_HEADERS := $(wildcard *.h libfdt/*.h ../nohost-model/*.h)
MODEL_OBJS := $(MODEL:%=build/%.o)
NEW_OBJS := $(FILES:%=build/new/%.o)
BASE_OBJS := $(FILES:%=build/base/%.o)

build/new/%.o: build/new/src/%.c $(new_HEADERS) $(MODEL_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ibuild/new/src -c -o $@ $<

build/base/%.o: build/base/src/%.c $(base_HEADERS) $(MODEL_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ibuild/base/src -c -o $@ $<

$(MODEL_OBJS): build/%.o: %.c $(new_HEADERS) $(MODEL_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Ibuild/new/src -c -o $@ $<

build/dcp-model: $(MODEL_OBJS) $(NEW_OBJS)
	$(CC) -o $@ $^

build/dcp-model-base: $(MODEL_OBJS) $(BASE_OBJS)
	$(CC) -o $@ $^

# ---- the same as before ----

same: build/dcp-model build/dcp-model-base
	@for c in $$(build/dcp-model --same-cases); do \
		build/dcp-model-base --transcript $$c > build/$$c.base || exit 1; \
		build/dcp-model --transcript $$c > build/$$c.new || exit 1; \
		test $$(wc -l < build/$$c.base) -gt 2 || { echo "$$c: no transcript"; exit 1; }; \
		cmp build/$$c.base build/$$c.new || \
			{ diff build/$$c.base build/$$c.new | head -20; exit 1; }; \
		echo "$$c: the same, $$(wc -l < build/$$c.new) lines"; \
	done
	@echo "same: all as at $(BASE)"

# ---- one thing wrong at a time ----

include broken.mk

# $(1): the file that is broken
define BROKEN_RULES
build/new/src/broken-$(1)-%.c: build/new/src/$(1).c broken.mk
	sed -e '$$(BREAK_$$*)' $$< > $$@
	! cmp -s $$< $$@

build/broken-$(1)-%: build/new/src/broken-$(1)-%.c $$(MODEL_OBJS) $$(NEW_OBJS)
	$$(CC) $$(CFLAGS) -w -Ibuild/new/src -o $$@ $$(filter-out build/new/$(1).o,$$^)

BROKEN_BINS += $$(BROKEN_$(1):%=build/broken-$(1)-%)
endef

$(foreach f,afk dcp_iboot display firmware dcp rtkit dart,$(eval $(call BROKEN_RULES,$(f))))

# Each fails a check, or hangs where the real one has a timeout
BROKEN_LOGS := $(BROKEN_BINS:%=%.log)

$(BROKEN_LOGS): %.log: %
	@timeout $(TIMEOUT) $< > $@.out 2>&1; ret=$$?; \
	if [ $$ret = 0 ]; then echo "$<: the test passes and must not"; exit 1; fi; \
	if [ $$ret != 124 ] && ! grep -q '^FAIL' $@.out; then \
		echo "$<: ended without a failed check"; exit 1; \
	fi; \
	mv $@.out $@

broken: $(BROKEN_LOGS)
	@for b in $(BROKEN_BINS); do \
		echo "$$(basename $$b): $$(grep -m1 '^FAIL' $$b.log || echo hangs)"; \
	done
	@echo "broken: all $(words $(BROKEN_BINS)) failed, as they must"
