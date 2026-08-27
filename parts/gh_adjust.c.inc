/* gh_hugepage_reserve — adjust worker (§7): one run = one pipeline pass.
 * Phases run in fixed order to natural completion; the profile only gates
 * the acquire-stage intensities. Scan sets/cursors are pool state; ctx here
 * is flow cursors plus MAIN's health points; the CMA stages carry no hp — they
 * exit on satisfied-or-tried-all against the run's scan set. */

#define GH_ADJUST_RUN_MAX	120000		/* watchdog, not the loop (§11) */
#define GH_CHEAP_BATCH		64
#define GH_SETTLE_BATCH		64	/* = max intake/round; the round-tail flip
					 * must keep up with the fastest producer
					 * or avail transiently hoards the
					 * reserve's memory (§7) */
#define GH_FULL_BATCH		16
#define GH_SWEEP_SCAN		256
#define GH_PRECISE_BATCH	16
#define GH_CTA_BATCH		8
#define GH_FLIP_BATCH		8
#define GH_DROP_BATCH		8
#define GH_SHED_BATCH		32
#define GH_A_STRIDE		8

/* tunables (module params in kernel build, §10) */
static int gh_param_refill_enable = 1;		/* gates RELEASE's CHEAP+FULL */
static int gh_param_drop_slab = 1;
static int gh_param_mem_floor_mb = 512;

enum gh_phase { GH_PH_PREPARE, GH_PH_ACQUIRE, GH_PH_VERIFY_CMA, GH_PH_COLLECT_CMA,
		GH_PH_AVAIL_TO_CMA, GH_PH_CMA_FREE, GH_PH_AVAIL_FREE };
enum gh_astage { GH_AS_PRECISE, GH_AS_CHEAP, GH_AS_CTA, GH_AS_FULL, GH_AS_MAIN };
enum gh_approach { GH_APPROACH_CONTIG_ANY, GH_APPROACH_CONTIG_AT };
enum gh_cheap_lvl { GH_CHEAP_BLOCK, GH_CHEAP_SINGLE };

struct gh_adjust_ctx {
	int  run;
	enum gh_profile profile;
	int  want, want_cma;			/* normalized snapshot (§5.1) */
	enum gh_phase phase;
	int  acquire_stages;
	enum gh_astage astage;
	enum gh_approach approach;
	enum gh_evict_mode evict;
	int  main_hp, since_a;
	enum gh_cheap_lvl cheap_lvl;
	int  flipped_this_phase;
	int  did_prepare_cma;
};
static gh_spinlock_t gh_adjust_lock;
static struct gh_adjust_ctx gh_actx;
static int gh_adjust_g_active;
static enum gh_profile gh_next_profile;
static const char *gh_stop_reason = "idle";
static enum gh_approach gh_user_approach = GH_APPROACH_CONTIG_ANY;
static enum gh_evict_mode gh_user_evict = GH_EVICT_MEMCG;

static int gh_profile_stages(enum gh_profile p)
{
	switch (p) {
	case GH_PROFILE_SHRINK:  return GH_A_CTA;
	case GH_PROFILE_INSMOD:  return GH_A_CHEAP | GH_A_FULL;
	case GH_PROFILE_RELEASE: return GH_A_PRECISE | GH_A_CTA |
		(gh_param_refill_enable ? (GH_A_CHEAP | GH_A_FULL) : 0);
	case GH_PROFILE_USER:    return GH_A_PRECISE | GH_A_CHEAP | GH_A_CTA |
				       GH_A_FULL | GH_A_MAIN;
	default: return 0;
	}
}

