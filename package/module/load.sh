#!/system/bin/sh
# Load the module with the correct preflight arguments.
#
# This is the SINGLE SOURCE OF TRUTH for "how to insmod this module": the boot
# path (post-fs-data.sh) runs it, and so does the DroidVM app when the user
# re-enables the module at runtime. Keeping one script means a runtime enable
# always reproduces the boot-time configuration - most importantly the v10 CMA
# preflight values (migrate_cma_val / pageblock_order_val), without which the
# module's whole CMA side stays off for that load - and a future change to the
# preflight lands in one place instead of drifting between the two callers.
#
# Deliberately side-effect free apart from the insmod: no crash stamp, no
# disable-file check, no log redirection, no dmesg tap. Those are boot-path
# concerns and post-fs-data.sh keeps them.
#
# Prints its preflight decisions on stdout (post-fs-data.sh redirects them into
# load.log; the app captures them). Exits with the insmod's status.
DIR=/data/adb/modules/gh-hugepage-reserve
KO="$DIR/gh_hugepage_reserve.ko"

if ! [ -f "$DIR/settings.prop" ]; then
	echo "pool_want=1024" > "$DIR/settings.prop"
fi
. "$DIR/settings.prop"

# Accept legacy pool_target= settings.prop for backward compatibility.
SZ="${pool_want:-${pool_target:-1024}}"

# ABI preflight: compare the running kernel's real symbol signatures (from
# /sys/kernel/btf/vmlinux) to what this .ko was built to expect, and disable any
# that drifted so a mismatched symbol is left unresolved (its feature returns
# -ENOSYS) instead of kCFI-panicking on first call. Also reads MIGRATE_CMA's
# value for the v10 CMA reservoir (the enumerator is config/vendor-dependent, so
# the module must not trust its build headers). Parsed per line: the output is
# multi-line (migrate_cma= + disable=). Fail-open: if the helper is absent or
# cannot read BTF, trust the compile-time version gates (old behaviour) and
# leave the reservoir off (migrate_cma_val=-1).
DIS_ARG=""
MIGRATE_CMA=""
CHK="$DIR/kapi_check"
if [ -x "$CHK" ]; then
	OUT="$("$CHK" /sys/kernel/btf/vmlinux)"
	DISABLE="$(printf '%s\n' "$OUT" | sed -n 's/^disable=//p' | head -n1)"
	MIGRATE_CMA="$(printf '%s\n' "$OUT" | sed -n 's/^migrate_cma=//p' | head -n1)"
	echo "kapi_check -> disable_kapi='${DISABLE}' migrate_cma='${MIGRATE_CMA}'"
	[ -n "$DISABLE" ] && DIS_ARG="disable_kapi=$DISABLE"
fi

# v10 CMA reservoir preflight: pageblock_order is a pure macro in the kernel
# (no variable, not in kallsyms/BTF), but /proc/pagetypeinfo prints it. Missing
# or unreadable -> -1 -> the module keeps the reservoir off (v9 path).
PB_ORDER="$(sed -n 's/^Page block order:[[:space:]]*//p' /proc/pagetypeinfo 2>/dev/null | head -n1)"
WCMA="${pool_want_with_cma:-0}"		# from settings.prop; 0 = no reservoir
V10_ARGS="migrate_cma_val=${MIGRATE_CMA:--1} pageblock_order_val=${PB_ORDER:--1} pool_want_with_cma=${WCMA}"

# cma_reservoir_floor_mb (settings.prop, optional; module default 512): the
# headroom floor for flipping pageblocks to CMA - available-minus-CmaFree must
# stay above it, because a CMA label is invisible to GFP_KERNEL and the
# unmovable working set cannot follow memory in there. Small-RAM phones need it
# lower or the reservoir can never grow at all.
#
# It has to be an INSMOD arg, not a later write to its (0600) sysfs file: the
# synchronous prefill inside insmod already builds the reservoir - AVAIL_TO_CMA
# has no stage gate, and the reservoir is meant to take the cleanest memory of
# the boot - and it consults this floor on every flip. A write after insmod
# returns is one boot too late. Rides the v10 CMA group because it is a v10-era
# param: it belongs with them and falls away at the same rung.
if [ -n "${cma_reservoir_floor_mb:-}" ]; then
	V10_ARGS="$V10_ARGS cma_reservoir_floor_mb=$cma_reservoir_floor_mb"
fi
echo "v10 args: $V10_ARGS"

