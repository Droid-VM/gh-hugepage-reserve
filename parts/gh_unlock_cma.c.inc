/* gh_hugepage_reserve — unlock_cma bypass (POOL_DESIGN.md §9).
 *
 * KERNEL-ONLY: this file is #included by the unity root after real linux
 * headers; it never builds under the userspace mock harness (it touches
 * vendor tracepoints and a global static key). It is a side-channel: it uses
 * only kapi/kernel symbols and NEVER touches pool/worker state or any state
 * machine edge — its whole job is to make the `cma` reservoir actually
 * borrowable (a plain MOVABLE alloc reaches the CMA freelist), not to move a
 * page. It can be switched on/off and can fail independently.
 *
 * It is a BET, not a feature: there is no API to ask "does this kernel honor
 * it" or "did arming take effect". ALLOC_CMA is an mm/internal.h #define with
 * no BTF, so the preflight can't verify it; arming has no return value. Hence
 * everything here is verified ON-DEVICE ONLY (build reservoir → run stress →
 * watch lent rise). The module reports "I attached it", never "it worked".
 *
 * Gated to kernel [6.1,6.16) with the rest of the CMA feature.
 */

/* Free system CMA floor (MB) below which the gfp bypass stops granting
 * ALLOC_CMA to non-__GFP_CMA movable allocations — an absolute brake so the
 * bypass can never drain the reservoir to exhaustion (§9 floor gate). */
static unsigned int cma_bypass_floor_mb = 256;
module_param(cma_bypass_floor_mb, uint, 0600);
MODULE_PARM_DESC(cma_bypass_floor_mb,
	"Free system CMA floor (MB) below which the gfp bypass stops granting ALLOC_CMA to plain movable allocations (default 256)");

/* Live arm state of the gfp bypass, driven by the moveable_to_cma_gfp_cma_hook
 * knob. Soft flag: the tracepoint probe stays registered; cma_bypass_wants()
 * bails on its first line when false (a disabled no-op probe writes nothing,
 * so it never perturbs another probe on the same shared vendor hook). */
static bool cma_bypass_enabled;

/* Settled ONCE at boot: 1 iff restrict_cma_redirect resolved AND is currently
 * false (vendor already redirects all movable → CMA), else 0. When 1 both
 * levers are redundant and their writes become accepted no-ops. */
static int mtc_vender_already_allowed;

/* insmod-time desires, applied by gh_unlock_cma_boot_apply() once capability
 * and vendor status are known (runtime writes apply immediately instead).
 * restrict: -1 = untouched, 0/1 = desired knob value (1 = redirect enabled /
 * restrict disabled, i.e. movable CAN migrate into CMA). hook: 0/1 arm. */
static int mtc_restrict_want = -1;
static int mtc_gfp_hook_want;

/* ── restrict_cma_redirect key: unlock_cma-private resolution ──────────────
 * The static key is a DATA symbol (no call signature, no btf_trace typedef),
 * so it is unverifiable by the preflight and absent from the coarse gh_kapi
 * table; resolve it locally by name via kallsyms, exactly as before. The
 * togglers are kprobe-resolved funcs. A resolver private to this file (named
 * mtc_* so it can't collide with the kapi backend's own resolver in the unity
 * build). */
static struct static_key *mtc_restrict_key;
static void (*mtc_static_key_enable)(struct static_key *key);
static void (*mtc_static_key_disable)(struct static_key *key);