static void gh_init_ctx(enum gh_profile p)
{
	struct gh_targets t = gh_pool_targets_snapshot(p == GH_PROFILE_RELEASE);

	memset(&gh_actx, 0, sizeof(gh_actx));
	gh_actx.profile = p;
	gh_actx.acquire_stages = gh_profile_stages(p);
	gh_actx.phase = GH_PH_PREPARE;
	gh_actx.astage = GH_AS_PRECISE;
	gh_actx.approach = (p == GH_PROFILE_USER) ? gh_user_approach : GH_APPROACH_CONTIG_ANY;
	gh_actx.evict = gh_user_evict;
	gh_actx.main_hp = 8;
	gh_actx.cheap_lvl = GH_CHEAP_BLOCK;
	gh_actx.want = t.want;
	gh_actx.want_cma = t.want_cma;
	/* PENDING params: only INSMOD/USER chase the configured total (they can
	 * verify); background folds to pure pool until VERIFIED (§5.1) */
	if (gh_pool_cma_params_state() == GH_CMA_PENDING &&
	    p != GH_PROFILE_INSMOD && p != GH_PROFILE_USER)
		gh_actx.want_cma = gh_actx.want;
}

static void gh_adjust_finish(void)
{
	unsigned long fl;

	gh_pool_cand_to_ext();			/* unfinished assemblies back to buddy */
	gh_pool_scan_finish();			/* run-local scan sets die with the run */
	gh_spin_lock(&gh_adjust_lock, &fl);
	if (!gh_adjust_g_active) {
		gh_spin_unlock(&gh_adjust_lock, &fl);
		return;				/* idempotent */
	}
	GH_WRITE_ONCE(gh_adjust_g_active, 0);
	gh_actx.run = 0;
	if (gh_next_profile) {			/* hand-off (§5.1) */
		enum gh_profile p = gh_next_profile;

		gh_next_profile = GH_PROFILE_NONE;
		gh_init_ctx(p);
		GH_WRITE_ONCE(gh_adjust_g_active, 1);
		gh_actx.run = GH_ADJUST_RUN_MAX;
		gh_sched_adjust(10);
	}
	gh_spin_unlock(&gh_adjust_lock, &fl);
}

static int gh_adjust_try(enum gh_profile profile)
{
	unsigned long fl;

	gh_spin_lock(&gh_adjust_lock, &fl);
	if (!GH_READ_ONCE(gh_adjust_g_active)) {
		gh_init_ctx(profile);
		GH_WRITE_ONCE(gh_adjust_g_active, 1);
		gh_actx.run = GH_ADJUST_RUN_MAX;
		gh_sched_adjust(10);
		gh_spin_unlock(&gh_adjust_lock, &fl);
		return 0;
	}
	if (gh_actx.profile == GH_PROFILE_USER) {	/* nobody preempts USER */
		gh_spin_unlock(&gh_adjust_lock, &fl);
		return profile == GH_PROFILE_USER ? -GH_EBUSY : 0;
	}
	gh_actx.run = 0;			/* interrupt background, hand off */
	if (profile > gh_next_profile)		/* USER > SHRINK > RELEASE > INSMOD */
		gh_next_profile = profile;
	gh_spin_unlock(&gh_adjust_lock, &fl);
	return 0;
}

static void gh_adjust_cancel(void)
{
	unsigned long fl;

	gh_spin_lock(&gh_adjust_lock, &fl);
	gh_actx.run = 0;
	gh_next_profile = GH_PROFILE_NONE;
	GH_WRITE_ONCE(gh_stop_reason, "stopped by user");
	gh_spin_unlock(&gh_adjust_lock, &fl);
}

