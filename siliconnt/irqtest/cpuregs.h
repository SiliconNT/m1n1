/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The system registers that R50 reads on every core, in one place for the three that use the
 * list: cputest-regs.S (the guest, at EL1), el2probe.S (in m1n1, at EL2) and run_cpuregs.py,
 * which reads this file for the names.
 *
 *   BOTH  at EL2, and by the guest at EL1
 *   EL2   at EL2 only
 *   LATE  at EL2, and by the guest after everything else: an access that may end in the
 *         hypervisor, and in the host's script from there (the CTRR registers)
 *
 * Registers of the ID space that are not allocated read as 0. The others in this list that a
 * core may not have end in an undefined instruction, which the readers skip and report.
 */

#ifndef CPUREGS_H
#define CPUREGS_H

#define CPU_REGS                                                                                   \
    REG(MIDR_EL1, BOTH, 3, 0, 0, 0, 0)                                                             \
    REG(MPIDR_EL1, BOTH, 3, 0, 0, 0, 5)                                                            \
    REG(REVIDR_EL1, BOTH, 3, 0, 0, 0, 6)                                                           \
    REG(AIDR_EL1, BOTH, 3, 1, 0, 0, 7)                                                             \
    REG(CTR_EL0, BOTH, 3, 3, 0, 0, 1)                                                              \
    REG(DCZID_EL0, BOTH, 3, 3, 0, 0, 7)                                                            \
    REG(CLIDR_EL1, BOTH, 3, 1, 0, 0, 1)                                                            \
    REG(CNTFRQ_EL0, BOTH, 3, 3, 14, 0, 0)                                                          \
    REG(SCTLR_EL1, BOTH, 3, 0, 1, 0, 0)                                                            \
    REG(ID_AA64PFR0_EL1, BOTH, 3, 0, 0, 4, 0)                                                      \
    REG(ID_AA64PFR1_EL1, BOTH, 3, 0, 0, 4, 1)                                                      \
    REG(ID_AA64PFR2_EL1, BOTH, 3, 0, 0, 4, 2)                                                      \
    REG(ID_AA64ZFR0_EL1, BOTH, 3, 0, 0, 4, 4)                                                      \
    REG(ID_AA64SMFR0_EL1, BOTH, 3, 0, 0, 4, 5)                                                     \
    REG(ID_AA64FPFR0_EL1, BOTH, 3, 0, 0, 4, 7)                                                     \
    REG(ID_AA64DFR0_EL1, BOTH, 3, 0, 0, 5, 0)                                                      \
    REG(ID_AA64DFR1_EL1, BOTH, 3, 0, 0, 5, 1)                                                      \
    REG(ID_AA64AFR0_EL1, BOTH, 3, 0, 0, 5, 4)                                                      \
    REG(ID_AA64AFR1_EL1, BOTH, 3, 0, 0, 5, 5)                                                      \
    REG(ID_AA64ISAR0_EL1, BOTH, 3, 0, 0, 6, 0)                                                     \
    REG(ID_AA64ISAR1_EL1, BOTH, 3, 0, 0, 6, 1)                                                     \
    REG(ID_AA64ISAR2_EL1, BOTH, 3, 0, 0, 6, 2)                                                     \
    REG(ID_AA64ISAR3_EL1, BOTH, 3, 0, 0, 6, 3)                                                     \
    REG(ID_AA64MMFR0_EL1, BOTH, 3, 0, 0, 7, 0)                                                     \
    REG(ID_AA64MMFR1_EL1, BOTH, 3, 0, 0, 7, 1)                                                     \
    REG(ID_AA64MMFR2_EL1, BOTH, 3, 0, 0, 7, 2)                                                     \
    REG(ID_AA64MMFR3_EL1, BOTH, 3, 0, 0, 7, 3)                                                     \
    REG(ID_AA64MMFR4_EL1, BOTH, 3, 0, 0, 7, 4)                                                     \
    REG(VPIDR_EL2, EL2, 3, 4, 0, 0, 0)                                                             \
    REG(VMPIDR_EL2, EL2, 3, 4, 0, 0, 5)                                                            \
    REG(HCR_EL2, EL2, 3, 4, 1, 1, 0)                                                               \
    REG(MDCR_EL2, EL2, 3, 4, 1, 1, 1)                                                              \
    REG(CNTVOFF_EL2, EL2, 3, 4, 14, 0, 3)                                                          \
    REG(CNTPOFF_EL2, EL2, 3, 4, 14, 0, 6)                                                          \
    REG(CNTHCTL_EL2, EL2, 3, 4, 14, 1, 0)                                                          \
    REG(ICC_SRE_EL2, EL2, 3, 4, 12, 9, 5)                                                          \
    REG(ICH_VTR_EL2, EL2, 3, 4, 12, 11, 1)                                                         \
    REG(ISR_EL1, EL2, 3, 0, 12, 1, 0)                                                              \
    REG(SCTLR_EL12, EL2, 3, 5, 1, 0, 0)                                                            \
    REG(VBAR_EL12, EL2, 3, 5, 12, 0, 0)                                                            \
    REG(CNTV_CTL_EL02, EL2, 3, 5, 14, 3, 1)                                                        \
    REG(CNTP_CTL_EL02, EL2, 3, 5, 14, 2, 1)                                                        \
    REG(MDSCR_EL1, EL2, 2, 0, 0, 2, 2)                                                             \
    REG(OSLSR_EL1, EL2, 2, 0, 1, 1, 4)                                                             \
    REG(MPAMIDR_EL1, EL2, 3, 0, 10, 4, 4)                                                          \
    REG(MPAM1_EL1, EL2, 3, 0, 10, 5, 0)                                                            \
    REG(CTRR_C_LWR_EL1, LATE, 3, 0, 11, 0, 0)                                                      \
    REG(CTRR_C_UPR_EL1, LATE, 3, 0, 11, 0, 1)                                                      \
    REG(CTRR_C_CTL_EL1, EL2, 3, 0, 11, 1, 4)                                                       \
    REG(CTRR_D_LWR_EL1, EL2, 3, 0, 11, 0, 2)                                                       \
    REG(CTRR_D_UPR_EL1, EL2, 3, 0, 11, 0, 3)                                                       \
    REG(CTRR_D_CTL_EL1, EL2, 3, 0, 11, 1, 5)                                                       \
    REG(CTRR_C_LWR_EL12, EL2, 3, 0, 11, 0, 4)                                                      \
    REG(CTRR_C_UPR_EL12, EL2, 3, 0, 11, 0, 5)                                                      \
    REG(CTRR_C_CTL_EL12, EL2, 3, 0, 11, 1, 6)

#endif