/* Resolve a non-exported symbol via a throwaway kprobe. NULL if absent. */
static void *mtc_resolve_kfunc(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	void *addr = NULL;

	if (register_kprobe(&kp) == 0) {
		addr = (void *)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

/* Resolve the restrict lever pieces. Called once at init before boot-apply.
 * CONFIG_KALLSYMS_ALL=y on these GKI bases keeps the data symbol in kallsyms.
 * Any miss leaves the restrict lever unavailable (writes -ENOSYS). The lever
 * stays opt-in (default untouched) and only ever flips on an explicit sysfs
 * write, bounding the blast radius if a kernel ever carried a different symbol
 * under this name. */
static void mtc_restrict_resolve(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 16, 0)
	unsigned long (*klookup)(const char *name);

	klookup = mtc_resolve_kfunc("kallsyms_lookup_name");
	mtc_static_key_enable  = mtc_resolve_kfunc("static_key_enable");
	mtc_static_key_disable = mtc_resolve_kfunc("static_key_disable");
	if (klookup)
		mtc_restrict_key =
			(struct static_key *)klookup("restrict_cma_redirect");
#endif
}

/*
 * ALLOC_CMA (mm/internal.h): the bit gfp_to_alloc_flags_cma() ORs into
 * alloc_flags to admit a request to the CMA freelist. A plain #define, not an
 * enum — carries no BTF type, cannot be verified at preflight. Confirmed 0x80
 * on every stock GKI KMI 5.10..mainline; the low bits are structurally fixed
 * (0-2 watermark, 0x40 CPUSET, 0x80 CMA) across the 6.4 reserve-flag rename.
 * Gated to [6.1,6.16) with the rest of the cma feature.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 16, 0)
#define GH_ALLOC_CMA	0x80u
#endif

/* Which of the two ABI-era hook variants cma_adjust_hook_register() found.
 * Declared unconditionally so the init log line compiles either way. */
static bool cma_adjust_is_calc;

#ifdef GH_ALLOC_CMA
/*
 * The bypass decision, shared by both hook variants. Cheapest checks first.
 * Widens ALLOC_CMA past its normal __GFP_CMA gate for plain MOVABLE requests
 * (page cache, mTHP anon) — but only while it costs the reservoir and the
 * system nothing:
 *   0. the gfp_cma_hook lever is armed (moveable_to_cma_gfp_cma_hook=1);
 *   1. a reservoir exists AND capability is settled (§9);
 *   2. gathering is quiescent — prefill finished (gh_pool_ready) and no adjust
 *      run in flight AT ALL (gh_adjust_running()) — else app consumption races
 *      the gatherer for the free pages / CMA blocks it is working on. Any
 *      profile can carry CMA-touching stages (CTA/FULL — release-triggered runs
 *      included), so the gate is the run bit, not the profile. Lends whatever
 *      is built, even if acquire stalled below target;
 *   3. free system CMA stays above cma_bypass_floor_mb after granting this.
 *
 * Testing __GFP_MOVABLE set and __GFP_RECLAIMABLE clear is exactly
 * MIGRATE_MOVABLE in this encoding, and sidesteps gfp_migratetype()'s read of
 * the un-exported page_group_by_mobility_disabled. It names the public __GFP_*
 * macros, so it recompiles correctly against any target kernel's bit values.
 */
static bool cma_bypass_wants(gfp_t gfp_mask)
{
	unsigned long floor_pages;

	if (!READ_ONCE(cma_bypass_enabled))
		return false;
	if ((gfp_mask & (__GFP_MOVABLE | __GFP_RECLAIMABLE)) != __GFP_MOVABLE)
		return false;
	if (!gh_pool_cma_capable())			/* was: cma_capable (VERIFIED) */
		return false;
	if (gh_pool_cma() <= 0)				/* no reservoir assembled yet */
		return false;
	/* Don't lend while the reservoir is being GATHERED: prefill not finished,
	 * or ANY adjust run in flight. App consumption would race the gatherer
	 * for the very free pages / pageblocks it is assembling into or evicting
	 * from CMA. What matters is whether the run touches the CMA area — a
	 * per-stage gate would need the worker's racy internals, and every
	 * profile (release-triggered runs included) can reach CMA-touching
	 * stages, so one bit gates it all (§9). Runs are single-pass and short;
	 * between runs the reservoir is as built as it will get, so lend whatever
	 * assembled — including when acquire stalled below target. */
	if (!gh_pool_ready() || gh_adjust_running())
		return false;

	floor_pages = (unsigned long)READ_ONCE(cma_bypass_floor_mb) << (20 - PAGE_SHIFT);
	if (gh_kapi.cma_free_pages() < floor_pages)
		return false;

	return true;
}

/* <6.12-era hook: android_vh_alloc_flags_cma_adjust(gfp_mask, alloc_flags*),
 * called AFTER the built-in ALLOC_CMA decision. */
static void gh_alloc_flags_cma_adjust(void *data, gfp_t gfp_mask,
				      unsigned int *alloc_flags)
{
	(void)data;
	if (cma_bypass_wants(gfp_mask))
		*alloc_flags |= GH_ALLOC_CMA;
}

/* 6.12+-era hook: android_vh_calc_alloc_flags(gfp_mask, alloc_flags*, bypass*),
 * called BEFORE the built-in decision. *bypass is left untouched so that
 * decision still runs afterward unmodified — we only ever OR the same bit it
 * would, so which runs first is irrelevant. */
static void gh_calc_alloc_flags_adjust(void *data, gfp_t gfp_mask,
				       unsigned int *alloc_flags, bool *bypass)
{
	(void)data; (void)bypass;
	if (cma_bypass_wants(gfp_mask))
		*alloc_flags |= GH_ALLOC_CMA;
}

/* Both located by name (no exported symbol; no btf_trace typedef in GKI
 * vmlinux BTF, so never disabled by the preflight — kapi_abi.tsv keeps them
 * for docs + the manual disable_kapi path). At most one name exists on any
 * kernel. */
static struct tracepoint *cma_adjust_tp;
static bool cma_adjust_active;

static void find_cma_adjust_tp(struct tracepoint *tp, void *priv)
{
	(void)priv;
	if (!cma_adjust_tp && !strcmp(tp->name, "android_vh_alloc_flags_cma_adjust"))
		cma_adjust_tp = tp;
}

static void find_calc_alloc_flags_tp(struct tracepoint *tp, void *priv)
{
	(void)priv;
	if (!cma_adjust_tp && !strcmp(tp->name, "android_vh_calc_alloc_flags"))
		cma_adjust_tp = tp;
}

static void *cma_adjust_probe_fn(void)
{
	return cma_adjust_is_calc ? (void *)gh_calc_alloc_flags_adjust
				  : (void *)gh_alloc_flags_cma_adjust;
}

static int cma_adjust_hook_register(void)
{
	int ret;

	if (cma_adjust_active)
		return 0;
	if (!cma_adjust_tp) {
		if (!gh_kapi_sym_disabled("android_vh_alloc_flags_cma_adjust")) {
			for_each_kernel_tracepoint(find_cma_adjust_tp, NULL);
			if (cma_adjust_tp)
				cma_adjust_is_calc = false;
		}
		if (!cma_adjust_tp && !gh_kapi_sym_disabled("android_vh_calc_alloc_flags")) {
			for_each_kernel_tracepoint(find_calc_alloc_flags_tp, NULL);
			if (cma_adjust_tp)
				cma_adjust_is_calc = true;
		}
		if (!cma_adjust_tp)
			return -ENOENT;
	}
	ret = tracepoint_probe_register(cma_adjust_tp, cma_adjust_probe_fn(), NULL);
	if (ret == 0)
		cma_adjust_active = true;
	return ret;
}

static void cma_adjust_hook_unregister(void)
{
	if (!cma_adjust_active || !cma_adjust_tp)
		return;
	tracepoint_probe_unregister(cma_adjust_tp, cma_adjust_probe_fn(), NULL);
	tracepoint_synchronize_unregister();
	cma_adjust_active = false;
}

/* True iff this kernel exposes one of the two gfp adjust hooks AND we can OR
 * the ALLOC_CMA bit (GH_ALLOC_CMA defined = in-range build). */
static bool kapi_can_gfp_hook(void)
{
	return cma_adjust_tp != NULL;
}
#else
static int cma_adjust_hook_register(void) { return -ENOENT; }
static void cma_adjust_hook_unregister(void) {}
static bool kapi_can_gfp_hook(void) { return false; }
#endif /* GH_ALLOC_CMA */

/*
 * restrict_cma_redirect flip. The static key is a GLOBAL switch: false makes
 * every movable allocation eligible for CMA (and turns on the "prefer CMA when
 * CMA-free > half" balancer), affecting vendor carveouts too — heavier than
 * the gfp hook, which only widens our reservoir's intake. Both togglers sleep
 * (jump_label_mutex + cpus_read_lock), so PROCESS CONTEXT only (sysfs store /
 * init). On 6.6/6.12 the same key backs cma_has_pcplist(): a live flip can
 * strand CMA pages on the per-cpu CMA list, so drain pcp right after (6.1 has
 * no such overload; production DEBUG_VM is off so order_to_pindex's VM_BUG_ON
 * is a no-op regardless). The key is NOT restored at rmmod — its state is a
 * user policy choice, not our resource, and there is no safe "previous value"
 * to roll back to.
 */
static int mtc_restrict_get_state(void)		/* -1 unavailable, else 0/1 */
{
	if (!mtc_restrict_key)
		return -1;
	return static_key_enabled(mtc_restrict_key) ? 1 : 0;
}

static int mtc_restrict_set_state(bool restricted)
{
	if (!mtc_restrict_key ||
	    !mtc_static_key_enable || !mtc_static_key_disable)
		return -ENOSYS;
	if (restricted == (mtc_restrict_get_state() == 1))
		return 0;			/* already in target state */
	if (restricted)
		mtc_static_key_enable(mtc_restrict_key);
	else
		mtc_static_key_disable(mtc_restrict_key);
	if (gh_kapi.drain_pages)
		gh_kapi.drain_pages();		/* flush stale CMA pcp entries */
	return 0;
}

static bool kapi_can_restrict_flip(void)
{
	return mtc_restrict_key &&
	       mtc_static_key_enable && mtc_static_key_disable;
}

/*
 * Boot-time: settle the vendor status and apply insmod-time lever desires.
 * Called from module_init AFTER capability is settled and BEFORE the gfp hook
 * is registered (the soft flag pre-arms it). Runtime sysfs writes apply
 * immediately instead. Both levers are inert when the vendor already redirects
 * movable → CMA, or when a lever's kernel support is missing.
 */
static void gh_unlock_cma_boot_apply(void)
{
	mtc_restrict_resolve();

	/* Vendor status (RO): does the kernel already redirect ALL movable to
	 * CMA (restrict_cma_redirect resolved AND currently false)? Just an
	 * observation of the key, independent of capability. When 1, both levers
	 * are redundant and their writes become accepted no-ops. */
	mtc_vender_already_allowed =
		(mtc_restrict_key && !static_key_enabled(mtc_restrict_key)) ? 1 : 0;

	if (!mtc_vender_already_allowed) {
		if (mtc_restrict_want >= 0 && kapi_can_restrict_flip())
			mtc_restrict_set_state(mtc_restrict_want == 0);	/* knob 0 => restrict on */
		if (mtc_gfp_hook_want)
			WRITE_ONCE(cma_bypass_enabled, true);
	}
}

/* ================================================================== */
/*  moveable_to_cma: three lever files                                */
/*                                                                    */
/*  Precedence for the two writable levers:                           */
/*    insmod-time (!gh_pool_ready): record; boot-apply applies once    */
/*                                  capability + vendor status settle.  */
/*    !cma_capable:                 -ENOSYS (whole feature off).        */
/*    vender_already_allowed:       accepted no-op (reads show real).   */
/*    lever unsupported:            -ENOSYS (restrict key / hook absent).*/
/* ================================================================== */
static int mtc_vender_already_allowed_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return sysfs_emit(buf, "%d\n", mtc_vender_already_allowed);
}
static const struct kernel_param_ops mtc_vender_ops = {
	.get = mtc_vender_already_allowed_get,
};
module_param_cb(moveable_to_cma_vender_already_allowed, &mtc_vender_ops, NULL, 0400);
MODULE_PARM_DESC(moveable_to_cma_vender_already_allowed,
	"1 = vendor kernel already redirects all movable allocations to CMA (restrict_cma_redirect resolved AND false); then both levers' writes are no-ops (read-only)");

