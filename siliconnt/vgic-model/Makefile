# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host-side model tests for src/hv/hv_vgic.c and src/hv/hv_wfi.c (test.c), for the masking
# functions of src/aic.c (aic_test.c, with the stand-in headers of aic/) and for
# src/hv/hv_psci.c (psci_test.c), src/hv/hv_pmu.c (pmu_test.c), the page tables of
# src/hv/hv_vm.c (pt_test.c), src/hv/hv_xtrack.c (xtrack_test.c; on several cores at once,
# xtrack_smp_test.c) and src/hv/hv_xinsn.c (xinsn_test.c, test_xinsn.py), and src/hv/hv.c with
# src/smp.c (cpus_test.c). They compile
# the real files for the build machine against stand-in headers and play the hardware, the
# guest and the host around them.
#
#   make        build them and run them
#   make pt     only the page table one
#   make pt-broken  ...against hv_vm.c with one thing wrong at a time in where the tables come
#               from, in the check of the range of CTRR or in the count of changes of state: it
#               must fail on each
#   make xtrack only the execution tracking one
#   make xtrack-broken  ...against hv_xtrack.c with one thing wrong in it at a time: it must
#               fail on each
#   make xtrack-smp  only the execution tracking one on several cores
#   make xtrack-smp-broken  ...against hv_xtrack.c with the repeat count of before: it must fail
#   make xinsn  only the one for the instruction decoder of execution tracking
#   make dbg    only the one for the debug registers, src/hv/hv_dbg.c (dbg_test.c)
#   make aic    only the one for aic_mask(), aic_unmask() and aic_mask_all() of src/aic.c
#   make psci   only the PSCI one, and test_hv_events.py: that the host has a handler for
#               every event the hypervisor sends (needs the Python of the proxy client)
#   make psci-broken  the PSCI one against hv_psci.c with one thing wrong in CPU_ON or CPU_OFF
#               at a time: it must fail on each
#   make cpus   only the one for how the cores get into EL2, into the guest and out of it, and who
#               ticks: src/hv/hv.c and src/smp.c, and a few functions of src/memory.c and
#               src/hv/hv_exc.c (cpus_test.c, with what is in cpus/); and test_hv_cpus.py: what the
#               host knows of them (needs the Python of the proxy client)
#   make cpus-broken  ...against one of those with one thing wrong at a time: it must fail on
#               each, with a failed check or by hanging
#   make tsan   test.c and xtrack_smp_test.c under ThreadSanitizer, with fewer rounds
#   make vgic-broken  test.c against hv_vgic.c or hv_wfi.c with one thing wrong at a time, in how
#               it forwards AIC lines, in what it does when the guest turns a core off, in which
#               interrupts it keeps in the list registers or in when a waiting core takes SErrors
#               again: it must fail on each

CC := gcc
PYTHON ?= $(HOME)/src/m1n1-venv/bin/python
SRC := ../../src
CFLAGS := -O2 -g -Wall -Werror -pthread -I. -Ibuild
ROUNDS ?= 200000

.PHONY: all psci psci-broken pmu pt pt-broken xtrack xtrack-broken xtrack-smp xtrack-smp-broken xinsn dbg aic cpus cpus-broken tsan vgic-broken clean

all: build/model psci psci-broken cpus cpus-broken pmu pt pt-broken xinsn xtrack xtrack-broken xtrack-smp xtrack-smp-broken dbg aic vgic-broken
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

# The same on several cores at once: threads for the cores, see xtrack_smp_test.c
XTRACK_SMP_DEPS := xtrack_smp_test.c $(wildcard *.h) build/hv_xpage.h build/hv_pmu.c \
	build/hv_xinsn.c build/hv_xinsn.h build/arm_cpu_regs.h build/hv_aicmap.h
XTRACK_SMP_SRCS := xtrack_smp_test.c build/hv_pmu.c build/hv_xinsn.c

xtrack-smp: build/xtrack-smp-model
	build/xtrack-smp-model $(XTRACK_SMP_ROUNDS)

build/xtrack-smp-model: $(XTRACK_SMP_DEPS) build/hv_xtrack.inc
	$(CC) $(CFLAGS) -o $@ $(XTRACK_SMP_SRCS)

build/xtrack-smp-tsan: $(XTRACK_SMP_DEPS) build/hv_xtrack.inc
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o $@ $(XTRACK_SMP_SRCS)

