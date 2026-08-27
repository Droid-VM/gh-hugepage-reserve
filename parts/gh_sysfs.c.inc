/* gh_hugepage_reserve — sysfs / module params (§10). KERNEL-only.
 * Readers take ONE gh_pool_stats_snapshot / gh_pool_owner_stats_snapshot so a
 * single output never mixes generations. Writers go through gh_pool_set_target
 * (never READ/WRITE the target fields directly). On-device verified only.
 */

/* ── the target knobs ──────────────────────────────────── */
/* insmod-time snapshots: params are applied BEFORE gh_init builds the table
 * (so pool_size_max is still 0 and set_target would clamp to 0). gh_init reads
 * these after the table is up and applies them via set_target (§8). */
static int gh_insmod_want = -1, gh_insmod_want_cma = -1;

static int gh_want_set(const char *val, const struct kernel_param *kp)
{
	int v, rc;
	enum gh_target_change ch;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	if (!gh_pool_ready()) {			/* insmod: stash, init applies (§8) */
		if ((enum gh_target)(long)kp->arg == GH_TARGET_WANT)
			gh_insmod_want = v;
		else
			gh_insmod_want_cma = v;
		return 0;
	}
	if (gh_adjust_running())
		return -EBUSY;	/* DELIBERATE (§4): ANY run chases fixed targets
				 * for its whole life; changing them is the
				 * explicit acquire=0 -> write -> acquire
				 * sequence, never a mid-run swap.
				 * acquire_active IS the writability signal. */
	rc = gh_pool_set_target((enum gh_target)(long)kp->arg, v, &ch);
	if (rc)
		return rc;
	if (ch == GH_TGT_SHRINK)
		gh_adjust_try(GH_PROFILE_SHRINK);
	/* GROW only records; the user presses acquire (§4) */
	return 0;
}
static int gh_want_get(char *buf, const struct kernel_param *kp)
{
	struct gh_pool_stats st;
	bool is_want = (enum gh_target)(long)kp->arg == GH_TARGET_WANT;

	if (!gh_pool_ready())
		return sysfs_emit(buf, "%d\n", is_want ? gh_insmod_want : gh_insmod_want_cma);
	gh_pool_stats_snapshot(&st);
	return sysfs_emit(buf, "%d\n", is_want ? st.want : st.want_cma);
}
static const struct kernel_param_ops gh_want_ops = { .set = gh_want_set, .get = gh_want_get };
module_param_cb(pool_want, &gh_want_ops, (void *)GH_TARGET_WANT, 0600);
module_param_cb(pool_want_with_cma, &gh_want_ops, (void *)GH_TARGET_WANT_CMA, 0600);

/* ── acquire ───────────────────────────────────────────── */
static int gh_acquire_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v < 0 || v > 3)
		return -EINVAL;
	if (!gh_pool_ready())
		return -ENODEV;
	if (v == 0) { gh_adjust_cancel(); return 0; }
	/* -ENOSYS per mode: required kapi caps (§10). id1 needs contig_pages;
	 * id2/3 need contig_range; id3 additionally evict-isolate. */
	if (v == 1 && !gh_kapi.cap(GH_CAP_CONTIG_PAGES))
		return -ENOSYS;
	if ((v == 2 || v == 3) && !gh_kapi.cap(GH_CAP_CONTIG_RANGE))
		return -ENOSYS;
	if (v == 3 && !gh_kapi.cap(GH_CAP_EVICT_ISOLATE))
		return -ENOSYS;
	return gh_adjust_user_acquire(v);	/* 0 / -EBUSY */
}
static const struct kernel_param_ops gh_acquire_ops = { .set = gh_acquire_set };
module_param_cb(acquire, &gh_acquire_ops, NULL, 0200);

