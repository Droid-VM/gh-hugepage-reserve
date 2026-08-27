#define GH_MOCK 1
/* gh_hugepage_reserve CI harness: unity-builds the pool parts over the shim
 * and replays deterministic scenarios (POOL_DESIGN.md §3.3 mock backend). */
#include "shim.h"
#include "../parts/gh_defs.h"
#include "../parts/gh_owner.c.inc"
#include "../parts/gh_pool.c.inc"
#include "../parts/gh_release.c.inc"
#include "../parts/gh_adjust.c.inc"
#include "mock_kapi.c.inc"

/* free-hook dispatch: shim's put_page routes order-9 zero-refcount frees here */
static int mock_return_dispatch(struct page *p)
{
	return gh_pool_released_return(p) == GH_RETURN_KEEP;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, "-" __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECKP() CHECK(gh_pool_check(), "pool_check")

/* world: RAM = [2GB,3GB) + [4GB,5GB) with a 1GB hole at [3GB,4GB) */
static struct gh_ram_range world[] = {
	{ MOCK_BASE_PFN,               MOCK_BASE_PFN + (1UL << 18) },
	{ MOCK_BASE_PFN + (2UL << 18), MOCK_BASE_PFN + (3UL << 18) },
};
static struct mock_mm crosvm = { .mm_users = 2 };
static struct mock_mm crosvm2 = { .mm_users = 2 };

static void world_reset(int cma_order)
{
	gh_pool_teardown_table();
	memset(gh_owners, 0, sizeof(gh_owners));
	memset(gh_nstate, 0, sizeof(gh_nstate));
	memset(mock_memmap, 0, sizeof(mock_memmap));
	gh_dbg_reject = gh_dbg_orphan = gh_dbg_gate_drop = gh_dbg_purged = 0;
	gh_pool_total = 0; gh_released_oldest = 0;
	gh_cma_params = GH_CMA_UNAVAILABLE;
	gh_cma_order = cma_order;
	mock_now_sec = 1000; mock_pcp_lag = 0; mock_freed_to_buddy = 0;
	mock_floor_mb = 0; mock_cma_floor_pass = 1;
	mock_free_hook_armed = 1;
	mock_alloc_fail_after = -1;
	mock_pb_order = 9; mock_evicted = 0; mock_pcp_n = 0;
	gh_adjust_g_active = 0; gh_next_profile = GH_PROFILE_NONE;
	memset(&gh_actx, 0, sizeof(gh_actx));
	gh_rel_run = gh_rel_round = 0; gh_stop_reason = "idle";
	gh_param_refill_enable = 1;
	memset(mock_mt, 1, sizeof(mock_mt));	/* all MOVABLE */
	gh_migrate_cma = -1; gh_cand_n = 0;
	mock_movable_lo = mock_movable_hi = 0;
	gh_purge_log_n = 0;
	mock_kapi_install();
	CHECK(gh_pool_init_table(world, 2, 4096) == 0);
	mock_fill_buddy(world[0].start_pfn, world[0].end_pfn);
	crosvm.mm_users = 2; crosvm.grabbed = 0;
	crosvm2.mm_users = 2; crosvm2.grabbed = 0;
}

static void t_addressing(void)
{
	world_reset(GH_PAGE_ORDER);
	CHECK(gh_nstate[GH_EXT] == 1024, "ext=%ld", gh_nstate[GH_EXT]);	/* 2 RAM GBs */
	CHECK(gh_slot_of_pfn(MOCK_BASE_PFN) != NULL);
	CHECK(gh_slot_of_pfn(MOCK_BASE_PFN + (1UL << 18)) == NULL, "hole GB has no chunk");
	CHECK(gh_slot_of_pfn(MOCK_BASE_PFN + 1) == NULL, "unaligned pfn rejected");
	gh_pool_mark_not_useable(MOCK_BASE_PFN, MOCK_BASE_PFN + 512 * 4);
	CHECK(gh_nstate[GH_EXT] == 1020, "carveout marked");
	CHECKP();
}

static void t_serve_return_s1(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	CHECK(ch == GH_TGT_GROW);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	CHECK(gh_pool_avail() == 4, "avail=%d", gh_pool_avail());	/* hard cap at want */
	CHECK(gh_pool_total == 4, "total proven=%d", gh_pool_total);
	CHECKP();

	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg == NULL, "no owner yet -> no serve");
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg && gh_pool_served() == 1 && gh_pool_avail() == 3);
	CHECK(page_count(pg) == 2, "guest ref + protection ref");
	CHECKP();

	/* guest unpins -> release round puts our ref -> hook KEEPs it back */
	set_page_count(pg, 1);
	CHECK(gh_pool_served_to_released() == 0, "no pending");
	CHECK(gh_pool_avail() == 4 && gh_pool_served() == 0 && gh_pool_released() == 0,
	      "returned via hook: avail=%d", gh_pool_avail());
	CHECKP();
}

static void t_shrink_gate_drop(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	gh_pool_set_target(GH_TARGET_WANT, 0, &ch);	/* soft-disable mid-lend */
	CHECK(ch == GH_TGT_SHRINK);
	CHECK(gh_pool_avail_to_ext(64) == 1, "shed the idle page");
	set_page_count(pg, 1);
	gh_pool_served_to_released();
	CHECK(gh_pool_held() == 0 && gh_dbg_gate_drop == 1,
	      "returning page DROPped after shrink (gate_drop=%ld)", gh_dbg_gate_drop);
	CHECK(pg->free_in_buddy, "page really left for buddy");
	CHECKP();
}

static void t_dead_owner_giveup(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg);

	/* VM closes but crosvm keeps the pin (stuck / slow teardown). Give-up is
	 * the EVENT consequence of pid death (§2): while crosvm LIVES the page is
	 * never taken from it, no matter how long -- vm_count is only the serve
	 * gate, there is no idle clock to race a VM restart. */
	gh_pool_owner_vm_dec(&crosvm, 1234);
	CHECK(gh_pool_served_to_released() == 0, "live owner: not pending");
	mock_now_sec += 5 * GH_GRACE_SEC;
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 0,
	      "live owner: never given up, however long");
	CHECK(gh_pool_served() == 1 && gh_owners[0].abandoned == 0 &&
	      page_count(pg) == 2, "page stays SERVED under the live pin");
	CHECKP();

	/* crosvm dies: THE give-up event -- but a short grace first, so the exit
	 * path's own frees can still come home as a collect (§2) */
	gh_pool_owner_mark_dead(1234);
	CHECK(gh_pool_served_to_released() == 1, "dead owner: pending now");
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 0, "death grace not over yet");
	mock_now_sec += GH_DEATH_GRACE_SEC;
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 1, "grace over: given up");
	CHECK(gh_owners[0].abandoned == 1 && gh_pool_served() == 0);
	CHECK(page_count(pg) == 1, "our ref gone, the stale pin remains");
	CHECKP();

	/* the pin drains with the corpse: EXT slot -> hook MISS -> buddy */
	set_page_count(pg, 1); pg->compound_order = 9;
	put_page(pg);
	CHECK(pg->free_in_buddy, "abandoned page drains to buddy, not the pool");

	crosvm.mm_users = 0;
	gh_pool_owner_sweep();
	CHECK(gh_owners[0].mm == NULL && crosvm.grabbed == 0, "swept after death");
	CHECKP();
}

