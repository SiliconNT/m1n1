# SPDX-License-Identifier: GPL-2.0-or-later
#
# src/hv/hv_xtrack.c with one thing wrong in it at a time, as sed expressions on the file
# without its #include lines. The tests of execution tracking must fail on each: that is what
# shows that they look. Used by vgic-model/Makefile (make xtrack-broken) and by
# irqtest/Makefile (make qemu-xi-broken).

BREAK_1 := s/^        return orig;$$/        return word;/
BREAK_1_IS := a read gives the guest the replaced word
BREAK_2 := s/^            ctx->regs\[d->rn\] += d->imm;$$/            ;/
BREAK_2_IS := no writeback to the base register
BREAK_3 := s/^                words\[i\] = xt_view(r, pa, i);$$/                ;/
BREAK_3_IS := a page that becomes data keeps its replaced words
BREAK_4 := s/^            word = hv_pmu_patch_word(word);$$/            ;/
BREAK_4_IS := an access stored into its own page of code is not replaced
BREAK_5 := s/^            val = xt_sext(val, size);$$/            ;/
BREAK_5_IS := no sign extension of a load that ESR_EL2 describes
BREAK_6 := s/if (hv_pt_set_scanned(page, HV_XPAGE_NONE) < 0)/if (0)/
BREAK_6_IS := a page changes between data and code while the guest has access to it
BREAK_7 := s/ \&\& !(off == 0 \&\& size > 1)//
BREAK_7_IS := the address that faulted is taken for the start of the access
BREAK_8 := s/if (hv_pt_set_scanned(page, HV_XPAGE_XONLY) < 0) {/if (0) {/
BREAK_8_IS := a page of code that gets its first replaced word stays readable
BREAK_9 := s/^                new\[i\] = 0;$$/                new[i] = 1;/
BREAK_9_IS := DC ZVA carried out here does not write zeroes
BREAK_10 := s/^                wr = false;$$/                ;/
BREAK_10_IS := a CAS that does not find what it expects stores all the same
BREAK_11 := s/^                    simd\[r\]\.d\[1\] = 0;$$/                    ;/
BREAK_11_IS := a load into a SIMD register leaves its upper half as it was
BREAK_12 := s/^            \*sp += d->imm;$$/            msr(SP_EL1, *sp + d->imm);/
BREAK_12_IS := writeback to the stack pointer goes to the register and not to the context
BROKEN := 1 2 3 4 5 6 7 8 9 10 11 12