/* ── refill_stat: the 17-key contract (§10) ────────────── */
static int gh_refill_stat_get(char *buf, const struct kernel_param *kp)
{
	struct gh_pool_stats st;
	int n = 0, active_vms = 0, i;
	struct gh_owner_stats os;

	gh_pool_stats_snapshot(&st);
	gh_pool_owner_stats_snapshot(&os);
	for (i = 0; i < os.n; i++)
		active_vms += os.row[i].vm_count;
	n += sysfs_emit_at(buf, n, "state=%s\n", gh_release_active() ? "running" : "idle");
	n += sysfs_emit_at(buf, n, "pool_avail=%d\n", st.avail);
	n += sysfs_emit_at(buf, n, "pool_total=%d\n", st.total);
	n += sysfs_emit_at(buf, n, "served=%d\n", st.served + st.released);
	n += sysfs_emit_at(buf, n, "pool_want=%d\n", st.want);
	n += sysfs_emit_at(buf, n, "total_served=%ld\n", atomic_long_read(&gh_dbg_total_served));
	n += sysfs_emit_at(buf, n, "total_refilled=%ld\n",
			   st.origin[GH_IN_HOOK] + st.origin[GH_IN_SWEEP] +
			   st.origin[GH_IN_CMA] + st.origin[GH_IN_USER] + st.origin[GH_IN_REFILL]);
	n += sysfs_emit_at(buf, n, "active_vms=%d\n", active_vms);
	n += sysfs_emit_at(buf, n, "acquire_active=%d\n", gh_adjust_running() ? 1 : 0);
	n += sysfs_emit_at(buf, n, "acquire_mode=%d\n", gh_user_approach == GH_APPROACH_CONTIG_ANY ? 1 :
			   (gh_user_evict == GH_EVICT_MEMCG ? 2 : 3));
	n += sysfs_emit_at(buf, n, "acquire_stop_reason=%s\n", gh_adjust_stop_reason_str());
	n += sysfs_emit_at(buf, n, "refill_enable=%d\n", gh_param_refill_enable);
	n += sysfs_emit_at(buf, n, "free_reclaim=%d\n", gh_free_hook_active() ? 1 : 0);
	n += sysfs_emit_at(buf, n, "pool_want_with_cma=%d\n", st.want_cma);
	n += sysfs_emit_at(buf, n, "pool_cma=%d\n", st.cma);
	n += sysfs_emit_at(buf, n, "pool_avail_cma_able=%d\n", gh_pool_avail_cma_able());
	n += sysfs_emit_at(buf, n, "cma_pb_order=%d\n",
			   st.cma_params == GH_CMA_VERIFIED ? gh_cma_order : -1);
	return n;
}
static const struct kernel_param_ops gh_refill_stat_ops = { .get = gh_refill_stat_get };
module_param_cb(refill_stat, &gh_refill_stat_ops, NULL, 0400);

/* ── plain query files ─────────────────────────────────── */
#define GH_RO_INT(name, expr) \
	static int gh_##name##_get(char *b, const struct kernel_param *kp) \
	{ struct gh_pool_stats st; gh_pool_stats_snapshot(&st); (void)st; return sysfs_emit(b, "%d\n", (expr)); } \
	static const struct kernel_param_ops gh_##name##_ops = { .get = gh_##name##_get }; \
	module_param_cb(name, &gh_##name##_ops, NULL, 0400)
GH_RO_INT(pool_avail, st.avail);
GH_RO_INT(pool_cma, st.cma);
GH_RO_INT(pool_avail_cma_able, gh_pool_avail_cma_able());
GH_RO_INT(pool_size_max, st.size_max);

static int gh_reclaim_debug_get(char *b, const struct kernel_param *kp)
{
	struct gh_pool_stats st;

	gh_pool_stats_snapshot(&st);
	return sysfs_emit(b,
		"o9_seen=%ld\ndel_hit=%ld\ndel_miss=%ld\ngate_drop=%ld\nskip_unmovable=%ld\n"
		"reject=%ld\norphan=%ld\npurged=%ld\n"
		"in_hook=%ld\nin_sweep=%ld\nin_cma=%ld\nin_user=%ld\nin_refill=%ld\n",
		atomic_long_read(&gh_dbg_o9_seen), atomic_long_read(&gh_dbg_del_hit),
		atomic_long_read(&gh_dbg_del_miss), st.gate_drop,
		atomic_long_read(&gh_dbg_skip_unmovable),
		st.reject, st.orphan, st.purged,
		st.origin[GH_IN_HOOK], st.origin[GH_IN_SWEEP], st.origin[GH_IN_CMA],
		st.origin[GH_IN_USER], st.origin[GH_IN_REFILL]);
}
static const struct kernel_param_ops gh_reclaim_debug_ops = { .get = gh_reclaim_debug_get };
module_param_cb(reclaim_debug, &gh_reclaim_debug_ops, NULL, 0400);

static int gh_vm_owners_get(char *b, const struct kernel_param *kp)
{
	struct gh_owner_stats os;
	int i, n = 0;

	gh_pool_owner_stats_snapshot(&os);
	for (i = 0; i < os.n; i++)
		n += sysfs_emit_at(b, n, "pid=%d vm_count=%d served=%ld abandoned=%ld comm=%s\n",
				   os.row[i].tgid, os.row[i].vm_count, os.row[i].served,
				   os.row[i].abandoned, os.row[i].comm);
	return n;
}
static const struct kernel_param_ops gh_vm_owners_ops = { .get = gh_vm_owners_get };
module_param_cb(vm_owners, &gh_vm_owners_ops, NULL, 0400);

/* ── reconcile / manual_release / manual_refill ────────── */
/* reconcile: legacy-compat NO-OP (§10). The pre-v12 module computed its
 * per-owner stats and its ghost-entry cleanup inside a synchronous reconcile
 * pass, so old apps write this knob (some once per second) before every stats
 * read. v12 maintains the stats live and gives up on timestamps, so the effect
 * an old app wants from this write - fresh numbers on its next read - happens
 * by itself: accepting and doing nothing IS the faithful implementation, and
 * it keeps a polling writer from flapping state=running (an armed round lives
 * ~1s, §6). "Run one release round now" moved to manual_release below. */
static int gh_reconcile_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v != 1)
		return -EINVAL;
	return 0;				/* accepted; nothing to do (§10) */
}
static const struct kernel_param_ops gh_reconcile_ops = { .set = gh_reconcile_set };
module_param_cb(reconcile, &gh_reconcile_ops, NULL, 0200);

