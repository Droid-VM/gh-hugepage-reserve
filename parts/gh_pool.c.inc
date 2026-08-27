/* gh_hugepage_reserve — the pool object (POOL_DESIGN.md §2/§3).
 *
 * C has no objects; this file IS the object: every piece of pool state and
 * every lock lives here, consumers (hooks / workers / sysfs) only call the
 * gh_pool_* public api and get values back. No caller ever holds pool_lock
 * or reads top[]/slots.
 *
 * Complete: addressing, avail tri-list + block classification, serve,
 * released return (KEEP/DROP), served->released, EXPIRED give-up
 * (owner death), released purge, shed, all five ext_to_avail intake modes,
 * cand/grab/promote-conservation, scan sets + ext_cursor, CMA edges
 * (flip/stage_in/drop + occupancy bucket scan), verify_cma_params, window
 * actions, carveout marking, targets, snapshots, pool_check.
 */

/* ── state ─────────────────────────────────────────────── */
static gh_spinlock_t gh_pool_lock;

static struct gh_slot *gh_top[GH_SPAN_GB_MAX];
static unsigned long gh_span_base_pfn;		/* aligned down to 1GB */
static u32 gh_span_slots;			/* 2MB slots covered (incl. holes) */

static int gh_cma_order = GH_PAGE_ORDER;	/* runtime; sim_cma_order may raise */
#define GH_S		(1u << (gh_cma_order - GH_PAGE_ORDER))

static struct gh_slist gh_avail[GH_L_MAX];
static long gh_nstate[GH_STATE_MAX];		/* pages (2MB units) per state */
static u32 gh_released_oldest;			/* seconds; valid when RELEASED>0 */

/* targets & capability (pool state, api-guarded; §2) */
static int gh_pool_want, gh_pool_want_with_cma, gh_pool_size_max;
static int gh_pool_total;			/* proven-capacity ceiling (§5.1) */
static enum gh_cma_params_state gh_cma_params = GH_CMA_UNAVAILABLE;
/* CMA verify: a small CmaFree delta can be a transient (pcp/vmstat fold lag),
 * not proof CMA is unobservable. Retry the probe up to this many times before
 * settling the boot-constant UNAVAILABLE; structural failures skip the count. */
static int gh_cma_verify_misses;
#define GH_CMA_VERIFY_MISS_MAX 8

/* debug/forensic counters (§10 reclaim_debug) */
static long gh_dbg_reject, gh_dbg_orphan, gh_dbg_gate_drop, gh_dbg_purged;
static long gh_dbg_origin[GH_ORIGIN_MAX];

static struct gh_kapi gh_kapi;			/* backend table (kernel or mock) */
static int gh_param_debug;			/* pool_check per round (§10 debug) */
static struct gh_purge_rec gh_purge_log[GH_PURGE_LOG_MAX];
static unsigned int gh_purge_log_n;		/* total ever; index = n % MAX */

/* record a given-up page before its ref is dropped (§10 purge_log). The
 * refcount NOW is what other holders still have — recorded only when debug=1
 * so the "pool short vs phone lost memory" question is answerable. */
static void gh_purge_log_add(unsigned long pfn, pid_t tgid, int count_now)
{
	if (!gh_param_debug)
		return;
	gh_purge_log[gh_purge_log_n % GH_PURGE_LOG_MAX] =
		(struct gh_purge_rec){ .pfn = pfn, .count_at_purge = count_now,
				       .tgid = tgid };
	gh_purge_log_n++;
}
#ifndef GH_MOCK				/* kernel: prefill-done / worker-quiescent flag (§8/§9) */
static bool gh_pool_ready_flag;
bool gh_pool_ready(void) { return GH_READ_ONCE(gh_pool_ready_flag); }
void gh_pool_set_ready(bool v) { gh_pool_ready_flag = v; }
#endif

/* ── addressing (two-level, hole-immune; §0.2) ─────────── */
static inline u32 gh_idx_of_pfn(unsigned long pfn)
{
	unsigned long off;

	if (pfn < gh_span_base_pfn)
		return GH_NIL;
	off = (pfn - gh_span_base_pfn) >> GH_PAGE_ORDER;
	if (off >= gh_span_slots || (pfn & (GH_PAGES_PER_SLOT - 1)))
		return GH_NIL;
	return (u32)off;
}

static inline unsigned long gh_pfn_of_idx(u32 idx)
{
	return gh_span_base_pfn + ((unsigned long)idx << GH_PAGE_ORDER);
}

static inline struct gh_slot *gh_slot_at(u32 idx)
{
	struct gh_slot *chunk;

	if (idx == GH_NIL || idx >= gh_span_slots)
		return NULL;
	chunk = gh_top[idx / GH_SLOTS_PER_GB];
	return chunk ? &chunk[idx % GH_SLOTS_PER_GB] : NULL;
}

static inline struct gh_slot *gh_slot_of_pfn(unsigned long pfn)
{
	return gh_slot_at(gh_idx_of_pfn(pfn));
}

/* ── state counter discipline: every transition goes through here ── */
static inline void gh_set_state(struct gh_slot *s, u8 st)
{
	GH_WRITE_ONCE(gh_nstate[s->state], gh_nstate[s->state] - 1);
	GH_WRITE_ONCE(gh_nstate[st], gh_nstate[st] + 1);
	GH_WRITE_ONCE(s->state, st);		/* advisory readers pre-scan this */
}

/* ── avail tri-list primitives ─────────────────────────── */
static void gh_slist_push(struct gh_slist *l, u32 idx)
{
	struct gh_slot *s = gh_slot_at(idx);

	s->prev = l->tail;
	s->next = GH_NIL;
	if (l->tail != GH_NIL)
		gh_slot_at(l->tail)->next = idx;
	else
		l->head = idx;
	l->tail = idx;
	GH_WRITE_ONCE(l->n, l->n + 1);
}

static void gh_slist_unlink(struct gh_slist *l, u32 idx)
{
	struct gh_slot *s = gh_slot_at(idx);

	if (s->prev != GH_NIL)
		gh_slot_at(s->prev)->next = s->next;
	else
		l->head = s->next;
	if (s->next != GH_NIL)
		gh_slot_at(s->next)->prev = s->prev;
	else
		l->tail = s->prev;
	s->prev = s->next = GH_NIL;
	GH_WRITE_ONCE(l->n, l->n - 1);
}

static u32 gh_slist_pop(struct gh_slist *l)
{
	u32 idx = l->head;

	if (idx != GH_NIL)
		gh_slist_unlink(l, idx);
	return idx;
}

/* ── block classification (derived, never stored; §1.1) ── */
static inline u32 gh_blk_base(u32 idx) { return idx & ~(GH_S - 1); }

static enum gh_class gh_classify(u32 bidx)
{
	u32 i;
	bool all_avail = true;

	for (i = 0; i < GH_S; i++) {
		u8 st = gh_slot_at(bidx + i)->state;

		if (st != GH_AVAIL)
			all_avail = false;
		if (st != GH_AVAIL && st != GH_SERVED && st != GH_RELEASED &&
		    st != GH_CAND && st != GH_VERIFY)
			return GH_L_NON;	/* custody broken: straggler block */
	}
	return all_avail ? GH_L_READY : GH_L_ABLE;
}

/* Re-home every AVAIL cell of the block from list @from to @to (no-op when
 * equal), skipping @exclude (a cell being inserted/removed by the caller —
 * it is not on any list, unlinking it would corrupt head/tail). All avail
 * cells of one block always sit on the SAME list, so the caller derives
 * @from by classifying BEFORE its own state change. */
static void gh_rehome(u32 bidx, enum gh_class from, enum gh_class to, u32 exclude)
{
	u32 i;

	if (from == to)
		return;
	for (i = 0; i < GH_S; i++) {
		u32 idx = bidx + i;

		if (idx != exclude && gh_slot_at(idx)->state == GH_AVAIL) {
			gh_slist_unlink(&gh_avail[from], idx);
			gh_slist_push(&gh_avail[to], idx);
		}
	}
}

/* ── private: avail entry funnel (§3.2 pool_private_avail_in) ── */
static bool gh_avail_in(unsigned long pfn, u8 expect, u8 origin)
{
	struct gh_slot *s = gh_slot_of_pfn(pfn);
	u32 idx = gh_idx_of_pfn(pfn), bidx;
	enum gh_class before, after;

	if (!s || s->state != expect) {
		gh_dbg_reject++;	/* caller frees, MUST NOT change state */
		return false;
	}
	bidx = gh_blk_base(idx);
	before = gh_classify(bidx);
	gh_set_state(s, GH_AVAIL);
	s->origin = origin;
	after = gh_classify(bidx);
	gh_rehome(bidx, before, after, idx);	/* siblings follow; self not listed yet */
	gh_slist_push(&gh_avail[after], idx);	/* self joins the new class */
	if (origin < GH_ORIGIN_MAX)
		gh_dbg_origin[origin]++;
	return true;
}

