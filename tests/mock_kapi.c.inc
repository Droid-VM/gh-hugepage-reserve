/* Mock kapi backend (§3.3): deterministic fake buddy + real-pageblock label
 * world over the fake memmap. The harness scripts edge cases by planting
 * squatters (hard/pinned/evictable/movable) and tightening windows. */

static unsigned long mock_buddy_lo, mock_buddy_hi;	/* allocatable pfn window */
static int mock_alloc_fail_after = -1;			/* -1 = never; else countdown */
static int mock_pb_order = 9;				/* REAL pageblock order */
static int mock_migrate_cma = 4;			/* runtime MIGRATE_CMA value */
static u8 mock_mt[MOCK_NPAGES >> 9];			/* label per real pageblock */
static int mock_mem_avail_mb = 4096;
static int mock_cma_floor_pass = 1;
static int mock_floor_mb = 0;		/* 0 = legacy scripted verdict; >0 =
					 * real noncma formula (§3.3 fidelity:
					 * the self-balancing disposal RELIES
					 * on sheds raising available while
					 * flips move it in lockstep with
					 * CmaFree — a static verdict cannot
					 * express that) */
static long mock_evicted;				/* evict_range invocations */

static inline u8 *mock_mt_slot(unsigned long pfn)
{
	return &mock_mt[(pfn - MOCK_BASE_PFN) >> mock_pb_order];
}
static unsigned long mock_get_mt(unsigned long pfn) { return *mock_mt_slot(pfn); }
static void mock_set_mt(unsigned long pfn, int mt)
{	/* real granularity: labels exactly one REAL pageblock (off-by-one sim) */
	*mock_mt_slot(pfn) = (u8)mt;
}

static unsigned long mock_cma_free(void)
{	/* NR_FREE_CMA model: free pages sitting in CMA-labeled blocks */
	unsigned long p, n = 0;

	mock_world_lock();
	for (p = 0; p < MOCK_NPAGES; p++)
		if (MOCK_LD(mock_memmap[p].free_in_buddy) &&
		    mock_mt[p >> mock_pb_order] == mock_migrate_cma)
			n++;
	mock_world_unlock();
	return n;
}

static unsigned long mock_free_pages(void)
{	/* all free-in-buddy pages, any label (si_mem_available's free term) */
	unsigned long p, n = 0;

	mock_world_lock();
	for (p = 0; p < MOCK_NPAGES; p++)
		if (MOCK_LD(mock_memmap[p].free_in_buddy))
			n++;
	mock_world_unlock();
	return n;
}

static struct page *mock_alloc_try(unsigned int order, int strong)
{
	unsigned long step = 1UL << order, p;

	(void)strong;
	mock_world_lock();
	if (mock_alloc_fail_after == 0) {
		mock_world_unlock();
		return NULL;
	}
	if (mock_alloc_fail_after > 0)
		mock_alloc_fail_after--;
	for (p = (mock_buddy_lo + step - 1) & ~(step - 1);
	     p + step <= mock_buddy_hi; p += step) {
		unsigned long i;
		bool ok = true;

		for (i = 0; i < step; i++) {
			struct page *pg = pfn_to_page(p + i);

			if (page_count(pg) != 0 || !MOCK_LD(pg->free_in_buddy) ||
			    mock_mt[(p + i - MOCK_BASE_PFN) >> mock_pb_order] ==
			    (u8)mock_migrate_cma) { ok = false; break; }
		}
		if (!ok)
			continue;
		for (i = 0; i < step; i++)
			MOCK_ST(pfn_to_page(p + i)->free_in_buddy, 0);
		gh_rebuild_order9(pfn_to_page(p));
		if (order > 9) {
			for (i = 512; i < step; i += 512) {
				gh_rebuild_order9(pfn_to_page(p + i));
				set_page_count(pfn_to_page(p + i), 0);
			}
			set_page_count(pfn_to_page(p), 1);
		}
		mock_world_unlock();
		return pfn_to_page(p);
	}
	mock_world_unlock();
	return NULL;
}

