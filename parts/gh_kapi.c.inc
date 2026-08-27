/* gh_hugepage_reserve — kernel kapi backend (POOL_DESIGN.md §3.3).
 *
 * The ONE layer that touches kernel symbols. It fills the coarse `gh_kapi`
 * adapter table (parts/gh_defs.h) from the running kernel, hiding every
 * version difference (symbol names, signatures, argument/return SEMANTICS)
 * behind a `kraw` struct of version-TRUE prototypes plus normalizing shims.
 * A missing or ABI-guard-disabled symbol leaves the backing pointer NULL and
 * the matching gh_kapi.cap() returns false, so a whole feature is refused up
 * front rather than half-run through a mistyped pointer (a kCFI type-id
 * mismatch = panic).
 *
 * folio-flag interpretation lives ONLY here (candidate_range, evict_range) —
 * the pool object never reads a struct page beyond the §3.3 primitive set.
 *
 * ON-DEVICE VERIFIED ONLY: this part is compiled into the KERNEL unity build,
 * never the userspace mock harness. It has no local CI; correctness is proven
 * on the three target devices. Ported faithfully from the previous module
 * (old parts/gh_data.c.inc kapi/kraw/shims, gh_cma.c.inc resolve/kapi_init/
 * block_candidate/reclaim_block_b/reclaim_system_a/cma_free_pages/
 * cma_floor_ok/cma_set_block_mt). Toolchain discipline preserved: no
 * hweight8/strlen/snprintf libcalls on the hot lists.
 *
 * Assumes the unity root already included the real <linux/...> headers and defined
 * the preflight module params consumed below.
 */

/* ── preflight params (load.sh, §8/§10) ────────────────── */
/* MIGRATE_CMA enumerator value on the running kernel (config/vendor
 * dependent; read from /sys/kernel/btf by kapi_check). -1 = CMA off. */
static int migrate_cma_val = -1;
module_param(migrate_cma_val, int, 0400);
MODULE_PARM_DESC(migrate_cma_val,
	"MIGRATE_CMA value on the running kernel (kapi_check BTF preflight); -1 = CMA off");

/* pageblock_order of the running kernel — a pure macro, absent from
 * kallsyms/BTF; read from /proc/pagetypeinfo. -1 = CMA off. */
static int pageblock_order_val = -1;
module_param(pageblock_order_val, int, 0400);
MODULE_PARM_DESC(pageblock_order_val,
	"pageblock_order of the running kernel (/proc/pagetypeinfo preflight); -1 = CMA off");

/*
 * ABI guard: comma/space-separated list of logical kapi symbol names the
 * userspace preflight (abi/kapi_check via /sys/kernel/btf) found
 * signature-INCOMPATIBLE on THIS kernel. Each listed symbol resolves to NULL,
 * so the feature that needs it is refused instead of called through a mistyped
 * pointer. Empty (the normal case) trusts the compile-time version gates.
 */
static char *disable_kapi;
module_param(disable_kapi, charp, 0400);
MODULE_PARM_DESC(disable_kapi,
	"ABI guard: comma/space list of kapi symbols to leave unresolved (from kapi_check)");

/* Headroom floor (MB) for flipping pageblocks to CMA: refuse a flip once
 * available-minus-CMA would drop under this — the unmovable working set (~3.5G
 * on these devices) can't follow memory into CMA. Distinct from the sweep's
 * anti-livelock floor. Consumed only by gh_a_cma_floor_ok below, so it lives
 * here with its consumer (§9/§10; sysfs exposes it read-write as a module
 * param). */
static unsigned int cma_reservoir_floor_mb = 512;
module_param(cma_reservoir_floor_mb, uint, 0600);
MODULE_PARM_DESC(cma_reservoir_floor_mb,
	"Refuse to flip more pageblocks to CMA when non-CMA available memory would drop below this many MB (default 512)");

/* Zone the pool pages live in (cached at init); the CMA-free counters read off
 * its pgdat. Single-node phones: one pgdat is the whole story. */
static struct zone *gh_acq_zone;

/* ── raw kernel symbols behind the shims ───────────────── */
/* version-TRUE prototypes: kCFI matches the indirect call against the kernel
 * function's type id. Called ONLY from the kapi_*_shim functions below. */