/* ── targets (§4 write rules live HERE, one lock) ──────── */
static int gh_align_up_s(int v)
{
	int s = (int)GH_S;

	if (gh_cma_params == GH_CMA_UNAVAILABLE || s <= 1 || v <= 0)
		return v;
	v = (v + s - 1) / s * s;
	if (v > gh_pool_size_max)
		v = gh_pool_size_max / s * s;
	return v;
}

static int gh_effective_total_locked(void)
{
	if (gh_cma_params == GH_CMA_UNAVAILABLE || gh_pool_want_with_cma == 0)
		return gh_pool_want;
	return gh_pool_want_with_cma > gh_pool_want ?
	       gh_pool_want_with_cma : gh_pool_want;
}

int gh_pool_set_target(enum gh_target which, int value, enum gh_target_change *change)
{
	unsigned long fl;
	int old_eff, new_eff, old_want;

	gh_spin_lock(&gh_pool_lock, &fl);
	if (which == GH_TARGET_WANT_CMA && value != 0 &&
	    gh_cma_params == GH_CMA_UNAVAILABLE) {
		gh_spin_unlock(&gh_pool_lock, &fl);
		return -GH_ENOSYS;
	}
	old_eff = gh_effective_total_locked();
	old_want = gh_pool_want;

	if (value < 0)
		value = 0;
	if (value > gh_pool_size_max)
		value = gh_pool_size_max;

	if (which == GH_TARGET_WANT) {
		gh_pool_want = gh_align_up_s(value);
		if (gh_pool_want_with_cma != 0 && gh_pool_want > gh_pool_want_with_cma)
			gh_pool_want_with_cma = gh_pool_want;	/* pull up (W) */
	} else {
		if (value != 0 && value < gh_pool_want)
			value = gh_pool_want;			/* lift, never shrink want */
		gh_pool_want_with_cma = gh_align_up_s(value);
	}

	new_eff = gh_effective_total_locked();
	if (gh_pool_total > new_eff)
		gh_pool_total = new_eff;			/* shrink clamps ceiling */
	if (change)
		*change = (new_eff < old_eff || gh_pool_want < old_want) ? GH_TGT_SHRINK :
			  (new_eff > old_eff || gh_pool_want > old_want) ? GH_TGT_GROW : GH_TGT_SAME;
	gh_spin_unlock(&gh_pool_lock, &fl);
	return 0;
}

struct gh_targets gh_pool_targets_snapshot(int is_release_profile)
{
	struct gh_targets t;
	unsigned long fl;

	gh_spin_lock(&gh_pool_lock, &fl);
	t.want = gh_pool_want;
	t.want_cma = gh_effective_total_locked();
	if (is_release_profile) {			/* §5.1: cap by proven */
		if (t.want_cma > gh_pool_total)
			t.want_cma = gh_pool_total;
		if (t.want > t.want_cma)
			t.want = t.want_cma;
	}
	gh_spin_unlock(&gh_pool_lock, &fl);
	return t;
}

/* ── queries (O(1) counters) ───────────────────────────── */
int gh_pool_avail(void)     { return GH_READ_ONCE(gh_avail[GH_L_NON].n) + GH_READ_ONCE(gh_avail[GH_L_ABLE].n) + GH_READ_ONCE(gh_avail[GH_L_READY].n); }
int gh_pool_served(void)    { return (int)GH_READ_ONCE(gh_nstate[GH_SERVED]); }
int gh_pool_released(void)  { return (int)GH_READ_ONCE(gh_nstate[GH_RELEASED]); }
int gh_pool_cma(void)       { return (int)GH_READ_ONCE(gh_nstate[GH_CMA]); }
int gh_pool_held(void)      { return gh_pool_avail() + (int)GH_READ_ONCE(gh_nstate[GH_SERVED]) +
				     (int)GH_READ_ONCE(gh_nstate[GH_RELEASED]) + (int)GH_READ_ONCE(gh_nstate[GH_VERIFY]); }
int gh_pool_noncma_able(void)    { return GH_READ_ONCE(gh_avail[GH_L_NON].n); }
int gh_pool_avail_cma_able(void) { return gh_cma_params == GH_CMA_VERIFIED ? GH_READ_ONCE(gh_avail[GH_L_READY].n) : 0; }
enum gh_cma_params_state gh_pool_cma_params_state(void) { return gh_cma_params; }
bool gh_pool_cma_capable(void)   { return gh_cma_params == GH_CMA_VERIFIED; }

/* ── serve: avail -> served (atomic hot path; §3.2) ────── */
struct page *gh_pool_avail_to_served(void *mm, pid_t tgid)
{
	unsigned long fl;
	u32 idx = GH_NIL, owner_id = 0;
	int i, cls;
	struct gh_owner *o = NULL;
	struct page *pg;

	/* fixed lock order pid(read) -> pool: owner check + transition is ONE
	 * event, no TOCTOU against the sweep (§2) */
	gh_read_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++)
		if (gh_owners[i].mm == mm && !gh_owners[i].dead &&
		    gh_owners[i].vm_count > 0) {
			o = &gh_owners[i];
			owner_id = gh_owner_id(i);
			break;
		}
	if (!o) {
		gh_read_unlock(&gh_pid_lock);
		return NULL;
	}
	(void)tgid;
	gh_spin_lock(&gh_pool_lock, &fl);
	for (cls = GH_L_NON; cls < GH_L_MAX; cls++) {	/* non -> able -> ready */
		idx = gh_slist_pop(&gh_avail[cls]);
		if (idx != GH_NIL)
			break;
	}
	if (idx == GH_NIL) {
		gh_spin_unlock(&gh_pool_lock, &fl);
		gh_read_unlock(&gh_pid_lock);
		return NULL;
	}
	gh_set_state(gh_slot_at(idx), GH_SERVED);	/* state first: cell already off-list */
	if (cls == GH_L_READY)				/* broke a ready block: S-1 siblings -> able */
		gh_rehome(gh_blk_base(idx), GH_L_READY, GH_L_ABLE, GH_NIL);
	GH_WRITE_ONCE(gh_slot_at(idx)->owner_id, owner_id);	/* read locklessly in give-up (§2) */
	GH_CTR_ADD(o->served, 1);
	pg = pfn_to_page(gh_pfn_of_idx(idx));
	get_page(pg);					/* protection ref (§3.2) */
	gh_spin_unlock(&gh_pool_lock, &fl);
	gh_read_unlock(&gh_pid_lock);
	return pg;
}

/* ── return: free hook event, KEEP or DROP in one lock (§3.2) ── */
enum gh_return_result gh_pool_released_return(struct page *pg)
{
	unsigned long fl, pfn = page_to_pfn(pg);
	struct gh_slot *s;
	u32 idx, bidx;
	int held, held_cma, eff;
	enum gh_return_result r;

	idx = gh_idx_of_pfn(pfn);
	s = gh_slot_at(idx);
	if (!s || GH_READ_ONCE(s->state) != GH_RELEASED)
		return GH_RETURN_MISS;
	gh_spin_lock(&gh_pool_lock, &fl);
	if (s->state != GH_RELEASED) {			/* re-check under lock */
		gh_spin_unlock(&gh_pool_lock, &fl);
		return GH_RETURN_MISS;
	}
	bidx = gh_blk_base(idx);
	held = gh_pool_held();				/* this page counts already */
	held_cma = held + (int)gh_nstate[GH_CMA];
	eff = gh_effective_total_locked();
	if (held <= gh_pool_want ||
	    (held_cma <= eff && gh_classify(bidx) != GH_L_NON)) {
		/* KEEP: rebuild the order-9 compound inside the same event
		 * (free_pages_prepare already tore it down), then list it */
		gh_rebuild_order9(pg);
		gh_avail_in(pfn, GH_RELEASED, GH_IN_HOOK);
		if (held_cma > gh_pool_total && held_cma <= eff)
			gh_pool_total = held_cma;	/* proven capacity (§5.1) */
		r = GH_RETURN_KEEP;
	} else {
		enum gh_class before = gh_classify(bidx);

		gh_set_state(s, GH_EXT);		/* DROP: let the free continue */
		gh_rehome(bidx, before, gh_classify(bidx), GH_NIL);
		gh_dbg_gate_drop++;
		r = GH_RETURN_DROP;
	}
	if (GH_READ_ONCE(gh_nstate[GH_RELEASED]) == 0)
		GH_WRITE_ONCE(gh_released_oldest, 0);
	gh_spin_unlock(&gh_pool_lock, &fl);
	return r;
}

