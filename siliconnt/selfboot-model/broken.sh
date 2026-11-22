#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# src/hv/hv_layout.c and src/selfboot_run.c with one thing wrong in them at a time. The model
# tests must fail on each: that is what shows that they look. Every change must change
# something, or the source has moved on. Run by "make broken", after the copies in build/ are
# made; the sources are not changed.
set -u
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CFLAGS="-O2 -g -Wall -Wno-error -I. -Ibuild"
bad=0
n=0

# What is wrong @ the file @ the change, for sed @ the test that must fail: layout, aicmap
# or run
while IFS='@' read -r name file expr test; do
    [ -z "$name" ] && continue
    n=$((n + 1))
    cp "build/$file" "$work/$file"
    sed -i -e "$expr" "$work/$file"
    if cmp -s "build/$file" "$work/$file"; then
        echo "$name: the change changes nothing"; bad=1; continue
    fi
    if [ "$test" = layout ]; then
        gcc $CFLAGS -o "$work/t" layout_test.c model.c "$work/hv_layout.c" 2>"$work/cc.log"
    elif [ "$test" = aicmap ]; then
        gcc $CFLAGS -o "$work/t" aicmap_test.c model.c "$work/hv_layout.c" 2>"$work/cc.log"
    else
        # The files of run_test.c, with the broken one in place of its copy
        files=""
        for f in selfboot.c selfboot_files.c selfboot_run.c hv_layout.c; do
            if [ "$f" = "$file" ]; then files="$files $work/$f"; else files="$files build/$f"; fi
        done
        gcc $CFLAGS -o "$work/t" run_test.c model.c $files 2>"$work/cc.log"
    fi || { echo "$name: does not compile"; cat "$work/cc.log"; bad=1; continue; }
    # In a shell of its own, which is what says so if the test is killed for reading where
    # nothing is: that is a way of failing too, and goes into the log
    ("$work/t"; echo "exit status $?") >"$work/out.log" 2>&1
    if grep -qx 'exit status 0' "$work/out.log"; then
        echo "$name: the test passes and must not"; bad=1
    elif grep -q '^FAILED' "$work/out.log"; then
        echo "$name ($test): $(grep -c '^FAILED' "$work/out.log") failed, first: $(grep -m1 '^FAILED' "$work/out.log")"
    else
        echo "$name ($test): did not get as far as a failed check"
    fi