static struct {
	struct page *(*k_alloc_contig_pages)(unsigned long nr_pages, gfp_t gfp_mask,
			int nid, nodemask_t *nodemask);
	int (*k_alloc_contig_range)(unsigned long start, unsigned long end,
			unsigned int migratetype_or_acr, gfp_t gfp_mask);
	void (*k_prep_compound_page)(struct page *page, unsigned int order);
	void (*k_lru_add_drain_all)(void);
	void (*k_drop_slab)(void);
	void (*k_drain_all_pages)(struct zone *zone);
	struct mem_cgroup *(*k_mem_cgroup_from_task)(struct task_struct *p);
	void (*k_set_pageblock_migratetype)(struct page *page, int migratetype);
	unsigned long (*k_get_pfnblock_flags_mask)(const struct page *page,
			unsigned long pfn, unsigned long mask);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	int (*k_folio_isolate_lru)(struct folio *folio);		/* 0 = isolated */
#else
	bool (*k_folio_isolate_lru)(struct folio *folio);		/* true = isolated */
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 12, 0)
	unsigned long (*k_reclaim_pages)(struct list_head *list, bool ignore_refs);
#else
	unsigned long (*k_reclaim_pages)(struct list_head *list);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, bool may_swap);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, unsigned int opts);
#else
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr, gfp_t gfp, unsigned int opts, int *swappiness);
#endif
} kraw;

/* Which coarse capabilities resolved (kept explicit so gh_kapi.cap() is O(1)
 * and never re-touches the raw pointers). */
static bool gh_cap[GH_CAP_MAX];

/* ── symbol resolution (throwaway kprobe; ABI guard) ───── */
static void *gh_resolve_kfunc(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	void *addr = NULL;

	if (register_kprobe(&kp) == 0) {
		addr = (void *)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

/* first name in a NULL-terminated list that exists (the *_noprof rename). */
static void *gh_resolve_first(const char * const *names)
{
	void *addr = NULL;
	int i;

	for (i = 0; names[i] && !addr; i++)
		addr = gh_resolve_kfunc(names[i]);
	return addr;
}

/* whole-token (not substring) match of @name against disable_kapi. */
static bool gh_sym_disabled(const char *name)
{
	const char *p = disable_kapi;
	size_t nlen = strlen(name);

	if (!p)
		return false;
	while (*p) {
		size_t tlen;

		p += strspn(p, ", \t");		/* skip separators */
		tlen = strcspn(p, ", \t");	/* length of this token */
		if (tlen == nlen && !strncmp(p, name, nlen))
			return true;
		p += tlen;
	}
	return false;
}

static void *gh_resolve_gated(const char *name)
{
	if (gh_sym_disabled(name)) {
		pr_info("kapi: %s left NULL (disable_kapi ABI guard)\n", name);
		return NULL;
	}
	return gh_resolve_kfunc(name);
}

/* Warn on unknown disable_kapi tokens (typo guard). Known set = the generated
 * KAPI_SYMBOLS list (abi/kapi_abi.gen.h), kept in lockstep with the ABI table.
 * Whole-token match without strlen: a matched strncmp implies known[i] has
 * >= tlen chars, so [tlen] is a valid read (the NUL iff lengths match too) —
 * strlen(known[i]) would become an out-of-line libcall the ABI check forbids. */
static void gh_kapi_validate_disable(void)
{
	static const char * const known[] = {
#define X(n) #n,
		KAPI_SYMBOLS(X)
#undef X
	};
	const char *p = disable_kapi;

	if (!p)
		return;
	while (*p) {
		size_t tlen, i;
		bool hit = false;

		p += strspn(p, ", \t");
		tlen = strcspn(p, ", \t");
		if (!tlen)
			break;
		for (i = 0; i < ARRAY_SIZE(known); i++)
			if (!strncmp(p, known[i], tlen) && known[i][tlen] == '\0') {
				hit = true;
				break;
			}
		if (!hit)
			pr_warn("kapi: disable_kapi token \"%.*s\" not a known symbol\n",
				(int)tlen, p);
		p += tlen;
	}
}

/* ── version-normalizing shims ─────────────────────────── */
/* alloc_contig_range MOVABLE-mode. arg 3 changed meaning across bases:
 * migratetype (<=6.12: MIGRATE_MOVABLE) became acr_flags_t (>=6.16:
 * ACR_FLAGS_NONE=0); MIGRATE_MOVABLE=1 would alias ACR_FLAGS_CMA and silently
 * run in CMA isolation mode, so the shim owns arg 3 and callers never pass it. */
static int gh_acr_movable(unsigned long start, unsigned long end, gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 0 /* ACR_FLAGS_NONE */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end, MIGRATE_MOVABLE, gfp_mask);
#endif
}

/* CMA-mode grab: arg 3 is the runtime MIGRATE_CMA value on <=6.12 (build-time
 * MIGRATE_CMA may not match the device kernel — the preflight supplies it),
 * ACR_FLAGS_CMA on the >=6.16 contract (dormant: whole feature gates off there). */
static int gh_acr_cma(unsigned long start, unsigned long end, gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 1 /* ACR_FLAGS_CMA */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end,
					 (unsigned int)READ_ONCE(migrate_cma_val),
					 gfp_mask);
#endif
}

