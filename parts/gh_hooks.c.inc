/* gh_hugepage_reserve — kernel hooks (§4). KERNEL-only (real linux headers
 * via the unity root). On-device verified only; the mock harness drives the
 * pool api directly instead of through these probes.
 *
 * Every hook does "one pool/facade call, no policy" (§0.1). Ported from the
 * old parts/gh_hooks.c.inc; the pool/owner/release api names are new.
 */

#define GH_IOCTL_TYPE	'G'
#define GH_CREATE_VM	_IO(GH_IOCTL_TYPE, 0x0)

/* symbol-name overrides (empty = builtin list). Vendor renames, and the Tier-1
 * QEMU harness points these at its companion stubs to exercise the probes on a
 * kernel without Gunyah. */
static char *gh_vm_create_sym, *gh_vm_destroy_sym, *gh_vm_reclaim_sym;
module_param_named(vm_create_sym, gh_vm_create_sym, charp, 0400);
module_param_named(vm_destroy_sym, gh_vm_destroy_sym, charp, 0400);
module_param_named(vm_reclaim_sym, gh_vm_reclaim_sym, charp, 0400);

static struct kretprobe gh_kretp;
static struct kprobe gh_vm_create_kp, gh_vm_destroy_kp, gh_vm_reclaim_kp;
static bool gh_hook_active, gh_detect_active, gh_destroy_active, gh_reclaim_active;
static struct tracepoint *gh_free_tp, *gh_exit_tp;
static bool gh_free_intercept_active;
static DEFINE_MUTEX(gh_hook_mutex);
static atomic_long_t gh_dbg_o9_seen, gh_dbg_del_hit, gh_dbg_del_miss, gh_dbg_skip_unmovable;
static atomic_long_t gh_dbg_total_served;

bool gh_free_hook_active(void) { return READ_ONCE(gh_free_intercept_active); }

/* ── free path: android_vh_free_one_page_bypass -> pool_released_return ── */
static void gh_free_one_page_cb(void *data, struct page *page, struct zone *zone,
				int order, int migratetype, int fpi_flags, bool *bypass)
{
	enum gh_return_result r;

	(void)data; (void)zone; (void)migratetype; (void)fpi_flags;
	if (order != GH_PAGE_ORDER || !page)
		return;
	atomic_long_inc(&gh_dbg_o9_seen);
	if (gh_pool_served() == 0 && gh_pool_released() == 0)
		return;
	r = gh_pool_released_return(page);	/* KEEP rebuilds the compound + lists,
						 * DROP demotes, MISS = not ours */
	if (r == GH_RETURN_MISS)
		atomic_long_inc(&gh_dbg_del_miss);
	else
		atomic_long_inc(&gh_dbg_del_hit);
	if (r == GH_RETURN_KEEP)
		*bypass = true;			/* remove before it enters buddy */
}

static void gh_find_free_tp(struct tracepoint *tp, void *priv)
{
	if (!gh_free_tp && !strcmp(tp->name, "android_vh_free_one_page_bypass"))
		gh_free_tp = tp;
}

/* prep_compound_page for the rebuild lives in the kapi backend; the free hook
 * needs it available. gh_rebuild_order9 checks that internally. */
static int gh_free_hook_register(void)
{
	int ret;

	if (gh_free_intercept_active)
		return 0;
	if (!gh_free_tp) {
		for_each_kernel_tracepoint(gh_find_free_tp, NULL);
		if (!gh_free_tp)
			return -ENOENT;
	}
	ret = tracepoint_probe_register(gh_free_tp, (void *)gh_free_one_page_cb, NULL);
	if (ret == 0)
		WRITE_ONCE(gh_free_intercept_active, true);
	return ret;
}
static void gh_free_hook_unregister(void)
{
	if (!gh_free_intercept_active || !gh_free_tp)
		return;
	tracepoint_probe_unregister(gh_free_tp, (void *)gh_free_one_page_cb, NULL);
	tracepoint_synchronize_unregister();
	WRITE_ONCE(gh_free_intercept_active, false);
}