/* Death grace splits the dead owner's pages by what the exit path managed:
 * pages whose refs came home inside the grace are COLLECTED (back to the
 * pool), only the still-pinned remainder is given up -- and the freed slot's
 * reuse gets a fresh generation, so stale owner_ids never bite the newcomer. */
static void t_death_grace_split(void)
{
	struct page *pa, *pb;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pa = gh_pool_avail_to_served(&crosvm, 1234);
	pb = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pa && pb);

	gh_pool_owner_mark_dead(1234);
	/* inside the grace, the exit path frees page A (ref drops to ours) */
	set_page_count(pa, 1);
	CHECK(gh_pool_served_to_released() == 1, "B still pinned = pending");
	CHECK(gh_pool_avail() == 1 && gh_pool_served() == 1,
	      "A came home as a COLLECT inside the grace");
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 0, "grace shields B too");

	mock_now_sec += GH_DEATH_GRACE_SEC;
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 1, "grace over: only B given up");
	CHECK(gh_owners[0].abandoned == 1,
	      "abandoned counts ONLY what death actually cost");
	CHECKP();

	/* slot reuse: sweep the corpse, register a new owner in its place */
	crosvm.mm_users = 0;
	gh_pool_owner_sweep();
	CHECK(gh_owners[0].mm == NULL, "corpse swept");
	gh_pool_owner_add(&crosvm2, 5678, "crosvm2");
	CHECK(gh_owners[0].mm == &crosvm2 && gh_owners[0].abandoned == 0,
	      "slot reused fresh (new generation, clean counters)");
	CHECK(gh_pool_avail_to_served(&crosvm2, 5678) != NULL,
	      "newcomer serves normally; stale owner_ids can't bite it");
	CHECKP();
}

/* u32 timestamp wrap: died_at and released_at comparisons must stay wrap-safe
 * ((u32)(now - then)), or a stamp near the wrap point stalls give-up/purge for
 * half a wrap period (§11 discipline note -- previously never exercised). */
static void t_timestamp_wrap(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	mock_now_sec = 0xFFFFFFFEu;		/* two ticks before the wrap */
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 1, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg);

	gh_pool_owner_mark_dead(1234);		/* died_at = 0xFFFFFFFE */
	mock_now_sec += 2;			/* now wrapped to 0 */
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 0,
	      "2s across the wrap: grace(3) not over");
	mock_now_sec += GH_DEATH_GRACE_SEC;	/* 5s since death, across wrap */
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 1, "wrap-safe give-up fired");
	CHECKP();

	/* released_at across the wrap: purge gate must fire too */
	world_reset(GH_PAGE_ORDER);
	mock_now_sec = 0xFFFFFFFFu;
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 1, &ch);
	b.single = 8; b.ready = 8;
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	mock_free_hook_armed = 0;		/* park it in RELEASED */
	set_page_count(pg, 1);
	gh_pool_served_to_released();		/* released_at = 0xFFFFFFFF */
	CHECK(gh_pool_released() == 1);
	mock_now_sec += GH_GRACE_SEC - 1;	/* wrapped, 9s elapsed */
	CHECK(gh_pool_released_to_ext(GH_EXPIRED) == 0, "9s: purge gate holds");
	mock_now_sec += 1;			/* 10s elapsed */
	CHECK(gh_pool_released_to_ext(GH_EXPIRED) == 1, "wrap-safe purge fired");
	CHECKP();
}

/* Two live owners: the mm-less destroy attribution (exit path, delayed_fput)
 * must NOT guess -- with several live owners and no tgid match it skips (the
 * gate stays up; pid death corrects everything later), never decrements the
 * wrong owner. */
static void t_two_owner_attribution(void)
{
	enum gh_target_change ch;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_owner_add(&crosvm2, 5678, "crosvm2");

	/* mm-less + tgid known (the dying owner's own fd release) */
	gh_pool_owner_vm_dec(NULL, 1234);
	CHECK(gh_owners[0].vm_count == 0 && gh_owners[1].vm_count == 1,
	      "tgid attribution hit the right owner only");

	/* mm-less + foreign tgid (kworker delayed_fput): two live owners ->
	 * ambiguous -> must skip, NOT guess */
	gh_pool_owner_vm_dec(NULL, 999);
	CHECK(gh_owners[0].vm_count == 0 && gh_owners[1].vm_count == 1,
	      "ambiguous mm-less destroy decremented nobody");

	/* with only ONE live-vm owner left, the sole-owner fallback may kick */
	gh_pool_owner_add(&crosvm, 1234, "crosvm");	/* back to 1 each */
	gh_pool_owner_vm_dec(&crosvm2, 5678);		/* mm-attributed: exact */
	CHECK(gh_owners[1].vm_count == 0, "mm attribution exact");

	/* the tgid prefilter the pid-exit path relies on (mm is NULL there) */
	CHECK(gh_owner_tgid_maybe(1234) && gh_owner_tgid_maybe(5678),
	      "tgid prefilter finds tracked owners");
	CHECK(!gh_owner_tgid_maybe(9999), "tgid prefilter rejects strangers");
	CHECKP();
}

static void t_vm_restart_serve_gate(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg);

	/* 1->0: vm_count is purely the serve gate -- serving stops, nothing is
	 * stamped, nothing will ever be given up off a live owner */
	gh_pool_owner_vm_dec(&crosvm, 1234);
	CHECK(gh_pool_avail_to_served(&crosvm, 1234) == NULL,
	      "vm_count==0 gates serve");
	mock_now_sec += 2 * GH_GRACE_SEC;
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 0, "no clock, no give-up");

	/* 0->1: the restart that used to race the idle clock -- now it simply
	 * reopens the gate; the old page was never touched */
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	CHECK(gh_pool_avail_to_served(&crosvm, 1234) != NULL, "gate reopens");
	CHECK(page_count(pg) == 2 && gh_owners[0].abandoned == 0,
	      "restart never cost the old page anything");
	CHECKP();
}

