/* gh_hugepage_reserve — kernel environment for the pool parts (§8).
 *
 * gh_defs.h + gh_owner/pool/release/adjust are environment-agnostic; this
 * header provides, for the KERNEL unity build, everything they name that is
 * not standard C: types, lock wrappers, the page primitive set, clock,
 * allocation, and the free-hook query. The mock counterpart is tests/shim.h.
 * Included by gh_hugepage_reserve.c after the real <linux/...> headers.
 */
#ifndef GH_KERNEL_ENV_H
#define GH_KERNEL_ENV_H

/* raw spinlock: the pool lock is taken from atomic serve/return context */
typedef struct { raw_spinlock_t l; } gh_spinlock_t;
typedef rwlock_t gh_rwlock_t;
static inline void gh_spin_lock(gh_spinlock_t *s, unsigned long *fl)
{ raw_spin_lock_irqsave(&s->l, *fl); }
static inline void gh_spin_unlock(gh_spinlock_t *s, unsigned long *fl)
{ raw_spin_unlock_irqrestore(&s->l, *fl); }
static inline void gh_read_lock(gh_rwlock_t *l) { read_lock(l); }
static inline void gh_read_unlock(gh_rwlock_t *l) { read_unlock(l); }
/* owner writes are process context only (§2): plain write_lock, no irqsave */
static inline void gh_write_lock(gh_rwlock_t *l) { write_lock(l); }
static inline void gh_write_unlock(gh_rwlock_t *l) { write_unlock(l); }
/* NOTE(on-device): all
 * pool_owner_* callers are process context (they are: hooks arm workers, the
 * owner writes happen in vm_boot/shutdown/exit probe handlers = process ctx),
 * so write_lock (no irqsave) suffices; simplify at wire-up. */

#define GH_READ_ONCE(x)     READ_ONCE(x)
#define GH_PUBLISH(dst, v)  smp_store_release(&(dst), (v))
#define GH_WRITE_ONCE(x, v) WRITE_ONCE(x, v)
#define GH_CTR_ADD(x, v)    ((x) += (v))	/* lock-serialized writers (§2) */
#define gh_pr_warn(...)     pr_warn(__VA_ARGS__)
#define gh_pr_info(...)     pr_info(__VA_ARGS__)
#define gh_cond_resched_every(i) do { if (((i) & 0x3f) == 0) cond_resched(); } while (0)
static inline void gh_strscpy(char *d, const char *s, int n) { strscpy(d, s, n); }

/* mm handle is a real struct mm_struct * */
static inline void gh_mmgrab(void *mm) { mmgrab((struct mm_struct *)mm); }
static inline void gh_mmdrop(void *mm) { mmdrop((struct mm_struct *)mm); }
static inline int  gh_mm_users(void *mm) { return atomic_read(&((struct mm_struct *)mm)->mm_users); }

/* seconds clock, wrap-safe by construction (u32 diff compares, §11) */
static inline u32 gh_now_sec(void) { return (u32)(jiffies / HZ); }

/* page primitives — the ONLY window into struct page the pool has (§3.3) */
/* pfn_to_page/page_to_pfn/page_count/get_page/put_page are kernel-provided. */
/* raw ->mapping field: version-stable (page_mapping() helper was removed on
 * 6.12 GKI in favour of folio_mapping); old code used !pg->mapping too. */
static inline int gh_page_has_mapping(struct page *p) { return p->mapping != NULL; }
/* rebuild a pool-grade order-9 compound (kapi backend owns prep_compound_page) */
void gh_rebuild_order9(struct page *head);	/* defined in gh_kapi.c.inc */

static inline void *gh_kzalloc_chunk(void)
{ return kzalloc(512 * sizeof(struct gh_slot), GFP_KERNEL); }
static inline void *gh_zalloc(unsigned long n) { return kvzalloc(n, GFP_KERNEL); }
static inline void  gh_free(void *p) { kvfree(p); }

/* worker scheduling: defined in the unity root (delayed_work wrappers) */
void gh_sched_adjust(int ms);
void gh_sched_release(int ms);

#define GH_EBUSY EBUSY
#endif