/* folio_isolate_lru: 6.1/6.2 return int (0 = isolated); 6.3+ return bool
 * (true = isolated). Normalize to bool true=isolated. */
static bool gh_folio_isolate(struct folio *folio)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	return kraw.k_folio_isolate_lru(folio) == 0;
#else
	return kraw.k_folio_isolate_lru(folio);
#endif
}

/* reclaim_pages: 6.6..6.12 take (list, ignore_refs); others (list). */
static unsigned long gh_reclaim_pages(struct list_head *list)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 12, 0)
	return kraw.k_reclaim_pages(list, true);
#else
	return kraw.k_reclaim_pages(list);
#endif
}

/* try_to_free_mem_cgroup_pages: signature drifts across bases. GFP_KERNEL,
 * swap allowed. */
static unsigned long gh_try_to_free_memcg(struct mem_cgroup *memcg, unsigned long nr)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL, true);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL,
						   MEMCG_RECLAIM_MAY_SWAP);
#else
	return kraw.k_try_to_free_mem_cgroup_pages(memcg, nr, GFP_KERNEL,
						   MEMCG_RECLAIM_MAY_SWAP, NULL);
#endif
}

/* ── coarse adapters (the gh_kapi table entries) ───────── */
static bool gh_a_cap(enum gh_kapi_cap f)
{
	return (unsigned)f < GH_CAP_MAX && gh_cap[f];
}

/* order-9 (or order-cma) compound allocation. GFP rule §3: GFP_KERNEL system,
 * NEVER movable/__GFP_CMA — an unmovable request can't be served from a CMA
 * freelist, and served pages must survive FOLL_LONGTERM pin. */
static struct page *gh_a_alloc_try(unsigned int order, int strong)
{
	gfp_t gfp = GFP_KERNEL | __GFP_COMP | __GFP_NOWARN |
		    (strong ? __GFP_RETRY_MAYFAIL : __GFP_NORETRY);

	return alloc_pages(gfp, order);
}

static unsigned long gh_a_get_pfnblock_mt(unsigned long pfn);	/* defined below */

/* feasibility gate (old block_candidate) over a raw half-open pfn range: every
 * page free, or on the LRU / __PageMovable; none reserved, longterm-pinned, or
 * an unmovable straggler; pageblock not CMA / ISOLATE / HIGHATOMIC. The one
 * place folio flags are read. Pure read, any context; REQUIRES a prior
 * lru_add_drain_all() for accurate PageLRU (the caller does it once per run). */