static void t_orphan_and_purge(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 1, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);

	/* pcp-lag: the free happened but the hook never saw it (refcount 0) */
	mock_pcp_lag = 1;
	set_page_count(pg, 1);
	put_page(pg);					/* swallowed: no hook */
	CHECK(page_count(pg) == 0 && gh_pool_served() == 1, "hook missed the free");
	gh_pool_served_to_released();
	CHECK(gh_dbg_orphan == 1 && gh_pool_released() == 1, "orphan detected");
	CHECK(gh_pool_released_to_ext(GH_EXPIRED) == 0, "purge gated by GRACE");
	mock_now_sec += GH_GRACE_SEC;
	CHECK(gh_pool_released_to_ext(GH_EXPIRED) == 1, "purged after grace");
	CHECK(gh_pool_held() == 0);
	CHECKP();
}

static void t_s2_block_classes(void)
{
	struct gh_intake_budget b = { .single = 64, .ready = 64 };
	enum gh_target_change ch;
	struct page *pg;

	world_reset(10);				/* S = 2 (sim_cma_order) */
	gh_cma_params = GH_CMA_PENDING;			/* alignment applies */
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 3, &ch);
	CHECK(gh_pool_want == 4, "want aligned up to S: %d", gh_pool_want);

	CHECK(gh_pool_ext_to_avail(GH_ALLOC_BLOCK, 0, &b, 0) == 2, "block intake = S");
	CHECK(gh_avail[GH_L_READY].n == 2 && b.ready == 62, "born ready");
	CHECK(b.single == 64, "ready block does not eat single budget (S>1)");
	CHECKP();

	pg = gh_pool_avail_to_served(&crosvm, 1234);	/* breaks the ready block */
	CHECK(pg && gh_avail[GH_L_ABLE].n == 1 && gh_avail[GH_L_READY].n == 0,
	      "sibling demoted ready->able");
	CHECKP();
	set_page_count(pg, 1);
	gh_pool_served_to_released();			/* comes home -> completes block */
	CHECK(gh_avail[GH_L_READY].n == 2 && gh_avail[GH_L_ABLE].n == 0,
	      "return re-promotes able->ready");
	CHECKP();

	/* shed from a ready block: whole block should leave together */
	CHECK(gh_pool_avail_to_ext(1) == 1);
	CHECK(gh_avail[GH_L_NON].n == 1 && gh_avail[GH_L_READY].n == 0,
	      "custody broken: sibling flooded to non");
	CHECK(gh_pool_avail_to_ext(8) == 1, "sibling goes next");
	CHECKP();
}

static void t_s2_single_budget(void)
{
	struct gh_intake_budget b = { .single = 0, .ready = 8 };

	world_reset(10);
	gh_cma_params = GH_CMA_PENDING;
	gh_pool_set_target(GH_TARGET_WANT, 2, NULL);
	/* non-ready singles with zero single budget must not pass */
	CHECK(gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0) == 0,
	      "S>1 single blocked by single budget");
	b.single = 2;
	CHECK(gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0) == 1);
	CHECK(b.single == 1 && b.ready == 7, "non-ready deducts both");
	CHECKP();
}

static void t_alloc_fail_sequence(void)
{
	struct gh_intake_budget b = { .single = 64, .ready = 64 };

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 8, NULL);
	mock_alloc_fail_after = 3;			/* scripted failure */
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	CHECK(gh_pool_avail() == 3, "partial intake: %d", gh_pool_avail());
	CHECK(gh_pool_total == 3, "proven follows actual");
	CHECKP();
}


static void t_verify_and_flip(void)
{
	struct gh_intake_budget b = { .single = 512, .ready = 512 };
	enum gh_target_change ch;
	unsigned long before;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);	/* preflight ok -> PENDING */
	CHECK(gh_pool_cma_params_state() == GH_CMA_PENDING);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 8, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_BLOCK, 0, &b, 0)) ;
	CHECK(gh_pool_held() == 8, "prefill to total: %d", gh_pool_held());

	CHECK(gh_pool_avail_to_cma(1) == 0, "no flip before VERIFIED");
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_OK);
	CHECK(gh_pool_cma_capable() && gh_pool_held() == 8, "window restored");
	CHECKP();

	before = mock_cma_free();
	CHECK(gh_pool_avail_to_cma(4) == 4, "flip 4 blocks");
	CHECK(gh_pool_cma() == 4 && gh_pool_held() == 4);
	CHECK(mock_cma_free() - before == 4 * 512, "CmaFree accounts the flip");
	CHECKP();

	/* stage_in: emptiest-first from the bucket scan */
	CHECK(gh_pool_prepare_cma_scan(0) == 4);
	CHECK(gh_pool_cma_to_avail(2) == 2, "staged back 2");
	CHECK(gh_pool_cma() == 2 && gh_pool_avail() == 6);
	CHECKP();

	/* drop: one block stuck (pinned squatter inside) stays CMA */
	{
		struct gh_pool_stats st;

		gh_pool_stats_snapshot(&st);
		CHECK(st.cma == 2 && st.avail_ready == 6, "snapshot coherent");
	}
	gh_pool_prepare_cma_scan(0);
	mock_plant(gh_pfn_of_idx(gh_scan_cma[0]) + 7, 0, 1, 0);	/* pin in 1st candidate */
	CHECK(gh_pool_cma_to_ext(8) == 1, "one dropped, one stuck");
	CHECK(gh_pool_cma() == 1, "stuck block keeps counting");
	CHECKP();
}

static void t_verify_off_by_one(void)
{
	struct gh_intake_budget b = { .single = 512, .ready = 512 };
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	mock_pb_order = 10;			/* REAL order 10; belief (below) = 9 */
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 8, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 16, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_BLOCK, 0, &b, 0)) ;
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_FAILED,
	      "boundary read catches the wrong belief");
	CHECK(gh_pool_cma_params_state() == GH_CMA_UNAVAILABLE);
	CHECK(gh_pool_targets_snapshot(0).want_cma == 8, "with_cma folded to pure pool");
	CHECK(gh_pool_held() == 16, "window fully restored");
	CHECKP();
	mock_pb_order = 9;
}

static void t_verify_deferred(void)
{
	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_DEFERRED, "no held window yet");
	CHECK(gh_pool_cma_params_state() == GH_CMA_PENDING, "still retryable");
}