# v11 movable->CMA lever: the DroidVM app records the user's saved choice in
# cma_movable_lever (hook|flag). Re-apply it at insmod so a reservoir the user
# chose to keep is actually consumable by apps after a reboot - without this the
# reservoir rebuilds but nothing arms the movable->CMA redirect, so it sits idle.
# Absent = no lever (Module CMA only). The module records the desire at insmod
# and arms it in cma_boot_build; it is a no-op when the vendor already redirects.
MTC_ARG=""
case "${cma_movable_lever:-}" in
	hook) MTC_ARG="moveable_to_cma_gfp_cma_hook=1" ;;
	flag) MTC_ARG="moveable_to_cma_restrict_cma_redirect_disabled=1" ;;
esac
[ -n "$MTC_ARG" ] && echo "mtc lever: $MTC_ARG"

# system_reserve_mb (settings.prop, optional): RAM the module must leave to the
# system when computing pool_size_max (§8; module default 6144, floor 64).
# Temp-root / small-RAM phones set it lower. Only passed when configured: the
# extra top rung below is rejected by an older .ko lacking the param and the
# ladder falls through unchanged.
SR_ARG=""
if [ -n "${system_reserve_mb:-}" ]; then
	SR_ARG="system_reserve_mb=$system_reserve_mb"
	echo "system reserve: ${system_reserve_mb}MB"
fi

# boot_acquire* readback: feed the configured values so the module's 0400 marker
# params show what this boot requested (the module takes no action on any of the
# three; the firing block after insmod is the policy). Same top ladder rung as
# system_reserve_mb - an older .ko rejects the unknown params and the ladder
# falls through to the line below unchanged.
BA_ARG=""
if [ -n "${boot_acquire:-}" ]; then
	BA_ARG="boot_acquire=$boot_acquire"
fi
if [ -n "${boot_acquire_runs:-}" ]; then
	BA_ARG="$BA_ARG boot_acquire_runs=$boot_acquire_runs"
fi
if [ -n "${boot_acquire_wait:-}" ]; then
	BA_ARG="$BA_ARG boot_acquire_wait=$boot_acquire_wait"
fi

# Pass BOTH size params first: a lenient kernel silently ignores the one the
# module lacks, so it still gets SZ via pool_want (v7) or pool_target (v6).
# Strict kernels reject an unknown param, so fall back progressively: the full
# v11 set (v10 args + lever), then v10 without the lever (older .ko lacking the
# moveable_to_cma_* params), then without the v10 params at all, then without the
# ABI guard for a .ko predating disable_kapi, then a bare (default-size) load.
#
# Rung 2 is rung 1 minus pool_target alone. This module deliberately never
# defines that legacy name (the ladder degrades by BEING rejected, so accepting
# it would invert the mechanism), which means a strict kernel rejects rung 1
# outright for a current .ko - and rung 3 has already dropped $SR_ARG and
# $BA_ARG with it, making system_reserve_mb and the boot_acquire trio
# structurally unreachable there. Dropping only the museum-piece name keeps
# them. No-op on a lenient kernel, where rung 1 wins anyway.
insmod "$KO" $DIS_ARG $SR_ARG $BA_ARG pool_want="$SZ" pool_target="$SZ" $V10_ARGS $MTC_ARG ||
	insmod "$KO" $DIS_ARG $SR_ARG $BA_ARG pool_want="$SZ" $V10_ARGS $MTC_ARG ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" $V10_ARGS $MTC_ARG ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" $V10_ARGS $MTC_ARG ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" $V10_ARGS ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" $V10_ARGS ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" $DIS_ARG pool_want="$SZ" ||
	insmod "$KO" $DIS_ARG pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" pool_target="$SZ" ||
	insmod "$KO" pool_want="$SZ" ||
	insmod "$KO" pool_target="$SZ" ||
	insmod "$KO"
RC=$?