done <<'EOF'
range-end-inclusive@hv_layout.c@s/if (base > last_of(vgic_base, vgic_size) ||/if (base >= last_of(vgic_base, vgic_size) ||/@layout
range-start-inclusive@hv_layout.c@s/|| vgic_base > last_of(base, size))/|| vgic_base >= last_of(base, size))/@layout
bus-address@hv_layout.c@s/\*base = cells(it->next + 4 \* it->bus_cells, it->cpu_cells);/*base = cells(it->next, it->bus_cells);/@layout
no-memory@hv_layout.c@/clear &= !in_the_way("memory"/d@layout
one-redistributor@hv_layout.c@s/(MAX_CPUS \* HV_VGIC_REDIST_SIZE)/(HV_VGIC_REDIST_SIZE)/@layout
less-than-an-entry-accepted@hv_layout.c@s/if (len < entry_size(it) || len & 0x80000000)/if (len \& 0x80000000)/@layout
flagged-length-accepted@hv_layout.c@s/if (len < entry_size(it) || len & 0x80000000)/if (len < entry_size(it))/@layout
cell-count-of-any-length@hv_layout.c@s/if (!value || len != sizeof(\*count))/if (!value)/@layout
wrap-not-clamped@hv_layout.c@s/return last < base ? ~0UL : last;/return last;/@layout
empty-range-counts@hv_layout.c@/^    if (!size)$/,/^        return false;$/d@layout
cells-not-checked@hv_layout.c@s/if (!it->bus_cells || it->bus_cells > 2 ||/if (0 \&\& it->bus_cells > 2 ||/@layout
only-first-in-the-way@hv_layout.c@s/        is = true;/        return true;/@layout
stops-at-first-device@hv_layout.c@s/        clear &= !in_the_way("a range of " ARM_IO, base, size);/        if (in_the_way("a range of " ARM_IO, base, size)) return false;/@layout
selfboot-does-not-ask@selfboot_run.c@s/    if (!hv_vgic_ranges_free())/    if (0)/@run
selfboot-maps-bus-address@hv_layout.c@s/\*base = cells(it->next + 4 \* it->bus_cells, it->cpu_cells);/*base = cells(it->next, it->bus_cells);/@run
selfboot-asks-too-late@selfboot_run.c@s/    if (selfboot_map(l, host) < 0)/    if (selfboot_map(l, host) < 0 || !hv_vgic_ranges_free())/;0,/    if (!hv_vgic_ranges_free())/s//    if (0)/@run
aic-die-inclusive@hv_layout.c@s/if (value \/ aic->max_irq >= aic->nr_die ||/if (value \/ aic->max_irq > aic->nr_die ||/@aicmap
aic-line-inclusive@hv_layout.c@s/value % aic->max_irq >= aic->nr_irq) {/value % aic->max_irq > aic->nr_irq) {/@aicmap
aicmap-not-cleared@hv_layout.c@/memset(aicmap_lines, 0, sizeof(aicmap_lines));/d@aicmap
aicmap-988-refused@hv_layout.c@s/if (count > HV_AICMAP_MAX) {/if (count >= HV_AICMAP_MAX) {/@aicmap
aicmap-only-the-root@hv_layout.c@/to_come += adt_get_child_count(adt, node);/d@aicmap
aicmap-any-parent@hv_layout.c@s/off \/ sizeof(u32)) != aic_phandle)/off \/ sizeof(u32)) != aic_phandle \&\& 0)/@aicmap
aicmap-first-parent-only@hv_layout.c@s/(index < count ? index : count - 1)/0/@aicmap
aicmap-last-parent-only@hv_layout.c@s/(index < count ? index : count - 1)/(count - 1)/@aicmap
aicmap-parents-go-round@hv_layout.c@s/(index < count ? index : count - 1)/(index % count)/@aicmap
aicmap-half-a-parent@hv_layout.c@s/        parents_len < sizeof(u32))/        0)/@aicmap
aicmap-placeholder-read@hv_layout.c@s/!parents || len \& 0x80000000 ||/!parents ||/@aicmap
aicmap-placeholder-parent@hv_layout.c@s/ || parents_len \& 0x80000000 ||/ ||/@aicmap
aicmap-part-of-a-value@hv_layout.c@s/off + sizeof(u32) <= len;/off < len;/@aicmap
aicmap-size-without-header@hv_layout.c@s/return 2 \* sizeof(u32) + count \* sizeof(u32);/return count * sizeof(u32);/@aicmap
aicmap-out-of-order@hv_layout.c@s/32 \* i + __builtin_ctz(bits)/32 * i + 31 - __builtin_clz(bits)/@aicmap
aicmap-no-version@hv_layout.c@s/map->version = HV_AICMAP_VERSION;/map->version = 0;/@aicmap
selfboot-table-size-untold@selfboot_run.c@/l->aicmap_size = aicmap_size;/d@run
selfboot-table-refusal-ignored@selfboot_run.c@s/    if (aicmap_size < 0)/    if (0)/@run
selfboot-table-not-written@selfboot_run.c@s/aicmap_size = hv_aicmap((struct hv_aicmap \*)l->aicmap);/aicmap_size = 8;/@run
nohost-vuart@selfboot_run.c@s/    if (host == NO_HOST) {/    if (0) {/@run
nohost-uart-left@selfboot_run.c@s/hv_unmap(uart, ALIGN_UP(size, STAGE2_PAGE)) < 0) {/0) {/@run
nohost-uart-part-left@selfboot_run.c@s/hv_unmap(uart, ALIGN_UP(size, STAGE2_PAGE))/hv_unmap(uart, STAGE2_PAGE)/@run
nohost-uart-named@selfboot_run.c@s/    if (host == NO_HOST && selfboot_adt_unname/    if (0 \&\& selfboot_adt_unname/@run
host-uart-unnamed@selfboot_run.c@s/    if (host == NO_HOST && selfboot_adt_unname/    if (selfboot_adt_unname/@run
loaded-no-host-ignored@selfboot_run.c@s/host_iodev == SELFBOOT_NO_HOST ? NO_HOST : host_iodev/host_iodev/@run
unname-part-of-it@selfboot.c@s/memset(copy_of(dest, prop)->value, 0, prop->size);/memset(copy_of(dest, prop)->value, 0, 1);/@run
unname-m1n1-tree@selfboot.c@s/memset(copy_of(dest, prop)->value, 0, prop->size);/memset((void *)prop->value, 0, prop->size);/@run
nohost-ports-not-given-back@selfboot_run.c@s/            iodev_set_usage(i, usage\[i\]);/            ;/@run
ports-given-back-with-host@selfboot_run.c@s/^    if (host == NO_HOST)$/    if (1)/@run
nohost-not-entered-is-left@selfboot_run.c@s/^    if (host != NO_HOST)$/    if (1)/@run
EOF
[ $bad = 0 ] && echo "broken: all $n failed, as they must"
exit $bad