static bool gh_a_candidate_range(unsigned long start, unsigned long end)
{
	unsigned long i;
	int mt;

	if (!pfn_valid(start))
		return false;
	mt = (int)gh_a_get_pfnblock_mt(start);	/* resolved reader, not the
						 * unexported get_pfnblock macro */
	if (is_migrate_cma(mt) || mt == MIGRATE_HIGHATOMIC
#ifdef CONFIG_MEMORY_ISOLATION
	    || mt == MIGRATE_ISOLATE
#endif
	   )
		return false;			/* CMA/isolated/atomic: never touch */
	for (i = start; i < end; i++) {
		struct page *p, *head;

		if (!pfn_valid(i))
			return false;		/* hole: can't assemble across it */
		p = pfn_to_page(i);
		head = compound_head(p);
		if (PageReserved(head))
			return false;		/* bootmem/firmware */
		if (page_count(head) == 0)
			continue;		/* free: buddy / pcp / being freed */
		if (!PageLRU(head) && !__PageMovable(head))
			return false;		/* in-use + not LRU/movable = straggler */
		if (
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
		    folio_maybe_dma_pinned(page_folio(head)))
#else
		    page_maybe_dma_pinned(head))
#endif
			return false;		/* longterm-pinned (e.g. a live VM) */
	}
	return true;
}

static int gh_a_contig_range(unsigned long start, unsigned long end, int noretry)
{
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN | (noretry ? __GFP_NORETRY : 0);

	if (!kraw.k_alloc_contig_range)
		return -ENOSYS;
	return gh_acr_movable(start, end, gfp);
}

static int gh_a_contig_range_cma(unsigned long start, unsigned long end)
{
	if (!kraw.k_alloc_contig_range || READ_ONCE(migrate_cma_val) < 0)
		return -ENOSYS;
	return gh_acr_cma(start, end, GFP_KERNEL | __GFP_NOWARN);
}

static struct page *gh_a_contig_pages(unsigned long nr)
{
	if (!kraw.k_alloc_contig_pages)
		return NULL;
	return kraw.k_alloc_contig_pages(nr, GFP_KERNEL | __GFP_NOWARN,
					 numa_node_id(), NULL);
}

/* GH_EVICT_ISOLATE (old reclaim_block_b): evict the window's OWN folios to
 * zram so it frees in place — folio_try_get -> folio_isolate_lru -> list_add
 * -> folio_put, then reclaim_pages drops the isolation refs. GH_EVICT_MEMCG
 * (old reclaim_system_a): a bounded system-wide reclaim to make migration
 * destinations elsewhere; the caller's SYNC contig then migrates into them.
 * Both bounded, both no-op when their symbols are absent. */
static void gh_a_evict_range(int mode, unsigned long start, unsigned long end)
{
	if (mode == GH_EVICT_ISOLATE) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
		unsigned long pfn = start;
		LIST_HEAD(folios);

		if (!kraw.k_folio_isolate_lru || !kraw.k_reclaim_pages)
			return;
		while (pfn < end) {
			struct folio *folio;
			struct page *page;
			unsigned long fnr;

			if (!pfn_valid(pfn)) {
				pfn++;
				continue;
			}
			page = pfn_to_page(pfn);
			if (PageBuddy(page) || PageReserved(page)) {
				pfn++;
				continue;
			}
			folio = page_folio(page);
			fnr = folio_nr_pages(folio);
			if (!folio_test_lru(folio) || folio_test_unevictable(folio) ||
			    !folio_try_get(folio)) {
				pfn += fnr ? fnr : 1;
				continue;
			}
			fnr = folio_nr_pages(folio);	/* re-read under our ref */
			if (gh_folio_isolate(folio))
				list_add(&folio->lru, &folios);
			folio_put(folio);
			pfn += fnr ? fnr : 1;
			if ((pfn & 0x3f) == 0)
				cond_resched();
		}
		if (!list_empty(&folios))
			gh_reclaim_pages(&folios);	/* pages out; drops isolation refs */
#endif
		return;
	}
	/* GH_EVICT_MEMCG: strided system-wide reclaim. The pool caps cadence
	 * (§7 since_a); nr scaled to the window. */
	if (kraw.k_try_to_free_mem_cgroup_pages && kraw.k_mem_cgroup_from_task) {
		struct mem_cgroup *memcg = kraw.k_mem_cgroup_from_task(current);

		if (memcg)
			gh_try_to_free_memcg(memcg, (end - start) * 8);
	}
}

static int gh_a_mem_available_mb(void)
{
	return (int)(si_mem_available() >> (20 - PAGE_SHIFT));
}

