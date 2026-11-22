# SPDX-License-Identifier: GPL-2.0-or-later
#
# src/hv/hv_xtrack.c with one thing wrong in it at a time, as sed expressions on the file
# without its #include lines. The tests of execution tracking must fail on each: that is what
# shows that they look. Used by vgic-model/Makefile (make xtrack-broken) and by
# irqtest/Makefile (make qemu-xi-broken). The numbers stay what they were when a change goes:
# 1 and 3 went with the replaced words that were kept in the guest's memory.

BREAK_2 := s/^            ctx->regs\[d->rn\] += d->imm;$$/            ;/
BREAK_2_IS := no writeback to the base register
BREAK_4 := s/^        u32 word = xt_shadow_word(words\[i\]);$$/        u32 word = words[i];/
BREAK_4_IS := an access stored into its own page of code is not replaced
BREAK_5 := s/^            val = xt_sext(val, size);$$/            ;/
BREAK_5_IS := no sign extension of a load that ESR_EL2 describes
BREAK_6 := /^static int xt_to_data/,/^}/s/if (hv_pt_set_scanned(page, HV_XPAGE_INVALID, 0) < 0)/if (0)/
BREAK_6_IS := a page of code becomes data without an invalid entry in between
BREAK_7 := s/ \&\& !(off == 0 \&\& size > 1)//
BREAK_7_IS := the address that faulted is taken for the start of the access
BREAK_8 := s/return hv_pt_set_scanned(page, HV_XPAGE_XONLY, (u64)xt_shadow(r));/return hv_pt_set_scanned(page, HV_XPAGE_SCANNED, orig);/
BREAK_8_IS := a page of code that gets its first access stays readable and executes it
BREAK_9 := s/^                new\[i\] = 0;$$/                new[i] = 1;/
BREAK_9_IS := DC ZVA carried out here does not write zeroes
BREAK_10 := s/^                wr = false;$$/                ;/
BREAK_10_IS := a CAS that does not find what it expects stores all the same
BREAK_11 := s/^                    simd\[r\]\.d\[1\] = 0;$$/                    ;/
BREAK_11_IS := a load into a SIMD register leaves its upper half as it was
BREAK_12 := s/^            \*sp += d->imm;$$/            msr(SP_EL1, *sp + d->imm);/
BREAK_12_IS := writeback to the stack pointer goes to the register and not to the context
BREAK_13 := s/^        xt_read(orig, off, size, buf);$$/        xt_read(pa, off, size, buf);/
BREAK_13_IS := a read of an execute-only page reads the shadow page
BREAK_14 := s/^            \*(volatile u32 \*)&shadow\[i\] = word;$$/            ;/
BREAK_14_IS := a store into its own execute-only page leaves the shadow page as it was
BREAK_15 := s/HV_XPAGE_XONLY, (u64)xt_shadow(r))/HV_XPAGE_XONLY, orig)/
BREAK_15_IS := an execute-only page executes the guest's page
BREAK_16 := /A device may write into the guest/,+2d
BREAK_16_IS := DC IVAC leaves a page of code as it is
BREAK_17 := s/^        if (hv_pt_set_scanned(page, HV_XPAGE_INVALID, 0) < 0)$$/        if (0)/
BREAK_17_IS := a page of code that gets its first access points to a shadow page without an invalid entry in between
BREAK_18 := /^    xt_free\[xt_nr_free++\] = r->shadow;$$/d
BREAK_18_IS := a page that becomes data does not give its shadow page back
BREAK_19 := /^    xt_clean(orig + off, len);$$/d
BREAK_19_IS := a store into an execute-only page leaves lines of the guest's page not clean
BROKEN := 2 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
