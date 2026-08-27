/* gh_hugepage_reserve — owner registry (pool facade, own pid_lock; §2).
 *
 * Entry lifetime = pid lifetime: an owner that closed its last VM stays in
 * the table while the process lives (the app watches vm_count==0 with
 * served>0 persisting as the slow/stuck-teardown signal, §10). Removal
 * happens only for dead owners with no SERVED/RELEASED left (sweep), keyed
 * by the pid-exit hook or the mm_users==0 backstop.
 *
 * vm_count is ONLY the serve gate (§2): boot ++, shutdown --, no serving
 * while 0. It drives no give-up and stamps no clock — giving up on a page
 * is the EVENT consequence of pid death (the old idle_since+10s clock raced
 * a 1->0->1 VM restart into false give-ups; removed 2026-08-28).
 */

struct gh_owner {
	u32  generation;		/* 0 = free slot; bumped on every (re)register */
	void *mm;			/* published last, release order (lockless readers) */
	pid_t tgid;
	char comm[GH_COMM_LEN];
	int  vm_count;
	int  dead;			/* pid-exit hook marked; the ONLY give-up driver */
	u32  died_at;			/* seconds at death; starts the short give-up
					 * grace (GH_DEATH_GRACE_SEC). Never cleared:
					 * death is one-way, so no restart race. */
	long served;			/* pages currently attributed (report) */
	long abandoned;			/* pages given up on this owner (report) */
};

static gh_rwlock_t gh_pid_lock;
static struct gh_owner gh_owners[GH_VM_OWNER_MAX];
static u32 gh_owner_next_gen = 1;

/* owner_id = generation<<8 | (slot+1); 0 = none. 24 bits of generation is
 * plenty: it only has to outrun slot reuse inside one boot. */
static inline u32 gh_owner_id(int slot) { return (GH_READ_ONCE(gh_owners[slot].generation) << 8) | (u32)(slot + 1); }
static inline int gh_owner_slot_of(u32 id) { return (int)(id & 0xff) - 1; }

/* resolve owner_id -> live entry, generation-checked; NULL = gone */
static struct gh_owner *gh_owner_by_id(u32 id)
{
	int s = gh_owner_slot_of(id);

	if (s < 0 || s >= GH_VM_OWNER_MAX)
		return NULL;
	if (gh_owner_id(s) != id || !GH_READ_ONCE(gh_owners[s].mm))
		return NULL;
	return &gh_owners[s];
}

/* lockless advisory for the serve entry filter (§4): may be stale for an
 * instant either way; the authoritative check runs in pool_avail_to_served
 * under pid_lock. Reads only published fields. */
static bool gh_pool_owner_maybe(void *mm)
{
	int i;

	for (i = 0; i < GH_VM_OWNER_MAX; i++)
		if (GH_READ_ONCE(gh_owners[i].mm) == mm &&
		    GH_READ_ONCE(gh_owners[i].vm_count) > 0)
			return true;
	return false;
}

/* lockless advisory: is this tgid a tracked owner? The pid-exit tracepoint
 * fires AFTER exit_mm, so task->mm is already NULL there -- an mm-based filter
 * (owner_maybe) would always miss on the exit path. Match by tgid instead;
 * mark_dead re-checks under the lock. (Tier-1 find: mm==NULL at sched_process_
 * exit meant death give-up never armed; mock can't see this — it calls
 * mark_dead directly, bypassing the tracepoint's mm timing.) */
static bool gh_owner_tgid_maybe(pid_t tgid)
{
	int i;

	for (i = 0; i < GH_VM_OWNER_MAX; i++)
		if (GH_READ_ONCE(gh_owners[i].mm) &&
		    GH_READ_ONCE(gh_owners[i].tgid) == tgid)
			return true;
	return false;
}

/* lockless advisory: is ANY owner tracked? The vendor's own Gunyah VMs
 * (trusted VM / OEM VM / TA VMs) hit the same destroy/reclaim symbols our
 * kprobes sit on; with no tracked owner there is nothing to release, and
 * arming the worker for vendor traffic only flaps the app's state display
 * (§4, Tier-2 find on SM8750). False negatives/positives are harmless —
 * the facades' owner apis re-verify under pid_lock. */
static bool gh_pool_owner_any(void)
{
	int i;

	for (i = 0; i < GH_VM_OWNER_MAX; i++)
		if (GH_READ_ONCE(gh_owners[i].mm))
			return true;
	return false;
}