/* CMA-free pages on the pool's pgdat (folded NR_FREE_CMA_PAGES, like CmaFree).
 * _snapshot, not zone_page_state: __mod_zone_freepage_state updates a per-cpu
 * vm_stat_diff that the global counter only absorbs once it crosses the fold
 * threshold. The verify probe frees exactly one 2MB block then reads back
 * immediately — below threshold, so the plain read lags a whole call behind
 * (delta reads 0, then 1024 on the next attempt). The snapshot sums the per-cpu
 * diffs, so the freed pages are visible the same call. */
static unsigned long gh_a_cma_free_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long sum = 0;
	int i;

	if (!gh_acq_zone)
		return 0;
	pgdat = gh_acq_zone->zone_pgdat;
	for (i = 0; i < MAX_NR_ZONES; i++)
		sum += zone_page_state_snapshot(&pgdat->node_zones[i],
						NR_FREE_CMA_PAGES);
	return sum;
}

/* Headroom floor (§9): OK to flip @nblocks more pageblocks to CMA? What must
 * stay above the floor is available-minus-CMA — the budget unmovable
 * allocations can still reach. Distinct from the sweep's mem_available floor. */
static bool gh_a_cma_floor_ok(int nblocks)
{
	unsigned long floor = (unsigned long)READ_ONCE(cma_reservoir_floor_mb)
				<< (20 - PAGE_SHIFT);
	unsigned long need = (unsigned long)nblocks << pageblock_order_val;
	long noncma = (long)si_mem_available() - (long)gh_a_cma_free_pages();

	return noncma > (long)(floor + need);
}

static void gh_a_drop_slab(void)
{
	if (kraw.k_drop_slab)
		kraw.k_drop_slab();
}

static void gh_a_drain_pages(void)
{
	if (kraw.k_drain_all_pages)
		kraw.k_drain_all_pages(NULL);	/* NULL: every populated zone */
}

static void gh_a_lru_add_drain_all(void)
{
	if (kraw.k_lru_add_drain_all)
		kraw.k_lru_add_drain_all();
}

/* Migratetype label through the KERNEL's reader (mask 0x7): the module's inline
 * would mis-index when build != device pageblock_order. */
static unsigned long gh_a_get_pfnblock_mt(unsigned long pfn)
{
	if (!kraw.k_get_pfnblock_flags_mask)
		return 0;
	return kraw.k_get_pfnblock_flags_mask(pfn_to_page(pfn), pfn, 0x7);
}

/* Relabel the block at @base. Under sim (block spans several real pageblocks)
 * set_pageblock_migratetype only touches the one containing its page, so a
 * single call would label the first half and leave the rest unmarked; relabel
 * every real pageblock inside the believed block. No-op loop on real hardware. */
static void gh_a_set_pageblock_mt(unsigned long base, int mt)
{
	unsigned long real = 1UL << pageblock_order_val;
	unsigned long span = 1UL << gh_cma_order;	/* pool's believed block (sim) */
	unsigned long i;

	if (!kraw.k_set_pageblock_migratetype)
		return;
	if (WARN_ON_ONCE(!IS_ALIGNED(base, real)))
		return;
	for (i = 0; i < span; i += real)
		kraw.k_set_pageblock_migratetype(pfn_to_page(base + i), mt);
}

/* ── init: resolve everything, fill the table (§8) ─────── */
/* forward decls: definitions follow gh_kapi_init (§8/§10 adapters) */
static int (*gh_walk_system_ram_range)(unsigned long, unsigned long, void *,
		int (*)(unsigned long, unsigned long, void *));
/* data symbols (not in the GKI KMI): resolved via kallsyms at init, else NULL */
static unsigned long (*gh_kallsyms_lookup_name)(const char *);
static unsigned long *gh_min_low_pfn_p, *gh_max_pfn_p;
static bool gh_a_in_zone_movable(unsigned long pfn);
static int  gh_a_walk_ram(struct gh_ram_range *out, int max);
static void gh_a_cma_occupancy(unsigned long base, unsigned long nr,
			       unsigned long *free, unsigned long *anon,
			       unsigned long *file);