static void t_scan_sets(void)
{
	struct gh_intake_budget b = { .single = 4, .ready = 4 };
	enum gh_target_change ch;
	unsigned long p, first;
	bool is_gap = false;

	world_reset(10);				/* S=2 for a real gap segment */
	gh_pool_set_cma_config(10, 4);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	/* hard page in the sibling window: auto-completion fails, gap persists */
	mock_plant(world[0].start_pfn + 512 + 3, 1, 0, 0);
	CHECK(gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0) == 1);
	CHECK(gh_pool_avail() == 1, "one single held, block partial");

	gh_pool_prepare_scan(GH_A_MAIN | GH_A_PRECISE);
	CHECK(gh_pool_scan_gap_n() == 1, "gap segment = the missing sibling");
	p = gh_pool_scan_pop(GH_SCAN_MAIN, &is_gap);
	CHECK(p != GH_PFN_NONE && is_gap, "gap first");
	first = gh_pool_scan_pop(GH_SCAN_MAIN, &is_gap);
	CHECK(first != GH_PFN_NONE && !is_gap, "then plain EXT");
	gh_pool_set_cursor(first);
	/* next run starts after the cursor */
	gh_pool_prepare_scan(GH_A_MAIN);
	(void)gh_pool_scan_pop(GH_SCAN_MAIN, &is_gap);	/* gap again */
	p = gh_pool_scan_pop(GH_SCAN_MAIN, &is_gap);
	CHECK(p == first + GH_PAGES_PER_SLOT, "sweep resumes after cursor: %lx", p);
	CHECKP();
}

static void t_window_gate(void)
{
	enum gh_target_change ch;
	unsigned long w = MOCK_BASE_PFN + 512 * 10;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	mock_plant(w + 3, 1, 0, 0);			/* hard straggler */
	CHECK(!gh_pool_window_candidate(w, GH_FILL_TOTAL), "hard blocks the window");
	mock_plant(w + 513, 0, 0, 1);			/* evictable squatter next window */
	CHECK(gh_pool_window_candidate(w + 512, GH_FILL_TOTAL), "evictable passes gate");
	/* async fails, evict clears, sync lands */
	CHECK(gh_pool_ext_to_avail(GH_CONTIG_AT, w + 512, NULL, GH_INTAKE_NORETRY) == 0);
	CHECK(gh_pool_evict(GH_EVICT_ISOLATE, w + 512, GH_FILL_TOTAL));
	CHECK(gh_pool_ext_to_avail(GH_CONTIG_AT, w + 512, NULL, 0) == 1);
	CHECK(gh_pool_avail() == 1);
	CHECKP();
}


/* deterministic worker drivers: one call = one scheduled round */
static int drive_adjust(int cap)
{
	int rounds = 0;

	while (gh_adjust_g_active && rounds < cap) {
		gh_adjust_round();
		rounds++;
	}
	CHECK(!gh_adjust_g_active, "adjust converged in %d rounds", rounds);
	return rounds;
}
static int drive_release(int cap)
{
	int rounds = 0;

	while (gh_release_active() && rounds < cap) {
		mock_now_sec++;				/* 1s cadence */
		gh_release_round();
		while (gh_adjust_g_active && rounds < cap)
			gh_adjust_round();		/* interleave triggered adjust */
		rounds++;
	}
	CHECK(!gh_release_active(), "release converged in %d rounds", rounds);
	return rounds;
}

static void t_insmod_run(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);	/* PENDING */
	gh_pool_set_target(GH_TARGET_WANT, 8, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 16, &ch);
	CHECK(gh_adjust_try(GH_PROFILE_INSMOD) == 0);
	drive_adjust(5000);
	CHECK(gh_pool_cma_capable(), "verify ran inside the INSMOD run");
	CHECK(gh_pool_held() == 8 && gh_pool_cma() == 8,
	      "pool+reservoir built in one run: held=%d cma=%d",
	      gh_pool_held(), gh_pool_cma());
	CHECKP();
}

static void t_release_cycle(void)
{
	enum gh_target_change ch;
	struct page *pg[4];
	int i;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	CHECK(gh_adjust_try(GH_PROFILE_INSMOD) == 0);
	drive_adjust(5000);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	for (i = 0; i < 4; i++) {
		pg[i] = gh_pool_avail_to_served(&crosvm, 1234);
		CHECK(pg[i]);
	}
	/* VM shuts down; guest frees land over the first rounds + one pcp-lag */
	mock_pcp_lag = 1;
	for (i = 0; i < 4; i++)
		set_page_count(pg[i], 1);
	put_page(pg[0]);				/* this one parks in pcp */
	gh_release_vm_shutdown(&crosvm, 1234);
	drive_release(20);
	CHECK(gh_pool_avail() == 4 && gh_pool_served() == 0 && gh_pool_released() == 0,
	      "all four home (incl. pcp-parked via drain): avail=%d served=%d",
	      gh_pool_avail(), gh_pool_served());
	CHECKP();
}

static void t_release_stuck_then_refill(void)
{
	enum gh_target_change ch;
	struct page *pg;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg);
	/* crosvm stuck: pin never drops. While crosvm LIVES nothing is given up
	 * (§2) -- the page stays SERVED, held keeps counting it, so no deficit
	 * opens and RELEASE buys no replacement: the pool simply runs short. */
	gh_release_vm_shutdown(&crosvm, 1234);
	drive_release(30);
	CHECK(gh_pool_served() == 1 && gh_pool_avail() == 3,
	      "live owner keeps the page; no give-up, no refill");
	CHECK(gh_owners[0].abandoned == 0, "nothing abandoned while alive");
	CHECKP();

	/* crosvm dies: the give-up event fires, the deficit opens, and the SAME
	 * event's release rounds refill the hole (RELEASE profile) */
	gh_release_vm_exit(1234);
	drive_release(30);
	CHECK(gh_pool_served() == 0, "death gave the stuck page up");
	CHECK(gh_pool_avail() == 4, "RELEASE refilled the hole: avail=%d",
	      gh_pool_avail());
	CHECK(gh_owners[0].abandoned == 1, "abandoned = pages held at death");
	CHECK(page_count(pg) == 1, "the stale pin remains, our ref is gone");
	CHECKP();
}

static void t_hookless_release(void)
{
	enum gh_target_change ch;
	struct page *pg[4];
	long sw0;
	int i;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	CHECK(gh_adjust_try(GH_PROFILE_INSMOD) == 0);
	drive_adjust(5000);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	for (i = 0; i < 4; i++) {
		pg[i] = gh_pool_avail_to_served(&crosvm, 1234);
		CHECK(pg[i]);
	}
	/* no free hook (temp-root kernel): the SAME flow must adapt with no
	 * mode flag — release marks+puts as always, the frees land in buddy
	 * uncaught, and the RELEASE-profile adjust sweeps them back by exact
	 * pfn; what it cannot sweep, purge would collect (§6). */
	mock_free_hook_armed = 0;
	sw0 = gh_dbg_origin[GH_IN_SWEEP];
	for (i = 0; i < 4; i++)
		set_page_count(pg[i], 1);		/* guest pins dropped */
	gh_release_vm_shutdown(&crosvm, 1234);
	drive_release(30);
	CHECK(gh_pool_avail() == 4 && gh_pool_served() == 0 &&
	      gh_pool_released() == 0,
	      "hookless: all four home: avail=%d served=%d released=%d",
	      gh_pool_avail(), gh_pool_served(), gh_pool_released());
	CHECK(gh_dbg_origin[GH_IN_SWEEP] - sw0 == 4,
	      "came back via AS_PRECISE sweep, not the hook (in_sweep +%ld)",
	      gh_dbg_origin[GH_IN_SWEEP] - sw0);
	CHECK(gh_dbg_purged == 0, "nothing had to be purged");
	CHECKP();
}