/* ── release worker edges ──────────────────────────────── */
/* served -> released for idle pages; returns pending (§3.2/§6) */
int gh_pool_served_to_released(void)
{
	unsigned long fl;
	u32 idx;
	int pending = 0;
	u32 now = gh_now_sec();

	for (idx = 0; idx < gh_span_slots; idx++) {
		struct gh_slot *s = gh_slot_at(idx);
		struct page *pg;
		int cnt;

		if (!s || GH_READ_ONCE(s->state) != GH_SERVED)
			continue;
		pg = pfn_to_page(gh_pfn_of_idx(idx));
		cnt = page_count(pg);
		gh_spin_lock(&gh_pool_lock, &fl);
		if (s->state != GH_SERVED) {		/* re-validate under lock */
			gh_spin_unlock(&gh_pool_lock, &fl);
			continue;
		}
		if (cnt == 1 && !gh_page_has_mapping(pg)) {
			gh_set_state(s, GH_RELEASED);
			GH_WRITE_ONCE(s->released_at, now);
			if (gh_nstate[GH_RELEASED] == 1)
				GH_WRITE_ONCE(gh_released_oldest, now);
			gh_owner_served_dec(s->owner_id);
			gh_spin_unlock(&gh_pool_lock, &fl);
			put_page(pg);			/* outside the lock (§2) */
			continue;
		}
		if (cnt == 0) {				/* orphan: our ref evaporated */
			gh_set_state(s, GH_RELEASED);
			GH_WRITE_ONCE(s->released_at, now);
			if (gh_nstate[GH_RELEASED] == 1)
				GH_WRITE_ONCE(gh_released_oldest, now);
			gh_owner_served_dec(s->owner_id);
			gh_dbg_orphan++;
			gh_spin_unlock(&gh_pool_lock, &fl);
			continue;
		}
		/* still pinned: pending iff the owner is DEAD (give-up under way,
		 * bounded by the next round; a LIVE owner's late unpin is simply
		 * left for a later trigger's round, §6). LOCKLESS
		 * owner read — taking pid_lock here would invert serve's
		 * pid->pool order (§2). Advisory: re-checked next round. */
		if (gh_owner_is_dead(GH_READ_ONCE(s->owner_id)))
			pending++;
		gh_spin_unlock(&gh_pool_lock, &fl);
		gh_cond_resched_every(idx);
	}
	return pending;
}

/* served -> ext: give-up on owner DEATH (§3.2 EXPIRED; a live owner is never
 * touched, whatever its vm_count) or rmmod ALL */
int gh_pool_served_to_ext(enum gh_exmode mode)
{
	unsigned long fl;
	u32 idx, now = gh_now_sec();
	int done = 0;

	for (idx = 0; idx < gh_span_slots; idx++) {
		struct gh_slot *s = gh_slot_at(idx);
		struct gh_owner *o;
		bool expire = (mode == GH_ALL);
		enum gh_class before;
		struct page *pg;

		if (!s || GH_READ_ONCE(s->state) != GH_SERVED)
			continue;
		if (!expire) {
			gh_read_lock(&gh_pid_lock);
			o = gh_owner_by_id(GH_READ_ONCE(s->owner_id));
			if (!o)
				expire = true;		/* generation gone */
			else if (GH_READ_ONCE(o->dead) &&
				 (u32)(now - GH_READ_ONCE(o->died_at)) >=
					GH_DEATH_GRACE_SEC)
				expire = true;		/* pid died AND the short grace is
							 * over — the exit path's own frees
							 * got their chance to come home as
							 * a collect. A LIVE owner is never
							 * touched, whatever its vm_count */
			if (expire && o)
				GH_CTR_ADD(o->abandoned, 1);
			gh_read_unlock(&gh_pid_lock);
		}
		if (!expire)
			continue;
		gh_spin_lock(&gh_pool_lock, &fl);
		if (s->state != GH_SERVED) {
			gh_spin_unlock(&gh_pool_lock, &fl);
			continue;
		}
		before = gh_classify(gh_blk_base(idx));
		gh_set_state(s, GH_EXT);		/* state first (§2 b) */
		gh_rehome(gh_blk_base(idx), before, gh_classify(gh_blk_base(idx)), GH_NIL);
		gh_owner_served_dec(s->owner_id);
		gh_dbg_purged++;
		gh_spin_unlock(&gh_pool_lock, &fl);
		pg = pfn_to_page(gh_pfn_of_idx(idx));
		gh_purge_log_add(gh_pfn_of_idx(idx), 0, page_count(pg));
		put_page(pg);				/* hand back our ref, outside lock */
		done++;
		gh_cond_resched_every(idx);
	}
	return done;
}

/* released -> ext purge: EXPIRED with (count,oldest) gate, or ALL */
int gh_pool_released_to_ext(enum gh_exmode mode)
{
	unsigned long fl;
	u32 idx, now = gh_now_sec(), new_oldest = 0;
	int done = 0;

	if (mode == GH_EXPIRED &&
	    (GH_READ_ONCE(gh_nstate[GH_RELEASED]) == 0 ||
	     (u32)(now - GH_READ_ONCE(gh_released_oldest)) < GH_GRACE_SEC))
		return 0;				/* O(1) gate */
	for (idx = 0; idx < gh_span_slots; idx++) {
		struct gh_slot *s = gh_slot_at(idx);
		enum gh_class before;

		if (!s || GH_READ_ONCE(s->state) != GH_RELEASED)
			continue;
		if (mode == GH_EXPIRED &&
		    (u32)(now - GH_READ_ONCE(s->released_at)) < GH_GRACE_SEC) {
			u32 ra = GH_READ_ONCE(s->released_at);

			if (!new_oldest || (int)(ra - new_oldest) < 0)
				new_oldest = ra;
			continue;
		}
		gh_spin_lock(&gh_pool_lock, &fl);
		before = gh_classify(gh_blk_base(idx));
		gh_set_state(s, GH_EXT);		/* page is in buddy already */
		gh_rehome(gh_blk_base(idx), before, gh_classify(gh_blk_base(idx)), GH_NIL);
		gh_spin_unlock(&gh_pool_lock, &fl);
		done++;
		gh_cond_resched_every(idx);
	}
	{
		unsigned long fl2;

		gh_spin_lock(&gh_pool_lock, &fl2);	/* other writers hold pool_lock */
		GH_WRITE_ONCE(gh_released_oldest, new_oldest);
		gh_spin_unlock(&gh_pool_lock, &fl2);
	}
	return done;
}

/* ── shed: avail -> ext, tri-order (§3.2) ──────────────── */
int gh_pool_avail_to_ext(int n)
{
	unsigned long fl;
	int shed = 0, cls;

	while (shed < n) {
		u32 idx = GH_NIL;
		struct page *pg;

		gh_spin_lock(&gh_pool_lock, &fl);
		for (cls = GH_L_NON; cls < GH_L_MAX; cls++) {
			idx = gh_slist_pop(&gh_avail[cls]);
			if (idx != GH_NIL)
				break;
		}
		if (idx == GH_NIL) {
			gh_spin_unlock(&gh_pool_lock, &fl);
			break;
		}
		gh_set_state(gh_slot_at(idx), GH_EXT);	/* custody exit -> block demotes */
		if (cls != GH_L_NON)			/* siblings flood into NON */
			gh_rehome(gh_blk_base(idx), (enum gh_class)cls,
				  gh_classify(gh_blk_base(idx)), GH_NIL);
		gh_spin_unlock(&gh_pool_lock, &fl);
		pg = pfn_to_page(gh_pfn_of_idx(idx));
		__free_pages(pg, GH_PAGE_ORDER);	/* outside the lock */
		shed++;
	}
	return shed;
}


/* released -> avail by address (AS_PRECISE sweep; §3.2): only pages truly
 * back in buddy/pcp (page_count==0) — never migrate a new occupant. */
bool gh_pool_released_to_avail_sweep(unsigned long pfn)
{
	unsigned long fl;
	struct gh_slot *s = gh_slot_of_pfn(pfn);
	bool ok;

	if (!s || GH_READ_ONCE(s->state) != GH_RELEASED)
		return false;
	if (page_count(pfn_to_page(pfn)) != 0)
		return false;
	if (gh_kapi.contig_range(pfn, pfn + GH_PAGES_PER_SLOT, 0) != 0)
		return false;			/* stays released, purge later */
	gh_rebuild_order9(pfn_to_page(pfn));
	gh_spin_lock(&gh_pool_lock, &fl);
	ok = gh_avail_in(pfn, GH_RELEASED, GH_IN_SWEEP);
	if (gh_nstate[GH_RELEASED] == 0)
		gh_released_oldest = 0;
	gh_spin_unlock(&gh_pool_lock, &fl);
	if (!ok)
		__free_pages(pfn_to_page(pfn), GH_PAGE_ORDER);
	return ok;
}

