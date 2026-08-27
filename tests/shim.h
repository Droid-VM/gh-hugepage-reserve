/* Userspace shim for the unity-build mock harness (POOL_DESIGN.md §3.3).
 * Provides the environment gh_defs.h expects: types, no-op locks (the
 * harness is single-threaded and deterministic), a fake memmap with real
 * refcounts, a controllable clock, and the page primitive set. */
#ifndef GH_TEST_SHIM_H
#define GH_TEST_SHIM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int pid_t_shim;
#define pid_t pid_t_shim

#ifdef GH_RACE
/* race harness (TSan): REAL locks + atomics. Errorcheck mutex: relocking from
 * the free hook (the "no put_page under pool_lock" discipline, §2) aborts
 * loudly instead of silently deadlocking. */
#include <pthread.h>
typedef struct { pthread_mutex_t m; int inited; } gh_spinlock_t;
typedef struct { pthread_rwlock_t rw; } gh_rwlock_t;
static pthread_mutex_t gh_race_init_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void gh_lock_init_once(gh_spinlock_t *l)
{
	if (__atomic_load_n(&l->inited, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&gh_race_init_mu);
	if (!l->inited) {
		pthread_mutexattr_t a;

		pthread_mutexattr_init(&a);
		pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
		pthread_mutex_init(&l->m, &a);
		__atomic_store_n(&l->inited, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&gh_race_init_mu);
}
static inline void gh_spin_lock(gh_spinlock_t *l, unsigned long *fl)
{ (void)fl; gh_lock_init_once(l); if (pthread_mutex_lock(&l->m)) abort(); }
static inline void gh_spin_unlock(gh_spinlock_t *l, unsigned long *fl)
{ (void)fl; if (pthread_mutex_unlock(&l->m)) abort(); }
static inline void gh_read_lock(gh_rwlock_t *l) { pthread_rwlock_rdlock(&l->rw); }
static inline void gh_read_unlock(gh_rwlock_t *l) { pthread_rwlock_unlock(&l->rw); }
static inline void gh_write_lock(gh_rwlock_t *l) { pthread_rwlock_wrlock(&l->rw); }
static inline void gh_write_unlock(gh_rwlock_t *l) { pthread_rwlock_unlock(&l->rw); }
#define GH_READ_ONCE(x)     __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define GH_PUBLISH(dst, v)  __atomic_store_n(&(dst), (v), __ATOMIC_RELEASE)
#define GH_WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define GH_CTR_ADD(x, v)    __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)
#else
/* deterministic harness: single-threaded -> counters only (imbalance = bug) */
typedef struct { int held; } gh_spinlock_t;
typedef struct { int held; } gh_rwlock_t;
static inline void gh_spin_lock(gh_spinlock_t *l, unsigned long *fl) { (void)fl; l->held++; }
static inline void gh_spin_unlock(gh_spinlock_t *l, unsigned long *fl) { (void)fl; l->held--; }
static inline void gh_read_lock(gh_rwlock_t *l) { l->held++; }
static inline void gh_read_unlock(gh_rwlock_t *l) { l->held--; }
static inline void gh_write_lock(gh_rwlock_t *l) { l->held++; }
static inline void gh_write_unlock(gh_rwlock_t *l) { l->held--; }
#define GH_READ_ONCE(x) (x)
#define GH_PUBLISH(dst, v) ((dst) = (v))
#define GH_WRITE_ONCE(x, v) ((x) = (v))
#define GH_CTR_ADD(x, v)    ((x) += (v))
#endif
#define gh_pr_warn(...) fprintf(stderr, "[warn] " __VA_ARGS__)
#define gh_pr_info(...) fprintf(stderr, "[info] " __VA_ARGS__)
#define gh_cond_resched_every(i) do { } while (0)
static inline void gh_strscpy(char *d, const char *s, int n) { snprintf(d, (size_t)n, "%s", s); }

/* fake mm: an opaque token with a users count the harness controls */
struct mock_mm { int mm_users; int grabbed; };
#ifdef GH_RACE
static inline void gh_mmgrab(void *mm) { __atomic_fetch_add(&((struct mock_mm *)mm)->grabbed, 1, __ATOMIC_RELAXED); }
static inline void gh_mmdrop(void *mm) { __atomic_fetch_sub(&((struct mock_mm *)mm)->grabbed, 1, __ATOMIC_RELAXED); }
static inline int gh_mm_users(void *mm) { return __atomic_load_n(&((struct mock_mm *)mm)->mm_users, __ATOMIC_RELAXED); }
#else
static inline void gh_mmgrab(void *mm) { ((struct mock_mm *)mm)->grabbed++; }
static inline void gh_mmdrop(void *mm) { ((struct mock_mm *)mm)->grabbed--; }
static inline int gh_mm_users(void *mm) { return ((struct mock_mm *)mm)->mm_users; }
#endif

/* controllable clock */
static u32 mock_now_sec = 1000;
static inline u32 gh_now_sec(void) { return GH_READ_ONCE(mock_now_sec); }

/* ── fake memmap ─────────────────────────────────────────
 * pfn space [MOCK_BASE_PFN, MOCK_BASE_PFN + MOCK_NPAGES). page->_refcount is
 * real; the mock buddy (mock_kapi.c) allocates from it. */
#define MOCK_BASE_PFN  0x80000UL		/* 2GB phys, 1GB-aligned */
#define MOCK_NPAGES    (3UL << 18)		/* 3GB of pfn space (incl. hole) */
/* squatter kinds for candidate/evict/contig modelling (mock_kapi.c.inc):
 * hard: never movable/evictable (slab, page table)
 * pinned: FOLL_LONGTERM — migration and eviction both fail
 * evictable: only evict_range clears it (page cache to zram)
 * plain refcount>0: movable squatter — sync contig migrates it away */
struct page { int _refcount; int has_mapping; int compound_order;
	      int free_in_buddy; int hard; int pinned; int evictable; };
#ifdef GH_RACE
#define MOCK_LD(x)    __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define MOCK_ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#else
#define MOCK_LD(x)    (x)
#define MOCK_ST(x, v) ((x) = (v))
#endif
static struct page mock_memmap[MOCK_NPAGES];
static inline struct page *pfn_to_page(unsigned long pfn) { return &mock_memmap[pfn - MOCK_BASE_PFN]; }
static inline unsigned long page_to_pfn(struct page *p) { return MOCK_BASE_PFN + (unsigned long)(p - mock_memmap); }
#ifdef GH_RACE
static inline int page_count(struct page *p) { return __atomic_load_n(&p->_refcount, __ATOMIC_RELAXED); }
static inline void set_page_count(struct page *p, int v) { __atomic_store_n(&p->_refcount, v, __ATOMIC_RELAXED); }
static inline void get_page(struct page *p) { __atomic_fetch_add(&p->_refcount, 1, __ATOMIC_ACQ_REL); }
#else
static inline int page_count(struct page *p) { return p->_refcount; }
static inline void set_page_count(struct page *p, int v) { p->_refcount = v; }
static inline void get_page(struct page *p) { p->_refcount++; }
#endif
static inline int gh_page_has_mapping(struct page *p) { return MOCK_LD(p->has_mapping); }

/* free-hook wiring: __free_pages dropping to 0 routes through the hook the
 * way __free_one_page would — unless pcp-lag simulation swallows it */
#ifdef GH_RACE
static pthread_mutex_t mock_world_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void mock_world_lock(void)   { pthread_mutex_lock(&mock_world_mu); }
static inline void mock_world_unlock(void) { pthread_mutex_unlock(&mock_world_mu); }
#else
static inline void mock_world_lock(void)   { }
static inline void mock_world_unlock(void) { }
#endif
static int mock_free_hook_armed = 1;
static int mock_pcp_lag = 0;			/* >0: park next N order-9 frees in "pcp" */
static long mock_freed_to_buddy;		/* order-9 frees that reached buddy */
static struct page *mock_pcp[64];
static int mock_pcp_n;
static int mock_return_dispatch(struct page *p);	/* defined in harness after parts */
static inline void mock_buddy_free_span(struct page *p, unsigned int order)
{
	unsigned long i, n = 1UL << order;

	for (i = 0; i < n; i++) {
		set_page_count(&p[i], 0);
		MOCK_ST(p[i].free_in_buddy, 1);
		MOCK_ST(p[i].compound_order, 0);
	}
	if (order == 9)
		mock_freed_to_buddy++;
}
static inline void mock_free_order9(struct page *p)
{
	mock_world_lock();
	if (mock_pcp_lag > 0 && mock_pcp_n < 64) {	/* pcp-lag: parked, hook blind */
		mock_pcp_lag--;
		mock_pcp[mock_pcp_n++] = p;
		mock_world_unlock();
		return;
	}
	mock_world_unlock();
	/* hook dispatch OUTSIDE the world lock: pool_released_return takes
	 * pool_lock, and pool code frees pages while holding no lock (§2) */
	if (mock_free_hook_armed && mock_return_dispatch(p))
		return;				/* pool bypassed the free (KEEP) */
	mock_world_lock();
	mock_buddy_free_span(p, 9);
	mock_world_unlock();
}
static inline void gh_put_ref(struct page *p, unsigned int order)
{
#ifdef GH_RACE
	if (__atomic_sub_fetch(&p->_refcount, 1, __ATOMIC_ACQ_REL) > 0)
		return;
#else
	if (--p->_refcount > 0)
		return;
#endif
	if (order == 9) {
		mock_free_order9(p);
	} else {
		mock_world_lock();
		mock_buddy_free_span(p, order);
		mock_world_unlock();
	}
}
static inline void put_page(struct page *p)
{ gh_put_ref(p, MOCK_LD(p->compound_order) == 9 ? 9 : 0); }
static inline void __free_pages(struct page *p, unsigned int order)
{ gh_put_ref(p, order); }
/* drain_pages: push parked order-9 frees through the free path (§6) */
static inline void mock_drain_pcp(void)
{
	struct page *batch[64];
	int i, n;

	mock_world_lock();
	n = mock_pcp_n;
	for (i = 0; i < n; i++)
		batch[i] = mock_pcp[i];
	mock_pcp_n = 0;
	mock_world_unlock();
	for (i = 0; i < n; i++) {
		if (mock_free_hook_armed && mock_return_dispatch(batch[i]))
			continue;
		mock_world_lock();
		mock_buddy_free_span(batch[i], 9);
		mock_world_unlock();
	}
}
static inline void gh_rebuild_order9(struct page *head)
{
	int i;

	set_page_count(head, 1);
	MOCK_ST(head->compound_order, 9);
	MOCK_ST(head->free_in_buddy, 0);
	MOCK_ST(head->has_mapping, 0);
	for (i = 1; i < 512; i++)
		set_page_count(head + i, 0);
}

static inline void *gh_kzalloc_chunk(void)
{
	return calloc(512, 20 + 12);		/* >= sizeof(struct gh_slot) x 512 */
}
static inline void *gh_zalloc(unsigned long n) { return calloc(1, n); }
static inline void gh_free(void *p) { free(p); }

/* worker scheduling stubs: the harness ticks rounds deterministically */
static int mock_adjust_pending, mock_release_pending;
static inline void gh_sched_adjust(int ms) { (void)ms; mock_adjust_pending = 1; }
static inline void gh_sched_release(int ms) { (void)ms; mock_release_pending = 1; }
#define GH_EBUSY 16
#endif