static void t_shrink_below_served(void)
{
	enum gh_target_change ch;
	struct page *pg[4];
	int i, in_buddy = 0;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 8, &ch);
	CHECK(gh_adjust_try(GH_PROFILE_INSMOD) == 0);
	drive_adjust(5000);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	for (i = 0; i < 4; i++) {
		pg[i] = gh_pool_avail_to_served(&crosvm, 1234);
		CHECK(pg[i]);
	}
	/* want drops BELOW served (2 < 4): the SHRINK run sheds what is idle,
	 * leaves the loaned pages alone (no special logic), and must converge
	 * instead of spinning on the unreachable remainder (§7 AVAIL_FREE exit) */
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	CHECK(ch == GH_TGT_SHRINK);
	gh_adjust_try(GH_PROFILE_SHRINK);
	drive_adjust(5000);
	CHECK(gh_pool_avail() == 0 && gh_pool_served() == 4,
	      "idle shed, loaned pages untouched: avail=%d served=%d",
	      gh_pool_avail(), gh_pool_served());

	/* the VM returns all four: per-page arbitration at the return gate —
	 * held>want drops the early ones to buddy, then held reaches want and
	 * the rest KEEP; the pool converges to exactly the new target with no
	 * extra logic (held: 4>2 DROP, 3>2 DROP, 2<=2 KEEP, KEEP) */
	for (i = 0; i < 4; i++)
		set_page_count(pg[i], 1);
	gh_pool_served_to_released();
	CHECK(gh_pool_avail() == 2 && gh_pool_served() == 0 &&
	      gh_pool_released() == 0,
	      "converged to new want: avail=%d", gh_pool_avail());
	CHECK(gh_dbg_gate_drop == 2, "exactly the surplus dropped (gate_drop=%ld)",
	      gh_dbg_gate_drop);
	for (i = 0; i < 4; i++)
		if (MOCK_LD(pg[i]->free_in_buddy))
			in_buddy++;
	CHECK(in_buddy == 2, "the two dropped pages really left for buddy");
	CHECKP();
}

static void t_rmmod_while_served(void)
{
	enum gh_target_change ch;
	struct page *pinned, *loose;
	int passes;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);	/* PENDING -> verify in run */
	gh_pool_set_target(GH_TARGET_WANT, 8, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 16, &ch);
	CHECK(gh_adjust_try(GH_PROFILE_INSMOD) == 0);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 8 && gh_pool_cma() == 8, "pool+reservoir up");
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	pinned = gh_pool_avail_to_served(&crosvm, 1234);	/* VM live, pinned */
	loose  = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pinned && loose);
	/* guest dropped one page but the free hook never brought it home:
	 * park it in RELEASED so the rmmod released-ALL edge has work too */
	mock_free_hook_armed = 0;			/* = hooks detached first (§8) */
	set_page_count(loose, 1);
	gh_pool_served_to_released();
	CHECK(gh_pool_served() == 1 && gh_pool_released() == 1,
	      "one pinned out on loan, one parked released");
	CHECK(page_count(pinned) == 2, "guest ref + our protection ref");

	/* the §8 rmmod sequence, verbatim order: hooks off (above), workers off,
	 * then hand every reference back — no edge may be skipped */
	gh_release_run(0);
	gh_adjust_cancel();
	gh_pool_served_to_ext(GH_ALL);
	CHECK(gh_pool_served() == 0, "served ALL cleared");
	CHECK(page_count(pinned) == 1,
	      "custody released: OUR ref handed back, holder's pin remains");
	gh_pool_released_to_ext(GH_ALL);
	CHECK(gh_pool_released() == 0, "released ALL cleared");
	for (passes = 0; passes < 3 && gh_pool_cma() > 0; passes++) {
		gh_pool_prepare_cma_scan(1);
		gh_pool_cma_to_ext(gh_pool_cma());
	}
	CHECK(gh_pool_cma() == 0, "reservoir dropped, label follows freelist");
	gh_pool_cand_to_ext();
	while (gh_pool_avail_to_ext(64) > 0)
		;
	CHECK(gh_pool_avail() == 0 && gh_pool_held() == 0, "custody fully empty");
	CHECK(gh_dbg_purged >= 1, "give-up accounted (purge counters)");

	/* long after rmmod the VM exits: its last ref frees to buddy with no
	 * hook in the way — nothing may intercept or double-free */
	put_page(pinned);
	CHECK(page_count(pinned) == 0 && MOCK_LD(pinned->free_in_buddy),
	      "holder's eventual free lands in buddy untouched");
	CHECKP();
}

static void t_user_sweep_fragmented(void)
{
	enum gh_target_change ch;
	unsigned long w;
	int i;

	world_reset(GH_PAGE_ORDER);
	/* fragment the world: hard straggler in every window except a few
	 * evictable-only ones; kill cheap intake by occupying all order-9 */
	for (i = 0; i < 512; i++) {
		w = world[0].start_pfn + (unsigned long)i * 512;
		if (i % 8 == 3)
			mock_plant(w + 17, 0, 0, 1);	/* evictable: assemblable */
		else
			mock_plant(w + 17, 1, 0, 0);	/* hard: never */
	}
	gh_pool_set_target(GH_TARGET_WANT, 16, &ch);
	CHECK(gh_adjust_user_acquire(3) == 0);		/* sweep + evict-B */
	drive_adjust(20000);
	CHECK(gh_pool_avail() == 16, "assembled from evictable windows: %d",
	      gh_pool_avail());
	CHECK(mock_evicted > 0, "evict actually ran");
	CHECKP();
}

static void t_shrink_run(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 8, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 4 && gh_pool_cma() == 4);
	/* user writes both down; with_cma=0 disables: everything drains */
	gh_pool_set_target(GH_TARGET_WANT_CMA, 0, &ch);
	CHECK(ch == GH_TGT_SHRINK);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_adjust_try(GH_PROFILE_SHRINK);
	drive_adjust(5000);
	CHECK(gh_pool_cma() == 0, "reservoir drained on disable");
	CHECK(gh_pool_held() == 2, "pool shed to new want: %d", gh_pool_held());
	CHECKP();
}