/* ── scan sets & cursors (pool state; §3.2/§7.2) ───────── */
static u32 *gh_scan_main, *gh_scan_precise, *gh_scan_cma;
static u32 gh_main_total, gh_main_pos, gh_gap_end;
static u32 gh_precise_total, gh_precise_pos;
static u32 gh_cma_total, gh_cma_pos;
static u32 gh_ext_cursor;			/* persistent: whole-zone sweep position */
static u32 gh_cma_rr_cursor;			/* persistent: cma block round-robin */
static int gh_migrate_cma = -1;			/* runtime MIGRATE_CMA value (preflight) */

/* block custody scan helpers */
static bool gh_blk_has(u32 bidx, u8 st)
{
	u32 i;

	for (i = 0; i < GH_S; i++)
		if (GH_READ_ONCE(gh_slot_at(bidx + i)->state) == st)
			return true;
	return false;
}
static bool gh_blk_custody_partial(u32 bidx)
{	/* some cells ours, some EXT, none not_useable: a gap-repair target */
	u32 i;
	bool custody = false, ext = false;

	for (i = 0; i < GH_S; i++) {
		u8 st = GH_READ_ONCE(gh_slot_at(bidx + i)->state);

		if (st == GH_NOT_USEABLE || st == GH_CMA)
			return false;
		if (st == GH_EXT)
			ext = true;
		else
			custody = true;
	}
	return custody && ext;
}

void gh_pool_prepare_scan(int stages)
{
	u32 idx, start;

	gh_precise_total = gh_precise_pos = 0;
	gh_main_total = gh_main_pos = gh_gap_end = 0;
	if ((stages & GH_A_PRECISE) && gh_kapi.cap(GH_CAP_CONTIG_RANGE)) {
		for (idx = 0; idx < gh_span_slots; idx++) {
			struct gh_slot *s = gh_slot_at(idx);

			if (s && GH_READ_ONCE(s->state) == GH_RELEASED &&
			    page_count(pfn_to_page(gh_pfn_of_idx(idx))) == 0)
				gh_scan_precise[gh_precise_total++] = idx;
		}
	}
	if (!(stages & GH_A_MAIN) || !gh_kapi.cap(GH_CAP_CONTIG_RANGE))
		return;
	if (gh_kapi.lru_add_drain_all)
		gh_kapi.lru_add_drain_all();	/* USER-run cost only (§7 PREPARE) */
	/* (1) gap segment: EXT cells of partially-held blocks, address order */
	for (idx = 0; idx < gh_span_slots; idx += GH_S) {
		u32 i;

		if (!gh_slot_at(idx) || !gh_blk_custody_partial(idx))
			continue;
		for (i = 0; i < GH_S; i++)
			if (GH_READ_ONCE(gh_slot_at(idx + i)->state) == GH_EXT)
				gh_scan_main[gh_main_total++] = idx + i;
	}
	gh_gap_end = gh_main_total;
	/* (2)+(3) the rest of EXT strictly AFTER ext_cursor, wrapping once —
	 * the worker sets the cursor to the window it is about to process
	 * ("advance before work"), so the next run resumes past it */
	start = gh_ext_cursor + 1 < gh_span_slots ? gh_ext_cursor + 1 : 0;
	for (idx = 0; idx < gh_span_slots; idx++) {
		u32 k = (start + idx) % gh_span_slots;
		struct gh_slot *s = gh_slot_at(k);

		if (!s || GH_READ_ONCE(s->state) != GH_EXT)
			continue;
		if (gh_blk_custody_partial(gh_blk_base(k)))
			continue;		/* already in the gap segment */
		gh_scan_main[gh_main_total++] = k;
	}
}

unsigned long gh_pool_scan_pop(enum gh_scan_which which, bool *is_gap)
{
	u32 idx;

	switch (which) {
	case GH_SCAN_PRECISE:
		if (gh_precise_pos >= gh_precise_total)
			return GH_PFN_NONE;
		return gh_pfn_of_idx(gh_scan_precise[gh_precise_pos++]);
	case GH_SCAN_MAIN:
		if (gh_main_pos >= gh_main_total)
			return GH_PFN_NONE;
		idx = gh_scan_main[gh_main_pos];
		if (is_gap)
			*is_gap = gh_main_pos < gh_gap_end;
		gh_main_pos++;
		return gh_pfn_of_idx(idx);
	case GH_SCAN_GAP:
		if (gh_main_pos >= gh_gap_end)
			return GH_PFN_NONE;
		return gh_pfn_of_idx(gh_scan_main[gh_main_pos++]);
	}
	return GH_PFN_NONE;
}

int gh_pool_scan_gap_n(void)
{
	return gh_main_pos < gh_gap_end ? (int)(gh_gap_end - gh_main_pos) : 0;
}

void gh_pool_set_cursor(unsigned long pfn)
{
	u32 idx = gh_idx_of_pfn(pfn);

	if (idx != GH_NIL)
		gh_ext_cursor = idx;
}

void gh_pool_scan_finish(void)
{
	gh_precise_total = gh_precise_pos = 0;
	gh_main_total = gh_main_pos = gh_gap_end = 0;
	gh_cma_total = gh_cma_pos = 0;
}

/* ── window actions (token revalidate -> kapi range; §3.2) ── */
bool gh_pool_window_candidate(unsigned long pfn, enum gh_window_purpose purpose)
{
	unsigned long fl;
	struct gh_slot *s = gh_slot_of_pfn(pfn);
	bool ok;

	if (!s)
		return false;
	gh_spin_lock(&gh_pool_lock, &fl);
	ok = (GH_READ_ONCE(s->state) == GH_EXT);
	if (ok && purpose == GH_FILL_TOTAL)
		ok = gh_effective_total_locked() >
		     gh_pool_held() + (int)gh_nstate[GH_CMA];
	if (ok && purpose == GH_FILL_GAP)
		ok = gh_blk_custody_partial(gh_blk_base(gh_idx_of_pfn(pfn)));
	gh_spin_unlock(&gh_pool_lock, &fl);
	return ok && gh_kapi.candidate_range(pfn, pfn + GH_PAGES_PER_SLOT);
}

bool gh_pool_evict(enum gh_evict_mode mode, unsigned long pfn,
		   enum gh_window_purpose purpose)
{
	unsigned long fl;
	struct gh_slot *s = gh_slot_of_pfn(pfn);
	bool ok;

	if (!s)
		return false;
	gh_spin_lock(&gh_pool_lock, &fl);
	ok = (GH_READ_ONCE(s->state) == GH_EXT);
	gh_spin_unlock(&gh_pool_lock, &fl);
	(void)purpose;
	if (ok)
		gh_kapi.evict_range(mode, pfn, pfn + GH_PAGES_PER_SLOT);
	return ok;
}

/* ── cand: assembly holding area + promote conservation (§3.2/§1.1) ── */
#define GH_CAND_MAX 8
static u32 gh_cand_fifo[GH_CAND_MAX];
static int gh_cand_n;

/* promote: cand cells of @bidx -> AVAIL; conservation-free k avail_non cells
 * (collected into @victims as pfns, freed by the caller OUTSIDE the lock). */
static int gh_promote_locked(u32 bidx, unsigned long *victims, int maxv)
{
	u32 i;
	int k = 0, nv = 0, j;

	for (i = 0; i < GH_S; i++) {
		u32 idx = bidx + i;

		if (gh_slot_at(idx)->state != GH_CAND)
			continue;
		for (j = 0; j < gh_cand_n; j++)		/* drop from FIFO */
			if (gh_cand_fifo[j] == idx) {
				gh_cand_fifo[j] = gh_cand_fifo[--gh_cand_n];
				break;
			}
		gh_avail_in(gh_pfn_of_idx(idx), GH_CAND, GH_IN_USER);
		k++;
	}
	while (k-- > 0 && nv < maxv) {			/* conservation free (§1.1) */
		u32 v = gh_slist_pop(&gh_avail[GH_L_NON]);

		if (v == GH_NIL)
			break;				/* short: shed settles later */
		gh_set_state(gh_slot_at(v), GH_EXT);
		victims[nv++] = gh_pfn_of_idx(v);
	}
	return nv;
}