static void gh_adjust_round(void)
{
	unsigned long fl;
	int stop, held, cma, diff_want, new_page_need, reserve_target, cma_excess;
	int desired_cma, cma_gap, stage_in_budget;
	struct gh_intake_budget budget;
	int ready_budget, single_budget;

	gh_spin_lock(&gh_adjust_lock, &fl);
	stop = (gh_actx.run == 0);
	if (!stop)
		gh_actx.run--;
	if (gh_actx.run == 0)
		stop = 1;
	gh_spin_unlock(&gh_adjust_lock, &fl);
	if (stop) {
		gh_adjust_finish();
		return;
	}

	gh_pool_released_to_ext(GH_EXPIRED);	/* every round: gated O(1) (§7) */

	held           = gh_pool_held();
	cma            = gh_pool_cma();
	diff_want      = gh_actx.want - held;
	new_page_need  = gh_actx.want_cma - (held + cma);
	reserve_target = gh_actx.want_cma - gh_actx.want;
	cma_excess     = cma - reserve_target;
	/* composition invariant (§7): cma's due level is avail's overflow —
	 * total feeds want first, spill parks in cma, capped at the reserve */
	desired_cma    = held + cma - gh_actx.want;
	if (desired_cma < 0)
		desired_cma = 0;
	if (desired_cma > reserve_target)
		desired_cma = reserve_target;
	cma_gap        = desired_cma - cma;	/* >0: flip in; <0: stage out */
	stage_in_budget = gh_actx.profile == GH_PROFILE_USER ?
			  (cma_gap < 0 ? -cma_gap : 0) :  /* avail-first (§7) */
			  (cma_excess > 0 ? cma_excess : 0); /* background: excess
							      * only — never locks
							      * lent memory away
							      * on its own */
	ready_budget   = new_page_need;
	single_budget  = (GH_S == 1) ? ready_budget :
			 (diff_want > 0 ? (diff_want < new_page_need ?
					   diff_want : new_page_need) : 0);

	switch (gh_actx.phase) {
	case GH_PH_PREPARE:
		if (diff_want == 0 && new_page_need == 0 && cma_excess <= 0 &&
		    !(gh_pool_cma_params_state() == GH_CMA_PENDING &&
		      (gh_actx.profile == GH_PROFILE_INSMOD ||
		       gh_actx.profile == GH_PROFILE_USER)) &&
		    (!(gh_actx.acquire_stages & GH_A_PRECISE) ||
		     gh_pool_released() == 0) &&	/* our pages await sweep: RELEASED
						 * counts in held, so the at-target
						 * check is blind to them (§7) */
		    gh_pool_noncma_able() == 0) {
			GH_WRITE_ONCE(gh_stop_reason, "already at target");
			gh_adjust_finish();
			return;
		}
		if (new_page_need > 0 && (gh_actx.acquire_stages & GH_A_MAIN) &&
		    gh_param_drop_slab && gh_kapi.cap(GH_CAP_DROP_SLAB)) {
			gh_kapi.drop_slab();
			/* freed slab is order-0 sitting on the pcp; without a
			 * drain it cannot merge up in buddy, so the windows
			 * drop_slab just opened stay invisible to the
			 * cheap/full/sweep stages that follow (§7) */
			if (gh_kapi.drain_pages)
				gh_kapi.drain_pages();
		}
		gh_pool_prepare_scan(gh_actx.acquire_stages);
		if (gh_pool_cma_capable() &&
		    (cma_excess > 0 || stage_in_budget > 0)) {
			gh_pool_prepare_cma_scan(0);
			gh_actx.did_prepare_cma = 1;
		}
		gh_actx.astage = GH_AS_PRECISE;
		gh_actx.phase = GH_PH_ACQUIRE;
		break;

	case GH_PH_ACQUIRE:
		switch (gh_actx.astage) {
		case GH_AS_PRECISE: {
			unsigned long pfn;
			int i;

			if (gh_pool_released() == 0 ||
			    !(gh_actx.acquire_stages & GH_A_PRECISE)) {
				gh_actx.astage = GH_AS_CHEAP;
				break;
			}
			/* batch: with no free hook this is the primary return
			 * channel — 1/round loses a 2GB teardown to the GRACE
			 * purge clock. Hook present: the list is a few
			 * hook-missed pages and the loop pops dry instantly. */
			for (i = 0; i < GH_PRECISE_BATCH; i++) {
				pfn = gh_pool_scan_pop(GH_SCAN_PRECISE, NULL);
				if (pfn == GH_PFN_NONE) {
					gh_actx.astage = GH_AS_CHEAP;
					break;
				}
				gh_pool_released_to_avail_sweep(pfn);
			}
			break;
		}
		case GH_AS_CHEAP: {
			int i;

			if (!(gh_actx.acquire_stages & GH_A_CHEAP)) {
				gh_actx.astage = GH_AS_CTA;
				break;
			}
			budget.single = single_budget;
			budget.ready = ready_budget;
			if (gh_actx.cheap_lvl == GH_CHEAP_BLOCK &&
			    budget.ready < (int)GH_S)
				gh_actx.cheap_lvl = GH_CHEAP_SINGLE;
			if ((gh_actx.cheap_lvl == GH_CHEAP_BLOCK ?
			     budget.ready : budget.single) <= 0) {
				gh_actx.astage = GH_AS_CTA;
				break;
			}
			for (i = 0; i < GH_CHEAP_BATCH; i++) {
				enum gh_how how = gh_actx.cheap_lvl == GH_CHEAP_BLOCK ?
						  GH_ALLOC_BLOCK : GH_ALLOC_LIGHT;

				if (!gh_pool_ext_to_avail(how, 0, &budget, 0)) {
					if (gh_actx.cheap_lvl == GH_CHEAP_BLOCK) {
						gh_actx.cheap_lvl = GH_CHEAP_SINGLE;
						continue;
					}
					gh_actx.astage = GH_AS_CTA;
					break;
				}
				if ((gh_actx.cheap_lvl == GH_CHEAP_BLOCK ?
				     budget.ready : budget.single) <= 0) {
					gh_actx.astage = GH_AS_CTA;
					break;
				}
			}
			break;
		}
		case GH_AS_CTA: {
			int ask;

			if (!(gh_actx.acquire_stages & GH_A_CTA) ||
			    !gh_pool_cma_capable() ||
			    diff_want <= 0 || stage_in_budget <= 0 ||
			    gh_pool_cma_scan_done()) {
				/* exit = satisfied or tried-all: every block
				 * gets exactly one contig attempt per run */
				gh_actx.astage = GH_AS_FULL;
				break;
			}
			ask = stage_in_budget / (int)GH_S;
			if (ask > GH_CTA_BATCH)	/* per-round work cap only */
				ask = GH_CTA_BATCH;
			if (ask < 1)
				ask = 1;
			gh_pool_cma_to_avail(ask);
			break;
		}
		case GH_AS_FULL: {
			int i;

			budget.single = single_budget;
			budget.ready = ready_budget;
			if (!(gh_actx.acquire_stages & GH_A_FULL) ||
			    budget.single <= 0) {
				gh_actx.astage = GH_AS_MAIN;
				break;
			}
			for (i = 0; i < GH_FULL_BATCH; i++) {
				if (!gh_pool_ext_to_avail(GH_ALLOC_FULL, 0, &budget, 0) ||
				    budget.single <= 0) {
					gh_actx.astage = GH_AS_MAIN;
					break;
				}
			}
			break;
		}
		case GH_AS_MAIN:
			if (!(gh_actx.acquire_stages & GH_A_MAIN) ||
			    new_page_need <= 0) {
				gh_actx.phase = GH_PH_VERIFY_CMA;
				break;
			}
			if (gh_actx.approach == GH_APPROACH_CONTIG_ANY) {
				budget.single = single_budget;
				budget.ready = ready_budget;
				if (budget.single <= 0) {
					gh_actx.phase = GH_PH_VERIFY_CMA;
					break;
				}
				if (gh_pool_ext_to_avail(GH_CONTIG_ANY, 0, &budget, 0)) {
					gh_actx.main_hp = gh_actx.main_hp + 3 > 8 ?
							  8 : gh_actx.main_hp + 3;
				} else if (--gh_actx.main_hp == 0) {
					gh_actx.phase = GH_PH_VERIFY_CMA;
					GH_WRITE_ONCE(gh_stop_reason, "migration exhausted");
				}
			} else {		/* CONTIG_AT: the whole-zone sweep */
				unsigned long pfn = GH_PFN_NONE, p;
				bool is_gap = false;
				int i;

				if (gh_kapi.mem_available_mb() < gh_param_mem_floor_mb) {
					gh_actx.phase = GH_PH_VERIFY_CMA;
					GH_WRITE_ONCE(gh_stop_reason, "low-memory floor");
					break;
				}
				for (i = 0; i < GH_SWEEP_SCAN; i++) {
					p = gh_pool_scan_pop(GH_SCAN_MAIN, &is_gap);
					if (p == GH_PFN_NONE) {
						gh_actx.phase = GH_PH_VERIFY_CMA;
						GH_WRITE_ONCE(gh_stop_reason,
						    gh_pool_cma() < reserve_target ?
						    "cma sources exhausted" :
						    "scanned all present memory");
						break;
					}
					if (!is_gap)
						gh_pool_set_cursor(p);
					if (gh_pool_window_candidate(p, GH_FILL_TOTAL)) {
						pfn = p;
						break;
					}
				}
				if (pfn == GH_PFN_NONE)
					break;	/* scanning only this round */
				budget.single = single_budget;
				budget.ready = ready_budget;
				if (gh_pool_ext_to_avail(GH_CONTIG_AT, pfn, &budget,
							 GH_INTAKE_NORETRY))
					break;	/* async got it: cheap first */
				if (gh_kapi.mem_available_mb() < gh_param_mem_floor_mb) {
					gh_actx.phase = GH_PH_VERIFY_CMA;
					GH_WRITE_ONCE(gh_stop_reason, "low-memory floor");
					break;
				}
				if (gh_actx.evict == GH_EVICT_ISOLATE ||
				    ++gh_actx.since_a >= GH_A_STRIDE) {
					if (gh_pool_evict(gh_actx.evict, pfn,
							  GH_FILL_TOTAL) &&
					    gh_actx.evict == GH_EVICT_MEMCG)
						gh_actx.since_a = 0;
				}
				gh_pool_ext_to_avail(GH_CONTIG_AT, pfn, &budget, 0);
			}
			break;
		}
		/* round tail: settle composition toward desired_cma at intake
		 * rate — the flip is label+free, no migration, so matching the
		 * fastest producer bounds the locked-in-avail transient to one
		 * round of intake (§7). Recomputed live: the job is settling
		 * THIS round's intake, which the round-start snapshot predates. */
		if (gh_pool_cma_capable() && gh_pool_avail_cma_able() > 0) {
			int lcma = gh_pool_cma();
			int lgap = gh_pool_held() + lcma - gh_actx.want;

			if (lgap > reserve_target)
				lgap = reserve_target;
			lgap -= lcma;		/* desired_cma - cma, live */
			if (lgap >= (int)GH_S) {
				int ask = lgap / (int)GH_S;

				if (ask > GH_SETTLE_BATCH)
					ask = GH_SETTLE_BATCH;
				if (!gh_kapi.cma_floor_ok(ask))
					ask = gh_kapi.cma_floor_ok(1) ? 1 : 0;
				if (ask)
					gh_actx.flipped_this_phase +=
						gh_pool_avail_to_cma(ask);
			}
		}
		break;

	case GH_PH_VERIFY_CMA:
		if (gh_pool_cma_params_state() == GH_CMA_PENDING &&
		    (gh_actx.profile == GH_PROFILE_INSMOD ||
		     gh_actx.profile == GH_PROFILE_USER)) {
			enum gh_verify_result v = gh_pool_verify_cma_params();

			if (v == GH_VERIFY_FAILED) {
				gh_actx.want_cma = gh_actx.want;
				GH_WRITE_ONCE(gh_stop_reason, "cma flip failed (systemic)");
			}
		}
		gh_actx.phase = GH_PH_COLLECT_CMA;
		break;

	case GH_PH_COLLECT_CMA: {
		unsigned long pfn;

		if (!(gh_actx.acquire_stages & GH_A_MAIN) || !gh_pool_cma_capable() ||
		    !gh_kapi.cap(GH_CAP_CONTIG_RANGE) || reserve_target == 0 ||
		    gh_pool_noncma_able() == 0 || gh_pool_scan_gap_n() == 0) {
			gh_pool_cand_to_ext();
			gh_actx.phase = GH_PH_AVAIL_TO_CMA;
			break;
		}
		if (gh_kapi.mem_available_mb() < gh_param_mem_floor_mb) {
			gh_pool_cand_to_ext();
			gh_actx.phase = GH_PH_AVAIL_TO_CMA;
			GH_WRITE_ONCE(gh_stop_reason, "low-memory floor");
			break;
		}
		pfn = gh_pool_scan_pop(GH_SCAN_GAP, NULL);
		if (pfn == GH_PFN_NONE) {
			gh_pool_cand_to_ext();
			gh_actx.phase = GH_PH_AVAIL_TO_CMA;
			break;
		}
		if (!gh_pool_window_candidate(pfn, GH_FILL_GAP))
			break;
		if (gh_actx.evict == GH_EVICT_ISOLATE ||
		    ++gh_actx.since_a >= GH_A_STRIDE) {
			if (gh_pool_evict(gh_actx.evict, pfn, GH_FILL_GAP) &&
			    gh_actx.evict == GH_EVICT_MEMCG)
				gh_actx.since_a = 0;
		}
		gh_pool_ext_to_cand(pfn, 0);
		break;
	}

	case GH_PH_AVAIL_TO_CMA: {
		int ask;

		/* cma_gap = min(-diff_want, reserve_target - cma): the old
		 * three-way clamp, one term. Mostly a leftovers pass now — the
		 * ACQUIRE round tail settles the bulk at intake rate. */
		if (!gh_pool_cma_capable() || cma_gap < (int)GH_S ||
		    gh_pool_avail_cma_able() == 0 || !gh_kapi.cma_floor_ok(1)) {
			if (gh_actx.flipped_this_phase && gh_kapi.drain_pages) {
				gh_kapi.drain_pages();
				/* drained; AVAIL_FREE's own flips re-arm it */
				gh_actx.flipped_this_phase = 0;
			}
			gh_actx.phase = GH_PH_CMA_FREE;
			break;
		}
		ask = cma_gap / (int)GH_S;
		if (ask > GH_FLIP_BATCH)
			ask = GH_FLIP_BATCH;
		gh_actx.flipped_this_phase += gh_pool_avail_to_cma(ask);
		break;
	}

	case GH_PH_CMA_FREE: {
		int ask;

		if (!gh_pool_cma_capable() || cma_excess <= 0) {
			gh_actx.phase = GH_PH_AVAIL_FREE;
			break;
		}
		/* yield to stage_in while it is still viable: budget left and
		 * either the scan is unstarted (excess appeared mid-run — keep
		 * it for the next run's CTA) or unfinished. A consumed scan is
		 * the "tried-all" verdict: no yield, and nothing to drop either
		 * — what remains failed the same contig_range once already. */
		if (diff_want > 0 && (gh_actx.acquire_stages & GH_A_CTA) &&
		    stage_in_budget > 0 &&
		    (!gh_actx.did_prepare_cma || !gh_pool_cma_scan_done())) {
			gh_actx.phase = GH_PH_AVAIL_FREE;
			break;
		}
		if (!gh_actx.did_prepare_cma) {		/* excess appeared mid-run */
			gh_pool_prepare_cma_scan(0);
			gh_actx.did_prepare_cma = 1;
		}
		if (gh_pool_cma_scan_done()) {
			gh_actx.phase = GH_PH_AVAIL_FREE;
			GH_WRITE_ONCE(gh_stop_reason, "cma sources stuck");
			break;
		}
		ask = cma_excess / (int)GH_S;
		if (ask > GH_DROP_BATCH)
			ask = GH_DROP_BATCH;
		if (ask < 1)
			ask = 1;
		gh_pool_cma_to_ext(ask);
		break;
	}

	case GH_PH_AVAIL_FREE: {
		const char *reason;

		if (diff_want < 0) {
			int n;

			/* self-balancing disposal (§7): flip what the floor
			 * allows, shed a batch otherwise. Only a shed raises
			 * noncma — a flip moves MemAvailable and CmaFree in
			 * lockstep — so each shed pays the floor deficit down
			 * and the flip gate opens itself: the run sheds about
			 * the deficit and flips the rest, instead of round-
			 * tripping the whole surplus through buddy for the
			 * next acquire to buy back. */
			if (gh_pool_cma_capable() && cma_gap >= (int)GH_S &&
			    gh_pool_avail_cma_able() > 0) {
				int ask = cma_gap / (int)GH_S;

				if (ask > GH_FLIP_BATCH)
					ask = GH_FLIP_BATCH;
				if (!gh_kapi.cma_floor_ok(ask))
					ask = gh_kapi.cma_floor_ok(1) ? 1 : 0;
				if (ask && (n = gh_pool_avail_to_cma(ask)) > 0) {
					gh_actx.flipped_this_phase += n;
					break;
				}
			}
			n = gh_pool_avail_to_ext(-diff_want > GH_SHED_BATCH ?
						 GH_SHED_BATCH : -diff_want);
			if (n > 0)
				break;
			/* avail empty, surplus is out on loan: next trigger sheds */
		}
		if (gh_actx.flipped_this_phase && gh_kapi.drain_pages)
			gh_kapi.drain_pages();	/* flips can land here too now;
						 * floor/GUI read NR_FREE_CMA */
		if (gh_actx.profile == GH_PROFILE_USER &&
		    (reason = GH_READ_ONCE(gh_stop_reason)) != NULL &&
		    strcmp(reason, "acquiring") == 0)
			GH_WRITE_ONCE(gh_stop_reason, diff_want <= 0 ?
				(gh_actx.want_cma > gh_actx.want ?
				 "reached target,with_cma" : "reached target") :
				"migration exhausted");
		gh_adjust_finish();
		return;
	}
	}

	if (gh_param_debug)
		gh_pool_check();
	gh_sched_adjust(10);
}