static void t_preempt_handoff(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 64, &ch);	/* big: run stays busy */
	gh_adjust_try(GH_PROFILE_INSMOD);		/* (RELEASE would fold to
							 * pool_total=0 — invariant P) */
	gh_adjust_round();				/* background run in flight */
	CHECK(gh_adjust_g_active && gh_actx.profile == GH_PROFILE_INSMOD);
	CHECK(gh_adjust_user_acquire(1) == 0, "USER preempts background");
	gh_adjust_round();				/* interrupted run finishes... */
	CHECK(gh_adjust_g_active && gh_actx.profile == GH_PROFILE_USER,
	      "...and hands off to USER");
	CHECK(gh_adjust_user_acquire(1) == -GH_EBUSY, "USER vs USER = busy");
	drive_adjust(5000);
	CHECK(gh_pool_held() == 64, "USER run reached target: %d", gh_pool_held());
	CHECKP();
}


static void t_emptiest_first(void)
{
	struct gh_intake_budget b = { .single = 512, .ready = 512 };
	enum gh_target_change ch;
	unsigned long fuller, emptier;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 8, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_BLOCK, 0, &b, 0)) ;
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_OK);
	CHECK(gh_pool_avail_to_cma(6) == 6);
	/* squat one whole block heavily, leave another empty */
	gh_pool_prepare_cma_scan(0);
	fuller = gh_pfn_of_idx(gh_scan_cma[0]);
	emptier = gh_pfn_of_idx(gh_scan_cma[1]);
	{ int i; for (i = 0; i < 400; i++) set_page_count(pfn_to_page(fuller + i), 1); }
	gh_pool_prepare_cma_scan(0);
	CHECK(gh_pfn_of_idx(gh_scan_cma[0]) == emptier,
	      "bucket scan pops emptiest block first");
	CHECK(gh_pool_cma_to_avail(1) == 1);
	/* the empty one should have been taken, not the fuller */
	CHECK(page_count(pfn_to_page(emptier)) != 0 ||
	      pfn_to_page(emptier)->free_in_buddy == 0, "emptier staged in");
	CHECKP();
}

static void t_grow_records_only(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 2);
	/* grow: records target, does NOT auto-fill (user must press acquire) */
	gh_pool_set_target(GH_TARGET_WANT, 8, &ch);
	CHECK(ch == GH_TGT_GROW);
	CHECK(gh_pool_held() == 2, "grow alone does not fill: %d", gh_pool_held());
	CHECK(gh_adjust_user_acquire(1) == 0);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 8, "acquire fills to new target");
	CHECKP();
}

static void t_coupling(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 10, &ch);
	/* write want above with_cma: with_cma follows up (W) */
	gh_pool_set_target(GH_TARGET_WANT, 12, &ch);
	CHECK(gh_pool_targets_snapshot(0).want_cma == 12, "with_cma pulled up");
	/* write with_cma below want: lifts to want, NEVER shrinks want */
	gh_pool_set_target(GH_TARGET_WANT_CMA, 5, &ch);
	CHECK(gh_pool_targets_snapshot(0).want == 12, "want not shrunk by low with_cma");
	CHECK(gh_pool_targets_snapshot(0).want_cma == 12, "with_cma lifted to want");
}

/* CMA quality (§1.1/§7 collect_cma): completing a block promotes it whole and
 * conservation-frees an equal count of straggler avail_non pages — one cma_able
 * block replaces the scattered non pages, held is conserved, Q improves. And a
 * SERVED page is never a victim: victims are popped from avail_non, and served
 * is not on any avail list, so "can't replace a served page" is structural, not
 * a special case. This is the single-flow natural result, not hand-coded. */
static void t_cma_quality(void)
{
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	unsigned long base = MOCK_BASE_PFN;
	unsigned long blk3g0 = base + 3 * 1024;		/* block3 cell0 -> served */
	unsigned long blk2g0 = base + 2 * 1024;		/* block2 cell0 -> straggler */
	unsigned long blk0g0 = base;			/* block0 cell0 -> completes */
	enum gh_target_change ch;
	struct page *served;
	long held_before;

	world_reset(10);				/* S=2 */
	gh_pool_set_cma_config(10, 4);
	gh_cma_params = GH_CMA_VERIFIED;		/* quality optimisation runs post-verify */
	gh_pool_set_target(GH_TARGET_WANT, 64, &ch);	/* large: no hard cap */
	gh_pool_owner_add(&crosvm, 1234, "crosvm");

	/* block3: cell1 is a hard straggler so sibling-fill can't complete it;
	 * cell0 becomes an avail_non page we then SERVE (leaves the non list) */
	pfn_to_page(blk3g0 + 512)->hard = 1;
	CHECK(gh_pool_ext_to_avail(GH_CONTIG_AT, blk3g0, &b, 0) == 1, "block3 cell0 in");
	served = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(served == pfn_to_page(blk3g0), "served the only non page (block3)");
	CHECK(gh_pool_served() == 1 && gh_pool_noncma_able() == 0, "served left non list");

	/* block2: cell1 hard -> cell0 is a permanent straggler avail_non (victim) */
	pfn_to_page(blk2g0 + 512)->hard = 1;
	CHECK(gh_pool_ext_to_avail(GH_CONTIG_AT, blk2g0, &b, 0) == 1, "block2 cell0 in");
	CHECK(gh_pool_noncma_able() == 1, "one straggler avail_non present");
	CHECK(gh_pool_avail_cma_able() == 0, "no ready block yet");

	held_before = gh_pool_held();

	/* block0 cell0 in (returns 1 for the placed cell); as a side effect
	 * gh_try_complete_block sibling-fills cell1, the block completes and
	 * promotes to ready, and promote conservation-frees ONE avail_non ->
	 * block2 cell0. Net held: +1 (cell0), and the promote is k-in/k-out
	 * (cell1 in, victim out) = conserved. */
	CHECK(gh_pool_ext_to_avail(GH_CONTIG_AT, blk0g0, &b, 0) == 1,
	      "block0 cell0 placed (sibling-fill completes it as a side effect)");
	CHECK(gh_pool_avail_cma_able() == 2, "block0 is now a ready cma block");
	CHECK(gh_pool_noncma_able() == 0, "straggler was conservation-freed (Q improved)");
	CHECK(pfn_to_page(blk2g0)->free_in_buddy, "the victim really went to buddy");
	CHECK(gh_pool_held() == held_before + 1,
	      "held +1 (new cell0); promote conserved cell1-in/victim-out");

	/* the served page was never touched — structurally unreachable as a victim */
	CHECK(gh_pool_served() == 1 && pfn_to_page(blk3g0) == served &&
	      gh_slot_of_pfn(blk3g0)->state == GH_SERVED,
	      "served page untouched: can't replace what isn't on the avail list");
	CHECKP();
}