bool gh_pool_ext_to_cand(unsigned long pfn, int noretry)
{
	unsigned long fl, victims[GH_CAND_MAX + 1];
	struct gh_slot *s = gh_slot_of_pfn(pfn);
	u32 idx = gh_idx_of_pfn(pfn), bidx;
	int nv = 0, i;
	unsigned long evict_pfn = GH_PFN_NONE;

	if (!s || s->state != GH_EXT)
		return false;
	if (gh_kapi.contig_range(pfn, pfn + GH_PAGES_PER_SLOT, noretry) != 0)
		return false;
	gh_rebuild_order9(pfn_to_page(pfn));
	gh_spin_lock(&gh_pool_lock, &fl);
	if (s->state != GH_EXT) {			/* re-validate (§2) */
		gh_dbg_reject++;
		gh_spin_unlock(&gh_pool_lock, &fl);
		__free_pages(pfn_to_page(pfn), GH_PAGE_ORDER);
		return false;
	}
	if (gh_cand_n >= GH_CAND_MAX) {			/* overflow: oldest self-clears */
		u32 old = gh_cand_fifo[0], ob = gh_blk_base(old);
		enum gh_class oc = gh_classify(ob);

		for (i = 0; i < gh_cand_n - 1; i++)
			gh_cand_fifo[i] = gh_cand_fifo[i + 1];
		gh_cand_n--;
		gh_set_state(gh_slot_at(old), GH_EXT);
		gh_rehome(ob, oc, gh_classify(ob), GH_NIL);
		evict_pfn = gh_pfn_of_idx(old);
	}
	bidx = gh_blk_base(idx);
	{
		enum gh_class bc = gh_classify(bidx);

		gh_set_state(s, GH_CAND);		/* custody entry: block may rise */
		gh_rehome(bidx, bc, gh_classify(bidx), GH_NIL);
	}
	gh_cand_fifo[gh_cand_n++] = idx;
	if (gh_classify(bidx) != GH_L_NON && !gh_blk_has(bidx, GH_EXT))
		nv = gh_promote_locked(bidx, victims, GH_CAND_MAX);
	gh_spin_unlock(&gh_pool_lock, &fl);
	if (evict_pfn != GH_PFN_NONE)
		__free_pages(pfn_to_page(evict_pfn), GH_PAGE_ORDER);
	for (i = 0; i < nv; i++)
		__free_pages(pfn_to_page(victims[i]), GH_PAGE_ORDER);
	return true;
}

int gh_pool_cand_to_ext(void)
{
	unsigned long fl, pfns[GH_CAND_MAX];
	int i, n;

	gh_spin_lock(&gh_pool_lock, &fl);
	n = gh_cand_n;
	for (i = 0; i < n; i++) {
		u32 b = gh_blk_base(gh_cand_fifo[i]);
		enum gh_class c = gh_classify(b);

		pfns[i] = gh_pfn_of_idx(gh_cand_fifo[i]);
		gh_set_state(gh_slot_at(gh_cand_fifo[i]), GH_EXT);
		gh_rehome(b, c, gh_classify(b), GH_NIL);
	}
	gh_cand_n = 0;
	gh_spin_unlock(&gh_pool_lock, &fl);
	for (i = 0; i < n; i++)
		__free_pages(pfn_to_page(pfns[i]), GH_PAGE_ORDER);
	return n;
}

/* ── acquire: ext -> avail (§3.2, all five modes) ──────── */
/* S>1 sibling completion: after a single landed in a partial block, grab the
 * EXT siblings cheaply so the block can complete (design: <=S-1 NORETRY). */
static void gh_try_complete_block(u32 idx)
{
	u32 bidx = gh_blk_base(idx), i;

	if (GH_S == 1)
		return;
	for (i = 0; i < GH_S; i++) {
		struct gh_slot *sib = gh_slot_at(bidx + i);

		if (sib->state == GH_EXT)
			gh_pool_ext_to_cand(gh_pfn_of_idx(bidx + i), 1);
	}
}

int gh_pool_ext_to_avail(enum gh_how how, unsigned long at_pfn,
			 struct gh_intake_budget *budget, int flags)
{
	struct page *pg;
	unsigned long pfn;
	unsigned long fl;
	int need, accepted = 0;
	bool ready_intake;

	ready_intake = (how == GH_ALLOC_BLOCK) || (GH_S == 1);

	/* hard cap re-derived under lock: caller budget is a hint, not trust */
	gh_spin_lock(&gh_pool_lock, &fl);
	need = gh_effective_total_locked() -
	       (gh_pool_held() + (int)gh_nstate[GH_CMA]);
	if (!ready_intake && gh_pool_held() >= gh_pool_want)
		need = 0;				/* non-ready singles never pass want */
	gh_spin_unlock(&gh_pool_lock, &fl);
	if (need <= 0)
		return 0;
	if (budget && (ready_intake ? budget->ready : budget->single) <= 0)
		return 0;

	switch (how) {
	case GH_ALLOC_BLOCK: {
		u32 i;

		if (budget && budget->ready < (int)GH_S)
			return 0;
		pg = gh_kapi.alloc_try(gh_cma_order, 0);
		if (!pg)
			return 0;
		pfn = page_to_pfn(pg);
		gh_spin_lock(&gh_pool_lock, &fl);
		for (i = 0; i < GH_S; i++) {
			unsigned long sub = pfn + ((unsigned long)i << GH_PAGE_ORDER);

			gh_rebuild_order9(pfn_to_page(sub));
			if (gh_avail_in(sub, GH_EXT, GH_IN_USER))
				accepted++;
			else {
				gh_spin_unlock(&gh_pool_lock, &fl);
				__free_pages(pfn_to_page(sub), GH_PAGE_ORDER);
				gh_spin_lock(&gh_pool_lock, &fl);
			}
		}
		break;
	}
	case GH_ALLOC_LIGHT:
	case GH_ALLOC_FULL:
		pg = gh_kapi.alloc_try(GH_PAGE_ORDER, how == GH_ALLOC_FULL);
		if (!pg)
			return 0;
		pfn = page_to_pfn(pg);
		gh_spin_lock(&gh_pool_lock, &fl);
		if (gh_avail_in(pfn, GH_EXT,
				how == GH_ALLOC_FULL ? GH_IN_REFILL : GH_IN_USER))
			accepted = 1;
		else {
			gh_spin_unlock(&gh_pool_lock, &fl);
			__free_pages(pg, GH_PAGE_ORDER);
			return 0;
		}
		break;
	case GH_CONTIG_ANY:
		pg = gh_kapi.contig_pages(GH_PAGES_PER_SLOT);
		if (!pg)
			return 0;
		pfn = page_to_pfn(pg);
		gh_rebuild_order9(pg);
		gh_spin_lock(&gh_pool_lock, &fl);
		if (gh_avail_in(pfn, GH_EXT, GH_IN_USER))
			accepted = 1;
		else {
			gh_spin_unlock(&gh_pool_lock, &fl);
			__free_pages(pg, GH_PAGE_ORDER);
			return 0;
		}
		break;
	case GH_CONTIG_AT: {
		struct gh_slot *s = gh_slot_of_pfn(at_pfn);

		if (!s || s->state != GH_EXT)
			return 0;
		if (gh_kapi.contig_range(at_pfn, at_pfn + GH_PAGES_PER_SLOT,
					 flags & GH_INTAKE_NORETRY) != 0)
			return 0;
		pfn = at_pfn;
		gh_rebuild_order9(pfn_to_page(pfn));
		gh_spin_lock(&gh_pool_lock, &fl);
		if (gh_avail_in(pfn, GH_EXT, GH_IN_USER))
			accepted = 1;
		else {
			gh_spin_unlock(&gh_pool_lock, &fl);
			__free_pages(pfn_to_page(pfn), GH_PAGE_ORDER);
			return 0;
		}
		break;
	}
	}