static void gh_kapi_init(void)
{
	static const char * const acp[] = {	/* alloc_contig_pages */
		"alloc_contig_pages_noprof", "alloc_contig_pages", NULL };
	static const char * const acr[] = {	/* alloc_contig_range */
		"alloc_contig_range_noprof", "alloc_contig_range", NULL };
	bool have_prep, have_isolate, have_reclaim, have_memcg, have_pbflags;

	gh_kapi_validate_disable();

	/* *_noprof/base fallback pair: gate on the logical (base) name the
	 * guard/preflight use, not the resolved variant. */
	kraw.k_alloc_contig_pages = gh_sym_disabled("alloc_contig_pages") ?
					NULL : gh_resolve_first(acp);
	kraw.k_alloc_contig_range = gh_sym_disabled("alloc_contig_range") ?
					NULL : gh_resolve_first(acr);
	kraw.k_prep_compound_page	= gh_resolve_gated("prep_compound_page");
	kraw.k_lru_add_drain_all	= gh_resolve_gated("lru_add_drain_all");
	kraw.k_drop_slab		= gh_resolve_gated("drop_slab");
	kraw.k_drain_all_pages		= gh_resolve_gated("drain_all_pages");
	kraw.k_mem_cgroup_from_task	= gh_resolve_gated("mem_cgroup_from_task");
	kraw.k_folio_isolate_lru	= gh_resolve_gated("folio_isolate_lru");
	kraw.k_reclaim_pages		= gh_resolve_gated("reclaim_pages");
	kraw.k_try_to_free_mem_cgroup_pages =
					gh_resolve_gated("try_to_free_mem_cgroup_pages");

	/* CMA setter/reader: resolved only on [6.1, 6.16). Below 6.1 there is no
	 * Gunyah; from 6.16 the migratetype rework changes pageblock-flag
	 * semantics and 6.18 makes the setter static — feature auto-disables to
	 * the pure-pool path there, exactly like a failed resolution. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 16, 0)
	kraw.k_set_pageblock_migratetype = gh_resolve_gated("set_pageblock_migratetype");
	kraw.k_get_pfnblock_flags_mask	 = gh_resolve_gated("get_pfnblock_flags_mask");
#else
	kraw.k_set_pageblock_migratetype = NULL;
	kraw.k_get_pfnblock_flags_mask	 = NULL;
#endif

	have_prep    = kraw.k_prep_compound_page != NULL;
	have_isolate = kraw.k_folio_isolate_lru != NULL;
	have_reclaim = kraw.k_reclaim_pages != NULL;
	have_memcg   = kraw.k_try_to_free_mem_cgroup_pages && kraw.k_mem_cgroup_from_task;
	have_pbflags = kraw.k_set_pageblock_migratetype && kraw.k_get_pfnblock_flags_mask;

	/* capability map: coarser than the old per-symbol kapi_can_*.
	 * CONTIG_* still require prep_compound (frees rebuild compounds). */
	gh_cap[GH_CAP_CONTIG_RANGE] = kraw.k_alloc_contig_range && have_prep;
	gh_cap[GH_CAP_CONTIG_PAGES] = kraw.k_alloc_contig_pages && have_prep;
	gh_cap[GH_CAP_DROP_SLAB]    = kraw.k_drop_slab != NULL;
	gh_cap[GH_CAP_EVICT_ISOLATE] = have_isolate && have_reclaim;
	gh_cap[GH_CAP_SYS_RECLAIM] = have_memcg;
	gh_cap[GH_CAP_DRAIN]       = kraw.k_drain_all_pages != NULL;
	gh_cap[GH_CAP_LRU_DRAIN]   = kraw.k_lru_add_drain_all != NULL;
	gh_cap[GH_CAP_PB_FLAGS]    = have_pbflags;

	gh_kapi.cap		= gh_a_cap;
	gh_kapi.alloc_try	= gh_a_alloc_try;
	gh_kapi.candidate_range	= gh_a_candidate_range;
	gh_kapi.contig_range	= gh_a_contig_range;
	gh_kapi.contig_range_cma = gh_a_contig_range_cma;
	gh_kapi.contig_pages	= gh_a_contig_pages;
	gh_kapi.evict_range	= gh_a_evict_range;
	gh_kapi.mem_available_mb = gh_a_mem_available_mb;
	gh_kapi.cma_floor_ok	= gh_a_cma_floor_ok;
	gh_kapi.cma_free_pages	= gh_a_cma_free_pages;
	gh_kapi.drop_slab	= gh_a_drop_slab;
	gh_kapi.drain_pages	= gh_a_drain_pages;
	gh_kapi.lru_add_drain_all = gh_a_lru_add_drain_all;
	gh_kapi.get_pfnblock_mt	= gh_a_get_pfnblock_mt;
	gh_kapi.set_pageblock_mt = gh_a_set_pageblock_mt;
	gh_kapi.in_zone_movable	= gh_a_in_zone_movable;
	gh_walk_system_ram_range = gh_resolve_kfunc("walk_system_ram_range");
	gh_kallsyms_lookup_name = gh_resolve_kfunc("kallsyms_lookup_name");
	if (gh_kallsyms_lookup_name) {
		gh_min_low_pfn_p = (unsigned long *)gh_kallsyms_lookup_name("min_low_pfn");
		gh_max_pfn_p	 = (unsigned long *)gh_kallsyms_lookup_name("max_pfn");
	}
	gh_kapi.walk_ram	= gh_a_walk_ram;
	gh_kapi.cma_occupancy	= gh_a_cma_occupancy;

	/* Resolve the acquisition zone (old acq_resolve_zone): the node our pool
	 * pages live in. cma_free_pages()/cma_floor_ok() fold NR_FREE_CMA_PAGES
	 * over every zone of this node's pgdat, so any zone in the right node is a
	 * valid handle — take a scratch order-0 page's zone. Without this
	 * gh_acq_zone stays NULL and CmaFree always reads 0 (CMA verify can never
	 * pass, the reservoir never engages). */
	if (!gh_acq_zone) {
		struct page *zp = alloc_pages(GFP_KERNEL | __GFP_NOWARN, 0);

		if (zp) {
			gh_acq_zone = page_zone(zp);
			__free_pages(zp, 0);
		}
	}
	pr_info("kapi: acq_zone=%s cma_free_baseline=%lu\n",
		gh_acq_zone ? gh_acq_zone->name : "(null)",
		gh_a_cma_free_pages());

	if (disable_kapi && *disable_kapi)
		pr_info("kapi: ABI guard active, disable_kapi=\"%s\"\n", disable_kapi);
	pr_info("kapi: contig_range=%d contig_pages=%d evict_isolate=%d sys_reclaim=%d drain=%d drop_slab=%d pb_flags=%d migrate_cma=%d pb_order=%d\n",
		gh_cap[GH_CAP_CONTIG_RANGE], gh_cap[GH_CAP_CONTIG_PAGES],
		gh_cap[GH_CAP_EVICT_ISOLATE], gh_cap[GH_CAP_SYS_RECLAIM],
		gh_cap[GH_CAP_DRAIN], gh_cap[GH_CAP_DROP_SLAB], gh_cap[GH_CAP_PB_FLAGS],
		migrate_cma_val, pageblock_order_val);
}