/* ── serve: kretprobe on __alloc_pages, four-gate filter (§4) ── */
static int gh_serve_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	gfp_t gfp;

	if ((unsigned int)regs_get_kernel_argument(regs, 1) != GH_PAGE_ORDER)
		return 1;
	if (gh_pool_avail() == 0)
		return 1;
	if (!current->mm || !gh_pool_owner_maybe(current->mm))	/* lockless advisory */
		return 1;
	gfp = (gfp_t)regs_get_kernel_argument(regs, 0);
	if (!(gfp & __GFP_MOVABLE)) {		/* dma_heap unmovable never returns */
		atomic_long_inc(&gh_dbg_skip_unmovable);
		return 1;
	}
	return 0;
}
static int gh_serve_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct page *orig, *pool_page;

	pool_page = gh_pool_avail_to_served(current->mm, current->tgid);
	if (!pool_page)
		return 0;			/* authoritative check failed: original alloc */
	orig = (struct page *)regs_return_value(regs);
	regs_set_return_value(regs, (unsigned long)pool_page);
	atomic_long_inc(&gh_dbg_total_served);
	if (orig)
		__free_pages(orig, GH_PAGE_ORDER);	/* not in table, no bookkeeping */
	return 0;
}

/* ── VM create / destroy / unshare kprobes ─────────────── */
static int gh_vm_create_pre(struct kprobe *p, struct pt_regs *regs)
{
	unsigned int cmd = (unsigned int)regs_get_kernel_argument(regs, 1);

	if (cmd == GH_CREATE_VM && current->mm)
		gh_pool_owner_add(current->mm, current->tgid, current->comm);
	return 0;
}
static int gh_vm_destroy_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (!gh_pool_owner_any())
		return 0;	/* vendor Gunyah VM traffic: not ours (§4) */
	/* mm-less on the exit path (exit_mm before exit_files); the facade's
	 * attribution handles mm==NULL by tgid then sole-owner (§2). */
	gh_release_vm_shutdown(current->mm, current->tgid);
	return 0;
}
static int gh_vm_reclaim_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (!gh_pool_owner_any())
		return 0;	/* vendor Gunyah VM traffic: not ours (§4) */
	gh_release_vm_unshare();
	return 0;
}

/* ── pid death: sched_process_exit tracepoint (§4) ─────── */
static void gh_process_exit_cb(void *data, struct task_struct *task)
{
	(void)data;
	if (!task || task->group_leader != task)	/* group_dead: last thread */
		return;
	/* tgid, NOT mm: task->mm is already NULL here (exit_mm ran before this
	 * tracepoint), so an mm filter would always miss (§2). */
	if (gh_owner_tgid_maybe(task->tgid))
		gh_release_vm_exit(task->tgid);
}
static void gh_find_exit_tp(struct tracepoint *tp, void *priv)
{
	if (!gh_exit_tp && !strcmp(tp->name, "sched_process_exit"))
		gh_exit_tp = tp;
}
static int gh_exit_hook_register(void)
{
	if (!gh_exit_tp) {
		for_each_kernel_tracepoint(gh_find_exit_tp, NULL);
		if (!gh_exit_tp)
			return -ENOENT;
	}
	return tracepoint_probe_register(gh_exit_tp, (void *)gh_process_exit_cb, NULL);
}
static void gh_exit_hook_unregister(void)
{
	if (!gh_exit_tp)
		return;
	tracepoint_probe_unregister(gh_exit_tp, (void *)gh_process_exit_cb, NULL);
	tracepoint_synchronize_unregister();
}

/* ── kretprobe/kprobe registration with symbol fallback (ported) ── */
static int gh_register_kretp(void)
{
	static const char * const syms[] = {
		"__alloc_pages_noprof", "__alloc_pages", "__alloc_pages_nodemask", NULL,
	};
	int i, ret = -ENOENT;

	gh_kretp.handler = gh_serve_ret;
	gh_kretp.entry_handler = gh_serve_entry;
	gh_kretp.maxactive = 20;
	for (i = 0; syms[i]; i++) {
		gh_kretp.kp.symbol_name = syms[i];
		ret = register_kretprobe(&gh_kretp);
		if (ret == 0)
			return 0;
	}
	return ret;
}
static int gh_register_kprobe_any(struct kprobe *kp, int (*h)(struct kprobe *, struct pt_regs *),
				  const char *override, const char * const *syms)
{
	int i;

	kp->pre_handler = h;
	if (override && *override) {
		kp->symbol_name = override;
		if (register_kprobe(kp) == 0)
			return 0;
	}
	for (i = 0; syms[i]; i++) {
		kp->symbol_name = syms[i];
		if (register_kprobe(kp) == 0)
			return 0;
	}
	return -ENOENT;
}

