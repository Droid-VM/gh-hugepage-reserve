#!/bin/sh
# Tier-1 runtime: boot a real GKI aarch64 kernel in QEMU with a Debian aarch64
# rootfs, load the companion + main module, run the serve/reclaim + induced-
# fragmentation scenarios (init.sh). GKI is the real target (its vendor hooks
# and KMI are what the module resolves against); Debian gives a full userspace
# for the exerciser. Set:
#   KERNEL=path/to/GKI/Image        (arch/arm64/boot/Image from the GKI build)
#   ROOTFS=path/to/rootfs.img       (ext4, built by mkrootfs.sh)
set -eu
: "${KERNEL:?set KERNEL=GKI Image}"
: "${ROOTFS:?set ROOTFS=debian aarch64 rootfs.img}"

qemu-system-aarch64 \
	-machine virt -cpu max -smp 2 -m 4096 -nographic \
	-kernel "$KERNEL" \
	-append "console=ttyAMA0 root=/dev/vda rw init=/gh_tier1/init.sh panic=1" \
	-drive file="$ROOTFS",format=raw,if=virtio \
	-no-reboot 2>&1 | tee qemu.log

grep -q 'TIER1: ALL PASS' qemu.log