	if (accepted) {					/* proven capacity (§5.1) */
		int held_cma = gh_pool_held() + (int)gh_nstate[GH_CMA];
		int eff = gh_effective_total_locked();

		if (held_cma > gh_pool_total)
			gh_pool_total = held_cma > eff ? eff : held_cma;
	}
	gh_spin_unlock(&gh_pool_lock, &fl);
	if (budget) {					/* deduct per actual result (§3.2) */
		budget->ready -= accepted;
		if (!ready_intake || GH_S == 1)
			budget->single -= accepted;
	}
	if (accepted && !ready_intake && how != GH_ALLOC_BLOCK)
		gh_try_complete_block(gh_idx_of_pfn(pfn));	/* S>1 pairing */
	return accepted;
}
/* ── pool_check: bookkeeping vs table (debug=1; §1 G/U) ── */
bool gh_pool_check(void)
{
	long cnt[GH_STATE_MAX] = {0};
	int on_list[GH_L_MAX] = {0};
	u32 idx;
	int cls;
	bool ok = true;

	for (idx = 0; idx < gh_span_slots; idx++) {
		struct gh_slot *s = gh_slot_at(idx);

		if (!s) {
			idx |= (GH_SLOTS_PER_GB - 1);	/* skip hole GB */
			continue;
		}
		cnt[s->state]++;
		if (s->state == GH_AVAIL) {
			enum gh_class c = gh_classify(gh_blk_base(idx));
			u32 w;
			bool found = false;

			u32 guard = 0;

			for (w = gh_avail[c].head; w != GH_NIL && guard <= gh_span_slots;
			     w = gh_slot_at(w)->next, guard++)
				if (w == idx) { found = true; break; }
			if (guard > gh_span_slots) {
				gh_pr_warn("check: list %d cycles\n", c);
				return false;
			}
			if (!found) {
				gh_pr_warn("check: idx %u AVAIL not on class %d list\n", idx, c);
				ok = false;
			}
		}
		if (s->state == GH_CMA || gh_slot_at(gh_blk_base(idx))->state == GH_CMA) {
			u32 i, b = gh_blk_base(idx);	/* U: CMA blocks uniform */

			for (i = 0; i < GH_S; i++)
				if ((gh_slot_at(b + i)->state == GH_CMA) !=
				    (s->state == GH_CMA && idx == b + i ? true :
				     gh_slot_at(b)->state == GH_CMA)) {
					gh_pr_warn("check: block %u CMA not uniform\n", b);
					ok = false;
					break;
				}
		}
	}
	for (idx = 0; idx < GH_STATE_MAX; idx++)
		if (cnt[idx] != gh_nstate[idx]) {
			gh_pr_warn("check: state %u count %ld != %ld\n",
				   idx, cnt[idx], gh_nstate[idx]);
			ok = false;
		}
	for (cls = 0; cls < GH_L_MAX; cls++) {
		u32 w, guard = 0;

		for (w = gh_avail[cls].head; w != GH_NIL && guard <= gh_span_slots;
		     w = gh_slot_at(w)->next, guard++)
			on_list[cls]++;
		if (guard > gh_span_slots) {
			gh_pr_warn("check: list %d cycles\n", cls);
			return false;
		}
		if (on_list[cls] != gh_avail[cls].n) {
			gh_pr_warn("check: list %d walk %d != n %d\n",
				   cls, on_list[cls], gh_avail[cls].n);
			ok = false;
		}
	}
	return ok;
}

/* ── init (§8 subset: table build + not_useable marking) ── */
int gh_pool_init_table(const struct gh_ram_range *ranges, int nranges, int size_max)
{
	unsigned long lo = ~0UL, hi = 0;
	int i;
	u32 g;

	for (i = 0; i < nranges; i++) {
		if (ranges[i].start_pfn < lo)
			lo = ranges[i].start_pfn;
		if (ranges[i].end_pfn > hi)
			hi = ranges[i].end_pfn;
	}
	gh_span_base_pfn = lo & ~((1UL << 18) - 1);	/* 1GB align down */
	gh_span_slots = (u32)(((hi - gh_span_base_pfn) + (1UL << 18) - 1) >> 18)
			* GH_SLOTS_PER_GB;
	if (gh_span_slots / GH_SLOTS_PER_GB > GH_SPAN_GB_MAX)
		return -1;
	gh_pool_size_max = size_max;
	for (g = 0; g < gh_span_slots / GH_SLOTS_PER_GB; g++)
		gh_top[g] = NULL;
	for (i = 0; i < GH_L_MAX; i++)
		gh_avail[i].head = gh_avail[i].tail = GH_NIL, gh_avail[i].n = 0;
	for (i = 0; i < GH_STATE_MAX; i++)
		gh_nstate[i] = 0;
	gh_free(gh_scan_main); gh_free(gh_scan_precise); gh_free(gh_scan_cma);
	gh_scan_main = gh_zalloc(sizeof(u32) * gh_span_slots);
	gh_scan_precise = gh_zalloc(sizeof(u32) * gh_span_slots);
	gh_scan_cma = gh_zalloc(sizeof(u32) * gh_span_slots * 2);
	if (!gh_scan_main || !gh_scan_precise || !gh_scan_cma)
		return -1;
	gh_ext_cursor = 0; gh_cma_rr_cursor = 0;
	gh_pool_scan_finish();
	gh_cand_n = 0;

	for (i = 0; i < nranges; i++) {			/* chunks only where RAM lives */
		unsigned long p;

		for (p = ranges[i].start_pfn & ~((1UL << 18) - 1);
		     p < ranges[i].end_pfn; p += (1UL << 18)) {
			u32 gb = (u32)((p - gh_span_base_pfn) >> 18);

			if (!gh_top[gb]) {
				gh_top[gb] = gh_kzalloc_chunk();
				if (!gh_top[gb])
					return -1;
				gh_nstate[GH_NOT_USEABLE] += GH_SLOTS_PER_GB;
			}
		}
	}
	for (i = 0; i < nranges; i++) {			/* fully-inside cells -> EXT */
		unsigned long p = (ranges[i].start_pfn + GH_PAGES_PER_SLOT - 1)
				  & ~(GH_PAGES_PER_SLOT - 1);

		for (; p + GH_PAGES_PER_SLOT <= ranges[i].end_pfn; p += GH_PAGES_PER_SLOT) {
			struct gh_slot *s = gh_slot_of_pfn(p);

			if (s && s->state == GH_NOT_USEABLE)
				gh_set_state(s, GH_EXT);
		}
	}
	return 0;
}

void gh_pool_mark_not_useable(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long p;

	for (p = start_pfn & ~(GH_PAGES_PER_SLOT - 1); p < end_pfn; p += GH_PAGES_PER_SLOT) {
		struct gh_slot *s = gh_slot_of_pfn(p);

		if (s && s->state == GH_EXT)
			gh_set_state(s, GH_NOT_USEABLE);
	}
}

/* ── CMA reservoir edges (§3.2): flip / bucket scan / stage_in / drop ── */
/* release a held 2MB into the (CMA-labeled) block's own freelist as two
 * order-8 halves: bypasses pcp AND our own order-9 hook (§3.2 free_to_cma) */
/* Release one held 2MB page into its (CMA-labeled) block's freelist. The pool
 * holds these as order-9 COMPOUNDS (gh_rebuild_order9), so we free the compound
 * as-is with __free_pages(head, 9): free_pages_prepare correctly tears the
 * compound down. It lands on the CMA freelist because the pageblock is already
 * CMA-labeled (via a pcp transit whose stored pcppage_migratetype preserves
 * CMA); the caller drains pcp so the accounting is immediate. The order-9 free
 * hook sees it but the slot is already CMA (not RELEASED) so it MISSes.
 *
 * (The design's order-8 split — bypass pcp AND the hook — needs the page as
 * plain singles, not a compound; freeing a compound as order-8 trips
 * free_tail_page_prepare's nonzero-entire_mapcount check. Deferred; §11.) */
static void gh_free_to_cma(unsigned long pfn)
{
	struct page *head = pfn_to_page(pfn);

	set_page_count(head, 1);
	__free_pages(head, GH_PAGE_ORDER);
}

static void gh_label_block(u32 bidx, int mt)
{
	/* One call: gh_kapi.set_pageblock_mt already labels EVERY real pageblock
	 * inside the believed block (it spans 1<<cma_order stepping by the real
	 * pageblock size). Looping here too double-covers into the NEXT believed
	 * block whenever cma_order > real pageblock_order (i.e. sim, and any
	 * future case where they differ) — a real over-labeling bug, masked on
	 * devices only because there cma_order == pageblock_order makes the
	 * adapter's span exactly one real pageblock. */
	gh_kapi.set_pageblock_mt(gh_pfn_of_idx(bidx), mt);
}

int gh_pool_avail_to_cma(int n)
{
	unsigned long fl;
	int flipped = 0;

	if (gh_cma_params != GH_CMA_VERIFIED)
		return 0;				/* api self-defense (§3.2) */
	while (flipped < n) {
		u32 head, bidx, i;

		gh_spin_lock(&gh_pool_lock, &fl);
		head = gh_avail[GH_L_READY].head;
		if (head == GH_NIL) {
			gh_spin_unlock(&gh_pool_lock, &fl);
			break;
		}
		bidx = gh_blk_base(head);
		for (i = 0; i < GH_S; i++) {		/* whole block, structure-guaranteed */
			gh_slist_unlink(&gh_avail[GH_L_READY], bidx + i);
			gh_set_state(gh_slot_at(bidx + i), GH_CMA);
		}
		gh_spin_unlock(&gh_pool_lock, &fl);
		gh_label_block(bidx, gh_migrate_cma);
		for (i = 0; i < GH_S; i++)
			gh_free_to_cma(gh_pfn_of_idx(bidx + i));
		flipped++;
	}
	return flipped;
}