/* pure-read feasibility (block_candidate): every page free or clearable */
static bool mock_candidate_range(unsigned long start, unsigned long end)
{
	unsigned long p;

	for (p = start; p < end; p++) {
		struct page *pg = pfn_to_page(p);

		if (pg->hard || pg->pinned)
			return false;
	}
	return true;
}

/* movable-mode contig grab. noretry(async): only fully-free ranges assemble;
 * sync also migrates plain movable squatters away. CMA label / pinned / hard /
 * evictable-still-present fail. Success: whole range allocated (singles). */
static int mock_contig_range(unsigned long start, unsigned long end, int noretry)
{
	unsigned long p;

	mock_drain_pcp();	/* LSP fidelity: the real alloc_contig_range drains
				 * pcp internally (lru_add_drain_all + drain_all_pages)
				 * and §3.2 sweep relies on it — parked frees flow
				 * through the hook (or into buddy) before isolation */
	mock_world_lock();
	for (p = start; p < end; p++) {
		struct page *pg = pfn_to_page(p);

		if (mock_mt[(p - MOCK_BASE_PFN) >> mock_pb_order] == (u8)mock_migrate_cma)
			goto fail;
		if (pg->hard || pg->pinned || pg->evictable)
			goto fail;
		if (!MOCK_LD(pg->free_in_buddy) && page_count(pg) != 0 && noretry)
			goto fail;		/* async: squatter = fail fast */
		if (!MOCK_LD(pg->free_in_buddy) && page_count(pg) == 0)
			goto fail;		/* allocated to someone (e.g. pool page) */
	}
	for (p = start; p < end; p++) {		/* migrate movables + take frees */
		struct page *pg = pfn_to_page(p);

		set_page_count(pg, 0);
		MOCK_ST(pg->free_in_buddy, 0);
		MOCK_ST(pg->has_mapping, 0);
	}
	mock_world_unlock();
	return 0;
fail:
	mock_world_unlock();
	return -1;
}

/* CMA-mode grab: only inside CMA-labeled blocks; migrates movable squatters */
static int mock_contig_range_cma(unsigned long start, unsigned long end)
{
	unsigned long p;

	mock_drain_pcp();	/* same drain semantics as mock_contig_range */
	mock_world_lock();
	for (p = start; p < end; p++) {
		struct page *pg = pfn_to_page(p);

		if (mock_mt[(p - MOCK_BASE_PFN) >> mock_pb_order] != (u8)mock_migrate_cma)
			goto fail;
		if (pg->hard || pg->pinned)
			goto fail;
	}
	for (p = start; p < end; p++) {
		struct page *pg = pfn_to_page(p);

		set_page_count(pg, 0);
		MOCK_ST(pg->free_in_buddy, 0);
		pg->evictable = 0;
		MOCK_ST(pg->has_mapping, 0);
	}
	mock_world_unlock();
	return 0;
fail:
	mock_world_unlock();
	return -1;
}

static struct page *mock_contig_pages(unsigned long nr)
{
	unsigned long p;

	if (mock_alloc_fail_after == 0)
		return NULL;
	if (mock_alloc_fail_after > 0)
		mock_alloc_fail_after--;
	for (p = (mock_buddy_lo + nr - 1) & ~(nr - 1); p + nr <= mock_buddy_hi; p += nr)
		if (mock_contig_range(p, p + nr, 0) == 0)
			return pfn_to_page(p);
	return NULL;
}

static void mock_evict_range(int mode, unsigned long start, unsigned long end)
{
	unsigned long p;

	(void)mode;				/* MEMCG modelled same-shape in mock */
	mock_evicted++;
	mock_world_lock();
	for (p = start; p < end; p++) {
		struct page *pg = pfn_to_page(p);

		if (pg->evictable && !pg->pinned) {
			pg->evictable = 0;
			set_page_count(pg, 0);
			MOCK_ST(pg->free_in_buddy, 1);
		}
	}
	mock_world_unlock();
}

