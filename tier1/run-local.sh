#!/bin/bash
# Tier-1 local runner: build the two .ko for a GKI tag via DDK, build the
# static aarch64 init + initramfs, boot the GKI Image in QEMU, assert PASS.
# Needs: docker (DDK image), aarch64-linux-gnu-gcc, qemu-system-aarch64, and a
# built GKI Image (KIMG). Default tag android15-6.6.
set -euo pipefail
TAG="${TAG:-android15-6.6}"
DDK="ghcr.io/ylarod/ddk-min:${TAG}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${KIMG:?set KIMG=path to GKI arch/arm64/boot/Image for $TAG}"
cd "$ROOT"

mkdir -p tier1/out tier1/out_comp
docker run --rm -v "$ROOT:/src:ro" -v "$ROOT/tier1/out:/out_gh_hugepage_reserve" \
	"$DDK" sh /src/docker_exec.sh gh_hugepage_reserve.c:gh_hugepage_reserve
docker run --rm -v "$ROOT:/src:ro" -v "$ROOT/tier1/out_comp:/out" "$DDK" sh -c '
	set -e; C=$(ls -d /opt/ddk/clang/clang-*); K=$(ls /opt/ddk/kdir/); export PATH="$C/bin:$PATH"
	B=/tmp/cb; mkdir -p "$B"; cp /src/tier1/gh_test_companion.c "$B/gh_test_companion.c"
	echo "obj-m += gh_test_companion.o" > "$B/Makefile"
	make -C /opt/ddk/kdir/$K -j"$(nproc)" M="$B" ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
	llvm-strip -d "$B/gh_test_companion.ko"; cp "$B/gh_test_companion.ko" /out/$K.ko'
aarch64-linux-gnu-gcc -O2 -static -Wall -o tier1/tier1_init tier1/tier1_init.c

D=$(mktemp -d)
cp tier1/tier1_init "$D/init"
cp tier1/out/"$TAG".ko "$D/gh_hugepage_reserve.ko"
cp tier1/out_comp/"$TAG".ko "$D/gh_test_companion.ko"
mkdir -p "$D/proc" "$D/sys" "$D/dev"
( cd "$D" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip > "$ROOT/tier1/initramfs.cpio.gz" )
rm -rf "$D"

timeout 180 qemu-system-aarch64 -machine virt -cpu max -smp 2 -m 4096 -nographic \
	-kernel "$KIMG" -initrd tier1/initramfs.cpio.gz \
	-append "console=ttyAMA0 rdinit=/init panic=1" -no-reboot | tee tier1/qemu.log
grep -q 'TIER1: ALL PASS' tier1/qemu.log