/* run-local occupancy bucket scan (§7.2): snapshot -> probe -> counting sort */
int gh_pool_prepare_cma_scan(int all)
{
	u32 nblk = gh_span_slots / GH_S, b, n = 0, i;
	u32 *snap = gh_scan_cma;			/* reuse tail half as temp */
	u8 *score;
	u32 cnt[9] = {0}, pos[9];
	unsigned long span_pages = (unsigned long)GH_S << GH_PAGE_ORDER;
	u32 width = (u32)((span_pages + 7) / 8);

	(void)all;
	for (b = 0; b < nblk; b++) {			/* address order from rr_cursor */
		u32 bidx = ((gh_cma_rr_cursor / GH_S + b) % nblk) * GH_S;
		struct gh_slot *s = gh_slot_at(bidx);

		if (s && s->state == GH_CMA)
			snap[n++] = bidx;
	}
	score = (u8 *)(snap + n);			/* scratch after the snapshot */
	for (i = 0; i < n; i++) {
		unsigned long p = gh_pfn_of_idx(snap[i]), occ = 0, k;

		for (k = 0; k < span_pages; k++)	/* racy probe, ranking only */
			if (page_count(pfn_to_page(p + k)) != 0)
				occ++;
		score[i] = occ == 0 ? 0 : (u8)(1 + ((occ - 1) / width > 7 ? 7 : (occ - 1) / width));
		cnt[score[i]]++;
	}
	pos[0] = 0;
	for (i = 1; i < 9; i++)
		pos[i] = pos[i - 1] + cnt[i - 1];
	/* stable counting sort into the front of gh_scan_cma — sort into a
	 * temp then copy (snap aliases the buffer) */
	{
		static u32 sorted[1 << 16];		/* bounded by span slots/S; static: no kmalloc */
		for (i = 0; i < n; i++)
			sorted[pos[score[i]]++] = snap[i];
		for (i = 0; i < n; i++)
			gh_scan_cma[i] = sorted[i];
	}
	gh_cma_total = n;
	gh_cma_pos = 0;
	return (int)n;
}

/* run-local "tried-all" verdict: the pop has nothing left. Reads true with no
 * scan prepared (scan_finish zeroes both counters) — callers gate on it AFTER
 * their own prepare when that matters (§7 CTA/CMA_FREE exit conditions). */
int gh_pool_cma_scan_done(void)
{
	return gh_cma_pos >= gh_cma_total;
}

/* shared pop for stage_in/drop: verify still CMA, advance rr cursor */
static u32 gh_cma_scan_pop(void)
{
	while (gh_cma_pos < gh_cma_total) {
		u32 bidx = gh_scan_cma[gh_cma_pos++];

		gh_cma_rr_cursor = bidx;		/* success or fail: rotate */
		if (gh_slot_at(bidx)->state == GH_CMA)
			return bidx;
	}
	return GH_NIL;
}

int gh_pool_cma_to_avail(int n)
{
	unsigned long fl;
	int got = 0, tried = 0;

	while (got < n && tried < n) {
		u32 bidx = gh_cma_scan_pop(), i;
		unsigned long base;

		if (bidx == GH_NIL)
			break;
		tried++;
		base = gh_pfn_of_idx(bidx);
		if (gh_kapi.contig_range_cma(base,
				base + ((unsigned long)GH_S << GH_PAGE_ORDER)) != 0)
			continue;			/* SKIP_THIS_SCAN */
		gh_label_block(bidx, 1 /* MOVABLE */);
		for (i = 0; i < GH_S; i++) {
			unsigned long sub = base + ((unsigned long)i << GH_PAGE_ORDER);

			gh_rebuild_order9(pfn_to_page(sub));
			gh_spin_lock(&gh_pool_lock, &fl);
			if (!gh_avail_in(sub, GH_CMA, GH_IN_CMA)) {
				gh_spin_unlock(&gh_pool_lock, &fl);
				__free_pages(pfn_to_page(sub), GH_PAGE_ORDER);
				continue;
			}
			gh_spin_unlock(&gh_pool_lock, &fl);
		}
		got++;
	}
	return got;
}

int gh_pool_cma_to_ext(int n)
{
	unsigned long fl;
	int got = 0, tried = 0;

	while (got < n && tried < n) {
		u32 bidx = gh_cma_scan_pop(), i;
		unsigned long base;

		if (bidx == GH_NIL)
			break;
		tried++;
		base = gh_pfn_of_idx(bidx);
		if (gh_kapi.contig_range_cma(base,
				base + ((unsigned long)GH_S << GH_PAGE_ORDER)) != 0)
			continue;			/* stuck: stays CMA, keeps counting */
		gh_label_block(bidx, 1 /* MOVABLE */);
		gh_spin_lock(&gh_pool_lock, &fl);
		for (i = 0; i < GH_S; i++)
			gh_set_state(gh_slot_at(bidx + i), GH_EXT);
		gh_spin_unlock(&gh_pool_lock, &fl);
		for (i = 0; i < GH_S; i++) {
			unsigned long sub = base + ((unsigned long)i << GH_PAGE_ORDER);

			gh_rebuild_order9(pfn_to_page(sub));
			__free_pages(pfn_to_page(sub), GH_PAGE_ORDER);
		}
		got++;
	}
	return got;
}

/* ── cma parameter verification (§3.2 pool_verify_cma_params) ── */
enum gh_verify_result gh_pool_verify_cma_params(void)
{
	unsigned long fl;
	u32 span, base = GH_NIL, i;
	unsigned long p0, pre0, pre2, delta_before;
	int a, b;

	if (gh_cma_params == GH_CMA_VERIFIED)
		return GH_VERIFY_OK;
	if (gh_cma_params == GH_CMA_UNAVAILABLE)
		return GH_VERIFY_FAILED;

	/* window: 2 believed blocks + guard out to the largest supported
	 * order's block, fully AVAIL, aligned (§3.2: never touch unheld) */
	span = 2 * GH_S;
	if (span < (1u << (GH_CMA_ORDER_MAX - GH_PAGE_ORDER)))
		span = 1u << (GH_CMA_ORDER_MAX - GH_PAGE_ORDER);
	gh_spin_lock(&gh_pool_lock, &fl);
	for (i = 0; i + span <= gh_span_slots; i += span) {
		u32 k;
		bool ok = true;

		for (k = 0; k < span; k++) {
			struct gh_slot *s = gh_slot_at(i + k);

			if (!s || s->state != GH_AVAIL) { ok = false; break; }
		}
		if (ok) { base = i; break; }
	}
	if (base == GH_NIL) {
		gh_spin_unlock(&gh_pool_lock, &fl);
		return GH_VERIFY_DEFERRED;		/* no safe window this run */
	}
	for (i = 0; i < span; i += GH_S) {		/* hold: AVAIL -> VERIFY, per block */
		enum gh_class c = gh_classify(base + i);	/* all S cells share this list */
		u32 j;

		for (j = 0; j < GH_S; j++) {
			gh_slist_unlink(&gh_avail[c], base + i + j);
			gh_set_state(gh_slot_at(base + i + j), GH_VERIFY);
		}
	}
	gh_spin_unlock(&gh_pool_lock, &fl);

	p0 = gh_pfn_of_idx(base);
	pre0 = gh_kapi.get_pfnblock_mt(p0);
	pre2 = gh_kapi.get_pfnblock_mt(p0 + ((unsigned long)GH_S << GH_PAGE_ORDER));
	if (pre0 > 2 || pre2 > 2)
		goto restore_deferred;			/* not flippable labels: retry later */