# boot_acquire (settings.prop, 0-3; default 0): once the module is up, press
# the acquire button the user configured - the same USER run the app's button
# starts, so refill_stat shows progress and acquire=0 cancels it. The
# synchronous prefill already ran inside insmod; this tops up on temp-root /
# fragmented boots. A rejected mode (-ENOSYS: its symbols are missing on this
# kernel) falls back to the next lighter one; any failure is non-fatal.
#
# Gated on the module being PRESENT, not on this insmod having succeeded: a
# temp-root soft reboot restarts Android's userspace without the kernel going
# down, so this script runs again while the .ko is still loaded and its insmod
# fails with EEXIST. That moment - userspace torn down, no GUI yet - is the
# freest memory the module will ever see, and it is the one moment a temp-root
# device can apply heavy pressure at all. Losing the acquire to an EEXIST there
# is losing the only window.
#
# boot_acquire_runs (default 1): how many USER runs to press, each started once
# the previous one has finished. A run is a single pass - drop_slab once, then
# cheap -> full -> sweep, one direction only - so windows that the sweep's own
# eviction freed behind the cursor are only reachable by the NEXT run; retrying
# is a new trigger by design (POOL_DESIGN 7). A run with nothing left to do
# leaves at its at-target check in O(1), so an extra run costs nothing when the
# first one already met the target.
#
# boot_acquire_wait (seconds, default 0): how long this script may BLOCK on
# those runs. post-fs-data is a blocking boot stage, so a non-zero wait is what
# actually holds zygote back while the sweep works - the point on temp root,
# where light pressure is worthless and there is no GUI yet to press the button
# by hand. Budget it: root managers kill a post-fs-data script that overruns
# (Magisk: 40s). Whatever the budget does not cover continues in the background,
# so 0 is exactly the old fire-and-forget behaviour.
ACQ=/sys/module/gh_hugepage_reserve/parameters/acquire
STAT=/sys/module/gh_hugepage_reserve/parameters/refill_stat
BA="${boot_acquire:-0}"
BA_RUNS="${boot_acquire_runs:-1}"
BA_LEFT_S="${boot_acquire_wait:-0}"
BA_POLL=1
[ "$BA_RUNS" -ge 1 ] 2>/dev/null || BA_RUNS=1
[ "$BA_LEFT_S" -ge 0 ] 2>/dev/null || BA_LEFT_S=0
# Blocking is a boot-path concern (post-fs-data.sh sets GH_BOOT before calling
# us), like the crash stamp and the logs it keeps: holding zygote back is the
# whole point at boot and would just freeze the app's "Enable" at runtime. The
# runs themselves still happen either way - with no budget the sequence hands
# straight over to the background.
[ "${GH_BOOT:-0}" = 1 ] || BA_LEFT_S=0

# Wait out the run in flight. 0 = idle now, 1 = the wait budget ran out first.
ba_wait_idle() {
	while :; do
		case "$(cat "$STAT" 2>/dev/null)" in
			*acquire_active=0*) return 0 ;;
			"") return 0 ;;		# module gone: nothing to wait for
		esac
		[ "$BA_LEFT_S" -ge "$BA_POLL" ] || return 1
		sleep "$BA_POLL"
		BA_LEFT_S=$((BA_LEFT_S - BA_POLL))
	done
}

# Press the runs still owed, one per run completed, then see the last one out.
# 0 = sequence done, 1 = budget spent with a run still in flight.
ba_sequence() {
	while [ "$BA_LEFT" -gt 0 ]; do
		ba_wait_idle || return 1
		echo "$BA_MODE" > "$ACQ" 2>/dev/null || return 0
		BA_LEFT=$((BA_LEFT - 1))
		echo "boot_acquire: acquire=$BA_MODE run $((BA_RUNS - BA_LEFT))/$BA_RUNS started"
	done
	ba_wait_idle
}

BA_MODE=""
if [ -e "$ACQ" ] && [ "$BA" -ge 1 ] 2>/dev/null; then
	if [ "$RC" -ne 0 ]; then
		echo "insmod rc=$RC but the module is loaded (soft reboot?): acquiring anyway"
	fi
	for m in 3 2 1; do
		[ "$m" -le "$BA" ] || continue
		if echo "$m" > "$ACQ" 2>/dev/null; then
			BA_MODE=$m
			break
		fi
		echo "boot_acquire: acquire=$m rejected, trying lighter"
	done
fi
if [ -n "$BA_MODE" ]; then
	BA_LEFT=$((BA_RUNS - 1))
	echo "boot_acquire: acquire=$BA_MODE run 1/$BA_RUNS started (configured $BA, wait=${BA_LEFT_S}s)"
	if [ "$BA_LEFT" -gt 0 ] || [ "$BA_LEFT_S" -gt 0 ]; then
		if ! ba_sequence && [ "$BA_LEFT" -gt 0 ]; then
			echo "boot_acquire: wait budget spent, $BA_LEFT run(s) left: continuing in background"
			( BA_LEFT_S=3600; BA_POLL=5; ba_sequence ) &
		fi
	fi
fi
exit $RC