static void gh_pool_owner_add(void *mm, pid_t tgid, const char *comm)
{
	int i, free_slot = -1;

	gh_write_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++) {
		if (gh_owners[i].mm == mm && !gh_owners[i].dead) {
			GH_WRITE_ONCE(gh_owners[i].vm_count, gh_owners[i].vm_count + 1);
			goto out;
		}
		if (!gh_owners[i].mm && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		struct gh_owner *o = &gh_owners[free_slot];

		GH_WRITE_ONCE(o->generation, gh_owner_next_gen++);
		o->tgid = tgid;
		gh_strscpy(o->comm, comm, GH_COMM_LEN);
		GH_WRITE_ONCE(o->vm_count, 1);
		o->dead = 0;
		o->died_at = 0;
		o->served = 0;
		o->abandoned = 0;
		gh_mmgrab(mm);
		GH_PUBLISH(o->mm, mm);		/* publish last */
	} else {
		gh_pr_warn("owner table full, %d untracked\n", (int)tgid);
		/* sweep only clears the dead; a full table of live owners is
		 * the documented cost of pid-lifetime entries (§2) */
	}
out:
	gh_write_unlock(&gh_pid_lock);
}

/* vm_shutdown attribution (§2): mm -> tgid (exit path: current IS the dying
 * owner) -> sole live owner -> give up (log; give-up waits for pid death). */
static void gh_pool_owner_vm_dec(void *mm, pid_t tgid)
{
	int i, hit = -1, live = 0, sole = -1;

	gh_write_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++) {
		if (!gh_owners[i].mm)
			continue;
		live++;
		sole = i;
		if (hit < 0 && mm && gh_owners[i].mm == mm)
			hit = i;
		if (hit < 0 && !mm && gh_owners[i].tgid == tgid && !gh_owners[i].dead)
			hit = i;
	}
	if (hit < 0 && !mm && live == 1)
		hit = sole;
	if (hit >= 0) {
		if (gh_owners[hit].vm_count > 0)
			GH_WRITE_ONCE(gh_owners[hit].vm_count,
				      gh_owners[hit].vm_count - 1);
			/* pure serve-gate maintenance: no stamp, no give-up (§2) */
	} else {
		gh_pr_info("unattributed VM destroy (owners=%d)\n", live);
	}
	gh_write_unlock(&gh_pid_lock);
}

static void gh_pool_owner_mark_dead(pid_t tgid)
{
	int i;

	gh_write_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++) {
		if (gh_owners[i].mm && gh_owners[i].tgid == tgid && !gh_owners[i].dead) {
			GH_WRITE_ONCE(gh_owners[i].died_at, gh_now_sec());
			GH_WRITE_ONCE(gh_owners[i].dead, 1);	/* dead published last */
		}
	}
	gh_write_unlock(&gh_pid_lock);
}

/* GC: only dead owners with no SERVED/RELEASED left (page counts fed by the
 * pool side); mmdrop outside the lock. Never touches live entries. */
static void gh_pool_owner_sweep(void)
{
	void *drop[GH_VM_OWNER_MAX];
	int i, ndrop = 0;

	gh_write_lock(&gh_pid_lock);
	for (i = 0; i < GH_VM_OWNER_MAX; i++) {
		struct gh_owner *o = &gh_owners[i];

		if (!o->mm)
			continue;
		if (!(o->dead || gh_mm_users(o->mm) == 0))
			continue;			/* alive: keep (UI) */
		if (o->served > 0)
			continue;			/* pages still tracked */
		drop[ndrop++] = o->mm;
		o->mm = NULL;				/* generation stays burned */
	}
	gh_write_unlock(&gh_pid_lock);
	for (i = 0; i < ndrop; i++)
		gh_mmdrop(drop[i]);
}

/* Lockless advisory: is this owner gone or dead (the give-up event, §2)?
 * Reads only single-copy-atomic published fields — safe to call WITHOUT
 * pid_lock, which is exactly why the release pending-count uses it: taking
 * pid_lock under pool_lock would invert serve's pid->pool order (ABBA). The
 * value is a hint; the release loop re-evaluates every round (§2/§6). */
static bool gh_owner_is_dead(u32 owner_id)
{
	int s = gh_owner_slot_of(owner_id);
	struct gh_owner *o;

	if (s < 0 || s >= GH_VM_OWNER_MAX)
		return true;
	o = &gh_owners[s];
	if (gh_owner_id(s) != owner_id || !GH_READ_ONCE(o->mm))
		return true;			/* generation gone */
	return GH_READ_ONCE(o->dead);
}

/* pool side reports a page leaving SERVED/RELEASED attribution */
static void gh_owner_served_dec(u32 owner_id)
{
	struct gh_owner *o = gh_owner_by_id(owner_id);

	if (o && o->served > 0)
		GH_CTR_ADD(o->served, -1);
}