/* The inverse of the kernel's restrict_cma_redirect static key, phrased as "is
 * the redirect enabled" so the value tracks the outcome, not the switch:
 *   1 = restrict DISABLED  = plain movable CAN migrate into CMA;
 *   0 = restrict active    = it cannot.
 * The vendor already allowing (key false) therefore reads back 1. Write 1 to
 * open movable->CMA globally. */
static int mtc_restrict_set(const char *val, const struct kernel_param *kp)
{
	int v;

	(void)kp;
	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	v = !!v;				/* 1 = redirect enabled (restrict off) */

	if (!gh_pool_ready()) {			/* insmod-time: record; boot-apply applies */
		mtc_restrict_want = v;
		return 0;
	}
	if (!gh_pool_cma_capable())
		return -ENOSYS;
	if (mtc_vender_already_allowed)		/* redundant: accepted no-op */
		return 0;
	if (!kapi_can_restrict_flip())
		return -ENOSYS;
	return mtc_restrict_set_state(v == 0);	/* restrict on iff redirect off; may sleep */
}

static int mtc_restrict_get(char *buf, const struct kernel_param *kp)
{
	int st = mtc_restrict_get_state();	/* -1 unavail, else key state (1=restrict on) */

	(void)kp;
	return sysfs_emit(buf, "%d\n", st < 0 ? -1 : !st);
}

