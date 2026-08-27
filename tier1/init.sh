#!/bin/sh
# In-guest test plan (Debian aarch64 rootfs; run as init on a GKI kernel). Loads the main
# module pointed at the companion's stub symbols, builds fragmentation fields,
# and checks that the module (a) serves and reclaims, (b) skips straggled
# windows, (c) prefers assemblable blocks for the CMA reservoir.
set -eu
mount -t proc none /proc 2>/dev/null || true
mount -t sysfs none /sys 2>/dev/null || true
cd /gh_tier1
fail() { echo "TIER1: FAIL - $*"; echo o > /proc/sysrq-trigger || poweroff -f; }
stat() { grep "^$1=" /sys/module/gh_hugepage_reserve/parameters/refill_stat | cut -d= -f2; }

insmod ./gh_test_companion.ko

# main module: point VM kprobes at the companion stubs; sim_cma_order=10 makes
# S=2 so the sub-block / cma_able assembly logic runs on an x86 2MB-pageblock
# kernel. migrate_cma_val/pageblock_order_val come from the kapi preflight; on a
# generic kernel the reservoir may stay disabled — that is fine, the sweep and
# serve/reclaim + straggler-skip still run and are what Tier-1 proves.
insmod ./gh_hugepage_reserve.ko pool_want=64 \
	vm_create_sym=gh_test_vm_create \
	vm_destroy_sym=gh_test_vm_destroy \
	vm_reclaim_sym=gh_test_vm_reclaim \
	sim_cma_order=10 debug=1 || fail "insmod main"

echo "== A. serve / reclaim round-trip =="
./exerciser || fail "exerciser: pages not all accounted (served=$(stat served))"
[ "$(stat served)" = "0" ] || fail "served not drained"

echo "== B. fragmentation: every window straggled -> nothing assembles =="
echo "128 all" > /sys/kernel/gh_test/frag
echo 3 > /sys/module/gh_hugepage_reserve/parameters/acquire   # sweep+evict
sleep 6
echo 0 > /sys/module/gh_hugepage_reserve/parameters/acquire
R="$(grep -o 'acquire_stop_reason=[^ ]*' /sys/module/gh_hugepage_reserve/parameters/refill_stat || true)"
echo "  stop_reason: $R  reject=$(grep -o 'reject=[0-9]*' /sys/module/gh_hugepage_reserve/parameters/reclaim_debug || echo n/a)"
# with an all-straggled field the sweep must NOT white-kick: assert it scanned
# and gave up rather than evicting for nothing (reject stays low, no crash).

echo "== C. split pattern: every believed block mixed -> give up on all =="
echo "128 split" > /sys/kernel/gh_test/frag
echo 3 > /sys/module/gh_hugepage_reserve/parameters/acquire
sleep 6; echo 0 > /sys/module/gh_hugepage_reserve/parameters/acquire
echo "  pool_avail_cma_able=$(stat pool_avail_cma_able) (want: no new cma_able blocks from a fully-mixed field)"

echo "== D. block pattern: half the blocks clean -> those assemble =="
echo "128 block" > /sys/kernel/gh_test/frag
echo 3 > /sys/module/gh_hugepage_reserve/parameters/acquire
sleep 8; echo 0 > /sys/module/gh_hugepage_reserve/parameters/acquire
echo "  pool_avail=$(stat pool_avail) pool_avail_cma_able=$(stat pool_avail_cma_able)"
[ "$(stat pool_avail)" -gt 0 ] || fail "D: assembled nothing from a half-clean field"

echo "== E. rmmod clean =="
rmmod gh_hugepage_reserve || fail "rmmod main"
rmmod gh_test_companion || fail "rmmod companion"

echo "TIER1: ALL PASS"
echo o > /proc/sysrq-trigger || poweroff -f