/* worker report accessors: consumers (sysfs / unlock_cma) read these instead
 * of the ctx statics — the worker's internals stay its own (§0.1). */
static int gh_adjust_running(void)
{
	return GH_READ_ONCE(gh_adjust_g_active);	/* ANY run in flight (any
							 * profile). One bit, three
							 * consumers: acquire_active
							 * display (§10), size-param
							 * -EBUSY gate (§4), unlock_cma
							 * lending quiescence (§9). */
}
static const char *gh_adjust_stop_reason_str(void)
{
	return GH_READ_ONCE(gh_stop_reason);	/* literals only; annotated for TSan */
}

/* sysfs glue: acquire mode -> approach/evict mapping (§10) */
static int gh_adjust_user_acquire(int mode)
{
	switch (mode) {
	case 1: gh_user_approach = GH_APPROACH_CONTIG_ANY; break;
	case 2: gh_user_approach = GH_APPROACH_CONTIG_AT;
		gh_user_evict = GH_EVICT_MEMCG; break;
	case 3: gh_user_approach = GH_APPROACH_CONTIG_AT;
		gh_user_evict = GH_EVICT_ISOLATE; break;
	default: return -1;
	}
	GH_WRITE_ONCE(gh_stop_reason, "acquiring");
	return gh_adjust_try(GH_PROFILE_USER);
}