build/xtrack-smp-broken-%: $(XTRACK_SMP_DEPS) build/hv_xtrack-broken-%.inc
	$(CC) $(CFLAGS) -Wno-error -DXTRACK_INC='"hv_xtrack-broken-$*.inc"' -o $@ $(XTRACK_SMP_SRCS)

# ...against hv_xtrack.c with the repeat count of before, 25 of ../xtrack-broken.mk
xtrack-smp-broken: build/xtrack-smp-broken-25
	@if timeout 300 build/xtrack-smp-broken-25 > build/xtrack-smp-broken-25.log 2>&1; then \
		echo "xtrack-smp-broken-25: the test passes and must not"; exit 1; \
	fi
	@grep -q -m1 '^FAIL' build/xtrack-smp-broken-25.log || \
		{ echo "xtrack-smp-broken-25: ended without a failed check"; exit 1; }
	@echo "xtrack-smp-broken: 25 $(BREAK_25_IS): $$(grep -m1 '^FAIL' build/xtrack-smp-broken-25.log)"

build/pt-model: pt_test.c $(wildcard *.h) build/hv_vm_pt.inc build/hv_xpage.h build/arm_cpu_regs.h \
	build/hv_aicmap.h
	$(CC) $(CFLAGS) -o $@ pt_test.c

# Every change must change something (cmp), or the source has moved on
PT_BROKEN := small-chunk first-level-not-rounded never-reused l3-not-given-back \
	kboot-end page-step flag-ignored order-not-checked end-not-checked zero-not-special \
	refusal-ignored switch-not-counted
BREAK_PT_switch-not-counted := /^        hv_pt_switches++;$$/d
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
	wrong-context failure-ignored unknown-returns no-table start-forgotten \
	off-forgotten last-off-event last-off-returns off-not-counted
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
BREAK_PSCI_off-forgotten := /^            hv_cpu_off(ctx);$$/d
BREAK_PSCI_last-off-event := /its last one/,/^        default:/s/HV_GUEST_OFF/HV_GUEST_HVC/
BREAK_PSCI_last-off-returns := /its last one/,/^        default:/s/hv_psci_stop(ctx, HV_GUEST_OFF);/ret = 0; break;/
BREAK_PSCI_off-not-counted := /^            psci_stats.cpu_off++;$$/d

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

