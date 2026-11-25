/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What cpus_test.c cannot have without the headers of m1n1 */

/* features_m4 of src/chickens.c, as far as src/hv/hv.c and src/smp.c look at it */
static struct midr_part_features features = {
    .fast_ipi = true,
    .actlr_el2 = true,
    .counter_redirect = true,
};

const struct midr_part_features *cpu_features = &features;

void set_sysregs_unlocked(bool unlocked);

void set_sysregs_unlocked(bool unlocked)
{
    features.apple_sysregs_unlocked = unlocked;
}