/* ── discovery + reporting adapters (§8/§10) ───────────── */
static bool gh_a_in_zone_movable(unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return false;
	return page_zonenum(pfn_to_page(pfn)) == ZONE_MOVABLE;
}

/* Returns the TOTAL number of present-RAM ranges; stores at most max of them
 * (out==NULL/max==0 = pure count). Two-pass callers count first, allocate
 * exactly, then fill — no magic cap to guess (the OnePlus 15 has 19 ranges
 * with the 13GB main bank LAST; a guessed 16 once truncated it silently and
 * starved the whole pool, §8). A return > max means the fill was short. */
struct gh_ram_walk_ctx { struct gh_ram_range *out; int max, n; };
static int gh_ram_walk_cb(unsigned long start_pfn, unsigned long nr, void *arg)
{
	struct gh_ram_walk_ctx *c = arg;

	if (c->out && c->n < c->max) {
		c->out[c->n].start_pfn = start_pfn;
		c->out[c->n].end_pfn = start_pfn + nr;
	}
	c->n++;
	return 0;
}
static int gh_a_walk_ram(struct gh_ram_range *out, int max)
{
	struct gh_ram_walk_ctx c = { .out = out, .max = max, .n = 0 };

	if (gh_walk_system_ram_range && gh_max_pfn_p &&
	    gh_walk_system_ram_range(0, *gh_max_pfn_p, &c, gh_ram_walk_cb) == 0 && c.n)
		return c.n;
	/* fallback: one span (§8) from the kallsyms-resolved data symbols — the
	 * pool marks holes not_useable anyway. Both are outside the GKI KMI, so
	 * they are resolved at runtime, never linked. */
	if (gh_min_low_pfn_p && gh_max_pfn_p) {
		if (out && max > 0) {
			out[0].start_pfn = *gh_min_low_pfn_p;
			out[0].end_pfn = *gh_max_pfn_p;
		}
		return 1;
	}
	return 0;
}

