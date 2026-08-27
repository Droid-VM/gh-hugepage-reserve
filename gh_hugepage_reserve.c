// SPDX-License-Identifier: GPL-2.0
/*
 * gh_hugepage_reserve — 2MB hugepage pool for Gunyah VMs.
 *
 * Single source of truth: POOL_DESIGN.md. Unity build: the pool object and
 * workers (parts/gh_{owner,pool,release,adjust}) are environment-agnostic and
 * ALSO build in userspace under tests/shim.h — that is the CI mock harness
 * (§3.3), which replaces the kapi backend with a deterministic fake buddy.
 * The kernel-only parts (kapi/hooks/sysfs/unlock_cma) and this root provide
 * the on-device environment and the init/exit sequence (§8); they are
 * verified on-device only.
 */
#define pr_fmt(fmt) "gh_hugepage_reserve: " fmt
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/tracepoint.h>
#include <linux/mmzone.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/jiffies.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

/* KAPI_SYMBOLS x-macro (generated from abi/kapi_abi.tsv) — the kapi backend's
 * disable-list validator expands it. */
#include "abi/kapi_abi.gen.h"

#include "parts/gh_defs.h"
#include "parts/gh_kernel_env.h"

/* pool object + workers (shared with the mock harness) */
#include "parts/gh_owner.c.inc"
#include "parts/gh_pool.c.inc"
#include "parts/gh_release.c.inc"
#include "parts/gh_adjust.c.inc"

/* kernel-only backends */
#include "parts/gh_kapi.c.inc"
#include "parts/gh_hooks.c.inc"
#include "parts/gh_sysfs.c.inc"
#include "parts/gh_unlock_cma.c.inc"
#include "parts/gh_pinprobe.c.inc"		/* /dev/gh_pinprobe CMA-detect ioctl */

/* ── worker plumbing (delayed_work over the round functions) ── */
static struct delayed_work gh_adjust_work, gh_release_work;
static void gh_adjust_fn(struct work_struct *w) { gh_adjust_round(); }
static void gh_release_fn(struct work_struct *w) { gh_release_round(); }
void gh_sched_adjust(int ms) { mod_delayed_work(system_wq, &gh_adjust_work, msecs_to_jiffies(ms)); }
void gh_sched_release(int ms) { mod_delayed_work(system_wq, &gh_release_work, msecs_to_jiffies(ms)); }

/* preflight params (§8/§10). The kapi backend also reads migrate_cma_val /
 * pageblock_order_val — declared there; sim_cma_order is here. */
static int gh_sim_cma_order;
module_param_named(sim_cma_order, gh_sim_cma_order, int, 0400);

/* RAM (MB) never counted into pool_size_max: the pool leaves at least this
 * much (or half of RAM, whichever is less) to the rest of the system. 6G fits
 * a heavy Android resident set (~2-3G unswappable kernel+dmabuf/GPU plus
 * ~2.7G core services); temp-root / small-RAM devices set lower via
 * settings.prop (§8/§10). Insmod-time only (0400); clamped to the floor at
 * init so the readback shows the effective value. */
#define GH_SYSTEM_RESERVE_MIN_MB 64
#define GH_SYSTEM_RESERVE_DEFAULT_MB 6144
static unsigned int gh_system_reserve_mb = GH_SYSTEM_RESERVE_DEFAULT_MB;
module_param_named(system_reserve_mb, gh_system_reserve_mb, uint, 0400);
MODULE_PARM_DESC(system_reserve_mb,
	"RAM (MB) kept away from pool_size_max (default 6144, min 64)");

/* What the pool actually leaves behind: the request, capped at half of RAM —
 * a small phone must not hand the whole machine away because the default was
 * written for a big one. The one place the rule lives; both the configured
 * value and the default below go through it, so they cannot drift apart. */
static unsigned long gh_reserve_keep(unsigned long ram, unsigned int mb)
{
	return min(ram / 2, (unsigned long)mb << 20);
}

