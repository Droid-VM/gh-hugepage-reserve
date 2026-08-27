/* gh_hugepage_reserve race harness (TSan; POOL_DESIGN.md §2).
 *
 * The deterministic harness proves the state machine on scripted sequences;
 * THIS one attacks the §2 concurrency contract with real threads and real
 * (errorcheck) locks under ThreadSanitizer:
 *   R1  serve/return hot path vs release+adjust workers vs VM lifecycle churn
 *   R2  overlapping teardowns: shutdown storms from two owners mid-release
 *   R3  serve draining avail while a USER acquire run is filling it
 *   R4  the operator joins: resize/acquire/cancel/manual_release vs the storm
 * TSan flags data races; errorcheck mutexes abort on the "put_page under
 * pool_lock" class of discipline breaks; the final quiescent audit runs
 * pool_check + accounting invariants.
 */
#define GH_MOCK 1
#define GH_RACE 1
#include <unistd.h>
#include "shim.h"
#include "../parts/gh_defs.h"
#include "../parts/gh_owner.c.inc"
#include "../parts/gh_pool.c.inc"
#include "../parts/gh_release.c.inc"
#include "../parts/gh_adjust.c.inc"
#include "mock_kapi.c.inc"

static int mock_return_dispatch(struct page *p)
{
	return gh_pool_released_return(p) == GH_RETURN_KEEP;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, "-" __VA_ARGS__); fputc('\n', stderr); } } while (0)

static struct gh_ram_range world[] = {
	{ MOCK_BASE_PFN,               MOCK_BASE_PFN + (1UL << 18) },
	{ MOCK_BASE_PFN + (2UL << 18), MOCK_BASE_PFN + (3UL << 18) },
};
static struct mock_mm vm_a = { .mm_users = 2 }, vm_b = { .mm_users = 2 };
static int running;			/* __atomic; threads spin on it */
#define RUNNING() __atomic_load_n(&running, __ATOMIC_RELAXED)

static void world_reset(void)
{
	gh_pool_teardown_table();
	memset(gh_owners, 0, sizeof(gh_owners));
	memset(gh_nstate, 0, sizeof(gh_nstate));
	memset(mock_memmap, 0, sizeof(mock_memmap));
	gh_dbg_reject = gh_dbg_orphan = gh_dbg_gate_drop = gh_dbg_purged = 0;
	memset(gh_dbg_origin, 0, sizeof(gh_dbg_origin));
	gh_pool_total = 0; gh_released_oldest = 0; gh_purge_log_n = 0;
	gh_cma_params = GH_CMA_UNAVAILABLE; gh_cma_order = GH_PAGE_ORDER;
	gh_migrate_cma = -1; gh_cand_n = 0;
	__atomic_store_n(&mock_now_sec, 1000, __ATOMIC_RELAXED);
	mock_pcp_lag = 0; mock_pcp_n = 0; mock_freed_to_buddy = 0;
	mock_alloc_fail_after = -1; mock_pb_order = 9; mock_evicted = 0;
	memset(mock_mt, 1, sizeof(mock_mt));
	gh_adjust_g_active = 0; gh_next_profile = GH_PROFILE_NONE;
	memset(&gh_actx, 0, sizeof(gh_actx));
	gh_rel_run = gh_rel_round = 0; gh_stop_reason = "idle";
	gh_param_refill_enable = 1;
	mock_kapi_install();
	CHECK(gh_pool_init_table(world, 2, 4096) == 0);
	mock_fill_buddy(world[0].start_pfn, world[0].end_pfn);
	vm_a.mm_users = 2; vm_a.grabbed = 0;
	vm_b.mm_users = 2; vm_b.grabbed = 0;
}

