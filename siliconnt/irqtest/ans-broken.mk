# SPDX-License-Identifier: GPL-2.0-or-later
#
# anstest.c with one of its own checks broken at a time, for make qemu-ans-broken: the guest's
# self-test (step 0) has to find each, so that the guest does not touch the controller.
#
#   flags          commands with flags (fused, SGL) are let through
#   io-whitelist   every command of the I/O queue is let through
#   admin-identify every identify is let through
#   prp1           PRP1 of a read is not checked
#   prp-list       the entries of a PRP list are not checked
#   tcb            the TCB need not have the command's PRPs
#   ioq-registers  0x1200 and 0x1208 need not hold the I/O queues made
#   sart-in-use    a SART entry in use before the core may be written
#   sart-arena     a SART entry may be turned on for any memory

ANS_BROKEN := flags io-whitelist admin-identify prp1 prp-list tcb ioq-registers sart-in-use \
	sart-arena

ANS_BREAK_flags := s/^    if (flags)$$/    if (0)/
ANS_BREAK_io-whitelist := s/return opcode == 0x02 && nsid == 1;/return 1;/
ANS_BREAK_admin-identify := s/return (cdw10 == 1 && nsid == 0) || (cdw10 == 0 && nsid == 1);/return 1;/
ANS_BREAK_prp1 := s/if (!in_data(prp1, first))/if (0)/
ANS_BREAK_prp-list := s/if ((e & (PAGE - 1)) || !in_data(e, len))/if (0)/
ANS_BREAK_tcb := s/if (tcb\[3\] != prp1 || tcb\[4\] != prp2)/if (0)/
ANS_BREAK_ioq-registers := s/(ioq_cmds != iosq || ioq_cqes != iocq)/0/
ANS_BREAK_sart-in-use := s/if (sart\[idx\].in_use)/if (0)/
ANS_BREAK_sart-arena := s/!in_arena((u64)next.paddr << 12, sart_bytes(&next)))/0)/
