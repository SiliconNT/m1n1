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

# What is wrong @ the file @ the change, for sed @ the test that must fail: layout or run
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
    else
        other=hv_layout.c; [ "$file" = hv_layout.c ] && other=selfboot_run.c
        gcc $CFLAGS -o "$work/t" run_test.c model.c build/selfboot.c build/selfboot_files.c \
            "$work/$file" "build/$other" 2>"$work/cc.log"
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
EOF
[ $bad = 0 ] && echo "broken: all $n failed, as they must"
exit $bad
