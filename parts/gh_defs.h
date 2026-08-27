/* gh_hugepage_reserve — shared definitions for every part (unity build).
 *
 * This header includes NOTHING. The environment (types, locks, page
 * primitives, allocator, clock) must be provided before any part is
 * included: by the kernel unity root (gh_hugepage_reserve.c, real
 * linux headers) or by tests/shim.h (mock memmap + deterministic fakes).
 * Parts may only touch pages through the primitive set listed in
 * POOL_DESIGN.md §3.3 (pfn_to_page / page_to_pfn / page_count / get_page /
 * put_page / set_page_count / __free_pages / prep_compound_page); folio
 * flag interpretation lives only in the kapi kernel backend.
 */

#define GH_PAGE_ORDER		9			/* 2MB serve unit */
#define GH_PAGES_PER_SLOT	(1UL << GH_PAGE_ORDER)
#define GH_SLOTS_PER_GB		512			/* 1GB chunk = 512 x 2MB */
#define GH_SPAN_GB_MAX		1024			/* top[] bound: 1TB pathological span */
#define GH_NIL			((u32)~0u)
#define GH_PFN_NONE		(~0UL)
#define GH_GRACE_SEC		10			/* released_at purge clock (RELEASED only) */
/* Give-up on served pages is EVENT-driven — pid death — plus a short grace so
 * the exit path's own teardown (delayed_fput, late unpins) can hand pages back
 * as a normal COLLECT instead of a give-up. The clock starts only at death and
 * is never cleared (death is one-way), so it cannot race a VM restart the way
 * the old idle_since clock did. The grace must end inside the round window the
 * pid-exit event itself arms (GH_RELEASE_ROUNDS x 1s), so the guaranteed
 * rounds scan it after expiry with no reliance on self-extension — hence
 * ROUNDS-2, and ROUNDS must stay >= 2 (compile-time constants: re-check this
 * pairing whenever either changes). */
#define GH_RELEASE_ROUNDS	5			/* rounds armed per VM event (1s each) */
#define GH_DEATH_GRACE_SEC	(GH_RELEASE_ROUNDS - 2)
_Static_assert(GH_RELEASE_ROUNDS >= 2, "death grace needs ROUNDS >= 2");
#define GH_VM_OWNER_MAX		8
#define GH_COMM_LEN		16

/* slot states — NOT_USEABLE must be 0: chunks are zero-allocated and only
 * cells fully inside a RAM range are promoted to EXT at init (§8). */
enum gh_state {
	GH_NOT_USEABLE = 0, GH_EXT, GH_AVAIL, GH_SERVED,
	GH_RELEASED, GH_CAND, GH_VERIFY, GH_CMA,
	GH_STATE_MAX,
};

/* intake origins (reclaim_debug in_* counters, §10) */
enum gh_origin { GH_IN_HOOK, GH_IN_SWEEP, GH_IN_CMA, GH_IN_USER, GH_IN_REFILL, GH_ORIGIN_MAX };

/* the three avail lists = block classification (§0.2) */
enum gh_class { GH_L_NON = 0, GH_L_ABLE, GH_L_READY, GH_L_MAX };

struct gh_slot {			/* 20B, fixed (§11) */
	u8  state;
	u8  origin;
	u32 owner_id;			/* SERVED/RELEASED: owner generation+index */
	u32 released_at;		/* RELEASED: seconds */
	u32 prev, next;			/* global slot index, GH_NIL; AVAIL only */
};

struct gh_slist { u32 head, tail; int n; };

/* pool_ext_to_avail modes (§3.2) */
enum gh_how { GH_ALLOC_BLOCK, GH_ALLOC_LIGHT, GH_ALLOC_FULL, GH_CONTIG_ANY, GH_CONTIG_AT };
#define GH_INTAKE_NORETRY	1		/* CONTIG_AT: async first pass */

struct gh_intake_budget { int single; int ready; };	/* 2MB pages, §7 */

enum gh_return_result { GH_RETURN_MISS, GH_RETURN_KEEP, GH_RETURN_DROP };
enum gh_exmode { GH_EXPIRED, GH_ALL };
enum gh_cma_params_state { GH_CMA_UNAVAILABLE, GH_CMA_PENDING, GH_CMA_VERIFIED };
enum gh_verify_result { GH_VERIFY_OK, GH_VERIFY_DEFERRED, GH_VERIFY_FAILED };
enum gh_window_purpose { GH_FILL_TOTAL, GH_FILL_GAP };
enum gh_evict_mode { GH_EVICT_MEMCG, GH_EVICT_ISOLATE };
enum gh_scan_which { GH_SCAN_PRECISE, GH_SCAN_MAIN, GH_SCAN_GAP };
enum gh_target { GH_TARGET_WANT, GH_TARGET_WANT_CMA };
enum gh_target_change { GH_TGT_SAME, GH_TGT_GROW, GH_TGT_SHRINK };