/* manual_release: run one release round now - the manual/diagnostic twin of
 * manual_refill (kick release / kick adjust). Timestamp-driven give-up makes
 * any frequency harmless (§6); its one real use is collecting released
 * stragglers while idle. Not needed before any stats read. */
static int gh_manual_release_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v != 1)
		return -EINVAL;
	gh_release_run(1);
	return 0;
}
static const struct kernel_param_ops gh_manual_release_ops = { .set = gh_manual_release_set };
module_param_cb(manual_release, &gh_manual_release_ops, NULL, 0200);

static int gh_manual_refill_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v != 1)
		return -EINVAL;
	if (!gh_param_refill_enable)
		return -ENODEV;
	if (gh_adjust_running())
		return -EBUSY;			/* ABI -EBUSY: a run is already in flight —
						 * adjust_try would be silent (vs USER) or a
						 * pointless restart (vs background) (§10) */
	gh_adjust_try(GH_PROFILE_RELEASE);
	return 0;
}
static const struct kernel_param_ops gh_manual_refill_ops = { .set = gh_manual_refill_set };
module_param_cb(manual_refill, &gh_manual_refill_ops, NULL, 0200);

/* ── plain module params (§10) ─────────────────────────── */
module_param_named(refill_enable, gh_param_refill_enable, int, 0600);
module_param_named(acquire_drop_slab, gh_param_drop_slab, int, 0600);
module_param_named(acquire_mem_floor_mb, gh_param_mem_floor_mb, int, 0600);
module_param_named(debug, gh_param_debug, int, 0644);
/* refill_delay_ms: accepted and ignored — the loader feeds it; rejecting
 * would break the fallback ladder (§10). */
static int gh_refill_delay_ms;
module_param_named(refill_delay_ms, gh_refill_delay_ms, int, 0600);

/* ── purge_log: what became of pages we gave up on (§10; needs debug=1) ── */
static int gh_purge_log_get(char *b, const struct kernel_param *kp)
{
	struct gh_purge_log log;
	int i, n = 0;

	if (!gh_param_debug)
		return sysfs_emit(b, "off (echo 1 > parameters/debug)\n");
	gh_pool_purge_log_snapshot(&log);
	n += sysfs_emit_at(b, n, "purged_total=%u shown=%d\n", log.total, log.n);
	for (i = 0; i < log.n && n < PAGE_SIZE - 96; i++) {
		struct page *pg = pfn_valid(log.rec[i].pfn) ?
				  pfn_to_page(log.rec[i].pfn) : NULL;
		int now = pg ? page_count(pg) : -1;	/* page op, not slot access */

		n += sysfs_emit_at(b, n, "pfn=%lx at_purge=%d now=%d %s\n",
				   log.rec[i].pfn, log.rec[i].count_at_purge, now,
				   now == 0 ? "FREED" : "still held");
	}
	return n;
}
static const struct kernel_param_ops gh_purge_log_ops = { .get = gh_purge_log_get };
module_param_cb(purge_log, &gh_purge_log_ops, NULL, 0400);

/* ── served_summary (§10) ──────────────────────────────── */
static int gh_served_summary_get(char *b, const struct kernel_param *kp)
{
	struct gh_pool_stats st;
	struct gh_owner_stats os;
	int i, n = 0;

	gh_pool_stats_snapshot(&st);
	gh_pool_owner_stats_snapshot(&os);
	n += sysfs_emit_at(b, n, "tracked=%d\n", st.served + st.released);
	n += sysfs_emit_at(b, n, "served=%d\n", st.served);
	n += sysfs_emit_at(b, n, "released=%d\n", st.released);
	n += sysfs_emit_at(b, n, "live=%ld\n", os.live_served);	/* held by live VMs */
	n += sysfs_emit_at(b, n, "orphan=%ld\n", st.orphan);
	n += sysfs_emit_at(b, n, "purged=%ld\n", st.purged);
	for (i = 0; i < os.n; i++)
		n += sysfs_emit_at(b, n, "owner pid=%d pages=%ld abandoned=%ld comm=%s\n",
				   os.row[i].tgid, os.row[i].served,
				   os.row[i].abandoned, os.row[i].comm);
	/* per-owner field is named pages= — the pre-v12 ABI name the app's parser
	 * is pinned to (§10). The VALUE is the live owner.served count; only the
	 * label is legacy. */
	return n;
}
static const struct kernel_param_ops gh_served_summary_ops = { .get = gh_served_summary_get };
module_param_cb(served_summary, &gh_served_summary_ops, NULL, 0400);