/* ZONE_MOVABLE model: a harness-settable pfn window */
static unsigned long mock_movable_lo, mock_movable_hi;
static bool mock_in_zone_movable(unsigned long pfn)
{ return pfn >= mock_movable_lo && pfn < mock_movable_hi; }

static int mock_walk_ram(struct gh_ram_range *out, int max)
{ (void)out; (void)max; return 0; }	/* harness builds ranges directly */

static void mock_cma_occupancy(unsigned long base, unsigned long nr,
			       unsigned long *free, unsigned long *anon,
			       unsigned long *file)
{
	unsigned long p, f = 0, a = 0, fi = 0;

	for (p = base; p < base + nr; p++) {
		struct page *pg = pfn_to_page(p);

		if (pg->free_in_buddy || page_count(pg) == 0)
			f++;
		else if (pg->has_mapping)
			fi++;
		else
			a++;
	}
	*free = f; *anon = a; *file = fi;
}

static bool mock_cap(enum gh_kapi_cap f) { (void)f; return true; }
static int  mock_mem_available(void) { return mock_mem_avail_mb; }
static bool mock_floor_ok(int nblocks)
{
	unsigned long noncma_pages;

	if (!mock_floor_mb)
		return mock_cma_floor_pass;	/* legacy scripted verdict */
	/* real gh_a_cma_floor_ok formula over the fake world: available =
	 * scripted base (the live system's reclaimable estimate, in MB) +
	 * what actually sits free in the fake buddy; subtract CMA free.
	 * A shed enlarges non-CMA free, a flip enlarges free and CMA free
	 * equally — only sheds pay a floor deficit down, as in the kernel. */
	noncma_pages = (unsigned long)mock_mem_avail_mb * 256 +
		       mock_free_pages() - mock_cma_free();
	return noncma_pages > (unsigned long)mock_floor_mb * 256 +
			      ((unsigned long)nblocks << mock_pb_order);
}
static void mock_nop(void) { }

static void mock_kapi_install(void)
{
	gh_kapi.cap = mock_cap;
	gh_kapi.alloc_try = mock_alloc_try;
	gh_kapi.candidate_range = mock_candidate_range;
	gh_kapi.contig_range = mock_contig_range;
	gh_kapi.contig_range_cma = mock_contig_range_cma;
	gh_kapi.contig_pages = mock_contig_pages;
	gh_kapi.evict_range = mock_evict_range;
	gh_kapi.mem_available_mb = mock_mem_available;
	gh_kapi.cma_floor_ok = mock_floor_ok;
	gh_kapi.cma_free_pages = mock_cma_free;
	gh_kapi.drop_slab = mock_nop;
	gh_kapi.drain_pages = mock_drain_pcp;
	gh_kapi.lru_add_drain_all = mock_nop;
	gh_kapi.get_pfnblock_mt = mock_get_mt;
	gh_kapi.set_pageblock_mt = mock_set_mt;
	gh_kapi.in_zone_movable = mock_in_zone_movable;
	gh_kapi.walk_ram = mock_walk_ram;
	gh_kapi.cma_occupancy = mock_cma_occupancy;
}

/* world-building: mark a pfn window free-in-buddy, movable-labeled */
static void mock_fill_buddy(unsigned long lo, unsigned long hi)
{
	unsigned long p;

	mock_world_lock();
	mock_buddy_lo = lo; mock_buddy_hi = hi;
	for (p = lo; p < hi; p++) {
		struct page *pg = pfn_to_page(p);

		memset(pg, 0, sizeof(*pg));
		pg->free_in_buddy = 1;
	}
	mock_world_unlock();
}
static void mock_plant(unsigned long pfn, int hard, int pinned, int evictable)
{
	struct page *pg = pfn_to_page(pfn);

	pg->free_in_buddy = 0;
	pg->_refcount = 1;
	pg->hard = hard; pg->pinned = pinned; pg->evictable = evictable;
}
