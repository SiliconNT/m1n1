# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/hv/hv_vgic.c and src/hv/hv_wfi.c (test.c), for the masking
# functions of src/aic.c (aic_test.c, with the stand-in headers of aic/) and for
# src/hv/hv_psci.c (psci_test.c), src/hv/hv_pmu.c (pmu_test.c), the page tables of
# src/hv/hv_vm.c (pt_test.c), src/hv/hv_xtrack.c (xtrack_test.c) and src/hv/hv_xinsn.c
# (xinsn_test.c, test_xinsn.py), and src/hv/hv.c with src/smp.c (cpus_test.c). They compile
# the real files for the build machine against stand-in headers and play the hardware, the
# guest and the host around them.
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
#   make psci-broken  the PSCI one against hv_psci.c with one thing wrong in CPU_ON at a
#               time: it must fail on each
#   make cpus   only the one for how the cores get into EL2 and into the guest: src/hv/hv.c and
#               src/smp.c, and two functions of src/memory.c (cpus_test.c, with what is in cpus/)
#   make cpus-broken  ...against one of those with one thing wrong at a time: it must fail on
#               each, with a failed check or by hanging
#   make tsan   test.c under ThreadSanitizer, with fewer rounds
#   make vgic-broken  test.c against hv_vgic.c with one thing wrong in how it forwards AIC
#               lines at a time: it must fail on each

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all psci psci-broken pmu pt pt-broken xtrack xtrack-broken xinsn dbg aic cpus cpus-broken tsan vgic-broken clean

all: build/model psci psci-broken cpus cpus-broken pmu pt pt-broken xinsn xtrack xtrack-broken dbg aic vgic-broken
	build/model $(ROUNDS)

# The breakpoint and watchpoint registers of the guest, src/hv/hv_dbg.c
dbg: build/dbg-model
	build/dbg-model

build/hv_dbg.c: $(SRC)/hv/hv_dbg.c
	@mkdir -p build
	cp $< $@

build/dbg-model: dbg_test.c $(wildcard *.h) build/hv_dbg.c build/arm_cpu_regs.h build/hv_aicmap.h
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
	build/hv_xinsn.h build/arm_cpu_regs.h build/hv_aicmap.h

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

build/pmu-model: pmu_test.c $(wildcard *.h) build/hv_pmu.c build/hv_aicmap.h
	$(CC) $(CFLAGS) -o $@ pmu_test.c build/hv_pmu.c

psci: build/psci-model
	build/psci-model
	$(PYTHON) test_hv_events.py

# Every change must change something (cmp), or the source has moved on
PSCI_BROKEN := every-index no-boot-cpu whole-x1 no-aff3 not-already-on wrong-cpu wrong-entry \
	wrong-context failure-ignored unknown-returns no-table start-forgotten