/* ── cma_usage: reservoir occupancy, ~1s cached (§10/§11) ── */
static unsigned long gh_cu_v[5];		/* pages: total/free/used/anon/file */
static int gh_cu_b[3];				/* blocks: free/partial/full */
static unsigned long gh_cu_jiffies;
static DEFINE_MUTEX(gh_cu_mutex);
static unsigned long gh_cu_bases[POOL_CMA_BASES_MAX];

static int gh_cma_usage_get(char *b, const struct kernel_param *kp)
{
	int nb, i, n = 0, span = gh_pool_cma_block_span();
	unsigned long free_p = 0, anon_p = 0, file_p = 0, total_p;
	int bfree = 0, bpart = 0, bfull = 0;

	mutex_lock(&gh_cu_mutex);
	if (gh_cu_jiffies && time_before(jiffies, gh_cu_jiffies + HZ))
		goto emit;			/* ~1s cache */
	nb = gh_pool_cma_block_bases(gh_cu_bases, POOL_CMA_BASES_MAX);
	for (i = 0; i < nb; i++) {
		unsigned long f, a, fi;

		gh_kapi.cma_occupancy(gh_cu_bases[i], span, &f, &a, &fi);
		free_p += f; anon_p += a; file_p += fi;
		if (f >= (unsigned long)span) bfree++;
		else if (f == 0) bfull++;
		else bpart++;
	}
	total_p = (unsigned long)nb * span;
	gh_cu_v[0] = total_p; gh_cu_v[1] = free_p;
	gh_cu_v[2] = total_p - min(free_p, total_p);
	gh_cu_v[3] = anon_p; gh_cu_v[4] = file_p;
	gh_cu_b[0] = bfree; gh_cu_b[1] = bpart; gh_cu_b[2] = bfull;
	gh_cu_jiffies = jiffies ? jiffies : 1;
emit:
	n = sysfs_emit(b,
		"reservoir_mb=%lu\nfree_mb=%lu\nused_mb=%lu\nused_anon_mb=%lu\n"
		"used_file_mb=%lu\nblocks_free=%d\nblocks_partial=%d\nblocks_full=%d\n",
		gh_cu_v[0] >> (20 - PAGE_SHIFT), gh_cu_v[1] >> (20 - PAGE_SHIFT),
		gh_cu_v[2] >> (20 - PAGE_SHIFT), gh_cu_v[3] >> (20 - PAGE_SHIFT),
		gh_cu_v[4] >> (20 - PAGE_SHIFT), gh_cu_b[0], gh_cu_b[1], gh_cu_b[2]);
	mutex_unlock(&gh_cu_mutex);
	return n;
}
static const struct kernel_param_ops gh_cma_usage_ops = { .get = gh_cma_usage_get };
module_param_cb(cma_usage, &gh_cma_usage_ops, NULL, 0400);

/* ── hook_enable / reclaim_enable (§10; read ACTUAL state) ── */
static int gh_hook_enable_set(const char *v, const struct kernel_param *kp)
{ int x; if (kstrtoint(v, 10, &x)) return -EINVAL; return gh_hooks_serve_set(!!x); }
static int gh_hook_enable_get(char *b, const struct kernel_param *kp)
{ return sysfs_emit(b, "%d\n", gh_hooks_serve_active() ? 1 : 0); }
static const struct kernel_param_ops gh_hook_enable_ops =
	{ .set = gh_hook_enable_set, .get = gh_hook_enable_get };
module_param_cb(hook_enable, &gh_hook_enable_ops, NULL, 0600);

static int gh_reclaim_enable_set(const char *v, const struct kernel_param *kp)
{ int x; if (kstrtoint(v, 10, &x)) return -EINVAL; return gh_hooks_reclaim_set(!!x); }
static int gh_reclaim_enable_get(char *b, const struct kernel_param *kp)
{ return sysfs_emit(b, "%d\n", gh_hooks_reclaim_active() ? 1 : 0); }
static const struct kernel_param_ops gh_reclaim_enable_ops =
	{ .set = gh_reclaim_enable_set, .get = gh_reclaim_enable_get };
module_param_cb(reclaim_enable, &gh_reclaim_enable_ops, NULL, 0600);

/* sim_cma_order / migrate_cma_val / pageblock_order_val / disable_kapi are
 * owned by the kapi backend + unity root. */