/* max allocation order name drifts: MAX_PAGE_ORDER (>=6.8), MAX_ORDER
 * (inclusive 6.4..6.8; exclusive bound earlier). Only used to sanity-cap a
 * racy page_private() read. */
#if defined(MAX_PAGE_ORDER)
#define GH_MAX_ORDER	MAX_PAGE_ORDER
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GH_MAX_ORDER	MAX_ORDER
#else
#define GH_MAX_ORDER	(MAX_ORDER - 1)
#endif

/* one CMA block's occupancy in pages: free / anon / file (racy report) */
static void gh_a_cma_occupancy(unsigned long base, unsigned long nr,
			       unsigned long *free, unsigned long *anon,
			       unsigned long *file)
{
	unsigned long pfn = base, end = base + nr, f = 0, a = 0, fi = 0;

	while (pfn < end) {
		struct page *p;
		unsigned long step = 1;

		if (!pfn_valid(pfn)) { pfn++; continue; }
		p = pfn_to_page(pfn);
		if (PageBuddy(p)) {
			unsigned int order = (unsigned int)READ_ONCE(page_private(p));

			if (order > GH_MAX_ORDER)
				order = 0;
			step = min(1UL << order, end - pfn);
			f += step;
		} else if (page_count(p) == 0) {
			f++;
		} else {
			struct folio *folio = page_folio(p);
			unsigned long fn = folio_nr_pages(folio);

			step = fn ? min(fn, end - pfn) : 1;
			if (folio_test_anon(folio))
				a += step;
			else if (folio_test_lru(folio))
				fi += step;
		}
		pfn += step;
		cond_resched();
	}
	*free = f; *anon = a; *file = fi;
}

/* ── accessors + primitives the other kernel parts need ─── */
static int gh_kapi_migrate_cma_val(void)     { return READ_ONCE(migrate_cma_val); }
static int gh_kapi_pageblock_order_val(void) { return READ_ONCE(pageblock_order_val); }
/* alias for the unlock_cma disable-list check (§9 registers its own hooks) */
static bool gh_kapi_sym_disabled(const char *name) { return gh_sym_disabled(name); }

/* THE order-9 compound rebuild funnel (gh_kernel_env.h declares it): head
 * refcount 1, structure via prep_compound_page, tail refcounts 0 — the same
 * shape alloc_pages(__GFP_COMP) yields, so serve and __free_pages both work.
 * Non-sleeping (callers may hold raw locks). Requires prep_compound_page. */
void gh_rebuild_order9(struct page *head)
{
	int i;

	set_page_count(head, 1);
	if (kraw.k_prep_compound_page)
		kraw.k_prep_compound_page(head, GH_PAGE_ORDER);
	for (i = 1; i < (1 << GH_PAGE_ORDER); i++)
		set_page_count(head + i, 0);
	head->mapping = NULL;		/* sanitize: a page entering the pool must
					 * carry no stale mapping, else a later
					 * __free_pages trips bad_page (old
					 * pool_take_frozen cleared this too) */
	INIT_LIST_HEAD(&head->lru);	/* and no stale list linkage: served as a
					 * fresh THP, the guest fault does
					 * folio_add_lru(); a dirty ->lru (e.g. a
					 * buddy freelist node from a prior
					 * free/grab in verify) corrupts that list */
}
