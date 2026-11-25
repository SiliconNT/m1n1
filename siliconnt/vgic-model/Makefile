# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/hv/hv_vgic.c and src/hv/hv_wfi.c (test.c), for the masking
# functions of src/aic.c (aic_test.c, with the stand-in headers of aic/) and for
# src/hv/hv_psci.c (psci_test.c), src/hv/hv_pmu.c (pmu_test.c), the page tables of
# src/hv/hv_vm.c (pt_test.c), src/hv/hv_xtrack.c (xtrack_test.c) and src/hv/hv_xinsn.c
# (xinsn_test.c, test_xinsn.py). They compile the real files for the build machine against
# stand-in headers and play the hardware, the guest and the host around them.
#
#   make        build them and run them
#   make pt     only the page table one
#   make pt-broken  ...against hv_vm.c with one thing wrong at a time in where the tables come
#               from or in the check of the range of CTRR: it must fail on each
#   make xtrack only the execution tracking one
#   make xtrack-broken  ...against hv_xtrack.c with one thing wrong in it at a time: it must
#               fail on each
#   make xinsn  only the one for the instruction decoder of execution tracking
#   make dbg    only the one for the debug registers, src/hv/hv_dbg.c (dbg_test.c)
#   make aic    only the one for aic_mask(), aic_unmask() and aic_mask_all() of src/aic.c
#   make psci   only the PSCI one, and test_hv_events.py: that the host has a handler for
#               every event the hypervisor sends (needs the Python of the proxy client)
#   make tsan   test.c under ThreadSanitizer, with fewer rounds
#   make vgic-broken  test.c against hv_vgic.c with one thing wrong in how it forwards AIC
#               lines at a time: it must fail on each

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all psci pmu pt pt-broken xtrack xtrack-broken xinsn dbg aic tsan vgic-broken clean

all: build/model psci pmu pt pt-broken xinsn xtrack xtrack-broken dbg aic vgic-broken
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

# Execution tracking, src/hv/hv_xtrack.c, with the real hv_pmu.c and hv_xinsn.c and page
# states of its own: a random guest, see xtrack_test.c
xtrack: build/xtrack-model
	build/xtrack-model $(XTRACK_STEPS)

build/hv_xtrack.inc: $(SRC)/hv/hv_xtrack.c
	@mkdir -p build
	sed -e '/^#include /d' $< > $@

XTRACK_DEPS := xtrack_test.c $(wildcard *.h) build/hv_xpage.h build/hv_pmu.c build/hv_xinsn.c \
	build/hv_xinsn.h build/arm_cpu_regs.h

build/xtrack-model: $(XTRACK_DEPS) build/hv_xtrack.inc
	$(CC) $(CFLAGS) -o $@ xtrack_test.c build/hv_pmu.c build/hv_xinsn.c

# hv_xtrack.c with one thing wrong in it at a time. The test must fail on each: that is what
# shows that it looks. Every change must change something (cmp), or the source has moved on
include ../xtrack-broken.mk

build/hv_xtrack-broken-%.inc: build/hv_xtrack.inc Makefile ../xtrack-broken.mk
	sed -e '$(BREAK_$*)' $< > $@
	! cmp -s $< $@

build/xtrack-broken-%: $(XTRACK_DEPS) build/hv_xtrack-broken-%.inc
	$(CC) $(CFLAGS) -Wno-error -DXTRACK_INC='"hv_xtrack-broken-$*.inc"' -o $@ xtrack_test.c build/hv_pmu.c build/hv_xinsn.c

xtrack-broken: $(BROKEN:%=build/xtrack-broken-%)
	@for n in $(BROKEN); do \
		if timeout 300 build/xtrack-broken-$$n > build/xtrack-broken-$$n.log 2>&1; then \
			echo "xtrack-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL' build/xtrack-broken-$$n.log || \
			{ echo "xtrack-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(BROKEN),echo "xtrack-broken: $(n) $(BREAK_$(n)_IS): $$(grep -m1 '^FAIL' build/xtrack-broken-$(n).log)";)
	@echo "xtrack-broken: all $(words $(BROKEN)) failed, as they must"

build/pt-model: pt_test.c $(wildcard *.h) build/hv_vm_pt.inc build/hv_xpage.h build/arm_cpu_regs.h \
	build/hv_aicmap.h
	$(CC) $(CFLAGS) -o $@ pt_test.c

# Every change must change something (cmp), or the source has moved on
PT_BROKEN := small-chunk first-level-not-rounded never-reused l3-not-given-back \
	kboot-end page-step flag-ignored order-not-checked end-not-checked zero-not-special \
	refusal-ignored