/* ── thread bodies ─────────────────────────────────────── */
struct guest_arg { struct mock_mm *mm; pid_t tgid; long serves; };
static void *guest_thread(void *arg)
{
	struct guest_arg *g = arg;

	while (RUNNING()) {
		struct page *pg = gh_pool_avail_to_served(g->mm, g->tgid);

		if (pg) {
			g->serves++;
			/* guest "uses" the page, then unpins (its ref drops to
			 * ours-only); the release worker will collect it */
			set_page_count(pg, 1);
		}
	}
	return NULL;
}
static void *release_thread(void *arg)
{
	(void)arg;
	while (RUNNING()) {
		gh_release_round();
		__atomic_fetch_add(&mock_now_sec, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}
static void *adjust_thread(void *arg)
{
	(void)arg;
	while (RUNNING()) {
		if (__atomic_load_n(&gh_adjust_g_active, __ATOMIC_RELAXED))
			gh_adjust_round();
		else
			gh_adjust_try(GH_PROFILE_RELEASE);
	}
	return NULL;
}
/* the human at the sysfs: resize down and up, press acquire, cancel it,
 * poke manual_release -- all while guests serve and teardowns storm. The
 * sysfs UX gates (-EBUSY on USER runs) are POLICY, not protection: the pool
 * api's own in-lock revalidation must hold with no gate in front (§2/§4). */
static void *operator_thread(void *arg)
{
	enum gh_target_change ch;
	int i = 0;

	(void)arg;
	while (RUNNING()) {
		i++;
		switch (i % 8) {
		case 0:
			gh_pool_set_target(GH_TARGET_WANT, 24, &ch);
			gh_adjust_try(GH_PROFILE_SHRINK);
			break;
		case 2:
			gh_pool_set_target(GH_TARGET_WANT, 48, &ch);
			break;			/* grow only records (§4) */
		case 4:
			gh_adjust_user_acquire(1);
			break;
		case 6:
			gh_adjust_cancel();
			break;
		case 7:
			gh_release_run(1);	/* manual_release */
			break;
		}
	}
	return NULL;
}

struct event_arg { int with_b; int unshare; };
static void *event_thread(void *arg)
{
	struct event_arg *e = arg;
	int i = 0;

	while (RUNNING()) {
		i++;
		gh_release_vm_shutdown(&vm_a, 100);	/* teardown storm */
		if (e->unshare)
			gh_release_vm_unshare();
		gh_pool_owner_add(&vm_a, 100, "vm_a");	/* boot again (gate reopens) */
		if (e->with_b) {
			gh_release_vm_shutdown(&vm_b, 200);
			gh_pool_owner_add(&vm_b, 200, "vm_b");
			if (i % 7 == 0) {		/* occasional pid death + rebirth */
				gh_release_vm_exit(200);
				gh_pool_owner_sweep();
				gh_pool_owner_add(&vm_b, 200, "vm_b");
			}
		}
	}
	return NULL;
}

/* quiesce + audit: join done, drive workers single-threaded to a fixpoint */
static void audit(const char *phase)
{
	int i;

	for (i = 0; i < 40; i++) {
		gh_release_round();
		__atomic_fetch_add(&mock_now_sec, 1, __ATOMIC_RELAXED);
		while (GH_READ_ONCE(gh_adjust_g_active))
			gh_adjust_round();
		gh_adjust_try(GH_PROFILE_RELEASE);
		while (GH_READ_ONCE(gh_adjust_g_active))
			gh_adjust_round();
		if (!gh_release_active() && gh_pool_served() == 0 &&
		    gh_pool_released() == 0)
			break;
	}
	CHECK(gh_pool_check(), "%s: pool_check", phase);
	CHECK(gh_pool_served() == 0 && gh_pool_released() == 0,
	      "%s: all pages settled (served=%d released=%d)",
	      phase, gh_pool_served(), gh_pool_released());
	fprintf(stderr, "%s: avail=%d held=%d total=%d gate_drop=%ld purged=%ld reject=%ld\n",
		phase, gh_pool_avail(), gh_pool_held(), gh_pool_total,
		gh_dbg_gate_drop, gh_dbg_purged, gh_dbg_reject);
}

static void run_phase(const char *name, int nguests, struct event_arg *ev, int secs)
{
	pthread_t g[4], rel, adj, evt;
	struct guest_arg ga[4];
	int i;

	fprintf(stderr, "== %s ==\n", name);
	__atomic_store_n(&running, 1, __ATOMIC_RELAXED);
	for (i = 0; i < nguests; i++) {
		ga[i] = (struct guest_arg){ .mm = (i % 2 || !ev || !ev->with_b) ? &vm_a : &vm_b,
					    .tgid = (i % 2 || !ev || !ev->with_b) ? 100 : 200 };
		pthread_create(&g[i], NULL, guest_thread, &ga[i]);
	}
	pthread_create(&rel, NULL, release_thread, NULL);
	pthread_create(&adj, NULL, adjust_thread, NULL);
	if (ev)
		pthread_create(&evt, NULL, event_thread, ev);
	sleep(secs);
	__atomic_store_n(&running, 0, __ATOMIC_RELAXED);
	for (i = 0; i < nguests; i++)
		pthread_join(g[i], NULL);
	pthread_join(rel, NULL);
	pthread_join(adj, NULL);
	if (ev)
		pthread_join(evt, NULL);
	for (i = 0; i < nguests; i++)
		fprintf(stderr, "  guest%d serves=%ld\n", i, ga[i].serves);
	audit(name);
}

int main(void)
{
	enum gh_target_change ch;

	/* R1: hot path vs both workers vs single-owner lifecycle churn */
	world_reset();
	gh_pool_set_target(GH_TARGET_WANT, 32, &ch);
	gh_pool_owner_add(&vm_a, 100, "vm_a");
	gh_adjust_try(GH_PROFILE_INSMOD);
	while (GH_READ_ONCE(gh_adjust_g_active))
		gh_adjust_round();
	{
		struct event_arg ev = { .with_b = 0, .unshare = 1 };

		run_phase("R1 serve/return vs workers", 2, &ev, 3);
	}

	/* R2: overlapping teardowns from two owners mid-release */
	world_reset();
	gh_pool_set_target(GH_TARGET_WANT, 48, &ch);
	gh_pool_owner_add(&vm_a, 100, "vm_a");
	gh_pool_owner_add(&vm_b, 200, "vm_b");
	gh_adjust_try(GH_PROFILE_INSMOD);
	while (GH_READ_ONCE(gh_adjust_g_active))
		gh_adjust_round();
	{
		struct event_arg ev = { .with_b = 1, .unshare = 1 };

		run_phase("R2 overlapping teardowns", 4, &ev, 3);
	}

	/* R3: guests drain avail while a USER acquire run is filling it */
	world_reset();
	gh_pool_set_target(GH_TARGET_WANT, 64, &ch);
	gh_pool_owner_add(&vm_a, 100, "vm_a");
	{
		pthread_t g[2], rel;
		struct guest_arg ga[2] = { { .mm = &vm_a, .tgid = 100 },
					   { .mm = &vm_a, .tgid = 100 } };
		int i;

		__atomic_store_n(&running, 1, __ATOMIC_RELAXED);
		pthread_create(&g[0], NULL, guest_thread, &ga[0]);
		pthread_create(&g[1], NULL, guest_thread, &ga[1]);
		pthread_create(&rel, NULL, release_thread, NULL);
		CHECK(gh_adjust_user_acquire(1) == 0);
		for (i = 0; i < 2000000 && gh_adjust_g_active; i++)
			gh_adjust_round();	/* USER run races the guests */
		__atomic_store_n(&running, 0, __ATOMIC_RELAXED);
		pthread_join(g[0], NULL); pthread_join(g[1], NULL);
		pthread_join(rel, NULL);
		fprintf(stderr, "  guest serves=%ld+%ld\n", ga[0].serves, ga[1].serves);
		gh_release_vm_shutdown(&vm_a, 100);	/* live pages only settle once the VM is down */
		audit("R3 serve mid-acquire");
	}

	/* R4: the operator joins the storm */
	world_reset();
	gh_pool_set_target(GH_TARGET_WANT, 48, &ch);
	gh_pool_owner_add(&vm_a, 100, "vm_a");
	gh_pool_owner_add(&vm_b, 200, "vm_b");
	gh_adjust_try(GH_PROFILE_INSMOD);
	while (GH_READ_ONCE(gh_adjust_g_active))
		gh_adjust_round();
	{
		pthread_t g[4], rel, adj, evt, op;
		struct guest_arg ga[4];
		struct event_arg ev = { .with_b = 1, .unshare = 1 };
		int i;

		fprintf(stderr, "== R4 operator vs the storm ==\n");
		__atomic_store_n(&running, 1, __ATOMIC_RELAXED);
		for (i = 0; i < 4; i++) {
			ga[i] = (struct guest_arg){ .mm = i % 2 ? &vm_b : &vm_a,
						    .tgid = i % 2 ? 200 : 100 };
			pthread_create(&g[i], NULL, guest_thread, &ga[i]);
		}
		pthread_create(&rel, NULL, release_thread, NULL);
		pthread_create(&adj, NULL, adjust_thread, NULL);
		pthread_create(&evt, NULL, event_thread, &ev);
		pthread_create(&op, NULL, operator_thread, NULL);
		sleep(3);
		__atomic_store_n(&running, 0, __ATOMIC_RELAXED);
		for (i = 0; i < 4; i++)
			pthread_join(g[i], NULL);
		pthread_join(rel, NULL);
		pthread_join(adj, NULL);
		pthread_join(evt, NULL);
		pthread_join(op, NULL);
		for (i = 0; i < 4; i++)
			fprintf(stderr, "  guest%d serves=%ld\n", i, ga[i].serves);
		gh_adjust_cancel();
		gh_pool_set_target(GH_TARGET_WANT, 48, &ch);
		gh_release_vm_shutdown(&vm_a, 100);
		gh_release_vm_shutdown(&vm_b, 200);
		audit("R4 operator vs the storm");
	}

	if (failures) {
		fprintf(stderr, "%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("race harness: all phases passed\n");
	return 0;
}