BREAK_PSCI_every-index := s/return cpu == boot_cpu_idx || smp_is_alive(cpu);/return true;/
BREAK_PSCI_no-boot-cpu := s/return cpu == boot_cpu_idx || smp_is_alive(cpu);/return smp_is_alive(cpu);/
BREAK_PSCI_whole-x1 := s/u64 affinity = ctx->regs\[1\] & PSCI_AFFINITY;/u64 affinity = ctx->regs[1];/
BREAK_PSCI_no-aff3 := s/(GENMASK(39, 32) | GENMASK(23, 0))/GENMASK(23, 0)/
BREAK_PSCI_not-already-on := s/if (cpu >= 0 \&\& hv_started_cpus\[cpu\]) {/if (0) {/
BREAK_PSCI_wrong-cpu := s/hv_start_secondary(cpu, /hv_start_secondary(cpu ^ 1, /
BREAK_PSCI_wrong-entry := s/(void \*)ctx->regs\[2\], regs/(void *)ctx->regs[3], regs/
BREAK_PSCI_wrong-context := s/u64 regs\[4\] = {ctx->regs\[3\]};/u64 regs[4] = {ctx->regs[2]};/
BREAK_PSCI_failure-ignored := s/    if (cpu >= 0 \&\& hv_start_secondary(\(.*\)))$$/    if (cpu >= 0 \&\& (hv_start_secondary(\1) || 1))/
BREAK_PSCI_unknown-returns := /^    hv_psci_print_cpus();$$/,/^}/s/hv_psci_stop(ctx, HV_GUEST_HVC);/return -2;/
BREAK_PSCI_no-table := /^    hv_psci_print_cpus();$$/d
BREAK_PSCI_start-forgotten := s/hv_start_secondary(cpu, (void \*)ctx->regs\[2\], regs)/1/

build/hv_psci-broken-%.c: build/hv_psci.c Makefile
	sed -e '$(BREAK_PSCI_$*)' $< > $@
	! cmp -s $< $@

build/psci-broken-%: psci_test.c $(wildcard *.h) build/hv_psci-broken-%.c build/hv_aicmap.h
	$(CC) $(CFLAGS) -Wno-error -o $@ psci_test.c build/hv_psci-broken-$*.c

psci-broken: $(PSCI_BROKEN:%=build/psci-broken-%)
	@for n in $(PSCI_BROKEN); do \
		if timeout 60 build/psci-broken-$$n > build/psci-broken-$$n.log 2>&1; then \
			echo "psci-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL' build/psci-broken-$$n.log || \
			{ echo "psci-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(PSCI_BROKEN),echo "psci-broken: $(n): $$(grep -m1 '^FAIL' build/psci-broken-$(n).log)";)
	@echo "psci-broken: all $(words $(PSCI_BROKEN)) failed, as they must"

# hv.c and smp.c as m1n1 compiles them, with its own headers (BASE_CFLAGS of its Makefile), for
# the build machine: cpus/hw.h, which comes first, takes the place of utils.h
CPUS_CFLAGS := -O2 -g -Wall -Wundef -fno-common -ffreestanding -fno-stack-protector -nostdinc \
	-isystem $(shell $(CC) -print-file-name=include) -isystem ../../sysinc -I$(SRC) -I$(SRC)/hv \
	-include cpus/hw.h

# m1n1's utils.h, but for the instructions of the CPU. Every one that is named has to be there
build/cpus/utils.h: $(SRC)/utils.h
	@mkdir -p build/cpus
	test $$(grep -c -E '^#define (__mrs\(reg\)|__msr\(reg, val\)|sysop\(op\)) ' $<) = 3
	sed -e '/^#define __mrs(reg) /,/^    })$$/c\#define __mrs(reg) mock_mrs(#reg)' \
	    -e '/^#define __msr(reg, val) /,/^    })$$/c\#define __msr(reg, val) mock_msr(#reg, (u64)(val))' \
	    -e 's/^#define sysop(op) .*/#define sysop(op) mock_sysop(op)/' \
	    -e 's/^#define USEC_PER_SEC .*/&\nu64 mock_mrs(const char *name);\nvoid mock_msr(const char *name, u64 val);\nvoid mock_sysop(const char *op);/' \
	    $< > $@
	grep -q 'mock_mrs(const char' $@

build/cpus/hv.c: $(SRC)/hv/hv.c
	@mkdir -p build/cpus
	cp $< $@

build/cpus/smp.c: $(SRC)/smp.c
	@mkdir -p build/cpus
	cp $< $@

build/cpus/stubs.c: cpus/stubs.c
	@mkdir -p build/cpus
	cp $< $@

# The two functions of memory.c that turn on the MMU of a core that has come up
build/cpus/mmu.c: cpus/mmu_head.c $(SRC)/memory.c
	@mkdir -p build/cpus
	cp $< $@
	sed -n '/^static void mmu_secondary_setup(void)/,/^void mmu_shutdown(void)/p' $(SRC)/memory.c | sed '$$d' >> $@
	test $$(grep -c '^\(static void mmu_secondary_setup\|void mmu_init_secondary\|    dc_ivac_range\)(' $@) = 3

build/cpus/%.o: build/cpus/%.c build/cpus/utils.h cpus/hw.h $(wildcard $(SRC)/*.h $(SRC)/hv/*.h)
	$(CC) -c $(CPUS_CFLAGS) -Werror -o $@ $<

build/cpus-model: cpus_test.c build/cpus/hv.o build/cpus/smp.o build/cpus/mmu.o build/cpus/stubs.o
	$(CC) $(CFLAGS) -o $@ $^

cpus: build/cpus-model
	build/cpus-model

# Every change must change something (cmp), or the source has moved on
CPUS_BROKEN_hv := not-started after-vgic before-wfe-mode ready-ignored line-ignored busy-ignored \
	beyond-ignored no-idle-wait not-copied no-leaving exit-kept masks sctlr-kept vbar-kept \
	dbg-kept pmu-kept aborts-kept bit-after-lock started-not-set twice-in not-in-guest-bit \
	log-all no-reason
CPUS_BROKEN_smp := rvbar-ignored start-again no-wait-result beyond-not-counted idle-always \
	invalid-started boot-rvbar-kept boot-rvbar-locked
CPUS_BROKEN_mmu := mmu-again
CPUS_BROKEN := $(CPUS_BROKEN_hv) $(CPUS_BROKEN_smp) $(CPUS_BROKEN_mmu)
BREAK_CPUS_not-started := /^void hv_init(void)/,/^}/{/^    hv_start_cpus();$$/d}
BREAK_CPUS_after-vgic := /^void hv_init(void)/,/^}/{/^    hv_start_cpus();$$/d;s/^    hv_vgic_init();$$/&\n    hv_start_cpus();/}
BREAK_CPUS_before-wfe-mode := /^void hv_init(void)/,/^}/{s/^    smp_set_wfe_mode(true);$$/    hv_start_cpus();\n&/;/^    hv_start_cpus();$$/d}
BREAK_CPUS_ready-ignored := s/    if (!hv_cpus_ready()) {/    if (0) {/
BREAK_CPUS_line-ignored := s/if (line < HV_XTRACK_LINE) {/if (0) {/
BREAK_CPUS_busy-ignored := s/if (!hv_cpu_missing\[cpu\] \&\& .* !hv_cpu_waits(cpu))/if (0)/
BREAK_CPUS_beyond-ignored := s/    if (smp_cpus_beyond()) {/    if (0) {/
BREAK_CPUS_no-idle-wait := s/    if (!hv_cpu_waits(cpu)) {/    if (0) {/
BREAK_CPUS_not-copied := /^    memcpy(e->regs, regs, sizeof(e->regs));$$/d
BREAK_CPUS_no-leaving := s/    if (hv_leaving) {/    if (0) {/
BREAK_CPUS_exit-kept := /^    hv_should_exit\[cpu\] = false;$$/d
BREAK_CPUS_masks := s/(SPSR_D | SPSR_A | SPSR_I | SPSR_F | SPSR_M_EL1H)/(SPSR_I | SPSR_F | SPSR_M_EL1H)/
BREAK_CPUS_sctlr-kept := s/\& ~(SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_EE)/\& ~(SCTLR_M | SCTLR_C | SCTLR_EE)/
BREAK_CPUS_vbar-kept := /^static void hv_init_secondary(/,/^}/{/^    msr(VBAR_EL12, 0);$$/d}
BREAK_CPUS_dbg-kept := /^    hv_dbg_init_cpu(cpu);$$/d
BREAK_CPUS_pmu-kept := /^    hv_pmu_init_cpu(cpu);$$/d
BREAK_CPUS_aborts-kept := /^    hv_xtrack_init_cpu(cpu);$$/d
BREAK_CPUS_bit-after-lock := /^static void hv_enter_secondary(/,/^}/{/^    __atomic_and_fetch(/d;s/^    hv_started_cpus\[smp_id()\] = false;$$/&\n    __atomic_and_fetch(\&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);/}
BREAK_CPUS_started-not-set := /^    hv_started_cpus\[cpu\] = true;$$/d
BREAK_CPUS_twice-in := /^bool hv_start_secondary(/,/^}/{s/    if (hv_started_cpus\[cpu\]) {/    if (0) {/}
BREAK_CPUS_not-in-guest-bit := /^    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);$$/d
BREAK_CPUS_log-all := s/    if (!(e->count \& (e->count - 1)))/    if (1)/
BREAK_CPUS_no-reason := s/printf("HV: CPU %d is not in EL2: %s\\n", cpu, hv_cpu_missing\[cpu\]);/;/
BREAK_CPUS_rvbar-ignored := /^const char \*smp_try_start(/,/^}/{s/if (!cpu_features->apple_sysregs_unlocked \&\& .*{/if (0) {/}
BREAK_CPUS_start-again := /^const char \*smp_try_start(/,/^}/{/if (smp_is_alive(cpu))/,+1d};/^static void smp_start_cpu(/,/^}/{s/if (spin_table\[index\].flag)/if (0)/}
BREAK_CPUS_no-wait-result := s/return smp_is_alive(cpu) ? NULL : "it did not come up after it was started";/return NULL;/
BREAK_CPUS_beyond-not-counted := /^            cpus_beyond++;$$/d
BREAK_CPUS_idle-always := s/    return !spin_table\[cpu\].target;/    return true;/
BREAK_CPUS_invalid-started := /^const char \*smp_try_start(/,/^}/{/if (!cpu_info\[cpu\].valid)/,+1d}
BREAK_CPUS_boot-rvbar-kept := /^        smp_set_boot_rvbar(&cpu_info\[cpu\]);$$/d
BREAK_CPUS_boot-rvbar-locked := /if (FIELD_GET(RVBAR_LOCK, read64(cpu->impl_reg)))/,+1d
BREAK_CPUS_mmu-again := s/    if (read_sctlr() \& SCTLR_M)/    if (0)/

define CPUS_BROKEN_RULES
build/cpus/$(1)-broken-%.c: build/cpus/$(1).c Makefile
	sed -e '$$(BREAK_CPUS_$$*)' $$< > $$@
	! cmp -s $$< $$@
$(foreach n,$(CPUS_BROKEN_$(1)),build/cpus-broken-$(n)): build/cpus-broken-%: cpus_test.c \
		build/cpus/$(1)-broken-%.o $(foreach o,$(filter-out $(1),hv smp mmu stubs),build/cpus/$(o).o)
	$$(CC) $$(CFLAGS) -o $$@ $$^
endef
$(eval $(call CPUS_BROKEN_RULES,hv))
$(eval $(call CPUS_BROKEN_RULES,smp))
$(eval $(call CPUS_BROKEN_RULES,mmu))
$(foreach f,hv smp mmu,$(CPUS_BROKEN_$(f):%=build/cpus/$(f)-broken-%.o)): CPUS_CFLAGS += -Wno-error -Wno-unused

cpus-broken: $(CPUS_BROKEN:%=build/cpus-broken-%)
	@for n in $(CPUS_BROKEN); do \
		if CPUS_TIMEOUT=10 timeout 300 build/cpus-broken-$$n > build/cpus-broken-$$n.log 2>&1; then \
			echo "cpus-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL' build/cpus-broken-$$n.log || \
			{ echo "cpus-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(CPUS_BROKEN),echo "cpus-broken: $(n): $$(grep -m1 '^FAIL' build/cpus-broken-$(n).log)";)
	@echo "cpus-broken: all $(words $(CPUS_BROKEN)) failed, as they must"

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

build/psci-model: psci_test.c $(wildcard *.h) build/hv_psci.c build/hv_aicmap.h
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
