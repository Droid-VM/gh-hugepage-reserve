#!/bin/bash
# Build tier1/initramfs.cpio.gz from the freshly-built .ko's. Absolute output
# path on purpose: the cpio runs in a temp-dir subshell, so a relative redirect
# would land in (and vanish with) the temp dir — that stale-initramfs trap cost
# real debugging time.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/initramfs.cpio.gz"
D=$(mktemp -d)
cp "$HERE/tier1_init"            "$D/init"
cp "$HERE/out/android15-6.6.ko" "$D/gh_hugepage_reserve.ko"
cp "$HERE/out_comp/android15-6.6.ko" "$D/gh_test_companion.ko"
mkdir -p "$D/proc" "$D/sys" "$D/dev"
( cd "$D" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip > "$OUT" )
rm -rf "$D"
# verify what landed matches the source build
A=$(md5sum "$HERE/out/android15-6.6.ko" | cut -d' ' -f1)
V=$(mktemp -d); ( cd "$V" && zcat "$OUT" | cpio -idm 2>/dev/null )
B=$(md5sum "$V/gh_hugepage_reserve.ko" | cut -d' ' -f1); rm -rf "$V"
[ "$A" = "$B" ] || { echo "MKINITRAMFS: STALE ($A != $B)"; exit 1; }
echo "MKINITRAMFS: ok ($A)"