/* This DEVICE's default reserve (MB), settled at init: what keep would have
 * been had settings.prop said nothing. NOT the built-in constant — the RAM/2
 * cap makes them differ on small phones (8GB: default 6144 keeps only 4096),
 * and the constant alone would have the app warn about lowering a number the
 * machine never used. The configured value overwrites system_reserve_mb, so
 * once set the default is gone from the readback: this is the only place it
 * survives. Read-only; an insmod-time value is overwritten at init. */
static unsigned int gh_system_reserve_default_mb;
module_param_named(system_reserve_mb_default, gh_system_reserve_default_mb, uint, 0400);
MODULE_PARM_DESC(system_reserve_mb_default,
	"this device's default reserve (MB) = min(RAM/2, 6144); readback only");

/* boot_acquire / boot_acquire_runs / boot_acquire_wait: LOADER policy,
 * recorded here only — load.sh presses the configured acquire mode once the
 * module is up, that many times, blocking that many seconds (§10). The module
 * reads NONE of the three; grep them and you find these three declarations and
 * nothing else. They exist for two reasons: the app detects support by the
 * file existing ("param present = this package's load.sh understands the
 * prop", the same existence-is-capability convention the ladder runs on), and
 * reads back the configured value.
 *
 * WRITABLE (0600) precisely BECAUSE the module is blind to them: a write can
 * change no behaviour, and the app needs somewhere to reflect a saved setting
 * that would otherwise stay invisible until the next load — settings.prop is
 * app-private state, this is the value a user can see. Anything the module
 * acts on stays 0400 (system_reserve_mb sizes a table built once, so a later
 * write would only lie). The mode that actually RAN is refill_stat's
 * acquire_mode: load.sh degrades a rejected mode 3->2->1, so the two can
 * legitimately differ, and after an EEXIST reload these hold whatever was last
 * written rather than what this boot used. */
static unsigned int gh_boot_acquire;
module_param_named(boot_acquire, gh_boot_acquire, uint, 0600);
MODULE_PARM_DESC(boot_acquire,
	"boot-time acquire mode applied by load.sh (0=off; marker + readback, no module action)");

static unsigned int gh_boot_acquire_runs = 1;
module_param_named(boot_acquire_runs, gh_boot_acquire_runs, uint, 0600);
MODULE_PARM_DESC(boot_acquire_runs,
	"boot-time acquire runs pressed by load.sh (default 1; marker + readback, no module action)");

static unsigned int gh_boot_acquire_wait;
module_param_named(boot_acquire_wait, gh_boot_acquire_wait, uint, 0600);
MODULE_PARM_DESC(boot_acquire_wait,
	"seconds load.sh may block on those runs (default 0; marker + readback, no module action)");