/* called by the unity root init (§8) after the pool is built */
static void gh_hooks_attach(void)
{
	static const char * const create_syms[] = { "gh_dev_vm_mgr_ioctl", "gunyah_dev_vm_mgr_ioctl", NULL };
	static const char * const destroy_syms[] = { "gh_vm_free", "gunyah_vm_release", NULL };
	static const char * const reclaim_syms[] = { "gunyah_rm_mem_reclaim", "gh_rm_mem_reclaim",
						     "gunyah_vm_reclaim_range", "gunyah_gup_reclaim_parcel", NULL };

	mutex_lock(&gh_hook_mutex);
	gh_hook_active = (gh_register_kretp() == 0);
	gh_detect_active = (gh_register_kprobe_any(&gh_vm_create_kp, gh_vm_create_pre, gh_vm_create_sym, create_syms) == 0);
	gh_destroy_active = (gh_register_kprobe_any(&gh_vm_destroy_kp, gh_vm_destroy_pre, gh_vm_destroy_sym, destroy_syms) == 0);
	gh_reclaim_active = (gh_register_kprobe_any(&gh_vm_reclaim_kp, gh_vm_reclaim_pre, gh_vm_reclaim_sym, reclaim_syms) == 0);
	if (gh_exit_hook_register() != 0)
		pr_warn("sched_process_exit unavailable; give-up falls back to mm_users sweep (§11)\n");
	gh_free_hook_register();
	mutex_unlock(&gh_hook_mutex);
	pr_info("hooks: serve=%d create=%d destroy=%d reclaim=%d exit=%d free=%d\n",
		gh_hook_active, gh_detect_active, gh_destroy_active,
		gh_reclaim_active, gh_exit_tp != NULL, gh_free_intercept_active);
}
static void gh_hooks_detach(void)	/* rmmod: hooks FIRST (§8) */
{
	mutex_lock(&gh_hook_mutex);
	if (gh_hook_active) { unregister_kretprobe(&gh_kretp); gh_hook_active = false; }
	if (gh_detect_active) { unregister_kprobe(&gh_vm_create_kp); gh_detect_active = false; }
	if (gh_destroy_active) { unregister_kprobe(&gh_vm_destroy_kp); gh_destroy_active = false; }
	if (gh_reclaim_active) { unregister_kprobe(&gh_vm_reclaim_kp); gh_reclaim_active = false; }
	gh_exit_hook_unregister();
	gh_free_hook_unregister();
	mutex_unlock(&gh_hook_mutex);
}

/* ── hook_enable / reclaim_enable plumbing (§10) ───────── */
static int gh_reclaim_want = 1;			/* desired free-hook state */
bool gh_hooks_serve_active(void) { return gh_hook_active; }
bool gh_hooks_reclaim_active(void) { return gh_free_hook_active(); }

int gh_hooks_serve_set(int enable)		/* returns 0 / -errno */
{
	int ret = 0;

	mutex_lock(&gh_hook_mutex);
	if (enable && !gh_hook_active) {
		/* empty pool arms fine (same as insmod's unconditional attach):
		 * the entry filter idles on avail==0; paying the probe tax on
		 * an empty pool is the user's call, not a gate (§10) */
		gh_hook_active = (gh_register_kretp() == 0) ? true : (ret = -EIO, false);
	} else if (!enable && gh_hook_active) {
		unregister_kretprobe(&gh_kretp);
		gh_hook_active = false;
	}
	mutex_unlock(&gh_hook_mutex);
	return ret;
}
int gh_hooks_reclaim_set(int enable)
{
	int ret = 0;

	mutex_lock(&gh_hook_mutex);
	WRITE_ONCE(gh_reclaim_want, enable);
	if (enable)
		ret = gh_free_hook_register();
	else
		gh_free_hook_unregister();
	mutex_unlock(&gh_hook_mutex);
	return ret;
}
