#!/bin/sh
# Build a minimal Debian aarch64 ext4 rootfs with the modules + exerciser + the
# in-guest test plan installed at /gh_tier1/. Needs debootstrap + qemu-user
# (binfmt) for the aarch64 second stage, and mke2fs.
set -eu
: "${MAIN_KO:?set MAIN_KO=path to gh_hugepage_reserve.ko for this tag}"
: "${COMPANION_KO:?set COMPANION_KO=path to gh_test_companion.ko}"
: "${EXERCISER:?set EXERCISER=path to the aarch64 exerciser binary}"
ROOT=$(mktemp -d)

sudo debootstrap --arch=arm64 --variant=minbase \
	--include=kmod,procps stable "$ROOT" http://deb.debian.org/debian
sudo mkdir -p "$ROOT/gh_tier1"
sudo cp "$MAIN_KO"      "$ROOT/gh_tier1/gh_hugepage_reserve.ko"
sudo cp "$COMPANION_KO" "$ROOT/gh_tier1/gh_test_companion.ko"
sudo cp "$EXERCISER"    "$ROOT/gh_tier1/exerciser"
sudo cp init.sh         "$ROOT/gh_tier1/init.sh"
sudo chmod +x "$ROOT/gh_tier1/init.sh" "$ROOT/gh_tier1/exerciser"

dd if=/dev/zero of=rootfs.img bs=1M count=512
mkfs.ext4 -F -d "$ROOT" rootfs.img
sudo rm -rf "$ROOT"
echo "rootfs.img built"