BREAK_PT_small-chunk := s/HV_PT_CHUNK 0x4000UL/HV_PT_CHUNK 0x2000UL/
BREAK_PT_first-level-not-rounded := s/hv_Ltop = heapblock_alloc_aligned(ALIGN_UP(size, HV_PT_CHUNK),/hv_Ltop = heapblock_alloc_aligned(size,/
BREAK_PT_never-reused := s/^    if (!\*spares) {$$/    if (1) {/
BREAK_PT_l3-not-given-back := /^    hv_pt_table_free(l3, ENTRIES_PER_L3_TABLE/d
BREAK_PT_kboot-end := s/ALIGN_DOWN(upper, PAGE_SIZE) + PAGE_SIZE;/ALIGN_UP(upper, PAGE_SIZE);/
BREAK_PT_page-step := s/addr += BIT(VADDR_L4_OFFSET_BITS)/addr += PAGE_SIZE/
BREAK_PT_flag-ignored := s/if (cpu_features->apple_sysregs_unlocked)/if (0)/
BREAK_PT_order-not-checked := s/if (upper < lower || /if (/
BREAK_PT_end-not-checked := s/ || upper >= BIT(52)) {/) {/
BREAK_PT_zero-not-special := s/if (!lower \&\& !upper) {/if (0) {/
BREAK_PT_refusal-ignored := s/if (hv_pt_walk(addr)) {/if (0) {/

build/hv_vm_pt-broken-%.inc: build/hv_vm_pt.inc Makefile
	sed -e '$(BREAK_PT_$*)' $< > $@
	! cmp -s $< $@

build/pt-broken-%: build/pt-model build/hv_vm_pt-broken-%.inc
	$(CC) $(CFLAGS) -Wno-error -DPT_INC='"hv_vm_pt-broken-$*.inc"' -o $@ pt_test.c

pt-broken: $(PT_BROKEN:%=build/pt-broken-%)
	@for n in $(PT_BROKEN); do \
		if timeout 120 build/pt-broken-$$n > build/pt-broken-$$n.log 2>&1; then \
			echo "pt-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL' build/pt-broken-$$n.log || \
			{ echo "pt-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(PT_BROKEN),echo "pt-broken: $(n): $$(grep -m1 '^FAIL' build/pt-broken-$(n).log)";)
	@echo "pt-broken: all $(words $(PT_BROKEN)) failed, as they must"

# The instruction decoder of execution tracking, src/hv/hv_xinsn.c: where an instruction reads
# or writes (xinsn_test.c), and every word against the disassembler of binutils (test_xinsn.py)
xinsn: build/xinsn-model
	build/xinsn-model
	python3 test_xinsn.py build/xinsn-model $(XINSN_WORDS)

build/hv_xinsn.c: $(SRC)/hv/hv_xinsn.c
	@mkdir -p build
	cp $< $@

build/hv_xinsn.h: $(SRC)/hv/hv_xinsn.h
	@mkdir -p build
	cp $< $@

build/xinsn-model: xinsn_test.c $(wildcard *.h) build/hv_xinsn.c build/hv_xinsn.h
	$(CC) $(CFLAGS) -o $@ xinsn_test.c build/hv_xinsn.c

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

# AIC as hv_vgic.c knows it: the structure aic_init() fills in, the event register. test.c
# has an AIC of its own behind the functions.
build/aic.h: $(SRC)/aic.h
	@mkdir -p build
	cp $< $@

build/aic_regs.h: $(SRC)/aic_regs.h
	@mkdir -p build
	cp $< $@

# src/aic.c against the stand-ins of aic/, which come first: it is copied, so that its
# #include "..." lines do not find the real headers next to it
aic: build/aic-model
	build/aic-model

build/aic.c: $(SRC)/aic.c
	@mkdir -p build
	cp $< $@

build/aic-model: aic_test.c $(wildcard aic/*.h) types.h build/aic.c build/aic.h build/aic_regs.h
	$(CC) -O2 -g -Wall -Werror -Wno-unused-parameter -Iaic -I. -Ibuild -o $@ aic_test.c build/aic.c

# The table of AIC lines and SPIs, as the real hv.h has it; test.c makes one of its own
build/hv_aicmap.h: $(SRC)/hv/hv.h
	@mkdir -p build
	grep -E '^#define HV_AICMAP_[A-Z_]+ ' $< > $@
	sed -n '/^struct hv_aicmap {/,/^};/p' $< >> $@

MODEL_DEPS := test.c $(wildcard *.h) build/hv_wfi.c build/arm_cpu_regs.h build/aic.h \
	build/aic_regs.h build/hv_aicmap.h

build/psci-model: psci_test.c $(wildcard *.h) build/hv_psci.c
	$(CC) $(CFLAGS) -o $@ psci_test.c build/hv_psci.c

build/model: $(MODEL_DEPS) build/hv_vgic.c
	$(CC) $(CFLAGS) -o $@ test.c build/hv_vgic.c build/hv_wfi.c

build/model-tsan: $(MODEL_DEPS) build/hv_vgic.c
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o $@ test.c build/hv_vgic.c build/hv_wfi.c

# hv_vgic.c with one thing wrong in how it forwards AIC lines, at a time. test.c must fail on
# each, in its AIC tests (build/model aic runs only those). Every change must change
# something (cmp), or the source has moved on.
VGIC_BROKEN := unmask-on-enable mask-on-disable unmask-when-ended mask-when-ended-disabled \
	forward-any-state wrong-spi no-die no-mask-all end-not-seen last-line-missed \
	unmask-in-flight no-table-check late-is-stray stray-without-limit spurious-not-reset \
	not-enabled enabled-before-masking not-restored not-masked-after
BREAK_VGIC_unmask-on-enable := /^            aic_unmask(vgic_aicmap.line\[k\]);$$/d
BREAK_VGIC_mask-on-disable := /^            aic_mask(vgic_aicmap.line\[k\]);$$/d
BREAK_VGIC_unmask-when-ended := /^        aic_unmask(vgic_aicmap.line\[k\]);$$/d
BREAK_VGIC_mask-when-ended-disabled := s/if (vgic_load(vgic_dist.enable\[intid \/ 32\]) \& BIT(intid % 32)) {/if (1) {/
BREAK_VGIC_forward-any-state := /if (vgic_lines\[k\] == VGIC_LINE_MASKED)/,+1d;s/if (vgic_lines\[k\] != VGIC_LINE_WAITING)/if (0)/
BREAK_VGIC_late-is-stray := s/return VGIC_EVENT_LATE;/return VGIC_EVENT_STRAY;/
BREAK_VGIC_stray-without-limit := s/if (VGIC_PERCPU(unmapped) >= VGIC_UNMAPPED_LIMIT)/if (0)/
BREAK_VGIC_spurious-not-reset := /VGIC_PERCPU(spurious_run) = 0;/d
BREAK_VGIC_not-enabled := s/aic_write_config(vgic_aic_config | AIC2_CONFIG_ENABLE);/;/
BREAK_VGIC_enabled-before-masking := /aic_mask_all(); \/\/ before it is enabled/d;s/aic_write_config(vgic_aic_config | AIC2_CONFIG_ENABLE);/&\n    aic_mask_all();/
BREAK_VGIC_not-restored := s/aic_write_config(vgic_aic_config);/;/
BREAK_VGIC_not-masked-after := /aic_mask_all(); \/\/ as it was before/d
BREAK_VGIC_wrong-spi := s/vgic_pend_spi(HV_AICMAP_FIRST_SPI + k, true);/vgic_pend_spi(HV_AICMAP_FIRST_SPI + k + 1, true);/
BREAK_VGIC_no-die := s/FIELD_GET(AIC_EVENT_DIE, event) \* aic->max_irq +/0 * aic->max_irq +/
BREAK_VGIC_no-mask-all := /^        aic_mask_all();$$/d
BREAK_VGIC_end-not-seen := /vgic_line_ended(FIELD_GET(ICH_LR_VINTID, vgic_read_lr(i)));/d
BREAK_VGIC_last-line-missed := s/u32 lo = 0, hi = vgic_aicmap.count;/u32 lo = 0, hi = vgic_aicmap.count - 1;/
BREAK_VGIC_unmask-in-flight := s/if (enable \&\& vgic_lines\[k\] == VGIC_LINE_MASKED) {/if (enable) {/
BREAK_VGIC_no-table-check := s/    vgic_aicmap_ok = hv_aicmap(\&vgic_aicmap) >= 0;/    vgic_aicmap_ok = true; hv_aicmap(\&vgic_aicmap);/

build/hv_vgic-broken-%.c: build/hv_vgic.c Makefile
	sed -e '$(BREAK_VGIC_$*)' $< > $@
	! cmp -s $< $@

build/model-broken-%: $(MODEL_DEPS) build/hv_vgic-broken-%.c
	$(CC) $(CFLAGS) -Wno-error -o $@ test.c build/hv_vgic-broken-$*.c build/hv_wfi.c

vgic-broken: $(VGIC_BROKEN:%=build/model-broken-%)
	@for n in $(VGIC_BROKEN); do \
		if timeout 60 build/model-broken-$$n aic > build/model-broken-$$n.log 2>&1; then \
			echo "vgic-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL\|^CHECK FAILED' build/model-broken-$$n.log || \
			{ echo "vgic-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(VGIC_BROKEN),echo "vgic-broken: $(n): $$(grep -m1 '^FAIL\|^CHECK FAILED' build/model-broken-$(n).log)";)
	@echo "vgic-broken: all $(words $(VGIC_BROKEN)) failed, as they must"

clean:
	rm -rf build
