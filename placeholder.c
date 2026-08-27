// SPDX-License-Identifier: GPL-2.0
/*
 * gh_hugepage_reserve — PLACEHOLDER build.
 *
 * The real module (gh_hugepage_reserve.c) needs mm internals that only exist on
 * GKI 6.1+ (the kapi version gates, MAX_PAGE_ORDER, folio APIs, …), so it does
 * not compile on older kernels (5.10 / 5.15). Rather than ship no .ko for those
 * KMIs — which would make the Magisk/KernelSU module fail to install or the app
 * fail to load it — we ship this: a module that loads cleanly and does NOTHING.
 *
 * It is named `gh_hugepage_reserve` (so /sys/module/gh_hugepage_reserve exists
 * and the app sees "loaded"), and it declares every insmod parameter load.sh
 * passes so the very first insmod attempt succeeds with no "unknown parameter"
 * noise. All parameters are accepted and ignored. There are no hooks, no pool,
 * no sysfs behaviour, no CMA reservoir — the reserve is simply absent on this
 * kernel, and everything downstream degrades gracefully (the app reads no pool
 * data; crosvm's pin probe node is absent so it just continues; VMs run as they
 * would with the module uninstalled).
 */
#define pr_fmt(fmt) "gh_hugepage_reserve: " fmt
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>

/* Accept-and-ignore every parameter load.sh (package/module/load.sh) passes, so
 * insmod succeeds on the first attempt regardless of which fallback rung it is.
 * Values are never read. */
static int   ph_pool_want, ph_pool_target, ph_pool_want_with_cma;
static int   ph_migrate_cma_val = -1, ph_pageblock_order_val = -1;
static int   ph_mtc_hook, ph_mtc_flag, ph_mtc_allowed;
static char *ph_disable_kapi;

module_param_named(pool_want, ph_pool_want, int, 0644);
module_param_named(pool_target, ph_pool_target, int, 0644);
module_param_named(pool_want_with_cma, ph_pool_want_with_cma, int, 0644);
module_param_named(migrate_cma_val, ph_migrate_cma_val, int, 0444);
module_param_named(pageblock_order_val, ph_pageblock_order_val, int, 0444);
module_param_named(moveable_to_cma_gfp_cma_hook, ph_mtc_hook, int, 0444);
module_param_named(moveable_to_cma_restrict_cma_redirect_disabled, ph_mtc_flag, int, 0444);
module_param_named(moveable_to_cma_vender_already_allowed, ph_mtc_allowed, int, 0444);
module_param_named(disable_kapi, ph_disable_kapi, charp, 0444);

static int __init gh_placeholder_init(void)
{
	pr_info("placeholder build: this kernel is unsupported by the hugepage reserve; "
		"module loaded with NO functionality (no pool, no CMA, no hooks)\n");
	return 0;
}

static void __exit gh_placeholder_exit(void)
{
}

module_init(gh_placeholder_init);
module_exit(gh_placeholder_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gh_hugepage_reserve placeholder (unsupported kernel; no functionality)");
MODULE_AUTHOR("DroidVM");