# The ways of a core into EL2 and out of it, and the host's: what hv_exc.c keeps, and functions, to
# the end of which is the first line that is nothing but a closing brace
EXC_ARGS := (struct exc_info \*ctx, uartproxy_boot_reason_t reason, u32 type,
build/cpus/exc.c: cpus/exc_head.c cpus/exc_tail.c $(SRC)/hv/hv_exc.c Makefile
	@mkdir -p build/cpus
	cp $< $@
	test $$(grep -c '^#define PERCPU(x)\|^static bool time_stealing = true;$$' $(SRC)/hv/hv_exc.c) = 2
	sed -n '/^#define PERCPU(x)/,/^static bool time_stealing = true;$$/p' $(SRC)/hv/hv_exc.c >> $@
	for f in 'void hv_write_cntvoff(void)' 'static void _hv_exc_proxy$(EXC_ARGS)' \
		'void hv_maybe_switch_cpu$(EXC_ARGS)' 'void hv_exc_proxy$(EXC_ARGS) void \*extra)' \
		'void hv_exc_init_cpu(int cpu)' 'static void hv_get_context(struct exc_info \*ctx)' \
		'void hv_exc_init(void)' 'void hv_lock(void)' 'void hv_exc_report(void)' \
		'static void hv_exc_entry(void)' 'static void hv_exc_exit(struct exc_info \*ctx)' \
		'static int hv_interruptible_cpu(void)' 'void hv_exc_fiq(struct exc_info \*ctx)'; do \
		test $$(grep -c "^$$f$$" $(SRC)/hv/hv_exc.c) = 1 || exit 1; \
		sed -n "/^$$f$$/,/^}$$/p" $(SRC)/hv/hv_exc.c >> $@; \
	done
	cat cpus/exc_tail.c >> $@

CPUS_OBJS := hv smp mmu exc stubs

build/cpus-model: cpus_test.c $(CPUS_OBJS:%=build/cpus/%.o)
	$(CC) $(CFLAGS) -o $@ $^

cpus: build/cpus-model
	build/cpus-model
	$(PYTHON) test_hv_cpus.py

# Every change must change something (cmp), or the source has moved on
CPUS_BROKEN_hv := not-started after-vgic before-wfe-mode ready-ignored line-ignored busy-ignored \
	beyond-ignored no-idle-wait not-copied no-leaving exit-kept masks sctlr-kept vbar-kept \
	dbg-kept pmu-kept aborts-kept bit-after-lock started-not-set twice-in not-in-guest-bit \
	log-all no-reason boot-cntvoff cntvoff mask-shifted \
	last-leaves off-not-shown want-kept pin-kept exit-one exit-no-ipi started-kept tick-kept \
	vtimer-kept vgic-not-off last-exit-ignored off-log-all boot-waits-for-lock boot-no-irq \
	boot-not-parked lead-boot-only lead-not-boot tick-slow-on-entry no-exc-init back-not-awaited \
	global-again boot-refused tick-no-host state-hook-always
CPUS_BROKEN_smp := rvbar-ignored start-again no-wait-result beyond-not-counted idle-always \
	invalid-started boot-rvbar-kept boot-rvbar-locked boot-not-called boot-stays-alive
CPUS_BROKEN_mmu := mmu-again
CPUS_BROKEN_exc := bit-after-unlock bit-not-set exit-cntvoff take-over-forgotten stale-lead \
	told-while-leaving exit-alone records-kept pmcr0-kept no-host-ignored serror-before-lock
CPUS_BROKEN := $(foreach f,hv smp mmu exc,$(CPUS_BROKEN_$(f)))
BREAK_CPUS_state-hook-always := /^static bool hv_cpu_state_setup_hook(/,/^}/{/^    if (hv_sptm_mode == SPTM_NONE)$$/,+1d}
BREAK_CPUS_serror-before-lock := /^static void hv_exc_entry(/,/^}/{/^    hv_lock();$$/d;s/^        sysop("msr daifclr, 4");$$/&\n    hv_lock();/}
BREAK_CPUS_not-started := /^void hv_init(void)/,/^}/{/^    hv_start_cpus();$$/d}
BREAK_CPUS_after-vgic := /^void hv_init(void)/,/^}/{/^    hv_start_cpus();$$/d;s/^    hv_vgic_init();$$/&\n    hv_start_cpus();/}
BREAK_CPUS_before-wfe-mode := /^void hv_init(void)/,/^}/{s/^    smp_set_wfe_mode(true);$$/    hv_start_cpus();\n&/;/^    hv_start_cpus();$$/d}
BREAK_CPUS_ready-ignored := s/    if (!hv_cpus_ready()) {/    if (0) {/
BREAK_CPUS_line-ignored := s/if (line < HV_XTRACK_LINE) {/if (0) {/
BREAK_CPUS_busy-ignored := s/if (!hv_cpu_missing\[cpu\] \&\& .* !hv_cpu_waits(cpu))/if (0)/
BREAK_CPUS_beyond-ignored := s/    if (smp_cpus_beyond()) {/    if (0) {/
BREAK_CPUS_no-idle-wait := s/    if (!hv_cpu_waits(cpu)) {/    if (0) {/
BREAK_CPUS_not-copied := /^    memcpy(e->regs, regs, sizeof(e->regs));$$/d
BREAK_CPUS_no-leaving := /^bool hv_start_secondary(/,/^}/{s/    if (hv_leaving)$$/    if (0)/}
BREAK_CPUS_exit-kept := /^    hv_should_exit\[cpu\] = false;$$/d
BREAK_CPUS_masks := s/(SPSR_D | SPSR_A | SPSR_I | SPSR_F | SPSR_M_EL1H)/(SPSR_I | SPSR_F | SPSR_M_EL1H)/
BREAK_CPUS_sctlr-kept := s/\& ~(SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_EE)/\& ~(SCTLR_M | SCTLR_C | SCTLR_EE)/
BREAK_CPUS_vbar-kept := /^static void hv_init_secondary(/,/^}/{/^    msr(VBAR_EL12, 0);$$/d}
BREAK_CPUS_dbg-kept := /^    hv_dbg_init_cpu(cpu);$$/d
BREAK_CPUS_pmu-kept := /^    hv_pmu_init_cpu(cpu);$$/d
BREAK_CPUS_aborts-kept := /^    hv_xtrack_init_cpu(cpu);$$/d
BREAK_CPUS_bit-after-lock := /^void hv_maybe_exit(/,/^}/{/^    __atomic_and_fetch(/d;s/^    spin_lock(&bhl);$$/&\n    __atomic_and_fetch(\&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);/}
BREAK_CPUS_started-not-set := /^    __atomic_store_n(&hv_started_cpus\[cpu\], true, __ATOMIC_SEQ_CST);$$/d
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
BREAK_CPUS_boot-cntvoff := /^void hv_start(/,/^}/{/^    hv_write_cntvoff();$$/d}
BREAK_CPUS_cntvoff := /^static void hv_init_secondary(/,/^}/{/^    hv_write_cntvoff();$$/d}
BREAK_CPUS_last-leaves := /^void hv_cpu_off(/,/^}/{s/    if (!hv_leaving \&\& .*/    if (0)/}
BREAK_CPUS_off-not-shown := /^void hv_cpu_off(/,/^}/{/^    hv_maybe_switch_cpu(/d}
BREAK_CPUS_want-kept := /^    if (hv_want_cpu == cpu)$$/,+1d
BREAK_CPUS_pin-kept := /^    if (hv_pinned_cpu == cpu)$$/,+1d
BREAK_CPUS_exit-one := /^void hv_exit_all(/,/^}/{/^    hv_leave();$$/d}
BREAK_CPUS_exit-no-ipi := /^static void hv_leave(void)/,/^}/{s/smp_send_ipi(i);/;/}
BREAK_CPUS_started-kept := /^    __atomic_store_n(&hv_started_cpus\[cpu\], false, __ATOMIC_SEQ_CST);$$/d
BREAK_CPUS_tick-kept := /^    msr(CNTP_CTL_EL0, 0);$$/d
BREAK_CPUS_vtimer-kept := /^    msr(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, 0);$$/d
BREAK_CPUS_vgic-not-off := s/^        hv_vgic_cpu_off();$$/        ;/
BREAK_CPUS_last-exit-ignored := s/    if (hv_lead_cpu() < 0)/    if (0)/
BREAK_CPUS_off-log-all := s/    if (hv_leaving || !(count \& (count - 1)))/    if (1)/
BREAK_CPUS_boot-waits-for-lock := s/        if (hv_try_lock()) {/        spin_lock(\&bhl); {/
BREAK_CPUS_boot-no-irq := s/    if (mrs(ISR_EL1) \& ISR_I) {/    if (0) {/
BREAK_CPUS_boot-not-parked := /^    smp_boot_cpu_wait(hv_boot_cpu_waits);$$/d
BREAK_CPUS_lead-boot-only := /^int hv_lead_cpu(/,/^}/{s/^    if (boot_cpu_idx >= 0 .*/    if (1)/}
BREAK_CPUS_lead-not-boot := /^int hv_lead_cpu(/,/^}/{s/^    if (boot_cpu_idx >= 0 .*/    if (0)/}
BREAK_CPUS_tick-slow-on-entry := s/hv_arm_tick(smp_id() != hv_lead_cpu());/hv_arm_tick(true);/
BREAK_CPUS_no-exc-init := /^    hv_exc_init_cpu(cpu);$$/d
BREAK_CPUS_back-not-awaited := /^void hv_start(/,/^}/{/^        smp_wait(i);$$/d}
BREAK_CPUS_global-again := s/^    hv_vgic_init_cpu();$$/    if (cpu == boot_cpu_idx) hv_vgic_init();\n&/
BREAK_CPUS_boot-refused := s/    if (cpu != boot_cpu_idx \&\& !smp_is_alive(cpu)) {/    if (!smp_is_alive(cpu) || cpu == boot_cpu_idx) {/
BREAK_CPUS_boot-not-called := s/    if (cpu == boot_cpu_idx \&\& !target->flag)/    if (cpu == boot_cpu_idx)/
BREAK_CPUS_boot-stays-alive := /^void smp_boot_cpu_wait(/,/^}/{/^    me->flag = 0;$$/d}
BREAK_CPUS_take-over-forgotten := s/^            hv_take_over_tick();$$/            ;/
BREAK_CPUS_stale-lead := /^void hv_exc_fiq(/,/^}/{s/^    hv_exc_entry();$$/    int stale = hv_interruptible_cpu();\n&/;s/^    if (smp_id() == hv_interruptible_cpu()) {$$/    if (smp_id() == stale) {/}
BREAK_CPUS_tick-no-host := s/if (hv_has_host() \&\& iodev_can_read/if (iodev_can_read/
BREAK_CPUS_no-host-ignored := /^    hv_stop_if_no_host(ctx, reason, type);$$/d
BREAK_CPUS_told-while-leaving := s/^    if (hv_is_leaving())$$/    if (0)/
BREAK_CPUS_exit-alone := s/^            hv_exit_all();.*/            hv_leave_guest();/
BREAK_CPUS_records-kept := /^    pcpu\[cpu\] = (struct hv_pcpu_data){0};$$/d
BREAK_CPUS_pmcr0-kept := /^void hv_exc_init_cpu(/,/^}/{/^    reg_clr(/d}
BREAK_CPUS_mask-shifted := /^u64 hv_get_started_cpus(/,/^}/{s/BIT(i)/BIT(i + 1)/}
BREAK_CPUS_bit-after-unlock := /^static void hv_exc_exit(/,/^}/{/^    __atomic_or_fetch(/d;s/^    hv_maybe_exit();$$/&\n    __atomic_or_fetch(\&hv_cpus_in_guest, BIT(smp_id()), __ATOMIC_ACQUIRE);/}
BREAK_CPUS_bit-not-set := /^static void hv_exc_exit(/,/^}/{/^    __atomic_or_fetch(/d}
BREAK_CPUS_exit-cntvoff := /^static void hv_exc_exit(/,/^}/{/^    hv_write_cntvoff();$$/d}
BREAK_CPUS_mmu-again := s/    if (read_sctlr() \& SCTLR_M)/    if (0)/

define CPUS_BROKEN_RULES
build/cpus/$(1)-broken-%.c: build/cpus/$(1).c Makefile
	sed -e '$$(BREAK_CPUS_$$*)' $$< > $$@
	! cmp -s $$< $$@
$(foreach n,$(CPUS_BROKEN_$(1)),build/cpus-broken-$(n)): build/cpus-broken-%: cpus_test.c \
		build/cpus/$(1)-broken-%.o $(foreach o,$(filter-out $(1),$(CPUS_OBJS)),build/cpus/$(o).o)
	$$(CC) $$(CFLAGS) -o $$@ $$^
endef
$(eval $(call CPUS_BROKEN_RULES,hv))
$(eval $(call CPUS_BROKEN_RULES,smp))
$(eval $(call CPUS_BROKEN_RULES,mmu))
$(eval $(call CPUS_BROKEN_RULES,exc))
$(foreach f,hv smp mmu exc,$(CPUS_BROKEN_$(f):%=build/cpus/$(f)-broken-%.o)): CPUS_CFLAGS += -Wno-error -Wno-unused

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

tsan: build/model-tsan build/xtrack-smp-tsan
	build/model-tsan 3000
	build/xtrack-smp-tsan 3000

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

# hv_vgic.c with one thing wrong in how it forwards AIC lines, or in what it does when the guest
# turns a core off, at a time. test.c must fail on each, in its tests of that (build/model aic and
# build/model off run only those). Every change must change something (cmp), or the source has
# moved on. Likewise below for the list registers and for hv_wfi.c.
VGIC_BROKEN_OFF := lr-kept spi-lost sgi-kept-pending active-not-ended ended-not-seen active-only \
	hcr-kept events-kept lead-not-told irm-boot-only global-again off-sent-to
BREAK_VGIC_lr-kept := /^void hv_vgic_cpu_off(void)/,/^}/{/vgic_write_lr(i, 0);/d}
BREAK_VGIC_spi-lost := /^void hv_vgic_cpu_off(void)/,/^}/{s/if (state \& ICH_LR_STATE_PENDING)/if (0)/}
BREAK_VGIC_sgi-kept-pending := /^void hv_vgic_init_cpu(void)/,/^}/{/sgi_pending), 0, __ATOMIC_SEQ_CST);/d}
BREAK_VGIC_active-not-ended := /^void hv_vgic_cpu_off(void)/,/^}/{s/if (state != ICH_LR_STATE_PENDING)/if (!state)/}
BREAK_VGIC_ended-not-seen := /^void hv_vgic_cpu_off(void)/,/^}/{s/if (state != ICH_LR_STATE_PENDING)/if (state \& ICH_LR_STATE_ACTIVE)/}
BREAK_VGIC_active-only := /^void hv_vgic_cpu_off(void)/,/^}/{s/if (state \& ICH_LR_STATE_PENDING)/if (state == ICH_LR_STATE_PENDING)/}
BREAK_VGIC_hcr-kept := /^void hv_vgic_cpu_off(void)/,/^}/{/msr(SYS_ICH_HCR_EL2, 0);/d}
BREAK_VGIC_events-kept := /^void hv_vgic_cpu_off(void)/,/^}/{s/while (.*hv_vgic_handle_irq())/while (0)/}
BREAK_VGIC_lead-not-told := /^void hv_vgic_cpu_off(void)/,/^}/{s/if (hv_lead_cpu() >= 0)/if (0)/}
BREAK_VGIC_irm-boot-only := s/return hv_lead_cpu();/return boot_cpu_idx;/
BREAK_VGIC_global-again := /^void hv_vgic_init_cpu(void)/,/^}/{s/^    u64 sre = .*$$/&\n    if (smp_id() == boot_cpu_idx) vgic_init_global();/}
BREAK_VGIC_off-sent-to := s/if (cpu != smp_id() \&\& vgic_started(cpu)) {/if (cpu != smp_id()) {/
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
# ...in which interrupts are in the list registers (build/model lr)
VGIC_BROKEN_LR := no-swap swap-lost swap-not-told
BREAK_VGIC_no-swap := s/if (free < 0 \&\& lowest >= 0) {/if (0) {/
BREAK_VGIC_swap-lost := /^        vgic_put_back(out);$$/d
BREAK_VGIC_swap-not-told := /^static void vgic_put_back(/,/^}/{/VGIC_PERCPU(spi_left) = true;/d}
# hv_wfi.c with one thing wrong, the same way (build/model wfi)
WFI_BROKEN := serror-before-lock
BREAK_WFI_serror-before-lock := /^bool hv_handle_wfx(/,/^}/{/^    hv_lock();$$/d;s/^        sysop("msr daifclr, 4");$$/&\n    hv_lock();/}

build/hv_vgic-broken-%.c: build/hv_vgic.c Makefile
	sed -e '$(BREAK_VGIC_$*)' $< > $@
	! cmp -s $< $@

build/model-broken-%: $(MODEL_DEPS) build/hv_vgic-broken-%.c
	$(CC) $(CFLAGS) -Wno-error -o $@ test.c build/hv_vgic-broken-$*.c build/hv_wfi.c

build/hv_wfi-broken-%.c: build/hv_wfi.c Makefile
	sed -e '$(BREAK_WFI_$*)' $< > $@
	! cmp -s $< $@

$(WFI_BROKEN:%=build/model-broken-%): build/model-broken-%: $(MODEL_DEPS) build/hv_vgic.c \
		build/hv_wfi-broken-%.c
	$(CC) $(CFLAGS) -Wno-error -o $@ test.c build/hv_vgic.c build/hv_wfi-broken-$*.c

VGIC_BROKEN_NAMES := $(VGIC_BROKEN) $(VGIC_BROKEN_OFF) $(VGIC_BROKEN_LR) $(WFI_BROKEN)

vgic-broken: $(VGIC_BROKEN_NAMES:%=build/model-broken-%)
	@for n in $(VGIC_BROKEN:%=aic-%) $(VGIC_BROKEN_OFF:%=off-%) $(VGIC_BROKEN_LR:%=lr-%) \
			$(WFI_BROKEN:%=wfi-%); do \
		what=$${n%%-*}; n=$${n#*-}; \
		if timeout 60 build/model-broken-$$n $$what > build/model-broken-$$n.log 2>&1; then \
			echo "vgic-broken-$$n: the test passes and must not"; exit 1; \
		fi; \
		grep -q -m1 '^FAIL\|^CHECK FAILED' build/model-broken-$$n.log || \
			{ echo "vgic-broken-$$n: ended without a failed check"; exit 1; }; \
	done
	@$(foreach n,$(VGIC_BROKEN_NAMES),echo "vgic-broken: $(n): $$(grep -m1 '^FAIL\|^CHECK FAILED' build/model-broken-$(n).log)";)
	@echo "vgic-broken: all $(words $(VGIC_BROKEN_NAMES)) failed, as they must"

clean:
	rm -rf build