static void t_s2_alignment(void)
{
	enum gh_target_change ch;

	world_reset(10);				/* S=2 */
	gh_pool_set_cma_config(10, 4);
	gh_pool_set_target(GH_TARGET_WANT, 5, &ch);	/* -> 6 */
	CHECK(gh_pool_targets_snapshot(0).want == 6, "want aligned up to S");
	gh_pool_set_target(GH_TARGET_WANT_CMA, 9, &ch);	/* -> 10 */
	CHECK(gh_pool_targets_snapshot(0).want_cma == 10, "with_cma aligned");
}

static void t_no_serve_when_vm_gone(void)
{
	struct page *pg;
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_owner_vm_dec(&crosvm, 1234);		/* vm_count -> 0, still alive */
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	CHECK(pg == NULL, "no serve to an owner with vm_count==0 (§3.1)");
}


static void t_carveouts(void)
{
	world_reset(GH_PAGE_ORDER);
	CHECK(gh_nstate[GH_EXT] == 1024);
	/* vendor CMA carveout: label 4 pageblocks CMA before we own anything */
	{ int i; for (i = 0; i < 4; i++) mock_set_mt(world[0].start_pfn + i * 512, 4); }
	/* ZONE_MOVABLE: 2 windows */
	mock_movable_lo = world[1].start_pfn;
	mock_movable_hi = world[1].start_pfn + 2 * 512;
	gh_pool_mark_carveouts(4);
	CHECK(gh_nstate[GH_EXT] == 1024 - 4 - 2, "carveouts removed: ext=%ld",
	      gh_nstate[GH_EXT]);
	CHECK(gh_nstate[GH_NOT_USEABLE] > 0);
	CHECKP();
	/* a carved window can never be served/acquired: it's not EXT */
	CHECK(gh_slot_of_pfn(world[0].start_pfn)->state == GH_NOT_USEABLE);
}

static void t_purge_log(void)
{
	struct page *pg;
	struct gh_intake_budget b = { .single = 8, .ready = 8 };
	enum gh_target_change ch;
	struct gh_purge_log log;

	world_reset(GH_PAGE_ORDER);
	gh_param_debug = 1;
	gh_pool_owner_add(&crosvm, 1234, "crosvm");
	gh_pool_set_target(GH_TARGET_WANT, 1, &ch);
	while (gh_pool_ext_to_avail(GH_ALLOC_LIGHT, 0, &b, 0)) ;
	pg = gh_pool_avail_to_served(&crosvm, 1234);
	gh_pool_owner_vm_dec(&crosvm, 1234);
	gh_pool_owner_mark_dead(1234);		/* give-up = death event + grace (§2) */
	mock_now_sec += GH_DEATH_GRACE_SEC;
	CHECK(gh_pool_served_to_ext(GH_EXPIRED) == 1);
	gh_pool_purge_log_snapshot(&log);
	CHECK(log.total == 1 && log.n == 1, "purge logged");
	CHECK(log.rec[0].pfn == page_to_pfn(pg), "right page");
	CHECK(log.rec[0].count_at_purge == 2, "refcount at purge (holder + our ref)");
	gh_param_debug = 0;
	CHECKP();
}

/* case 3 (§7 avail-first): avail AND the reserve both short. A USER run stages
 * the reserve into avail even below reserve_target — the shortfall parks in
 * cma, never in avail. A background run keeps the excess-only budget and does
 * not lock lent memory away on its own. */
static void t_avail_first_case3(void)
{
	enum gh_target_change ch;
	int i;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 10, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 2 && gh_pool_cma() == 8, "converged 2+8");

	/* open both deficits: drop half the reserve to ext, then kill ext
	 * intake so nothing can be bought back — the only source left is the
	 * parked reserve itself */
	gh_pool_prepare_cma_scan(0);
	CHECK(gh_pool_cma_to_ext(4) == 4);
	for (i = 0; i < 512; i++) {
		unsigned long w = world[0].start_pfn + (unsigned long)i * 512;
		struct gh_slot *s = gh_slot_of_pfn(w);

		if (s && s->state == GH_EXT)
			mock_plant(w + 17, 1, 0, 0);	/* hard: never intake */
	}
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	CHECK(ch == GH_TGT_GROW);
	/* now: held=2 < want=4, cma=4 < reserve_target=6 — case 3 */

	CHECK(gh_adjust_try(GH_PROFILE_RELEASE) == 0);
	drive_adjust(20000);
	CHECK(gh_pool_held() == 2 && gh_pool_cma() == 4,
	      "background leaves the parked reserve alone: %d+%d",
	      gh_pool_held(), gh_pool_cma());

	CHECK(gh_adjust_user_acquire(1) == 0);
	drive_adjust(20000);
	CHECK(gh_pool_avail() == 4, "USER staged the reserve into avail: %d",
	      gh_pool_avail());
	CHECK(gh_pool_cma() == 2, "shortfall parks in cma: %d", gh_pool_cma());
	CHECKP();
}

/* cold build of a large reserve (§7 settle): the round tail flips at intake
 * rate, so avail never hoards the reserve's memory between rounds — the
 * locked transient observable after any round stays under one intake batch. */
static void t_settle_rate(void)
{
	enum gh_target_change ch;
	int spike = 0, rounds = 0;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 4, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 4, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_OK);
	CHECK(gh_pool_cma_capable(), "verified");

	gh_pool_set_target(GH_TARGET_WANT_CMA, 36, &ch);	/* 32-slot reserve */
	CHECK(gh_adjust_user_acquire(1) == 0);
	while (gh_adjust_g_active && rounds < 20000) {
		int over;

		gh_adjust_round();
		rounds++;
		over = gh_pool_avail() - 4;
		if (over > spike)
			spike = over;
	}
	CHECK(!gh_adjust_g_active, "converged in %d rounds", rounds);
	CHECK(gh_pool_held() == 4 && gh_pool_cma() == 32, "final composition %d+%d",
	      gh_pool_held(), gh_pool_cma());
	CHECK(spike <= 2, "post-round locked transient bounded: %d", spike);
	CHECKP();
}

/* no-hp CTA (§7): exit = satisfied or tried-all. With every reserve block
 * pinned, one run gives each block exactly one contig attempt, the stuck
 * blocks keep counting as cma, and CMA_FREE's tried-all verdict reports
 * "cma sources stuck" instead of retry-hammering. */
