/* gh_hugepage_reserve — release worker (§6).
 * ctx is only {run, round}: owner state (vm_count / dead) lives in the pool
 * owner table. Giving up on a page is the EVENT consequence of pid death
 * (§2) — no clock, no grace on the owner side; the fixed 1s rounds are just
 * the carrier that executes it. Event facades below are all any hook calls. */

static int gh_adjust_try(enum gh_profile profile);	/* gh_adjust.c.inc */

static gh_spinlock_t gh_release_lock;
static int gh_rel_run, gh_rel_round;

/* GH_RELEASE_ROUNDS (gh_defs.h) rounds per VM event: a floor of guaranteed 1s
 * rounds — the run ends early once the release list drains, and self-extends
 * while any pinned page's owner is DEAD (§6). The death grace is ROUNDS-2, so
 * the guaranteed rounds themselves outlive it (§2). */

static int gh_release_active(void)
{
	unsigned long fl;
	int r;

	gh_spin_lock(&gh_release_lock, &fl);
	r = gh_rel_run;
	gh_spin_unlock(&gh_release_lock, &fl);
	return r;
}

static void gh_release_round(void)
{
	unsigned long fl;
	int pending, final, round;

	gh_spin_lock(&gh_release_lock, &fl);
	if (gh_rel_run == 0) {
		gh_spin_unlock(&gh_release_lock, &fl);
		return;
	}
	gh_rel_run--;
	round = ++gh_rel_round;			/* snapshot under the lock; the
						 * round==3 test below runs unlocked */
	gh_spin_unlock(&gh_release_lock, &fl);

	pending = gh_pool_served_to_released();
	gh_pool_served_to_ext(GH_EXPIRED);	/* give-up: event(death)+grace, any
						 * round may execute it (§6) */
	if (round == 3 && gh_kapi.drain_pages)
		gh_kapi.drain_pages();		/* pcp-lag: push parked frees at hook */

	if (gh_pool_released() == 0 && pending == 0) {	/* drained */
		gh_spin_lock(&gh_release_lock, &fl);
		gh_rel_run = 0;
		gh_spin_unlock(&gh_release_lock, &fl);
	}
	final = (gh_release_active() == 0);

	if (final) {
		if (gh_kapi.drain_pages)
			gh_kapi.drain_pages();
		gh_pool_served_to_released();	/* catch what the drain shook out */
		gh_pool_served_to_ext(GH_EXPIRED);
		gh_pool_owner_sweep();		/* GC dead owners with no pages (§2) */
	}
	gh_adjust_try(GH_PROFILE_RELEASE);

	gh_spin_lock(&gh_release_lock, &fl);
	if (gh_rel_run == 0 && pending > 0)
		gh_rel_run = 1;			/* self-extend: grace still running.
						 * bounded: EXPIRED fires within GRACE,
						 * RELEASED converges via purge */
	if (gh_rel_run > 0)
		gh_sched_release(1000);
	gh_spin_unlock(&gh_release_lock, &fl);
}

static void gh_release_run(int n)
{
	unsigned long fl;

	gh_spin_lock(&gh_release_lock, &fl);
	if (n == 0) {
		gh_rel_run = 0;			/* cancel (rmmod) */
	} else if (gh_rel_run == 0) {
		gh_rel_round = 0;
		gh_rel_run = n;
		gh_sched_release(1000);
	} else if (n > gh_rel_run) {
		gh_rel_run = n;			/* extend, never shorten */
	}
	gh_spin_unlock(&gh_release_lock, &fl);
}

/* ── event facades (§4/§6): hooks touch nothing else ──── */
static void gh_release_vm_shutdown(void *mm, pid_t tgid)
{
	gh_pool_owner_vm_dec(mm, tgid);		/* attribution; pure serve-gate -- (§2) */
	gh_release_run(GH_RELEASE_ROUNDS);
}

static void gh_release_vm_unshare(void)
{
	gh_release_run(GH_RELEASE_ROUNDS);	/* pure reclaim rounds */
}

static void gh_release_vm_exit(pid_t tgid)
{
	gh_pool_owner_mark_dead(tgid);		/* stamp + mark; sweep GCs later */
	gh_release_run(GH_RELEASE_ROUNDS);
}