static const struct kernel_param_ops mtc_restrict_ops = {
	.set = mtc_restrict_set,
	.get = mtc_restrict_get,
};
module_param_cb(moveable_to_cma_restrict_cma_redirect_disabled, &mtc_restrict_ops, NULL, 0600);
MODULE_PARM_DESC(moveable_to_cma_restrict_cma_redirect_disabled,
	"restrict_cma_redirect DISABLED (= is movable->CMA redirect on): write 1=disable restrict (open movable->CMA globally), 0=restrict on (block). Read: 1=can migrate in, 0=blocked, -1=unresolvable. Vendor-already-allowed reads 1. No-op if vender_already_allowed; -ENOSYS if CMA/key unavailable");

/* The __GFP_CMA bypass hook lever: arm/disarm the soft flag the registered
 * probe checks (see cma_bypass_wants). Lets page cache / mTHP anon consume the
 * reservoir without the global restrict flip. */
static int mtc_gfp_hook_set(const char *val, const struct kernel_param *kp)
{
	int v;

	(void)kp;
	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	v = !!v;

	if (!gh_pool_ready()) {			/* insmod-time: record; boot-apply applies */
		mtc_gfp_hook_want = v;
		return 0;
	}
	if (!gh_pool_cma_capable())
		return -ENOSYS;
	if (mtc_vender_already_allowed)		/* redundant: accepted no-op */
		return 0;
	if (!kapi_can_gfp_hook())
		return -ENOSYS;
	WRITE_ONCE(cma_bypass_enabled, v);
	return 0;
}

static int mtc_gfp_hook_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return sysfs_emit(buf, "%d\n", READ_ONCE(cma_bypass_enabled) ? 1 : 0);
}

static const struct kernel_param_ops mtc_gfp_hook_ops = {
	.set = mtc_gfp_hook_set,
	.get = mtc_gfp_hook_get,
};
module_param_cb(moveable_to_cma_gfp_cma_hook, &mtc_gfp_hook_ops, NULL, 0600);
MODULE_PARM_DESC(moveable_to_cma_gfp_cma_hook,
	"__GFP_CMA bypass hook: write 1=arm 0=disarm (lets page cache / mTHP anon consume the reservoir). Read=armed state. No-op if vender_already_allowed; -ENOSYS if CMA/hook unavailable");