/* ── RAM range discovery (§8): kapi.walk_system_ram_range or fallback ── */
static int __init gh_init(void)
{
	struct gh_ram_range *ranges;
	int nr, filled, size_max, cma_order, migrate_cma;
	unsigned long ram = totalram_pages() << PAGE_SHIFT;
	unsigned long keep;

	if (gh_system_reserve_mb < GH_SYSTEM_RESERVE_MIN_MB)
		gh_system_reserve_mb = GH_SYSTEM_RESERVE_MIN_MB;
	keep = gh_reserve_keep(ram, gh_system_reserve_mb);
	gh_system_reserve_default_mb = (unsigned int)
		(gh_reserve_keep(ram, GH_SYSTEM_RESERVE_DEFAULT_MB) >> 20);
	size_max = (int)((min(ram - keep, (24UL << 30))) >> (PAGE_SHIFT + GH_PAGE_ORDER));

	INIT_DELAYED_WORK(&gh_adjust_work, gh_adjust_fn);
	INIT_DELAYED_WORK(&gh_release_work, gh_release_fn);

	gh_kapi_init();				/* fill the adapter table (fork) */
	migrate_cma = gh_kapi_migrate_cma_val();	/* -1 if unavailable */
	cma_order = gh_sim_cma_order > GH_PAGE_ORDER ? gh_sim_cma_order
		    : gh_kapi_pageblock_order_val();

	/* present RAM ranges, two passes: count, allocate exactly, fill — no
	 * guessed cap to truncate at (§8: the OnePlus 15 has 19 ranges and the
	 * 13GB main bank comes LAST; a guessed 16 silently starved the pool).
	 * insmod-time allocation only, freed right after the table is built. */
	nr = gh_kapi.walk_ram(NULL, 0);
	if (nr <= 0) {
		pr_err("no present-RAM ranges discovered\n");
		return -ENODEV;
	}
	ranges = kcalloc(nr, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return -ENOMEM;
	filled = gh_kapi.walk_ram(ranges, nr);
	if (filled != nr) {			/* hotplug between passes: rare */
		pr_warn("RAM ranges changed between passes (%d -> %d)\n", nr, filled);
		if (filled < nr)
			nr = filled;
	}
	{
		unsigned long cov = 0;
		int i;

		for (i = 0; i < nr; i++)
			cov += ranges[i].end_pfn - ranges[i].start_pfn;
		pr_info("RAM: %d ranges, %lu MB present\n",
			nr, cov >> (20 - PAGE_SHIFT));
	}
	if (gh_pool_init_table(ranges, nr, size_max)) {
		kfree(ranges);
		pr_err("table init failed\n");
		return -ENOMEM;
	}
	kfree(ranges);				/* consumed by init_table, not kept */
	gh_pool_set_cma_config(cma_order, migrate_cma);
	gh_pool_mark_carveouts(migrate_cma);	/* vendor CMA + ZONE_MOVABLE (§8) */

	/* apply insmod-time targets now that pool_size_max is known (§8): the
	 * param setters only stashed them (they ran before the table existed). */
	{
		enum gh_target_change ch;

		if (gh_insmod_want >= 0)
			gh_pool_set_target(GH_TARGET_WANT, gh_insmod_want, &ch);
		if (gh_insmod_want_cma >= 0)
			gh_pool_set_target(GH_TARGET_WANT_CMA, gh_insmod_want_cma, &ch);
	}

	/* synchronous prefill: drive one INSMOD run to completion here, in
	 * module_init context, before arming hooks (§8). */
	gh_adjust_try(GH_PROFILE_INSMOD);
	{
		int guard = GH_ADJUST_RUN_MAX;

		while (gh_adjust_g_active && guard-- > 0)
			gh_adjust_round();
	}

	gh_hooks_attach();			/* hooks AFTER the pool is built */
	gh_unlock_cma_boot_apply();		/* vendor status + insmod lever desires (fork) */
	if (gh_pool_cma_capable())
		cma_adjust_hook_register();	/* gfp bypass hook (§9; no-op if vendor already redirects) */
	gh_pool_set_ready(true);
	gh_pinprobe_register();			/* /dev/gh_pinprobe (CMA-detect); non-fatal */
	pr_info("ready: held=%d cma=%d (want=%d with_cma=%d)\n",
		gh_pool_held(), gh_pool_cma(), gh_actx.want, gh_actx.want_cma);
	return 0;
}

static void __exit gh_exit(void)
{
	int pass;

	/* rmmod order is law: hooks off, workers off, then hand refs back (§8) */
	gh_pinprobe_unregister();		/* stop answering /dev/gh_pinprobe first */
	cma_adjust_hook_unregister();		/* §9: detach bypass BEFORE reservoir teardown */
	gh_hooks_detach();
	gh_release_run(0);
	gh_adjust_cancel();
	cancel_delayed_work_sync(&gh_adjust_work);
	cancel_delayed_work_sync(&gh_release_work);

	gh_pool_served_to_ext(GH_ALL);
	gh_pool_released_to_ext(GH_ALL);
	for (pass = 0; pass < 3 && gh_pool_cma() > 0; pass++) {
		gh_pool_prepare_cma_scan(1);
		gh_pool_cma_to_ext(gh_pool_cma());
		if (gh_pool_cma())
			msleep(100);
	}
	gh_pool_cand_to_ext();
	while (gh_pool_avail_to_ext(64) > 0)
		cond_resched();
	gh_pool_teardown_table();		/* free chunks + top[] + scans */
	pr_info("unloaded\n");
}

module_init(gh_init);
module_exit(gh_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("HuJK");
MODULE_DESCRIPTION("2MB hugepage pool for Gunyah VMs (design: POOL_DESIGN.md)");