struct gh_targets { int want, want_cma; };

/* kapi feature ids for kapi.cap() (§3.3) */
enum gh_kapi_cap {
	GH_CAP_CONTIG_RANGE, GH_CAP_CONTIG_PAGES, GH_CAP_DROP_SLAB,
	GH_CAP_EVICT_ISOLATE, GH_CAP_SYS_RECLAIM, GH_CAP_DRAIN,
	GH_CAP_LRU_DRAIN, GH_CAP_PB_FLAGS, GH_CAP_MAX,
};

struct gh_ram_range { unsigned long start_pfn, end_pfn; };	/* half-open */

/* kapi backend contract (§3.3): logical adapters, version divergence and
 * symbol resolution live behind them (kernel backend); the mock backend
 * (tests/mock_kapi.c) drives the same table from a fake buddy. */
struct gh_kapi {
	bool (*cap)(enum gh_kapi_cap f);
	/* order-9 (or order-cma) allocation, compound; mode: 0=NORETRY cheap,
	 * 1=RETRY_MAYFAIL (full). NULL page = failure. */
	struct page *(*alloc_try)(unsigned int order, int strong);
	/* half-open pfn range primitives; pool normalizes tokens to ranges */
	bool (*candidate_range)(unsigned long start, unsigned long end);
	int  (*contig_range)(unsigned long start, unsigned long end, int noretry);
	int  (*contig_range_cma)(unsigned long start, unsigned long end);
	struct page *(*contig_pages)(unsigned long nr);
	void (*evict_range)(int mode, unsigned long start, unsigned long end);
	int  (*mem_available_mb)(void);
	bool (*cma_floor_ok)(int nblocks);
	unsigned long (*cma_free_pages)(void);
	void (*drop_slab)(void);
	void (*drain_pages)(void);
	void (*lru_add_drain_all)(void);
	unsigned long (*get_pfnblock_mt)(unsigned long pfn);
	void (*set_pageblock_mt)(unsigned long pfn, int mt);
	/* discovery + reporting adapters (§8/§10): page/zone/folio judgment
	 * lives here (§3.3), the pool/sysfs only get values back. */
	bool (*in_zone_movable)(unsigned long pfn);	/* ZONE_MOVABLE cell */
	int  (*walk_ram)(struct gh_ram_range *out, int max);	/* present RAM ranges */
	/* one CMA block's occupancy (pages): free/anon/file. racy report only. */
	void (*cma_occupancy)(unsigned long base, unsigned long nr,
			      unsigned long *free, unsigned long *anon,
			      unsigned long *file);
};

#define GH_ENOSYS 38
#define POOL_CMA_BASES_MAX 12288	/* reservoir bound = pool_size_max cap */

#define GH_CMA_ORDER_MAX 11			/* verify guard bound (§3.2) */

struct gh_pool_stats {				/* one consistent O(1) snapshot (§10) */
	int want, want_cma, size_max, total;
	int avail, served, released, verify, cma, ext;
	int avail_non, avail_able, avail_ready;
	enum gh_cma_params_state cma_params;
	long reject, orphan, gate_drop, purged;
	long origin[GH_ORIGIN_MAX];
};
struct gh_owner_row {
	pid_t tgid; char comm[GH_COMM_LEN];
	int vm_count, dead; long served, abandoned;
};
struct gh_owner_stats { int n; long live_served; struct gh_owner_row row[GH_VM_OWNER_MAX]; };

#define GH_PURGE_LOG_MAX 64
struct gh_purge_rec { unsigned long pfn; int count_at_purge; pid_t tgid; };
struct gh_purge_log { unsigned int total; int n; struct gh_purge_rec rec[GH_PURGE_LOG_MAX]; };

/* acquire stage authorization bits (§5.1) */
#define GH_A_PRECISE	1
#define GH_A_CHEAP	2
#define GH_A_CTA	4
#define GH_A_FULL	8
#define GH_A_MAIN	16

/* adjust profiles (§5.1); priority order for the hand-off */
enum gh_profile { GH_PROFILE_NONE = 0, GH_PROFILE_INSMOD, GH_PROFILE_RELEASE,
		  GH_PROFILE_SHRINK, GH_PROFILE_USER };