	gh_label_block(base, gh_migrate_cma);		/* flip "first believed block" */
	a = (int)gh_kapi.get_pfnblock_mt(p0 + ((unsigned long)GH_S << GH_PAGE_ORDER) - 1);
	b = (int)gh_kapi.get_pfnblock_mt(p0 + ((unsigned long)GH_S << GH_PAGE_ORDER));
	if (a != gh_migrate_cma || b != (int)pre2) {	/* boundary reads: off-by-one */
		gh_pr_warn("cma verify: pageblock order belief wrong (a=%d b=%d)\n", a, b);
		gh_label_block(base, (int)pre0);
		goto fail;
	}
	/* semantic: +2MB free into the flipped block must move CmaFree ~+512 */
	delta_before = gh_kapi.cma_free_pages();
	gh_free_to_cma(p0);
	if (gh_kapi.drain_pages)
		gh_kapi.drain_pages();		/* order-9 free pcp-transits (THP order);
						 * drain so NR_FREE_CMA_PAGES and the
						 * grab-back below see it in buddy */
	if (gh_kapi.cma_free_pages() - delta_before < 384) {
		gh_kapi.contig_range_cma(p0, p0 + GH_PAGES_PER_SLOT);
		gh_label_block(base, (int)pre0);
		if (++gh_cma_verify_misses < GH_CMA_VERIFY_MISS_MAX) {
			gh_pr_info("cma verify: CmaFree delta too small (attempt %d/%d) — retrying\n",
				   gh_cma_verify_misses, GH_CMA_VERIFY_MISS_MAX);
			goto retry_deferred;		/* transient (pcp/vmstat lag) */
		}
		gh_pr_warn("cma verify: CmaFree delta too small after %d attempts\n",
			   gh_cma_verify_misses);
		goto fail;				/* persistent — CMA unobservable */
	}
	/* end-to-end CMA-mode grab-back of exactly what was freed */
	if (gh_kapi.contig_range_cma(p0, p0 + GH_PAGES_PER_SLOT) != 0) {
		gh_pr_warn("cma verify: CMA-mode grab-back failed\n");
		gh_label_block(base, (int)pre0);
		goto fail;
	}
	gh_label_block(base, (int)pre0);		/* restore, whole window intact */
	gh_spin_lock(&gh_pool_lock, &fl);
	for (i = 0; i < span; i++) {
		gh_rebuild_order9(pfn_to_page(gh_pfn_of_idx(base + i)));
		gh_avail_in(gh_pfn_of_idx(base + i), GH_VERIFY, GH_ORIGIN_MAX);
	}
	gh_cma_params = GH_CMA_VERIFIED;
	gh_spin_unlock(&gh_pool_lock, &fl);
	gh_pr_info("cma verify: VERIFIED — movable->CMA reservoir enabled\n");
	return GH_VERIFY_OK;

restore_deferred:
	gh_spin_lock(&gh_pool_lock, &fl);
	for (i = 0; i < span; i++)
		gh_avail_in(gh_pfn_of_idx(base + i), GH_VERIFY, GH_ORIGIN_MAX);
	gh_spin_unlock(&gh_pool_lock, &fl);
	return GH_VERIFY_DEFERRED;

retry_deferred:	/* transient probe miss: rebuild the touched compounds, stay PENDING */
	gh_spin_lock(&gh_pool_lock, &fl);
	for (i = 0; i < span; i++) {
		gh_rebuild_order9(pfn_to_page(gh_pfn_of_idx(base + i)));
		gh_avail_in(gh_pfn_of_idx(base + i), GH_VERIFY, GH_ORIGIN_MAX);
	}
	gh_spin_unlock(&gh_pool_lock, &fl);
	return GH_VERIFY_DEFERRED;

fail:
	gh_spin_lock(&gh_pool_lock, &fl);
	for (i = 0; i < span; i++) {
		gh_rebuild_order9(pfn_to_page(gh_pfn_of_idx(base + i)));
		gh_avail_in(gh_pfn_of_idx(base + i), GH_VERIFY, GH_ORIGIN_MAX);
	}
	gh_cma_params = GH_CMA_UNAVAILABLE;		/* boot constant: no retry */
	gh_pool_want_with_cma = 0;
	if (gh_pool_total > gh_pool_want)
		gh_pool_total = gh_pool_want;
	gh_spin_unlock(&gh_pool_lock, &fl);
	gh_pr_warn("cma verify: UNAVAILABLE — running pure-pool (no CMA reservoir)\n");
	return GH_VERIFY_FAILED;
}

/* ── snapshots (§10: sysfs never mixes generations) ────── */
void gh_pool_stats_snapshot(struct gh_pool_stats *o)
{
	unsigned long fl;
	int i;

	gh_spin_lock(&gh_pool_lock, &fl);
	o->want = gh_pool_want; o->want_cma = gh_pool_want_with_cma;
	o->size_max = gh_pool_size_max; o->total = gh_pool_total;
	o->avail = gh_pool_avail();
	o->served = (int)gh_nstate[GH_SERVED];
	o->released = (int)gh_nstate[GH_RELEASED];
	o->verify = (int)gh_nstate[GH_VERIFY];
	o->cma = (int)gh_nstate[GH_CMA];
	o->ext = (int)gh_nstate[GH_EXT];
	o->avail_non = gh_avail[GH_L_NON].n;
	o->avail_able = gh_avail[GH_L_ABLE].n;
	o->avail_ready = gh_avail[GH_L_READY].n;
	o->cma_params = gh_cma_params;
	o->reject = gh_dbg_reject; o->orphan = gh_dbg_orphan;
	o->gate_drop = gh_dbg_gate_drop; o->purged = gh_dbg_purged;
	for (i = 0; i < GH_ORIGIN_MAX; i++)
		o->origin[i] = gh_dbg_origin[i];
	gh_spin_unlock(&gh_pool_lock, &fl);
}

void gh_pool_owner_stats_snapshot(struct gh_owner_stats *o)
{
	int i;

	o->n = 0;
	o->live_served = 0;
	gh_read_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++) {
		struct gh_owner *w = &gh_owners[i];

		if (!w->mm)
			continue;
		if (w->vm_count > 0)
			o->live_served += w->served;
		o->row[o->n].tgid = w->tgid;
		gh_strscpy(o->row[o->n].comm, w->comm, GH_COMM_LEN);
		o->row[o->n].vm_count = w->vm_count;
		o->row[o->n].dead = w->dead;
		o->row[o->n].served = w->served;
		o->row[o->n].abandoned = w->abandoned;
		o->n++;
	}
	gh_read_unlock(&gh_pid_lock);
}

/* config: preflight results land here (insmod, §8; harness world_reset) */
void gh_pool_set_cma_config(int cma_order, int migrate_cma_val)
{
	gh_cma_order = cma_order;
	gh_migrate_cma = migrate_cma_val;
	gh_cma_params = (migrate_cma_val >= 0 &&
			 cma_order >= GH_PAGE_ORDER && cma_order <= GH_CMA_ORDER_MAX)
			? GH_CMA_PENDING : GH_CMA_UNAVAILABLE;
}

void gh_pool_teardown_table(void)		/* rmmod: free everything (§8) */
{
	u32 g;

	for (g = 0; g < gh_span_slots / GH_SLOTS_PER_GB; g++)
		if (gh_top[g]) { gh_free(gh_top[g]); gh_top[g] = NULL; }
	gh_free(gh_scan_main); gh_free(gh_scan_precise); gh_free(gh_scan_cma);
	gh_scan_main = gh_scan_precise = gh_scan_cma = NULL;
}

/* ── init-time carveout marking (§8): vendor CMA areas + ZONE_MOVABLE ── */
/* Single-threaded init context (no serve/hook yet), so no lock needed for the
 * walk; page/zone judgment goes through kapi (§3.3). A 2MB window is refused
 * if ANY of its pages is ZONE_MOVABLE, or its pageblock is already CMA-labeled
 * (someone else's carveout — we have flipped nothing yet). */
void gh_pool_mark_carveouts(int migrate_cma_val)
{
	u32 idx;

	for (idx = 0; idx < gh_span_slots; idx++) {
		struct gh_slot *s = gh_slot_at(idx);
		unsigned long pfn = gh_pfn_of_idx(idx);
		bool bad = false;
		unsigned long p;

		if (!s || s->state != GH_EXT)
			continue;
		if (migrate_cma_val >= 0 && gh_kapi.get_pfnblock_mt &&
		    (int)gh_kapi.get_pfnblock_mt(pfn) == migrate_cma_val)
			bad = true;
		if (!bad && gh_kapi.in_zone_movable)
			for (p = pfn; p < pfn + GH_PAGES_PER_SLOT; p += GH_PAGES_PER_SLOT)
				if (gh_kapi.in_zone_movable(p)) {
					bad = true;
					break;
				}
		if (bad)
			gh_set_state(s, GH_NOT_USEABLE);
	}
}

/* ── reporting accessors (sysfs; §10) ─────────────────── */
void gh_pool_purge_log_snapshot(struct gh_purge_log *o)
{
	unsigned long fl;
	unsigned int i, first;

	gh_spin_lock(&gh_pool_lock, &fl);
	o->total = gh_purge_log_n;
	first = gh_purge_log_n > GH_PURGE_LOG_MAX ?
		gh_purge_log_n - GH_PURGE_LOG_MAX : 0;
	o->n = 0;
	for (i = first; i < gh_purge_log_n; i++)
		o->rec[o->n++] = gh_purge_log[i % GH_PURGE_LOG_MAX];
	gh_spin_unlock(&gh_pool_lock, &fl);
}

/* base pfns of the blocks currently ours-and-CMA (for cma_usage). Returns
 * count; caller sizes @out to the reservoir bound. */
int gh_pool_cma_block_bases(unsigned long *out, int max)
{
	unsigned long fl;
	u32 bidx;
	int n = 0;

	gh_spin_lock(&gh_pool_lock, &fl);
	for (bidx = 0; bidx < gh_span_slots && n < max; bidx += GH_S) {
		struct gh_slot *s = gh_slot_at(bidx);

		if (s && s->state == GH_CMA)
			out[n++] = gh_pfn_of_idx(bidx);
	}
	gh_spin_unlock(&gh_pool_lock, &fl);
	return n;
}
int gh_pool_cma_block_span(void) { return (int)GH_S << GH_PAGE_ORDER; }