static void t_cta_tried_all(void)
{
	enum gh_target_change ch;
	u32 i;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 6, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 2 && gh_pool_cma() == 4, "converged 2+4");

	gh_pool_prepare_cma_scan(0);		/* enumerate to pin each block */
	for (i = 0; i < gh_cma_total; i++)
		mock_plant(gh_pfn_of_idx(gh_scan_cma[i]) + 7, 0, 1, 0);
	gh_pool_scan_finish();

	gh_pool_set_target(GH_TARGET_WANT, 6, &ch);	/* reserve_target -> 0:
		 * held_cma is already at want_cma, so new_page_need==0 blocks
		 * every ext source — the pinned reserve is the only one left */
	CHECK(gh_adjust_user_acquire(1) == 0);
	{
		u32 seen_total = 0, seen_pos = 0;
		int rounds = 0;

		while (gh_adjust_g_active && rounds < 20000) {
			gh_adjust_round();
			rounds++;
			if (gh_cma_total > seen_total)
				seen_total = gh_cma_total;
			if (gh_cma_pos > seen_pos)
				seen_pos = gh_cma_pos;	/* finish() zeroes both */
		}
		CHECK(!gh_adjust_g_active, "converged in %d rounds", rounds);
		CHECK(seen_total == 4 && seen_pos == 4,
		      "scan consumed: each block tried exactly once (%u/%u)",
		      seen_pos, seen_total);
	}
	CHECK(gh_pool_cma() == 4, "stuck blocks keep counting: %d", gh_pool_cma());
	CHECK(gh_pool_avail() == 2, "nothing faked into avail: %d", gh_pool_avail());
	CHECK(strcmp(gh_stop_reason, "cma sources stuck") == 0,
	      "tried-all verdict: %s", gh_stop_reason);
	CHECKP();
}

/* floor-pressed shrink (§7 AVAIL_FREE self-balancing disposal): with noncma
 * under the floor, flips are blocked at first; each shed batch raises noncma,
 * the floor unblocks itself mid-run, and the remainder flips — the run sheds
 * about the deficit instead of the whole surplus. */
static void t_floor_self_balance(void)
{
	enum gh_target_change ch;

	world_reset(GH_PAGE_ORDER);
	gh_pool_set_cma_config(GH_PAGE_ORDER, 4);
	gh_pool_set_target(GH_TARGET_WANT, 66, &ch);
	gh_pool_set_target(GH_TARGET_WANT_CMA, 66, &ch);
	gh_adjust_try(GH_PROFILE_INSMOD);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 66 && gh_pool_cma() == 0, "converged 66+0");
	CHECK(gh_pool_verify_cma_params() == GH_VERIFY_OK);

	/* park the floor 40MB above current noncma: flips blocked until sheds
	 * pay the deficit; one SHED_BATCH (32 slots = 64MB) covers it */
	mock_floor_mb = (int)((unsigned long)mock_mem_avail_mb +
			      (mock_free_pages() - mock_cma_free()) / 256) + 40;

	gh_pool_set_target(GH_TARGET_WANT, 2, &ch);	/* reserve_target -> 64 */
	CHECK(ch == GH_TGT_SHRINK);
	gh_adjust_try(GH_PROFILE_SHRINK);
	drive_adjust(5000);
	CHECK(gh_pool_held() == 2, "surplus disposed: held=%d", gh_pool_held());
	CHECK(gh_pool_cma() == 32,
	      "shed paid the 40MB deficit (32 slots), rest flipped: cma=%d",
	      gh_pool_cma());
	CHECKP();
}

int main(void)
{
	fprintf(stderr, "== t_addressing\n");
	t_addressing();
	fprintf(stderr, "== t_serve_return_s1\n");
	t_serve_return_s1();
	fprintf(stderr, "== t_shrink_gate_drop\n");
	t_shrink_gate_drop();
	fprintf(stderr, "== t_dead_owner_giveup\n");
	t_dead_owner_giveup();
	fprintf(stderr, "== t_death_grace_split\n");
	t_death_grace_split();
	fprintf(stderr, "== t_timestamp_wrap\n");
	t_timestamp_wrap();
	fprintf(stderr, "== t_two_owner_attribution\n");
	t_two_owner_attribution();
	fprintf(stderr, "== t_vm_restart_serve_gate\n");
	t_vm_restart_serve_gate();
	fprintf(stderr, "== t_orphan_and_purge\n");
	t_orphan_and_purge();
	fprintf(stderr, "== t_s2_block_classes\n");
	t_s2_block_classes();
	fprintf(stderr, "== t_s2_single_budget\n");
	t_s2_single_budget();
	fprintf(stderr, "== t_alloc_fail_sequence\n");
	t_alloc_fail_sequence();
	fprintf(stderr, "== t_verify_and_flip\n");
	t_verify_and_flip();
	fprintf(stderr, "== t_verify_off_by_one\n");
	t_verify_off_by_one();
	fprintf(stderr, "== t_verify_deferred\n");
	t_verify_deferred();
	fprintf(stderr, "== t_scan_sets\n");
	t_scan_sets();
	fprintf(stderr, "== t_window_gate\n");
	t_window_gate();
	fprintf(stderr, "== t_insmod_run\n");
	t_insmod_run();
	fprintf(stderr, "== t_release_cycle\n");
	t_release_cycle();
	fprintf(stderr, "== t_release_stuck_then_refill\n");
	t_release_stuck_then_refill();
	fprintf(stderr, "== t_hookless_release\n");
	t_hookless_release();
	fprintf(stderr, "== t_shrink_below_served\n");
	t_shrink_below_served();
	fprintf(stderr, "== t_rmmod_while_served\n");
	t_rmmod_while_served();
	fprintf(stderr, "== t_user_sweep_fragmented\n");
	t_user_sweep_fragmented();
	fprintf(stderr, "== t_shrink_run\n");
	t_shrink_run();
	fprintf(stderr, "== t_preempt_handoff\n");
	t_preempt_handoff();
	fprintf(stderr, "== t_emptiest_first\n");
	t_emptiest_first();
	fprintf(stderr, "== t_grow_records_only\n");
	t_grow_records_only();
	fprintf(stderr, "== t_coupling\n");
	t_coupling();
	fprintf(stderr, "== t_cma_quality\n");
	t_cma_quality();
	fprintf(stderr, "== t_s2_alignment\n");
	t_s2_alignment();
	fprintf(stderr, "== t_no_serve_when_vm_gone\n");
	t_no_serve_when_vm_gone();
	fprintf(stderr, "== t_carveouts\n");
	t_carveouts();
	fprintf(stderr, "== t_purge_log\n");
	t_purge_log();
	fprintf(stderr, "== t_avail_first_case3\n");
	t_avail_first_case3();
	fprintf(stderr, "== t_settle_rate\n");
	t_settle_rate();
	fprintf(stderr, "== t_cta_tried_all\n");
	t_cta_tried_all();
	fprintf(stderr, "== t_floor_self_balance\n");
	t_floor_self_balance();
	if (failures) {
		fprintf(stderr, "%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("all scenarios passed\n");
	return 0;
}
