// SPDX-License-Identifier: GPL-2.0
/*
 * gh_hugepage_reserve
 *
 * Pre-allocates 2MB compound pages at boot and serves them to Gunyah VM
 * memory allocations via kretprobe on __alloc_pages, ensuring VMs get
 * contiguous THP-backed memory even when the system is fragmented.
 *
 * Architecture:
 *
 *   1. Pool allocation (insmod):
 *      Pre-allocate order-9 (2MB) compound pages into a static pool.
 *
 *   2. Always-on kretprobe (__alloc_pages):
 *      Registered at init, intercepts order-9 allocations from tracked
 *      VM owner processes. Replaces the return value with a pool page
 *      and frees the original buddy page (if any) back to avoid double
 *      memory consumption. entry_handler filters by order, pool count,
 *      and mm - overhead is negligible for non-matching calls.
 *
 *   3. VM lifecycle tracking (kprobes):
 *      - Creation:    kprobe on gunyah_dev_vm_mgr_ioctl (GH_CREATE_VM)
 *                     records the caller's mm_struct for filtering.
 *                     Each owner carries a vm_count (VMs created by that mm).
 *      - Destruction: kprobe on gunyah_vm_release / gh_vm_free
 *                     schedules delayed pool refill after VM exits, then
 *                     sweeps owners per-entry: only those whose process is
 *                     dead (mm_users == 0) or whose last VM was closed are
 *                     released. Sibling VMs keep their tracking - one VM
 *                     dying (e.g. OOM kill of one crosvm) must never drop
 *                     the others.
 *
 *   4. Pool refill (delayed work):
 *      After VM shutdown + configurable delay, refills pool from buddy
 *      allocator with compaction/reclaim. Retries on fragmentation.
 *
 * Prerequisites:
 *   echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
 *
 * Usage:
 *   insmod gh_hugepage_reserve.ko pool_want=1024 refill_delay_ms=5000
 */

#define pr_fmt(fmt) "gh_hugepage_reserve: " fmt
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/compaction.h>
#include <linux/sched/mm.h>
#include <linux/page_ref.h>
#include <linux/topology.h>
#include <linux/nodemask.h>
#include <linux/tracepoint.h>
#include <linux/mmzone.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/vmstat.h>
/* Single source of truth for the resolvable kapi symbol names (KAPI_SYMBOLS),
 * generated from abi/kapi_abi.tsv by abi/gen_kapi.awk (also drives the userspace
 * ABI preflight). We only pull the name list here (not KAPI_ABI_WANT_TABLE). */
#include "kapi_abi.gen.h"
/*
 * The free-path reclaim rides the android_vh_free_one_page_bypass vendor hook.
 * It is NOT present on every KMI we build for (absent on some 5.10/5.15 vendor
 * hook sets, present on 5.15-android13, 6.1, 6.6, 6.12), and its availability is
 * not monotonic by version - so we do NOT reference register_trace_* (which
 * fails to compile where the DECLARE_HOOK is missing). Instead we locate the
 * tracepoint by name at runtime via for_each_kernel_tracepoint() and attach with
 * tracepoint_probe_register(); a missing hook simply disables free reclaim.
 */

#define POOL_SIZE_MAX_RAM	(24UL << 30)	/* hard cap on the pool, in bytes;
						 * sizes the static arrays */
#define POOL_SIZE_MAX	((int)(POOL_SIZE_MAX_RAM >> (PAGE_SHIFT + PAGE_ORDER)))
					/* the same cap in 2MB pages (12288). The
					 * effective cap is pool_size_max, computed
					 * from system RAM at insmod:
					 * min(ram - min(ram/2, SYSTEM_RESERVE),
					 *     POOL_SIZE_MAX_RAM). */
#define SYSTEM_RESERVE		(6UL << 30)	/* RAM never counted into pool_size_max:
						 * the pool leaves at least this much
						 * (or half of RAM, whichever is less)
						 * to the rest of the system. 6G fits
						 * a heavy Android resident set:
						 * ~2-3G unswappable kernel+dmabuf/GPU
						 * plus ~2.7G core services. */
#define ACQUIRE_MAX_FAILS	8	/* stop once the cumulative fail score reaches
					 * this (a failure +1, a success -3, floored
					 * at 0) */
#define ACQUIRE_FAIL_DECAY	3	/* a success drops the fail score by this */
#define ACQUIRE_DELAY_MS	10	/* breathe between migrations */
#define PAGE_ORDER		9	/* 2^9 * 4KB = 2MB */
/* Highest order alloc_pages accepts on this kernel: MAX_ORDER was an EXCLUSIVE
 * bound until 6.4 (23baf831a32c made it inclusive), and 6.8 renamed it
 * MAX_PAGE_ORDER. Used by the CMA first-block verification span, which wants
 * one allocation of pageblock_order + 1 when the kernel allows it. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define GH_MAX_ALLOC_ORDER	MAX_PAGE_ORDER
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define GH_MAX_ALLOC_ORDER	MAX_ORDER
#else
#define GH_MAX_ALLOC_ORDER	(MAX_ORDER - 1)
#endif
#define REFILL_DELAY_MS_DEFAULT	5000
#define REFILL_RETRY_MAX	3
#define REFILL_RETRY_INTERVAL_MS 3000

/* GH_CREATE_VM = _IO('G', 0x0) - same across */
#define GH_IOCTL_TYPE	'G'
#define GH_CREATE_VM	_IO(GH_IOCTL_TYPE, 0x0)

/* ---- Module parameters ---- */

/* pool_want is the single target knob (declared below): set at insmod and at
 * runtime via the same sysfs file. */

static int refill_delay_ms = REFILL_DELAY_MS_DEFAULT;
module_param(refill_delay_ms, int, 0600);
MODULE_PARM_DESC(refill_delay_ms,
	"Delay in ms before refilling pool after VM shutdown (default 5000)");

/*
 * refill_enable gates the OLD detect->alloc-back recovery path (the VM-destroy
 * kprobe scheduling refill_worker, and manual_refill). Set 0 to measure the new
 * free-path reclaim (android_vh_free_one_page_bypass) in isolation, so the pool
 * only refills from pages a VM actually returns - not from fresh alloc_pages.
 * Default 1 keeps normal behaviour. The vm-owner sweep still runs when disabled.
 */
static int refill_enable = 1;
module_param(refill_enable, int, 0600);
MODULE_PARM_DESC(refill_enable,
	"1=auto alloc-back refill after VM shutdown (default), 0=disable (test free-hook reclaim alone)");

/* ---- CMA reservoir parameters (v10) ----
 *
 * The reservoir is the elastic half of the dual-pool model: pageblocks we
 * labeled MIGRATE_CMA whose pages sit FREE in buddy. While a VM is down, app
 * movable allocations use them like ordinary memory; unmovable allocations can
 * never enter a CMA block, so when a VM is about to start, a targeted CMA-mode
 * alloc_contig_range reassembles each block into 2MB pages in seconds instead
 * of fighting the fragmentation wall (measured: 8GB in ~10s vs a sweep stalling
 * at ~30%). pool_want_with_cma=0 (default) keeps all of this off: v9 behavior.
 *
 * Both preflight values below come from userspace (post-fs-data.sh) because the
 * running kernel exposes neither: MIGRATE_CMA's value is config/vendor-dependent
 * (read from /sys/kernel/btf/vmlinux by kapi_check), and pageblock_order is a
 * pure macro - no kernel variable, absent from kallsyms and BTF (read from
 * /proc/pagetypeinfo "Page block order"). Either missing -> feature off.
 */

static int migrate_cma_val = -1;
module_param(migrate_cma_val, int, 0400);
MODULE_PARM_DESC(migrate_cma_val,
	"MIGRATE_CMA enumerator value on the running kernel (from kapi_check BTF preflight); -1 = CMA reservoir off");

static int pageblock_order_val = -1;
module_param(pageblock_order_val, int, 0400);
MODULE_PARM_DESC(pageblock_order_val,
	"pageblock_order of the running kernel (from /proc/pagetypeinfo preflight); -1 = CMA reservoir off");

/*
 * Headroom floor for FLIPPING blocks to CMA: every flip moves a pageblock from
 * the everyone-budget to the movable-only budget, and the kernel's unmovable
 * working set (~3.5G resident on these devices) cannot follow it there.
 * Flip too far and unmovable allocations starve while "MemAvailable" still looks
 * fine. So: refuse a flip once (si_mem_available - free CMA pages) would drop
 * under this. This is a DIFFERENT brake from acquire_mem_floor_mb (which stops
 * the sweep before reclaim livelocks); both apply, each guarding its own path.
 */
static unsigned int cma_reservoir_floor_mb = 1024;
module_param(cma_reservoir_floor_mb, uint, 0600);
MODULE_PARM_DESC(cma_reservoir_floor_mb,
	"Refuse to flip more pageblocks to CMA when non-CMA available memory would drop below this many MB (default 1024)");

/* ---- Pool ---- */

static struct page *page_pool[POOL_SIZE_MAX];
static atomic_t pool_count = ATOMIC_INIT(0);
static int pool_total;	/* current CAPACITY: pages we actually hold (avail +
			 * lent-out). Refill only ever chases this, so it must
			 * never exceed what we've actually obtained. The free
			 * hook re-pools against pool_want (the CURRENT target)
			 * instead and raises this high-water as pages return -
			 * so shrinking the target never permanently disowns
			 * pages that are lent out at the time. */
static int pool_want = 1024;	/* the single TARGET knob: set at insmod and at
				 * runtime via the pool_want sysfs. init allocates
				 * toward it; acquire raises capacity toward it. */
static int pool_want_with_cma;	/* v10 TOTAL guardianship target in 2MB pages:
				 * held pool (pool_want of it) + CMA reservoir
				 * (the rest). 0 (default) = reservoir feature
				 * off, exact v9 behavior. Invariant everywhere:
				 * pool_want <= pool_want_with_cma <= pool_size_max
				 * (0 is the disable sentinel, exempt). Set at
				 * insmod and at runtime via its sysfs. */
static int pool_size_max = POOL_SIZE_MAX;	/* effective cap on pool_want/capacity,
						 * computed from system RAM at insmod
						 * (see hugepage_reserve_init); every
						 * pool_want write site clamps to it. */
module_param(pool_size_max, int, 0400);
MODULE_PARM_DESC(pool_size_max,
	"Effective pool cap in 2MB pages: min(ram - min(ram/2, 6G), 24G); read-only");
static bool pool_ready;		/* true once module_init has built the pool */
static DEFINE_RAW_SPINLOCK(pool_lock);

/* ---- CMA reservoir state (v10) ----
 *
 * cma_blocks[] is the ONE persistent piece of reservoir state: the base pfn of
 * every pageblock that is currently ours-and-CMA. Everything else about the
 * feature is either a parameter, derived on demand, or rebuildable - no origin
 * tracking, no history masks. stage-in removes an entry, flip/build adds one,
 * write-small demolition and exit restore-and-remove; that is the full
 * lifecycle. Each entry is >= 2MB, so the count is bounded by POOL_SIZE_MAX.
 *
 * All mutation happens in process context under cma_mutex (build, demolition,
 * verification, exit). Readers that only want the count (stats) read
 * cma_blocks_n unlocked. The free hook NEVER touches this state: migratetype
 * flips and frees of reservoir pages are process-context-only by design (S9
 * invariant), so the hook's atomic context stays exactly as cheap as v9.
 */
static unsigned long cma_blocks[POOL_SIZE_MAX];
static int cma_blocks_n;
static u16 cma_est[POOL_SIZE_MAX];	/* per-block free-page estimates, parallel
					 * to cma_blocks[]; scratch for demolition
					 * and stage-in ordering (cma_mutex) */
static DEFINE_MUTEX(cma_mutex);
static int cma_pb_order = -1;	/* validated runtime pageblock order */
static bool cma_capable;	/* settled ONCE at init: symbols + preflight
				 * params all present AND the first-block
				 * verification passed on live memory. A boot
				 * constant - there is deliberately no lazy /
				 * first-run runtime state (v10 rev.). */
/* C4 tripwire: pages whose pageblock reads CMA that tried to enter the pool.
 * Normally constant 0 - the process-context-only flip rule plus the free-hook
 * gate guarantee no reservoir page ever reaches a pool entry point. Nonzero
 * means a logic bug upstream; the page itself is refused (it drains to buddy
 * and lands on the CMA freelist, which is its correct home either way). */
static atomic_long_t dbg_cma_leak;

/* Pages per pageblock / 2MB sub-blocks per pageblock, at the RUNTIME order.
 * Only meaningful once cma_capable; order-9 devices give SUBBLKS = 1 and every
 * pairing/exchange mechanism degenerates to a no-op. */
#define CMA_PB_NR	(1UL << cma_pb_order)
#define CMA_SUBBLKS	(1 << (cma_pb_order - PAGE_ORDER))

/* Reservoir size in 2MB-page equivalents (the unit every other pool number
 * uses), derived from the block count - never stored separately. Gated on the
 * validated order, NOT on cma_capable: a systemic verification failure clears
 * cma_capable but may leave committed blocks behind, and those must keep
 * counting (for the GUI and for demolition targets) until demolished. */
static int cma_pool_cma_2mb(void)
{
	if (READ_ONCE(cma_pb_order) < PAGE_ORDER)
		return 0;
	return READ_ONCE(cma_blocks_n) * CMA_SUBBLKS;
}

/* Round a 2MB-page target UP to whole pageblocks (§2: both wants are aligned
 * while the feature is on; no-op on order-9 devices), stepping back DOWN if
 * the round-up crossed the pool cap. */
static int cma_align_2mb(int v)
{
	int sub;

	if (!cma_capable)
		return v;
	sub = CMA_SUBBLKS;
	if (sub <= 1)
		return v;
	v = roundup(v, sub);
	if (v > pool_size_max)
		v = rounddown(pool_size_max, sub);
	return v;
}

/* pool_want raised past pool_want_with_cma: the total target follows (§5) -
 * legacy management apps only write pool_want, and this keeps that one knob
 * driving the whole elastic loop (grow -> with_cma follows -> acquire fills;
 * shrink -> excess flips back to reservoir). Only while enabled: 0 is the
 * disable sentinel and must never be pulled up. */
static void cma_want_follow(int want)
{
	int wc = READ_ONCE(pool_want_with_cma);

	if (wc > 0 && want > wc)
		WRITE_ONCE(pool_want_with_cma, cma_align_2mb(want));
}

/* External-source gate - defined with the reservoir engine below; used by
 * every external pool filler (refill worker, acquire loops). */
static bool cma_external_ok(void);

/* ================================================================== */
/*  pb-hash: pageblock-completeness index (v10 §3, SUBBLKS > 1 only)  */
/*                                                                    */
/*  A DERIVED index over every 2MB sub-block the module guards,       */
/*  grouped by pageblock: which siblings sit in the avail pool, which */
/*  are lent out, which are parked in limbo. Everything it feeds      */
/*  (pairing, exchange, shrink classes, cma_able stats) is an         */
/*  OPTIMIZATION - the index can be rebuilt at any time from the pool */
/*  array + served table + limbo pool, and losing an entry (overflow) */
/*  degrades pairing quality, never correctness. Not populated at all */
/*  on SUBBLKS == 1 devices, where every sub-block IS its pageblock.  */
/* ================================================================== */

#define PB_HASH_MAX	16384U		/* power of two; u16 chain indexes */
#define PB_NULL		0xFFFFU

#define PB_AVAIL	0x1		/* sub-block sits in the avail pool */
#define PB_SERVED	0x2		/* sub-block lent to a VM */
#define PB_LIMBO	0x4		/* sub-block parked in the limbo pool */

struct pb_node {
	unsigned long	pb;		/* key: pfn >> cma_pb_order */
	u8		avail_mask;
	u8		served_mask;
	u8		limbo_mask;
	u16		next;		/* freelist or bucket chain index */
};

static struct pb_node pb_nodes[PB_HASH_MAX];
static u16 pb_bucket[PB_HASH_MAX];
static u16 pb_free_head = PB_NULL;
static int pb_count, pb_overflow;
static int pb_full_avail;	/* entries whose avail_mask covers the whole
				 * pageblock = groups flippable to CMA now
				 * (pool_avail_cma_able, exchange capacity) */
/* LEAF raw lock: mask updates happen from under pool_lock, served_lock and
 * limbo_lock holders (both lock domains touch the same masks - §3), so this
 * lock must never wrap any of them. It never does: every pb_* helper takes
 * only pb_lock. */
static DEFINE_RAW_SPINLOCK(pb_lock);

static inline bool pb_enabled(void)
{
	return cma_capable && cma_pb_order > PAGE_ORDER;
}

static inline u8 pb_full_mask(void)
{
	return (u8)((1U << CMA_SUBBLKS) - 1);
}

static inline unsigned int pb_hash(unsigned long pb)
{
	return (unsigned int)pb & (PB_HASH_MAX - 1);
}

static inline u8 pb_bit_of(unsigned long pfn)
{
	return (u8)(1U << ((pfn >> PAGE_ORDER) & (CMA_SUBBLKS - 1)));
}

/* Inline byte popcount: hweight8 emits an out-of-line __sw_hweight8 libcall
 * on these toolchains - a new undefined symbol the ABI check forbids. */
static inline unsigned int pb_popcount8(u8 v)
{
	v = v - ((v >> 1) & 0x55);
	v = (v & 0x33) + ((v >> 2) & 0x33);
	return (v + (v >> 4)) & 0x0f;
}

/* pb_lock held. Returns node index or PB_NULL; *prevp (may be NULL) gets the
 * chain predecessor for unlinking (collisions resolved by exact key compare,
 * same scheme as the served table). */
static u16 pb_find_locked(unsigned long pb, u16 *prevp)
{
	unsigned int b = pb_hash(pb);
	u16 n, prev = PB_NULL;

	for (n = pb_bucket[b]; n != PB_NULL; prev = n, n = pb_nodes[n].next)
		if (pb_nodes[n].pb == pb)
			break;
	if (prevp)
		*prevp = prev;
	return n;
}

static void pb_reset_locked(void)
{
	unsigned int i;

	for (i = 0; i < PB_HASH_MAX; i++) {
		pb_nodes[i].pb = 0;
		pb_nodes[i].avail_mask = 0;
		pb_nodes[i].served_mask = 0;
		pb_nodes[i].limbo_mask = 0;
		pb_nodes[i].next = (i + 1 < PB_HASH_MAX) ? (u16)(i + 1) : PB_NULL;
		pb_bucket[i] = PB_NULL;
	}
	pb_free_head = 0;
	pb_count = 0;
	pb_full_avail = 0;
}

/*
 * THE mask mutation entry point: move @pfn's sub-block bit between tracking
 * sets (@set / @clear are PB_AVAIL/PB_SERVED/PB_LIMBO combos). All-bits-empty
 * frees the node. Atomic-legal: raw leaf lock, static freelist, no
 * allocation. Maintenance points (§3): serve (avail->served), hook reclaim
 * (served->avail), hook escape (served->gone), reacquire (served->avail),
 * reconcile purge (served->gone - the easy one to miss), pool entry (->avail),
 * pool drain (avail->gone), limbo in/out.
 */
static void pb_track(unsigned long pfn, int set, int clear)
{
	unsigned long flags, pb;
	u16 n, prev;
	u8 bit, old_avail, full;

	if (!pb_enabled())
		return;
	pb = pfn >> cma_pb_order;
	bit = pb_bit_of(pfn);

	raw_spin_lock_irqsave(&pb_lock, flags);
	n = pb_find_locked(pb, &prev);
	if (n == PB_NULL) {
		unsigned int b;

		if (!set)
			goto out;	/* clearing an untracked block: no-op */
		if (pb_free_head == PB_NULL) {
			pb_overflow++;	/* index full: this sub-block's tracking
					 * is lost until the next pb_rebuild */
			goto out;
		}
		n = pb_free_head;
		pb_free_head = pb_nodes[n].next;
		b = pb_hash(pb);
		pb_nodes[n].pb = pb;
		pb_nodes[n].avail_mask = 0;
		pb_nodes[n].served_mask = 0;
		pb_nodes[n].limbo_mask = 0;
		pb_nodes[n].next = pb_bucket[b];
		pb_bucket[b] = n;
		pb_count++;
		prev = PB_NULL;		/* inserted at bucket head */
	}
	old_avail = pb_nodes[n].avail_mask;
	if (set & PB_AVAIL)
		pb_nodes[n].avail_mask |= bit;
	if (set & PB_SERVED)
		pb_nodes[n].served_mask |= bit;
	if (set & PB_LIMBO)
		pb_nodes[n].limbo_mask |= bit;
	if (clear & PB_AVAIL)
		pb_nodes[n].avail_mask &= ~bit;
	if (clear & PB_SERVED)
		pb_nodes[n].served_mask &= ~bit;
	if (clear & PB_LIMBO)
		pb_nodes[n].limbo_mask &= ~bit;

	full = pb_full_mask();
	if (old_avail == full && pb_nodes[n].avail_mask != full)
		pb_full_avail--;
	else if (old_avail != full && pb_nodes[n].avail_mask == full)
		pb_full_avail++;

	if (!(pb_nodes[n].avail_mask | pb_nodes[n].served_mask |
	      pb_nodes[n].limbo_mask)) {
		if (prev == PB_NULL)
			pb_bucket[pb_hash(pb)] = pb_nodes[n].next;
		else
			pb_nodes[prev].next = pb_nodes[n].next;
		pb_nodes[n].next = pb_free_head;
		pb_free_head = n;
		pb_count--;
	}
out:
	raw_spin_unlock_irqrestore(&pb_lock, flags);
}

/* Snapshot @pfn's pageblock masks (zeros when untracked). Any context. */
static void pb_peek(unsigned long pfn, u8 *avail, u8 *served, u8 *limbo)
{
	unsigned long flags;
	u16 n;

	*avail = *served = *limbo = 0;
	if (!pb_enabled())
		return;
	raw_spin_lock_irqsave(&pb_lock, flags);
	n = pb_find_locked(pfn >> cma_pb_order, NULL);
	if (n != PB_NULL) {
		*avail = pb_nodes[n].avail_mask;
		*served = pb_nodes[n].served_mask;
		*limbo = pb_nodes[n].limbo_mask;
	}
	raw_spin_unlock_irqrestore(&pb_lock, flags);
}

/* ---- Limbo pool (§3 待配池): held order-9 compounds that cannot be
 * served (not counted avail) and are waiting for their siblings, capped
 * small. Outcomes run at existing process-context trigger points (acquire,
 * resize) - no new worker: block completed -> whole flip to CMA (deficit) or
 * seats refilled; grown old -> genuinely freed. ---- */

#define LIMBO_MAX	64
static struct page *limbo_pages[LIMBO_MAX];
static u8  limbo_age[LIMBO_MAX];	/* process passes survived */
static int limbo_n;
static DEFINE_RAW_SPINLOCK(limbo_lock);	/* leaf; may nest inside pool_lock */

/* Any context. Caller does the pb_track(PB_LIMBO) bookkeeping. */
static bool limbo_add(struct page *page)
{
	unsigned long flags;
	bool ok = false;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (limbo_n < LIMBO_MAX) {
		limbo_pages[limbo_n] = page;
		limbo_age[limbo_n] = 0;
		limbo_n++;
		ok = true;
	}
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return ok;
}

/* Remove and return the limbo entry at @idx (swap-with-last), NULL if gone.
 * Caller does the pb_track(0, PB_LIMBO) bookkeeping. */
static struct page *limbo_del_idx(int idx)
{
	unsigned long flags;
	struct page *p = NULL;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (idx >= 0 && idx < limbo_n) {
		p = limbo_pages[idx];
		limbo_pages[idx] = limbo_pages[limbo_n - 1];
		limbo_age[idx] = limbo_age[limbo_n - 1];
		limbo_n--;
	}
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return p;
}

/* Avail pool pages that could flip to CMA as WHOLE pageblocks right now.
 * SUBBLKS == 1: every avail page is a whole, aligned pageblock, so this is
 * simply pool_avail. SUBBLKS > 1: full-avail pb-hash entries x SUBBLKS. */
static int cma_avail_cma_able_2mb(void)
{
	if (!cma_capable)
		return 0;
	if (CMA_SUBBLKS == 1)
		return atomic_read(&pool_count);
	return READ_ONCE(pb_full_avail) * CMA_SUBBLKS;
}

/* ---- Probe state ---- */

static bool hook_active;
static bool detect_active;
static bool vm_destroy_active;
static bool free_intercept_active;
static int reclaim_want = 1;	/* desired free-hook state (insmod + runtime) */
static DEFINE_MUTEX(hook_mutex);

/* ---- Statistics and VM tracking ---- */

enum refill_state { REFILL_IDLE = 0, REFILL_WAITING, REFILL_RUNNING };

static atomic_t refill_status = ATOMIC_INIT(REFILL_IDLE);
static atomic_t total_served = ATOMIC_INIT(0);
static atomic_t total_refilled = ATOMIC_INIT(0);

/* ---- VM owner mm tracking ---- */

/*
 * Track mm_structs of processes that create Gunyah VMs.
 * The kprobe on the VM creation ioctl guarantees only actual Gunyah
 * users are tracked - no process name filtering needed.
 *
 * The kretprobe on __alloc_pages checks current->mm against this set
 * to avoid serving pool pages to unrelated order-9 allocations.
 *
 * Owners are released one at a time by the sweep worker, and only when
 * they are actually gone: process dead (mm_users == 0) or last VM closed
 * (vm_count == 0). Never clear the whole table on a destroy event - with
 * several VM processes, one dying (e.g. OOM kill) must not drop the
 * tracking of the survivors.
 *
 * Slots are reused in place (mm == NULL marks a free slot) so the
 * lockless readers never see entries move; vm_owner_count is the
 * high-water slot count.
 */
#define VM_OWNER_MAX	8

/*
 * One tracked Gunyah VM owner. tgid/comm are captured at GH_CREATE_VM time
 * so userspace can attribute pool pages to a concrete process (and, in the
 * app, to a VM). served counts the pool pages actually handed to this owner;
 * vm_count is the number of VMs this mm has created and not yet destroyed.
 */
struct vm_owner {
	struct mm_struct *mm;
	pid_t		  tgid;
	char		  comm[TASK_COMM_LEN];
	atomic_t	  served;
	int		  vm_count;
};

static struct vm_owner vm_owners[VM_OWNER_MAX];
static atomic_t vm_owner_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(vm_owner_lock);
static struct work_struct vm_owner_sweep_work;

static void vm_owner_add(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count, slot = -1;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);

	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm == mm) {
			vm_owners[i].vm_count++;
			goto out;
		}
		if (!vm_owners[i].mm && slot < 0)
			slot = i;	/* swept slot: reusable */
	}
	if (slot < 0 && count < VM_OWNER_MAX)
		slot = count;
	if (slot >= 0) {
		mmgrab(mm);
		vm_owners[slot].tgid = current->tgid;
		strscpy(vm_owners[slot].comm, current->comm,
			sizeof(vm_owners[slot].comm));
		atomic_set(&vm_owners[slot].served, 0);
		vm_owners[slot].vm_count = 1;
		/* mm gates the entry for lockless readers: publish it last */
		smp_store_release(&vm_owners[slot].mm, mm);
		if (slot == count)
			smp_store_release(&vm_owner_count.counter, count + 1);
		pr_info("tracking VM owner mm=%px pid=%d (comm=%s)\n",
			mm, current->tgid, current->comm);
	} else {
		pr_warn("too many VM owners (%d), ignoring %s\n",
			VM_OWNER_MAX, current->comm);
		schedule_work(&vm_owner_sweep_work);
	}
out:
	spin_unlock_irqrestore(&vm_owner_lock, flags);
}

/* Drop one VM from the owner matching @mm (explicit close by a live process). */
static void vm_owner_vm_dec(struct mm_struct *mm)
{
	unsigned long flags;
	int i, count;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm == mm) {
			if (vm_owners[i].vm_count > 0)
				vm_owners[i].vm_count--;
			break;
		}
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
}

/*
 * Drop one VM from the SOLE live owner - for gunyah-vm releases that arrive
 * without an mm to attribute by (delayed_fput from a kworker, late exit path).
 * Without this, a release the mm-path can't attribute leaves the owner's
 * vm_count inflated forever on a still-live process: the sweep only releases
 * owners with vm_count == 0 or mm_users == 0, so the slot stays pinned until
 * the process dies, and 8 such owners exhaust the table (new VMs untracked).
 * With exactly one live owner the attribution is unambiguous; with several,
 * decrementing a guess could drop a live VM's tracking, so we skip and return
 * false - the caller logs it and the mm_users sweep remains the GC of last
 * resort.
 */
static bool vm_owner_vm_dec_sole(void)
{
	unsigned long flags;
	int i, count, live = 0, idx = -1;
	bool ok = false;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (vm_owners[i].mm) {
			live++;
			idx = i;
		}
	}
	if (live == 1) {
		if (vm_owners[idx].vm_count > 0)
			vm_owners[idx].vm_count--;
		ok = true;
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
	return ok;
}

static bool vm_owner_contains(struct mm_struct *mm)
{
	int i, count;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owners[i].mm) == mm)
			return true;
	}
	return false;
}

/* Charge a served pool page to the owner matching @mm (best-effort). */
static void vm_owner_served_inc(struct mm_struct *mm)
{
	int i, count;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owners[i].mm) == mm) {
			atomic_inc(&vm_owners[i].served);
			return;
		}
	}
}

/* Live VMs = sum of per-owner vm_count (lockless, for stats/logs). */
static int vm_active_total(void)
{
	int i, count, total = 0;

	count = smp_load_acquire(&vm_owner_count.counter);
	for (i = 0; i < count; i++) {
		if (READ_ONCE(vm_owners[i].mm))
			total += READ_ONCE(vm_owners[i].vm_count);
	}
	return total;
}

/* ================================================================== */
/*  Served-page table                                                  */
/*                                                                     */
/*  Records every pool page handed to a guest (pfn -> owner tgid) so   */
/*  outstanding pages can be *located/attributed*, not just counted.   */
/*  Maintained from atomic probe context -> fully preallocated, no     */
/*  allocation on the hot path (chaining hash over a node freelist).   */
/*  Bounded: outstanding pages <= pool_total, so SERVED_MAX is plenty. */
/* ================================================================== */

#define SERVED_MAX	16384U		/* power of two, >= POOL_SIZE_MAX; u16
					 * node indexes cap this at 32768 */
#define SERVED_NULL	0xFFFFU

struct served_node {
	unsigned long	pfn;
	pid_t		tgid;
	u16		next;		/* freelist or bucket chain index */
};

static struct served_node served_nodes[SERVED_MAX];
static u16  served_bucket[SERVED_MAX];
static u16  served_free_head;
static int  served_count;
static int  served_overflow;
static DEFINE_RAW_SPINLOCK(served_lock);

/* Jiffies of the most recent VM-destroy detection. reconcile refuses to PURGE
 * orphan entries inside the grace window that follows: the dead VM's frees are
 * still in flight (including order-9 folios parked in pcp lists), and purging
 * an entry before its free arrives makes the free hook miss it - the page then
 * drains to buddy instead of returning to the pool. This is exactly the race
 * the DroidVM panel's 2s reconcile cadence kept hitting on SIGKILLed crosvm
 * (owner released 46ms after the kill -> whole table counted as orphans ->
 * mid-teardown reconcile discarded entries whose frees arrived moments later).
 * Init sets this to "grace already expired" so boot-time reconciles purge. */
static unsigned long last_destroy_jiffies;
#define RECONCILE_GRACE_MS	10000

/* Last reconcile result + per-owner live tallies (guarded by recon_mutex). */
static DEFINE_MUTEX(recon_mutex);
static int   recon_live, recon_orphan_freed, recon_orphan_inuse;
static int   recon_owner_n;
static pid_t recon_owner_tgid[VM_OWNER_MAX];
static char  recon_owner_comm[VM_OWNER_MAX][TASK_COMM_LEN];
static long  recon_owner_pages[VM_OWNER_MAX];

static void served_init(void)
{
	unsigned int i;

	for (i = 0; i < SERVED_MAX; i++) {
		served_nodes[i].next = (i + 1 < SERVED_MAX) ? (u16)(i + 1) : SERVED_NULL;
		served_bucket[i] = SERVED_NULL;
	}
	served_free_head = 0;
	served_count = 0;
	served_overflow = 0;
}

static inline unsigned int served_hash(unsigned long pfn)
{
	return (unsigned int)(pfn >> PAGE_ORDER) & (SERVED_MAX - 1);
}

/* served_lock must be held. Records @pfn's current holder @tgid (updates it if
 * the entry already exists). Only reached from pool_serve() after it has already
 * verified a free slot exists, so the overflow branch is a belt-and-suspenders. */
static void served_add_locked(unsigned long pfn, pid_t tgid)
{
	unsigned int b = served_hash(pfn);
	u16 n, idx;

	for (n = served_bucket[b]; n != SERVED_NULL; n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
			served_nodes[n].tgid = tgid;	/* re-served: update owner */
			return;
		}
	}
	if (served_free_head == SERVED_NULL) {
		served_overflow++;
		return;
	}
	idx = served_free_head;
	served_free_head = served_nodes[idx].next;
	served_nodes[idx].pfn = pfn;
	served_nodes[idx].tgid = tgid;
	served_nodes[idx].next = served_bucket[b];
	served_bucket[b] = idx;
	served_count++;
}

/* Returns true if @pfn was found in the served table and removed. */
static bool served_del(unsigned long pfn)
{
	unsigned long flags;
	unsigned int b;
	u16 n, prev = SERVED_NULL;
	bool found = false;

	raw_spin_lock_irqsave(&served_lock, flags);
	b = served_hash(pfn);
	for (n = served_bucket[b]; n != SERVED_NULL;
	     prev = n, n = served_nodes[n].next) {
		if (served_nodes[n].pfn == pfn) {
			if (prev == SERVED_NULL)
				served_bucket[b] = served_nodes[n].next;
			else
				served_nodes[prev].next = served_nodes[n].next;
			served_nodes[n].next = served_free_head;
			served_free_head = n;
			served_count--;
			found = true;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);
	/* Every served_del caller means "this sub-block stops being lent out"
	 * (hook match, reacquire); whether it becomes avail again is the
	 * caller's pool-entry step (which sets PB_AVAIL there). */
	if (found)
		pb_track(pfn, 0, PB_SERVED);
	return found;
}

/* Physical scavenger for hook-missed frees - defined after the pool helpers
 * it needs; called from the pcp-drain worker and from reconcile below. */
static int served_reacquire_free_orphans(void);

/*
 * Reconcile (process context only): drop every tracked page whose owner is no
 * longer a currently-tracked VM owner. Each dropped (orphan) page is classified
 * by whether its refcount is now 0 (returned to the system) or not (reused
 * elsewhere). Surviving entries are tallied per live owner.
 */
static void served_do_reconcile(void)
{
	unsigned long flags;
	pid_t live_tgid[VM_OWNER_MAX];
	char  live_comm[VM_OWNER_MAX][TASK_COMM_LEN];
	long  live_pages[VM_OWNER_MAX];
	int   live_n, i, b;
	int   live = 0, of = 0, oi = 0;
	/* Purge orphans only once the post-destroy grace window has passed -
	 * inside it their frees are still arriving and entries must survive so
	 * the free hook can match them (see last_destroy_jiffies). */
	bool  purge = time_after(jiffies, READ_ONCE(last_destroy_jiffies) +
				 msecs_to_jiffies(RECONCILE_GRACE_MS));

	/* Physically recover hook-missed frees FIRST, so a recoverable page is
	 * neither tallied as an orphan nor purged below once grace is over. */
	served_reacquire_free_orphans();

	spin_lock_irqsave(&vm_owner_lock, flags);
	{
		int count = atomic_read(&vm_owner_count);

		if (count > VM_OWNER_MAX)
			count = VM_OWNER_MAX;
		live_n = 0;
		for (i = 0; i < count; i++) {
			if (!vm_owners[i].mm)
				continue;	/* swept slot */
			live_tgid[live_n] = vm_owners[i].tgid;
			strscpy(live_comm[live_n], vm_owners[i].comm, TASK_COMM_LEN);
			live_pages[live_n] = 0;
			live_n++;
		}
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);

	raw_spin_lock_irqsave(&served_lock, flags);
	for (b = 0; b < SERVED_MAX; b++) {
		u16 prev = SERVED_NULL, n = served_bucket[b];

		while (n != SERVED_NULL) {
			int owner = -1;
			u16 nx;

			for (i = 0; i < live_n; i++) {
				if (live_tgid[i] == served_nodes[n].tgid) {
					owner = i;
					break;
				}
			}
			if (owner >= 0) {
				live++;
				live_pages[owner]++;
				prev = n;
				n = served_nodes[n].next;
				continue;
			}
			/* orphan: owner is no longer a tracked VM */
			{
				unsigned long pfn = served_nodes[n].pfn;
				struct page *pg = pfn_valid(pfn) ? pfn_to_page(pfn) : NULL;

				if (pg && page_count(pg) == 0)
					of++;
				else
					oi++;
			}
			if (!purge) {		/* grace window: count, keep entry */
				prev = n;
				n = served_nodes[n].next;
				continue;
			}
			nx = served_nodes[n].next;
			if (prev == SERVED_NULL)
				served_bucket[b] = nx;
			else
				served_nodes[prev].next = nx;
			/* purge = this sub-block stops being "served" without
			 * ever re-entering the pool: the pb-hash must drop the
			 * bit too or the block reads more complete than it is
			 * (§3 - the maintenance point that is easy to miss). */
			pb_track(served_nodes[n].pfn, 0, PB_SERVED);
			served_nodes[n].next = served_free_head;
			served_free_head = n;
			served_count--;
			n = nx;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);

	recon_live = live;
	recon_orphan_freed = of;
	recon_orphan_inuse = oi;
	recon_owner_n = live_n;
	for (i = 0; i < live_n; i++) {
		recon_owner_tgid[i] = live_tgid[i];
		strscpy(recon_owner_comm[i], live_comm[i], TASK_COMM_LEN);
		recon_owner_pages[i] = live_pages[i];
	}

	/* Log whenever orphans were seen: a PURGING reconcile discards their
	 * pending frees (the hook can no longer match them), so who ran it, and
	 * when, matters when auditing lost pages. The all-live case stays quiet -
	 * the DroidVM usage panel reconciles every ~2s and would spam dmesg. */
	if (of || oi)
		pr_info("reconcile by %s[%d]: live=%d orphan_freed=%d orphan_inuse=%d%s\n",
			current->comm, task_pid_nr(current), live, of, oi,
			purge ? "" : " (grace: kept)");
}

/* ---- Work and probe structs ---- */

static struct delayed_work refill_work;
static struct delayed_work pcp_drain_work;
#define PCP_DRAIN_DELAY_MS	3000	/* after the teardown free burst settles */
static struct kretprobe kretp;
static struct kprobe vm_detect_kp;
static struct kprobe vm_destroy_kp;

/* ---- Aggressive acquire (GUI-only): alloc_contig_pages migrates to build
 *      2MB blocks even under fragmentation. Neither helper is reliably exported:
 *      alloc_contig_pages is never exported, and prep_compound_page is only
 *      EXPORT_SYMBOL_GPL on some vendor trees (e.g. this 6.6 GKI) while merely
 *      declared in the private mm/internal.h on others (e.g. ACK 6.1). Both are
 *      non-static (hence in kallsyms), so resolve both via kprobe at init - a
 *      missing export must not fail module load, only disable acquire. */
static struct work_struct acquire_work;
static atomic_t acquire_running = ATOMIC_INIT(0);
static int acquire_mode;	/* algorithm the running worker uses: 1=original, 2=smart */
/* Why the last user-triggered acquire stopped - surfaced in refill_stat as
 * acquire_stop_reason= so the GUI can tell the user why the button's work ended
 * (target met, ran out of assemblable memory, hit the floor, interrupted...)
 * instead of them digging through dmesg. Points at a static string literal, so
 * the pointer read/write needs no lock. Set ONLY by the acquire path, never the
 * periodic refill worker. */
static const char *acquire_stop_reason = "idle";
/*
 * Unified kernel-API object - the module's OWN stable ABI over the kernel.
 * Its purpose is to hide every kernel-version difference (symbol names,
 * signatures, argument and return-value SEMANTICS) behind high-level
 * operations that mean the same thing on every supported kernel: call sites
 * just do kapi.op(...) and never see the raw kernel function. Every field is
 * named k_<kernel symbol> - the suffix keeps the backing API recognizable at
 * a glance, and the k_ does two jobs: it marks that the field's prototype and
 * semantics are the module-NORMALIZED contract (the per-field comments below),
 * not necessarily the running kernel's, and it keeps the identifier from ever
 * colliding with a kernel function-like macro (on 6.10+ alloc_contig_{range,
 * pages} are alloc_hooks() macros that would hijack a bare-named field call).
 * Where the kernel contract diverges across versions a shim settles it, e.g.
 * kapi.k_folio_isolate_lru returns bool true=isolated on EVERY kernel even
 * though the 6.1 symbol returns int 0=isolated, and kapi.k_alloc_contig_range
 * takes no migratetype/acr_flags argument at all.
 *
 * Resolution happens once at init (kapi_init, via kprobe). Symbols whose
 * contract is version-stable are stored directly. The four whose contract
 * diverges across GKI bases resolve into the private `kraw` struct instead
 * (prototypes version-gated to match the running kernel - kCFI checks the
 * indirect-call type id) and the kapi field gets a shim that settles the
 * divergence in ONE place:
 *   folio_isolate_lru: 6.1/6.2 return int (0 = isolated); 6.3+
 *     (be2d5756382a) return bool (true = isolated) - a bare truth test would
 *     invert on 6.1, leaking isolated folios off-LRU with the isolation ref
 *     held and list_add-ing folios that are still on the LRU
 *   reclaim_pages: 6.6..6.12 take (list, ignore_refs); others (list)
 *   try_to_free_mem_cgroup_pages: 5.10/5.15 (memcg,nr,gfp,bool may_swap);
 *     6.1/6.6 (…,uint reclaim_options); 6.12+ (…,+int *swappiness)
 *   alloc_contig_range: same C signature everywhere (no CFI difference), but
 *     arg 3 changed meaning: migratetype (<=6.12: pass MIGRATE_MOVABLE) became
 *     acr_flags_t (upstream ~6.16, next GKI base 6.18: pass ACR_FLAGS_NONE=0;
 *     MIGRATE_MOVABLE=1 would alias ACR_FLAGS_CMA and silently run the sweep
 *     in CMA isolation mode), so the shim owns arg 3 and callers never pass it
 *
 * A missing or guard-disabled symbol leaves BOTH the kraw and kapi pointers
 * NULL; callers then either refuse a whole feature (required op - see the
 * kapi_can_* checklists) or degrade (optional enhancer - guarded at the call
 * site).
 */
struct mem_cgroup;
struct folio;
struct kapi {
	/* --- assembly primitives (required for the acquire mode that uses each) --- */
	struct page *(*k_alloc_contig_pages)(unsigned long nr_pages, gfp_t gfp_mask,
			int nid, nodemask_t *nodemask);	/* id=1: contig run, anywhere */
	int (*k_alloc_contig_range)(unsigned long start, unsigned long end,
			gfp_t gfp_mask);	/* id=2/3 sweep: movable, non-CMA */
	void (*k_prep_compound_page)(struct page *page, unsigned int order);
	/* --- per-block evict B: required for id=3 (madvise(MADV_PAGEOUT) path) --- */
	bool (*k_folio_isolate_lru)(struct folio *folio);	/* true = isolated off-LRU */
	unsigned long (*k_reclaim_pages)(struct list_head *list);
	/* --- CMA reservoir (v10): required as a set, see kapi_can_cma() --- */
	void (*k_set_pageblock_migratetype)(struct page *page, int migratetype);
	/* Read a pageblock's migratetype label (mask 0x7). MUST be the kernel's
	 * copy, never the module's inline get/set: those compute the bit index
	 * from the BUILD kernel's pageblock_order, which lands on the wrong bits
	 * whenever build != device order (the exact case pageblock_order_val
	 * exists for). Resolved only on [6.1, 6.16). */
	unsigned long (*k_get_pfnblock_flags_mask)(const struct page *page,
			unsigned long pfn, unsigned long mask);
	/* CMA-mode contig grab (isolates/undoes with the CMA migratetype, like
	 * cma_alloc does). ONLY legal inside blocks whose label is already CMA -
	 * ours via cma_blocks[] - because the isolation undo rewrites the label
	 * with the mode's migratetype. */
	int (*k_alloc_contig_range_cma)(unsigned long start, unsigned long end,
			gfp_t gfp_mask);
	/* --- optional enhancers: NULL just degrades the feature, never disables --- */
	void (*k_lru_add_drain_all)(void);	/* accurate PageLRU for the gate */
	void (*k_drop_slab)(void);		/* unpoison slab-held windows */
	void (*k_drain_all_pages)(struct zone *);	/* flush pcp so parked order-9
						 * frees reach the reclaim hook */
	struct mem_cgroup *(*k_mem_cgroup_from_task)(struct task_struct *p);
	unsigned long (*k_try_to_free_mem_cgroup_pages)(struct mem_cgroup *memcg,
			unsigned long nr);	/* GFP_KERNEL, swap allowed */
};
static struct kapi kapi;

static struct zone *acq_zone;		/* zone the pool pages live in (cached at init) */
static unsigned long scan_cursor;	/* persistent pfn cursor for the smart scan (id=2) */

/* Raw kernel symbols behind the shims: version-TRUE prototypes (kCFI matches
 * the indirect call against the kernel function's type id), resolved by
 * kapi_init, and never called from anywhere except the kapi_*_shim functions
 * directly below - call sites use only the normalized kapi.op pointers. */
static struct {
	int (*k_alloc_contig_range)(unsigned long start, unsigned long end,
			unsigned int migratetype_or_acr, gfp_t gfp_mask);
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

static int kapi_alloc_contig_range_shim(unsigned long start, unsigned long end,
					gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 0 /* ACR_FLAGS_NONE */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end, MIGRATE_MOVABLE, gfp_mask);
#endif
}

/* CMA-mode grab: arg 3 is the runtime MIGRATE_CMA value on <= 6.12 (our
 * build-time MIGRATE_CMA may not match the device kernel's - that is why the
 * preflight supplies it), ACR_FLAGS_CMA on the >= 6.16 acr_flags_t contract.
 * The >= 6.16 branch is dormant today (the whole feature compile-gates off
 * there because the 6.16 migratetype rework changed pageblock-flag semantics
 * and 6.18 made the setter static), but the shim stays correct if that gate
 * is ever relaxed. */
static int kapi_alloc_contig_range_cma_shim(unsigned long start, unsigned long end,
					    gfp_t gfp_mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	return kraw.k_alloc_contig_range(start, end, 1 /* ACR_FLAGS_CMA */, gfp_mask);
#else
	return kraw.k_alloc_contig_range(start, end,
					 (unsigned int)READ_ONCE(migrate_cma_val),
					 gfp_mask);
#endif
}

static bool kapi_folio_isolate_lru_shim(struct folio *folio)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	return kraw.k_folio_isolate_lru(folio) == 0;
#else
	return kraw.k_folio_isolate_lru(folio);
#endif
}

static unsigned long kapi_reclaim_pages_shim(struct list_head *list)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 12, 0)
	return kraw.k_reclaim_pages(list, true);
#else
	return kraw.k_reclaim_pages(list);
#endif
}

static unsigned long kapi_try_to_free_memcg_shim(struct mem_cgroup *memcg,
					     unsigned long nr)
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

/*
 * ABI guard (boot-time): a comma/space-separated list of kapi logical symbol
 * names that the userspace preflight (abi/kapi_check, via /sys/kernel/btf) found
 * signature-INCOMPATIBLE on THIS running kernel. kapi_init leaves each listed
 * symbol NULL - resolved-but-disabled - so the feature that needs it is refused
 * by its kapi_can_* checklist (-ENOSYS) instead of being called through a
 * mistyped pointer (a kCFI type-id mismatch = panic). Empty (the normal case)
 * trusts the compile-time version gates; the guard only catches a signature the
 * gates did not anticipate (vendor divergence, an unforeseen future kernel).
 * Names are the canonical kernel symbols, e.g. "folio_isolate_lru,reclaim_pages"
 * (the vendor hook's id is "android_vh_free_one_page_bypass"). See abi/kapi_abi.tsv.
 */
static char *disable_kapi;
module_param(disable_kapi, charp, 0400);
MODULE_PARM_DESC(disable_kapi,
	"ABI guard: comma/space list of kapi symbols to leave unresolved (from kapi_check)");

/* Whole-token (not substring) match of @name against the disable_kapi list, so
 * "reclaim_pages" never matches "reclaim_pages_something". */
static bool kapi_sym_disabled(const char *name)
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

/*
 * Capability checklists (option 1): required-symbol sets. A feature whose
 * checklist is incomplete is refused up front (acquire_set -> -ENOSYS) rather
 * than half-run, because for these symbols "degrade" == "does nothing".
 * kapi_has_sys_reclaim() is the one enhancer worth naming (option 2): id=2 works
 * without it, just migration-only.
 */
static inline bool kapi_can_v1(void)		/* id=1 */
{
	return kapi.k_alloc_contig_pages && kapi.k_prep_compound_page;
}
static inline bool kapi_can_sweep(void)		/* id=2 core */
{
	return kapi.k_alloc_contig_range && kapi.k_prep_compound_page;
}
static inline bool kapi_can_evict_b(void)	/* id=3 = sweep + B */
{
	return kapi_can_sweep() && kapi.k_folio_isolate_lru && kapi.k_reclaim_pages;
}
static inline bool kapi_has_sys_reclaim(void)	/* A: optional enhancer for id=2 */
{
	return kapi.k_try_to_free_mem_cgroup_pages && kapi.k_mem_cgroup_from_task;
}
static inline bool kapi_can_cma(void)		/* v10 CMA reservoir */
{
	return kapi.k_set_pageblock_migratetype && kapi.k_get_pfnblock_flags_mask &&
	       kapi.k_alloc_contig_range_cma && kapi.k_alloc_contig_pages &&
	       kapi.k_prep_compound_page &&
	       migrate_cma_val >= 0 &&
	       pageblock_order_val >= PAGE_ORDER && pageblock_order_val <= 11;
}

/* ================================================================== */
/*  CMA reservoir: label read + pool-entry tripwire                   */
/* ================================================================== */

/* Migratetype label of @pfn's pageblock through the KERNEL's reader (see the
 * kapi field comment: the module's inline would mis-index when build != device
 * pageblock_order). Pure bitmap read - safe from any context, no lock. */
static inline int cma_pb_mt(unsigned long pfn)
{
	return (int)kapi.k_get_pfnblock_flags_mask(pfn_to_page(pfn), pfn, 0x7);
}

/* §9 flip whitelist: labels a fully-held block may carry into a flip.
 * UNMOVABLE(0)/MOVABLE(1)/RECLAIMABLE(2) are the stable first three
 * enumerators everywhere and are FUNGIBLE on a block whose every page we
 * hold (steal residue means nothing without free pages to steal). Everything
 * else - CMA (someone's carveout), HIGHATOMIC (reserve ledger), ISOLATE,
 * >= 6 (vendor CHP, panic-guarded) - is rejected without needing its value. */
static inline bool cma_mt_flippable(int mt)
{
	return mt == 0 || mt == 1 || mt == 2;
}

/*
 * C4 tripwire, called at EVERY pool entry point (take_frozen / push /
 * push_grow): a page whose pageblock is labeled CMA - ours or anyone's - must
 * never be held by the pool. Ours, because a reservoir page entering the held
 * pool without stage-in corrupts the guardianship accounting; anyone else's,
 * because pooling a vendor-CMA page steals from a carveout that cma_alloc will
 * later demand back. Returns true = refuse (caller lets the page continue to
 * buddy, where it lands on the CMA freelist - the correct home either way).
 * Normally never fires (v9 alloc paths are !movable so never draw from CMA
 * freelists; the B-class shrink protection keeps reservoir blocks whole); the
 * counter going nonzero is a logic-bug alarm, not an expected event - hence
 * counter + rate-limited pr_err, no VM_WARN (production kernels run DEBUG_VM=n)
 * and certainly no panic. Inert until the preflight supplied migrate_cma_val
 * and the reader resolved, so v9-path devices pay one branch. */
static bool cma_pool_entry_reject(struct page *page)
{
	unsigned long pfn;

	if (READ_ONCE(migrate_cma_val) < 0 || !kapi.k_get_pfnblock_flags_mask)
		return false;
	pfn = page_to_pfn(page);
	if (cma_pb_mt(pfn) != migrate_cma_val)
		return false;
	atomic_long_inc(&dbg_cma_leak);
	pr_err_ratelimited("cma: CMA-labeled page pfn=%lx refused pool entry (cma_leak tripwire)\n",
			   pfn);
	return true;
}

/* ================================================================== */
/*  Pool push/pop (protected by pool_lock)                            */
/* ================================================================== */

static struct page *pool_pop(void)
{
	struct page *page = NULL;
	unsigned long flags;
	int idx;

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	if (idx > 0) {
		page = page_pool[--idx];
		page_pool[idx] = NULL;
		atomic_set(&pool_count, idx);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	return page;
}

/*
 * Hand out one pool page AND record its holder in a single step: a page never
 * leaves the pool without a served-table entry (pfn -> current holder @tgid), and
 * never gets an entry without leaving the pool. This is what keeps
 * pool_avail + served == pool_total intact - in particular we refuse to hand out
 * a page we cannot track (served table full), because an untracked served page
 * can never be reclaimed by the free hook (which matches by pfn) and would leak.
 * Returns NULL when the pool is empty or the table is full; the caller then just
 * leaves the original buddy page in place.
 *
 * Holds served_lock across the pool_pop() (nesting pool_lock inside). Safe: the
 * free path takes served_lock and pool_lock sequentially, never nested the other
 * way, and no pool_* path takes served_lock - so there is no ABBA ordering.
 */
static struct page *pool_serve(pid_t tgid)
{
	unsigned long flags;
	struct page *page;

	raw_spin_lock_irqsave(&served_lock, flags);
	if (served_free_head == SERVED_NULL) {	/* no room to track -> don't serve */
		served_overflow++;
		raw_spin_unlock_irqrestore(&served_lock, flags);
		return NULL;
	}
	page = pool_pop();
	if (page)
		served_add_locked(page_to_pfn(page), tgid);
	raw_spin_unlock_irqrestore(&served_lock, flags);
	if (page)
		pb_track(page_to_pfn(page), PB_SERVED, PB_AVAIL);
	return page;
}

static bool pool_push(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	if (cma_pool_entry_reject(page))	/* C4 tripwire */
		return false;
	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < pool_total);
	if (ok) {
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
	return ok;
}

/*
 * Append a freshly *acquired* page and grow CAPACITY (pool_total) up to it.
 * Used only by the acquire path (real new allocations proven to succeed),
 * never by the free-hook - so capacity only rises by what we genuinely got.
 */
static bool pool_push_grow(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	if (cma_pool_entry_reject(page))	/* C4 tripwire */
		return false;
	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < READ_ONCE(pool_size_max));
	if (ok) {
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
	return ok;
}

/* ================================================================== */
/*  Free-path reclaim: android_vh_free_one_page_bypass                */
/*                                                                    */
/*  A served pool page returns to the host when its VM frees it. That */
/*  free travels the folio path (__folio_put_large / free_unref_folios*/
/*  on memfd truncate at crosvm exit), which NEVER passes through     */
/*  __free_pages - so the old __free_pages kprobe recovered almost    */
/*  nothing and the pool bled out. Every order-9 page that reaches the*/
/*  buddy allocator, by ANY path (single, batch, isolate, pcp drain), */
/*  passes through __free_one_page, whose top carries this exported   */
/*  vendor hook (present + EXPORT_TRACEPOINT_SYMBOL_GPL on GKI 6.1 -  */
/*  6.18). Setting *bypass=true removes the page before it enters the */
/*  freelist (before account_freepages), so we recover it verbatim.  */
/* ================================================================== */

/*
 * Rebuild @head as a pool-grade order-9 compound: head refcount 1, structure
 * from prep_compound_page, tail refcounts 0 - indistinguishable from an
 * alloc_pages(__GFP_COMP) page for serving and for __free_pages on release.
 * The ONE copy of the refcount protocol (a kernel-version-sensitive sequence):
 * used by the free-hook take, both acquire paths and the orphan re-acquire.
 * Non-sleeping (callers may hold raw locks). Requires kapi.k_prep_compound_page.
 * Setting the head count to 1 is idempotent for callers whose head already
 * holds a reference (alloc_contig_pages returns refcount-1 pages).
 */
static void rebuild_order9_compound(struct page *head)
{
	int i;

	set_page_count(head, 1);
	kapi.k_prep_compound_page(head, PAGE_ORDER);
	for (i = 1; i < (1 << PAGE_ORDER); i++)
		set_page_count(head + i, 0);	/* tails: rebuild as 0 */
}

/*
 * Re-pool a page the free-path is about to hand to the buddy allocator.
 * free_pages_prepare has already run: the compound is torn down, refcount is
 * frozen at 0, page->mapping cleared. If the pool holds fewer than the CURRENT
 * target (pool_want) we rebuild the order-9 compound and stash it - all
 * non-sleeping, under pool_lock, so it is safe from the hook's atomic
 * (zone->lock held, IRQs off) context. Gating on pool_want at return time (not
 * on the pool_total snapshot a resize left behind) is what makes a shrink
 * reversible: pages lent out across a pool_want=0 soft-disable are recovered
 * normally once the target is raised again - there is no "disowned" state.
 * The caller only gets here for pfns it just removed from the served table,
 * so the only page an unrelated free can lose here is one whose pfn WE served
 * and whose earlier free the hook missed (a stale entry) - see the reconcile
 * scavenger, which retires such entries by physically re-acquiring them.
 * pool_total (what we hold) is raised along, keeping avail+served ==
 * pool_total and refill consistent. The page is only mutated once we have
 * committed to taking it, so a failed take leaves it pristine to continue
 * into the buddy allocator. Caller must set *bypass=true iff this returns
 * true. Requires kapi.k_prep_compound_page (checked by caller). pool_want is
 * clamped to pool_size_max (itself at most POOL_SIZE_MAX) at every write
 * site, so idx < pool_want also bounds the array index.
 */
static bool pool_take_frozen(struct page *page)
{
	unsigned long flags;
	int idx;
	bool ok;

	if (cma_pool_entry_reject(page))	/* C4 tripwire (atomic-safe: bitmap read) */
		return false;
	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	ok = (idx < READ_ONCE(pool_want));
	if (ok) {
		rebuild_order9_compound(page);
		page->mapping = NULL;
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok)
		pb_track(page_to_pfn(page), PB_AVAIL, 0);
	return ok;
}

/*
 * §8 exchange path (SUBBLKS > 1 only, and only when the pool_want gate just
 * refused a returning page - the "want was shrunk mid-flight" corner): losing
 * this page to buddy would orphan its guarded siblings into a never-complete
 * block, so instead ACCEPT it and demote one low-completeness avail page to
 * the limbo pool - pool size unchanged, completeness strictly improved. If
 * nothing is demotable, the new block itself parks in limbo. Everything here
 * is list/bit work under raw locks (pool_lock -> pb_lock/limbo_lock, both
 * leaf): no free, no flip, atomic-legal (§8). A page is only MUTATED
 * (rebuild_order9_compound) after its destination is secured - a refused page
 * must continue into buddy pristine.
 */
static bool pool_take_frozen_exchange(struct page *page)
{
	unsigned long flags, pfn = page_to_pfn(page), vpfn = 0;
	unsigned int vcomp = ~0U;
	int i, scan, vslot = -1, idx;
	u8 av, sv, lb;
	bool ok = false, to_limbo = false;

	if (!pb_enabled() || !kapi.k_prep_compound_page)
		return false;
	pb_peek(pfn, &av, &sv, &lb);
	if (!((av | sv | lb) & (u8)~pb_bit_of(pfn)))
		return false;		/* no guarded sibling: drain as today */
	if (cma_pool_entry_reject(page))
		return false;		/* C4 applies here too */

	raw_spin_lock_irqsave(&pool_lock, flags);
	idx = atomic_read(&pool_count);
	if (idx < READ_ONCE(pool_want)) {
		/* raced: a seat opened between the gate and here - plain take */
		rebuild_order9_compound(page);
		page->mapping = NULL;
		page_pool[idx] = page;
		atomic_set(&pool_count, idx + 1);
		if (idx + 1 > pool_total)
			WRITE_ONCE(pool_total, idx + 1);
		ok = true;
		goto unlock;
	}
	/* bounded victim hunt from the stack top: lowest guardianship
	 * completeness wins; complete blocks are never victims */
	scan = min(idx, 32);
	for (i = 0; i < scan; i++) {
		struct page *cand = page_pool[idx - 1 - i];
		unsigned int comp;
		u8 cav, csv, clb;

		if (!cand)
			continue;
		pb_peek(page_to_pfn(cand), &cav, &csv, &clb);
		comp = pb_popcount8(cav | csv | clb);
		if (comp >= (unsigned int)CMA_SUBBLKS)
			continue;
		if (comp < vcomp) {
			vcomp = comp;
			vslot = idx - 1 - i;
		}
	}
	if (vslot >= 0) {
		struct page *victim = page_pool[vslot];

		if (limbo_add(victim)) {	/* destination secured first */
			rebuild_order9_compound(page);
			page->mapping = NULL;
			page_pool[vslot] = page;
			vpfn = page_to_pfn(victim);
			ok = true;
		}
		/* limbo full: fall through, drain as today */
	} else if (limbo_add(page)) {
		/* nothing demotable: the new block itself waits in limbo */
		rebuild_order9_compound(page);
		page->mapping = NULL;
		to_limbo = true;
	}
unlock:
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (ok) {
		pb_track(pfn, PB_AVAIL, 0);
		if (vpfn)
			pb_track(vpfn, PB_LIMBO, PB_AVAIL);
	} else if (to_limbo) {
		pb_track(pfn, PB_LIMBO, 0);
	}
	return ok || to_limbo;
}

/* Cap on re-acquire attempts per scavenge pass: bounds the worker's runtime;
 * leftovers are retried on the next pass (drain worker or any reconcile). */
#define REACQUIRE_BATCH	64

/*
 * Physically recover served pages whose free the hook MISSED - a THP split
 * frees 512 order-0 pages the order-9 gate never sees, and a free that lands
 * while the hook is unregistered escapes the same way. Such an entry lingers
 * with its page back in buddy (page_count == 0); left alone it either rots
 * until a purge writes the page off, or worse, matches the pfn's NEXT order-9
 * free and annexes it from whoever owns the page by then. Since the escaped
 * fragments coalesce back in the freelists, a targeted alloc_contig_range on
 * exactly that window usually recovers the SAME physical block almost for
 * free - so retire stale entries by taking their page back, not by
 * forgetting it.
 *
 * Racing the free hook is safe: if the page is flowing through
 * __free_one_page while we run, the hook consumes the entry and the range is
 * not free - alloc_contig_range fails and we skip. If alloc_contig_range succeeds, the
 * whole window was in buddy, the hook cannot have matched, and the entry is
 * still ours to remove: served_del AFTER the successful grab, never before.
 * Process context only (alloc_contig_range sleeps; it also drains pcp internally,
 * so a fragment parked there does not fake a "reused" page).
 * Returns the number of pages re-pooled.
 */
static int served_reacquire_free_orphans(void)
{
	unsigned long cand[REACQUIRE_BATCH];
	unsigned long flags;
	int b, i, n = 0, got = 0;

	if (!kapi.k_alloc_contig_range || !kapi.k_prep_compound_page)
		return 0;
	if (READ_ONCE(served_count) == 0)
		return 0;

	/* Collect candidates under the lock; act on them outside it. */
	raw_spin_lock_irqsave(&served_lock, flags);
	for (b = 0; b < SERVED_MAX && n < REACQUIRE_BATCH; b++) {
		u16 nd;

		for (nd = served_bucket[b];
		     nd != SERVED_NULL && n < REACQUIRE_BATCH;
		     nd = served_nodes[nd].next) {
			unsigned long pfn = served_nodes[nd].pfn;
			struct page *pg = pfn_valid(pfn) ? pfn_to_page(pfn) : NULL;

			if (pg && page_count(pg) == 0)
				cand[n++] = pfn;
		}
	}
	raw_spin_unlock_irqrestore(&served_lock, flags);

	for (i = 0; i < n; i++) {
		unsigned long pfn = cand[i];
		struct page *head;

		if (kapi.k_alloc_contig_range(pfn, pfn + (1UL << PAGE_ORDER),
				      GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY))
			continue;	/* window no longer whole: entry stays */

		served_del(pfn);
		head = pfn_to_page(pfn);
		rebuild_order9_compound(head);
		if (atomic_read(&pool_count) < READ_ONCE(pool_want) &&
		    pool_push_grow(head)) {
			atomic_inc(&total_refilled);
			got++;
		} else {
			/* Target already met (shrink since the serve): the
			 * entry is retired either way; let the page go back
			 * whole - its order-9 free re-enters the hook but no
			 * longer matches anything. */
			__free_pages(head, PAGE_ORDER);
		}
		cond_resched();
	}
	if (got)
		pr_info("re-acquired %d hook-missed page(s), avail=%d served=%d\n",
			got, atomic_read(&pool_count), READ_ONCE(served_count));
	return got;
}

/* Reclaim-path forensic counters (reclaim_debug, 0444). Cheap enough to keep
 * in production: one atomic inc per order-9 free system-wide, a few more per
 * matched free. dbg_take_fail should stay 0 - a nonzero value means a served
 * page's free arrived while the pool had no room for it (see the pr_warn). */
static atomic_long_t dbg_o9_seen;	/* order-9 frees the hook inspected */
static atomic_long_t dbg_del_hit;	/* ... that matched the served table */
static atomic_long_t dbg_del_miss;	/* ... order-9, but not ours */
static atomic_long_t dbg_take_fail;	/* matched but pool_take_frozen refused */

static void gh_free_one_page_cb(void *data, struct page *page, struct zone *zone,
				int order, int migratetype, int fpi_flags,
				bool *bypass)
{
	unsigned long pfn;

	/* Fires for every buddy free system-wide: the cheap gates come first. */
	if (order != PAGE_ORDER || !page)
		return;
	atomic_long_inc(&dbg_o9_seen);
	if (READ_ONCE(served_count) == 0)	/* nothing lent out -> not ours */
		return;

	pfn = page_to_pfn(page);
	/*
	 * Only reclaim pages WE served to a VM (present in the served table).
	 * Never steal unrelated order-9 frees. served_del removes the entry
	 * (the page is leaving the VM either way); the pool_want check inside
	 * pool_take_frozen lets it fall through to buddy when the CURRENT
	 * target says we already hold enough (e.g. after a shrink).
	 */
	if (!served_del(pfn)) {
		atomic_long_inc(&dbg_del_miss);
		return;
	}
	atomic_long_inc(&dbg_del_hit);

	if (kapi.k_prep_compound_page && pool_take_frozen(page)) {
		*bypass = true;
		atomic_inc(&total_refilled);
	} else if (pool_take_frozen_exchange(page)) {
		/* §8: gate-refused, but siblings are guarded - the page was
		 * exchanged into the pool (or parked in limbo) instead of
		 * orphaning its block. */
		*bypass = true;
		atomic_inc(&total_refilled);
	} else {
		atomic_long_inc(&dbg_take_fail);
		pr_warn_ratelimited("reclaim take FAILED pfn=%lx avail=%d want=%d total=%d\n",
				    pfn, atomic_read(&pool_count),
				    READ_ONCE(pool_want), pool_total);
	}
}

static int reclaim_debug_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "o9_seen=%ld\ndel_hit=%ld\ndel_miss=%ld\ntake_fail=%ld\ncma_leak=%ld\n",
			  atomic_long_read(&dbg_o9_seen),
			  atomic_long_read(&dbg_del_hit),
			  atomic_long_read(&dbg_del_miss),
			  atomic_long_read(&dbg_take_fail),
			  atomic_long_read(&dbg_cma_leak));
}

static const struct kernel_param_ops reclaim_debug_ops = {
	.get = reclaim_debug_get,
};
module_param_cb(reclaim_debug, &reclaim_debug_ops, NULL, 0400);
MODULE_PARM_DESC(reclaim_debug, "Free-path reclaim forensic counters (read-only)");

/* The android_vh_free_one_page_bypass tracepoint, located by name at runtime. */
static struct tracepoint *free_one_page_tp;

static void find_free_one_page_tp(struct tracepoint *tp, void *priv)
{
	if (!free_one_page_tp &&
	    !strcmp(tp->name, "android_vh_free_one_page_bypass"))
		free_one_page_tp = tp;
}

/* Attach/detach the free-path reclaim hook. Callers hold hook_mutex (except the
 * single-threaded init/exit). Returns 0, or -ENODEV (prep_compound_page
 * unresolved) / -ENOENT (hook absent on this kernel) / register errno. */
static int free_hook_register(void)
{
	int ret;

	if (free_intercept_active)
		return 0;
	/* ABI guard: skip the hook if the preflight flagged the vendor tracepoint's
	 * TP_PROTO incompatible (the kernel would call our probe through a mistyped
	 * pointer = kCFI panic). Treated like an absent hook. */
	if (kapi_sym_disabled("android_vh_free_one_page_bypass"))
		return -ENOENT;
	if (!kapi.k_prep_compound_page)
		return -ENODEV;
	if (!free_one_page_tp) {
		for_each_kernel_tracepoint(find_free_one_page_tp, NULL);
		if (!free_one_page_tp)
			return -ENOENT;
	}
	ret = tracepoint_probe_register(free_one_page_tp,
					(void *)gh_free_one_page_cb, NULL);
	if (ret == 0)
		free_intercept_active = true;
	return ret;
}

static void free_hook_unregister(void)
{
	if (!free_intercept_active || !free_one_page_tp)
		return;
	tracepoint_probe_unregister(free_one_page_tp,
				    (void *)gh_free_one_page_cb, NULL);
	tracepoint_synchronize_unregister();	/* no callback in flight after this */
	free_intercept_active = false;
}

/* ================================================================== */
/*  kretprobe on __alloc_pages - page replacement                     */
/* ================================================================== */

static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if ((unsigned int)regs_get_kernel_argument(regs, 1) != PAGE_ORDER)
		return 1;
	if (atomic_read(&pool_count) <= 0)
		return 1;
	/* Only intercept allocations from tracked Gunyah VM owners */
	if (!current->mm || !vm_owner_contains(current->mm))
		return 1;
	return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct page *orig, *pool_page;

	/* Hand out + record the holder as one step: never serve a page we can't
	 * track (would leak), never track one we didn't hand out. */
	pool_page = pool_serve(current->tgid);
	if (!pool_page)
		return 0;

	orig = (struct page *)regs_return_value(regs);
	regs_set_return_value(regs, (unsigned long)pool_page);
	atomic_inc(&total_served);
	if (current->mm)
		vm_owner_served_inc(current->mm);
	pr_info_ratelimited("served page, %d left\n",
			    atomic_read(&pool_count));

	if (orig) {
		/*
		 * Free the buddy page the allocator originally returned. It is
		 * NOT in the served table (only pool_page is), so the free-path
		 * hook won't re-grab it - no guard needed. It correctly returns
		 * to the buddy allocator.
		 */
		__free_pages(orig, PAGE_ORDER);
	}
	return 0;
}

/* ================================================================== */
/*  VM creation detection                                             */
/* ================================================================== */

static int vm_detect_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	unsigned int cmd = (unsigned int)regs_get_kernel_argument(regs, 1);

	if (cmd != GH_CREATE_VM)
		return 0;

	if (current->mm)
		vm_owner_add(current->mm);

	pr_info("VM creation detected (active=%d, comm=%s)\n",
		vm_active_total(), current->comm);
	return 0;
}

/*
 * Flush per-CPU page caches a few seconds after a VM destroy. Teardown frees
 * order-9 folios through free_unref_page, which can park a few of them in pcp
 * lists indefinitely on an idle system - they never reach __free_one_page, so
 * the reclaim hook never sees them and their served entries linger until a
 * post-grace reconcile writes them off as orphan_freed (a steady ~2-page loss
 * per killed VM, observed). Draining pushes them through free_pcppages_bulk ->
 * __free_one_page, where the hook recovers them like any other VM return.
 */
static void pcp_drain_worker(struct work_struct *work)
{
	if (kapi.k_drain_all_pages)
		kapi.k_drain_all_pages(NULL);	/* NULL: every populated zone */
	/*
	 * Then scavenge: any served entry whose page reads free AFTER the
	 * drain is a hook-missed free (e.g. a split THP freed as order-0s).
	 * Re-acquire it now, while the fragments are still coalesced in buddy
	 * and before anyone re-allocates from the window - this is the module-
	 * autonomous recovery path; reconcile only mops up the long tail.
	 */
	served_reacquire_free_orphans();
	pr_info("post-destroy scavenge done (avail=%d served=%d)\n",
		atomic_read(&pool_count), READ_ONCE(served_count));
}

/* ================================================================== */
/*  VM destruction detection                                          */
/* ================================================================== */

static int vm_destroy_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	/*
	 * Filter: only handle tracked VM owners. On the process-exit path
	 * (OOM kill, crash) current->mm is already NULL by the time the fd
	 * is released (exit_mm runs before exit_files), so accept mm==NULL
	 * whenever anyone is tracked - the sweep identifies which owner
	 * actually died via mm_users, not via current.
	 */
	if (current->mm && !vm_owner_contains(current->mm))
		return 0;
	if (!current->mm && atomic_read(&vm_owner_count) == 0)
		return 0;

	/* Drop one VM from the closing owner. current->mm identifies it for an
	 * explicit close by a live process; an mm-less release (process exit,
	 * delayed_fput from a kworker) falls back to the sole live owner. Only
	 * the ambiguous mm-less case (several live owners) is left entirely to
	 * the mm_users sweep - log it, because if that closer stays alive its
	 * vm_count stays inflated and its slot is pinned until it exits. */
	if (current->mm)
		vm_owner_vm_dec(current->mm);
	else if (!vm_owner_vm_dec_sole())
		pr_info_ratelimited("unattributed VM destroy (owners=%d)\n",
				    atomic_read(&vm_owner_count));

	/* Open the reconcile grace window: teardown frees are inbound. */
	WRITE_ONCE(last_destroy_jiffies, jiffies);

	/* And flush pcp + scavenge hook-missed frees once the free burst
	 * settles (the worker skips whatever its kernel can't do).
	 * mod_delayed_work: several fds released in quick succession (one
	 * kill = many gunyah fds) push the pass back. */
	if (kapi.k_drain_all_pages || kapi.k_alloc_contig_range)
		mod_delayed_work(system_wq, &pcp_drain_work,
				 msecs_to_jiffies(PCP_DRAIN_DELAY_MS));

	pr_info("VM destruction detected (active=%d, pool=%d/%d, comm=%s)\n",
		vm_active_total(), atomic_read(&pool_count), pool_total,
		current->comm);

	/*
	 * Schedule refill immediately - worker polls mm_users before allocating.
	 * When refill_enable=0 the old alloc-back path is off (measuring free-hook
	 * reclaim alone); we still sweep dead owners so tracking stays accurate.
	 */
	if (READ_ONCE(refill_enable) &&
	    atomic_read(&pool_count) + READ_ONCE(served_count) < pool_total &&
	    atomic_cmpxchg(&refill_status, REFILL_IDLE, REFILL_WAITING) == REFILL_IDLE) {
		schedule_delayed_work(&refill_work, 0);
		pr_info("refill scheduled\n");
	} else {
		/* Refill disabled/full/already running - sweep dead owners now */
		schedule_work(&vm_owner_sweep_work);
	}

	return 0;
}

/* ================================================================== */
/*  Helpers                                                           */
/* ================================================================== */

static int register_kretp_with_fallback(void)
{
	static const char * const sym_names[] = {
		"__alloc_pages_noprof",
		"__alloc_pages",
		"__alloc_pages_nodemask",
		NULL,
	};
	int i, ret = -ENOENT;

	for (i = 0; sym_names[i]; i++) {
		kretp.kp.symbol_name = sym_names[i];
		ret = register_kretprobe(&kretp);
		if (ret == 0) {
			pr_info("hooked %s\n",
				sym_names[i]);
			return 0;
		}
	}
	pr_err("no suitable __alloc_pages symbol\n");
	return ret;
}

/*
 * Sweep tracked owners, releasing only those that are gone: process dead
 * (mm_users == 0) or last VM closed (vm_count == 0). Owners with live VMs
 * are left untouched, so one VM dying never drops the tracking of the
 * others. Deferred to a worker because mmdrop may sleep.
 */
static void vm_owner_sweep_worker(struct work_struct *work)
{
	unsigned long flags;
	struct mm_struct *to_drop[VM_OWNER_MAX];
	int i, count, ndrop = 0;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		struct mm_struct *mm = vm_owners[i].mm;

		if (!mm)
			continue;
		if (vm_owners[i].vm_count > 0 &&
		    atomic_read(&mm->mm_users) > 0)
			continue;	/* live owner: keep tracking */
		to_drop[ndrop++] = mm;
		WRITE_ONCE(vm_owners[i].mm, NULL);
		vm_owners[i].tgid = 0;
		vm_owners[i].comm[0] = '\0';
		atomic_set(&vm_owners[i].served, 0);
		vm_owners[i].vm_count = 0;
	}
	/* Trim trailing free slots so count==0 again means "nobody tracked" */
	while (count > 0 && !vm_owners[count - 1].mm)
		count--;
	smp_store_release(&vm_owner_count.counter, count);
	spin_unlock_irqrestore(&vm_owner_lock, flags);

	for (i = 0; i < ndrop; i++) {
		pr_info("releasing VM owner mm=%px\n",
			to_drop[i]);
		mmdrop(to_drop[i]);
	}
}

/*
 * Refill pool from buddy allocator after VM shutdown.
 * The free_pages kprobe reclaims most pages directly from the free path;
 * this worker handles any remaining shortfall via alloc_pages + compaction.
 * Runs in kworker context (mm=NULL), safe from kretprobe interception.
 */
static void refill_worker(struct work_struct *work)
{
	int current_count, target, allocated, retry;
	int total_allocated = 0;

	atomic_set(&refill_status, REFILL_RUNNING);

	/*
	 * Phase 1: Wait for VM owner process to exit (mm_users == 0).
	 * Pages return to buddy only after exit_mmap() completes.
	 * We hold mm_count via mmgrab(), so mm_struct stays valid.
	 */
	{
		int wait_ms;

		for (wait_ms = 0; wait_ms < refill_delay_ms; wait_ms += 50) {
			bool exited = false;
			int i, count;

			if (atomic_read(&pool_count) + READ_ONCE(served_count) >=
			    pool_total)
				break;

			count = smp_load_acquire(&vm_owner_count.counter);
			if (count == 0) {
				exited = true;
			} else {
				for (i = 0; i < count; i++) {
					struct mm_struct *mm = READ_ONCE(vm_owners[i].mm);

					if (mm && atomic_read(&mm->mm_users) == 0) {
						exited = true;
						break;
					}
				}
			}
			if (exited)
				break;
			msleep(50);
		}
		pr_info("process exit wait: %d ms, pool %d/%d\n",
			wait_ms, atomic_read(&pool_count), pool_total);
	}

	/* Phase 2: Brief pause for free-intercept to catch initial burst */
	if (atomic_read(&pool_count) + READ_ONCE(served_count) < pool_total)
		msleep(200);

	for (retry = 0; retry <= REFILL_RETRY_MAX; retry++) {
		if (retry > 0) {
			pr_info("refill retry %d/%d, waiting %d ms\n",
				retry, REFILL_RETRY_MAX, REFILL_RETRY_INTERVAL_MS);
			msleep(REFILL_RETRY_INTERVAL_MS);
		}

		current_count = atomic_read(&pool_count);
		/*
		 * v10 §9 seat fix: lent-out pages OWN their seats. The v9
		 * target (pool_total - avail) ignored served, so this worker
		 * allocated fresh buddy pages into seats whose real pages were
		 * still coming home through the free hook - a replacement
		 * engine that evicted every returning VM page to buddy. Only
		 * the shortfall the hook could not recover (avail + served <
		 * capacity) is refilled from outside; cma_external_ok also
		 * caps it under the with_cma total like every external filler.
		 */
		target = pool_total - current_count - READ_ONCE(served_count);

		if (target <= 0) {
			pr_info("pool already full (%d+%d/%d)\n",
				current_count, READ_ONCE(served_count), pool_total);
			break;
		}

		pr_info("refill attempt %d: need %d pages (have %d+%d/%d)\n",
			retry + 1, target, current_count,
			READ_ONCE(served_count), pool_total);

		allocated = 0;
		while (atomic_read(&pool_count) + READ_ONCE(served_count) <
		       pool_total && cma_external_ok()) {
			struct page *p;

			p = alloc_pages(GFP_KERNEL | __GFP_COMP |
					__GFP_NOWARN | __GFP_RETRY_MAYFAIL,
					PAGE_ORDER);
			if (!p)
				break;

			if (!pool_push(p)) {
				__free_pages(p, PAGE_ORDER);
				break;
			}
			allocated++;
			atomic_inc(&total_refilled);

			if (allocated % 50 == 0)
				cond_resched();
		}

		total_allocated += allocated;
		pr_info("refill attempt %d: got %d pages (pool now %d/%d)\n",
			retry + 1, allocated, atomic_read(&pool_count), pool_total);

		if (atomic_read(&pool_count) + READ_ONCE(served_count) >= pool_total)
			break;
	}

	pr_info("refill complete: %d pages recovered, pool %d/%d\n",
		total_allocated, atomic_read(&pool_count), pool_total);

	/* Sweep owners whose process/VMs are gone - survivors keep tracking */
	schedule_work(&vm_owner_sweep_work);

	atomic_set(&refill_status, REFILL_IDLE);
}

/* ================================================================== */
/*  sysfs parameters                                                  */
/* ================================================================== */

/* hook_enable: manual override for kretprobe */
static int hook_enable_set(const char *val, const struct kernel_param *kp)
{
	int enable, ret = 0;

	if (kstrtoint(val, 10, &enable))
		return -EINVAL;

	mutex_lock(&hook_mutex);

	if (enable && !hook_active) {
		if (atomic_read(&pool_count) <= 0) {
			pr_err("pool empty, cannot enable\n");
			ret = -ENODATA;
		} else {
			ret = register_kretp_with_fallback();
			if (ret == 0)
				hook_active = true;
		}
	} else if (!enable && hook_active) {
		unregister_kretprobe(&kretp);
		hook_active = false;
		pr_info("hook manually disabled\n");
	}

	mutex_unlock(&hook_mutex);
	return ret;
}

static int hook_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", hook_active ? 1 : 0);
}

static const struct kernel_param_ops hook_enable_ops = {
	.set = hook_enable_set,
	.get = hook_enable_get,
};
module_param_cb(hook_enable, &hook_enable_ops, NULL, 0600);
MODULE_PARM_DESC(hook_enable, "Alloc-side serve hook (kretprobe): 1=activate 0=deactivate");

/* reclaim_enable: toggle the NEW free-path VM-return reclaim hook
 * (android_vh_free_one_page_bypass), separate from the alloc-side hook_enable.
 * 1=attach (default), 0=detach. Read reports the ACTUAL active state. */
static int reclaim_enable_set(const char *val, const struct kernel_param *kp)
{
	int enable, ret = 0;

	if (kstrtoint(val, 10, &enable))
		return -EINVAL;
	enable = !!enable;

	mutex_lock(&hook_mutex);
	WRITE_ONCE(reclaim_want, enable);
	if (READ_ONCE(pool_ready)) {		/* runtime: apply immediately */
		if (enable)
			ret = free_hook_register();
		else
			free_hook_unregister();
	}					/* else: module_init honors the want */
	mutex_unlock(&hook_mutex);
	return ret;
}

static int reclaim_enable_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", free_intercept_active ? 1 : 0);
}

static const struct kernel_param_ops reclaim_enable_ops = {
	.set = reclaim_enable_set,
	.get = reclaim_enable_get,
};
module_param_cb(reclaim_enable, &reclaim_enable_ops, NULL, 0600);
MODULE_PARM_DESC(reclaim_enable,
	"Free-path VM-return reclaim hook: 1=activate (default) 0=deactivate");

/* pool_avail: read-only */
static int pool_avail_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&pool_count));
}

static const struct kernel_param_ops pool_avail_ops = {
	.get = pool_avail_get,
};
module_param_cb(pool_avail, &pool_avail_ops, NULL, 0400);
MODULE_PARM_DESC(pool_avail, "Pages available in pool (read-only)");

/* refill_stat: read-only status */
static int refill_stat_get(char *buf, const struct kernel_param *kp)
{
	static const char * const state_names[] = {
		"idle", "waiting", "running",
	};
	int state = atomic_read(&refill_status);
	int n = 0;

	n += sysfs_emit_at(buf, n, "state=%s\n", state_names[state]);
	n += sysfs_emit_at(buf, n, "pool_avail=%d\n", atomic_read(&pool_count));
	n += sysfs_emit_at(buf, n, "pool_total=%d\n", pool_total);
	/* Live served-out (lent to VMs) count. owned+traced = pool_avail + served;
	 * this can exceed pool_total after a shrink that left served pages out. */
	n += sysfs_emit_at(buf, n, "served=%d\n", served_count);
	n += sysfs_emit_at(buf, n, "pool_want=%d\n", READ_ONCE(pool_want));
	n += sysfs_emit_at(buf, n, "total_served=%d\n", atomic_read(&total_served));
	n += sysfs_emit_at(buf, n, "total_refilled=%d\n", atomic_read(&total_refilled));
	n += sysfs_emit_at(buf, n, "active_vms=%d\n", vm_active_total());
	n += sysfs_emit_at(buf, n, "acquire_active=%d\n", atomic_read(&acquire_running));
	n += sysfs_emit_at(buf, n, "acquire_mode=%d\n", READ_ONCE(acquire_mode));
	/* Free text (may contain spaces); refill_stat is '=' -split on the Java side
	 * (parseProp: split("=",2)), so spaces are safe - no separator escaping. */
	n += sysfs_emit_at(buf, n, "acquire_stop_reason=%s\n", READ_ONCE(acquire_stop_reason));
	n += sysfs_emit_at(buf, n, "refill_enable=%d\n", READ_ONCE(refill_enable));
	n += sysfs_emit_at(buf, n, "free_reclaim=%d\n", free_intercept_active ? 1 : 0);
	/* v10 CMA reservoir block ('='-split like everything above, GUI-safe). */
	n += sysfs_emit_at(buf, n, "pool_want_with_cma=%d\n", READ_ONCE(pool_want_with_cma));
	n += sysfs_emit_at(buf, n, "pool_cma=%d\n", cma_pool_cma_2mb());
	n += sysfs_emit_at(buf, n, "pool_avail_cma_able=%d\n", cma_avail_cma_able_2mb());
	n += sysfs_emit_at(buf, n, "cma_pb_order=%d\n", cma_capable ? cma_pb_order : -1);

	return n;
}

static const struct kernel_param_ops refill_stat_ops = {
	.get = refill_stat_get,
};
module_param_cb(refill_stat, &refill_stat_ops, NULL, 0400);
MODULE_PARM_DESC(refill_stat, "Refill status and statistics (read-only)");

/* vm_owners: read-only per-VM-owner attribution */
static int vm_owners_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int i, count, n = 0;

	spin_lock_irqsave(&vm_owner_lock, flags);
	count = atomic_read(&vm_owner_count);
	for (i = 0; i < count; i++) {
		if (!vm_owners[i].mm)
			continue;
		/* comm is emitted last so userspace can keep spaces in it */
		n += sysfs_emit_at(buf, n, "pid=%d served=%d comm=%s\n",
				   vm_owners[i].tgid,
				   atomic_read(&vm_owners[i].served),
				   vm_owners[i].comm);
	}
	spin_unlock_irqrestore(&vm_owner_lock, flags);
	return n;
}

static const struct kernel_param_ops vm_owners_ops = {
	.get = vm_owners_get,
};
module_param_cb(vm_owners, &vm_owners_ops, NULL, 0400);
MODULE_PARM_DESC(vm_owners,
	"Per-VM-owner attribution: pid/served-pages/comm (read-only)");

/* reconcile: write 1 to reconcile the served-page table (process context) */
static int reconcile_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v != 1)
		return -EINVAL;
	mutex_lock(&recon_mutex);
	served_do_reconcile();
	mutex_unlock(&recon_mutex);
	return 0;
}

static const struct kernel_param_ops reconcile_ops = {
	.set = reconcile_set,
};
module_param_cb(reconcile, &reconcile_ops, NULL, 0200);
MODULE_PARM_DESC(reconcile, "Write 1 to reconcile the served-page table");

/* served_summary: read-only reconciled view of the served-page table */
static int served_summary_get(char *buf, const struct kernel_param *kp)
{
	int n = 0, i;

	mutex_lock(&recon_mutex);
	n += sysfs_emit_at(buf, n, "tracked=%d\n", READ_ONCE(served_count));
	n += sysfs_emit_at(buf, n, "overflow=%d\n", READ_ONCE(served_overflow));
	n += sysfs_emit_at(buf, n, "live=%d\n", recon_live);
	n += sysfs_emit_at(buf, n, "orphan_freed=%d\n", recon_orphan_freed);
	n += sysfs_emit_at(buf, n, "orphan_inuse=%d\n", recon_orphan_inuse);
	for (i = 0; i < recon_owner_n; i++)
		n += sysfs_emit_at(buf, n, "owner pid=%d pages=%ld comm=%s\n",
				   recon_owner_tgid[i], recon_owner_pages[i],
				   recon_owner_comm[i]);
	mutex_unlock(&recon_mutex);
	return n;
}

static const struct kernel_param_ops served_summary_ops = {
	.get = served_summary_get,
};
module_param_cb(served_summary, &served_summary_ops, NULL, 0400);
MODULE_PARM_DESC(served_summary,
	"Reconciled served-page table summary (read-only)");

/* manual_refill: write 1 to trigger manual refill */
static int manual_refill_set(const char *val, const struct kernel_param *kp)
{
	int trigger;

	if (kstrtoint(val, 10, &trigger) || trigger != 1)
		return -EINVAL;

	if (!READ_ONCE(refill_enable)) {
		pr_info("manual_refill ignored (refill_enable=0)\n");
		return -ENODEV;
	}

	if (atomic_read(&pool_count) + READ_ONCE(served_count) >= pool_total) {
		pr_info("pool already full\n");
		return 0;
	}

	if (atomic_cmpxchg(&refill_status, REFILL_IDLE, REFILL_WAITING) != REFILL_IDLE) {
		pr_warn("refill already in progress\n");
		return -EBUSY;
	}

	schedule_delayed_work(&refill_work, msecs_to_jiffies(100));
	pr_info("manual refill triggered\n");
	return 0;
}

static const struct kernel_param_ops manual_refill_ops = {
	.set = manual_refill_set,
};
module_param_cb(manual_refill, &manual_refill_ops, NULL, 0200);
MODULE_PARM_DESC(manual_refill, "Write 1 to trigger manual pool refill");

/* Headroom floor for CMA flips - defined with the reservoir engine below,
 * together with the §7 class-ordered shrink pieces. */
static bool cma_floor_ok(int nblocks);
static int pool_extract_block(unsigned long pb, struct page **subs, int max);
static struct page *pool_extract_pfn(unsigned long pfn);
static unsigned long pb_find_full_avail(void);
static unsigned long pb_find_class_c_avail(void);
static int cma_commit_compound_block(unsigned long base, struct page **subs);
static void cma_limbo_process(void);
static int cma_verify_first_span(void);

/*
 * pool_do_resize: change the target (pool_want).
 *   shrink (new < reserve): free available pages immediately, best effort
 *                           toward the target (served pages can't be freed).
 *   grow   (new > reserve): raise the target only; new pages are NOT
 *                           allocated here - use acquire to fill.
 *   0 = soft disable: every available page is freed now. Served pages stay
 *   tracked; whether a returning page is re-pooled is decided by the free
 *   hook against the target AT RETURN TIME (pool_count < pool_want), so
 *   want=0 lets them drain to buddy, while raising the target back first
 *   makes them re-pool normally - a soft-disable is fully reversible.
 */
static void pool_do_resize(int newt)
{
	struct page *batch[32];
	int target_avail;
	bool flip = false;
	int flip_budget = 0, flipped = 0;

	/*
	 * Clamp to the RAM-derived cap. A target below the currently lent-out
	 * count is allowed and safe: shrink frees only pooled pages, and when the
	 * lent pages are later returned the free-hook re-pools just up to
	 * pool_want and lets the rest go to buddy - no overflow, no leak.
	 */
	if (newt < 0)
		newt = 0;
	if (newt > pool_size_max)
		newt = pool_size_max;

	WRITE_ONCE(pool_want, newt);

	/*
	 * The real reserve held is owned+traced = pool_count(avail) + served. GROW
	 * stops here: nothing to free. pool_total is not touched - the free hook
	 * (as served pages return) and acquire (as new pages land) raise it to
	 * match what we actually hold.
	 */
	if (atomic_read(&pool_count) + served_count <= newt)
		return;

	/*
	 * SHRINK: free available pages so owned+traced drops to newt. Served pages
	 * can't be freed (VMs hold them), so the floor is target_avail =
	 * max(0, newt - served). Park pool_total at that floor *first* so a
	 * concurrently-running refill worker (whose pool_push fills whenever
	 * pool_count < pool_total) can't re-add what we drain here; restore
	 * pool_total = newt afterwards. The free hook doesn't need this dance -
	 * it gates on pool_want (already = newt) and its pages can't collide with
	 * the drain: a drained pool page was never in the served table.
	 */
	target_avail = newt - served_count;
	if (target_avail < 0)
		target_avail = 0;
	WRITE_ONCE(pool_total, target_avail);

	/*
	 * v10 §7 class-A slice (SUBBLKS==1 devices): a drained avail page IS a
	 * whole, aligned pageblock, so instead of losing it to buddy it can
	 * flip straight into the CMA reservoir - this is the return leg the
	 * one-way "-f fork" never had (shrink used to disown the memory; now
	 * the guardianship just changes form, capped at pool_want_with_cma -
	 * newt). Class B (served siblings) / class C (partial) and SUBBLKS>1
	 * pairing need the pb-hash and arrive with commits 2/3; with
	 * SUBBLKS==1 every avail page is class A. The flip happens while the
	 * page is still OUR fully-held compound (label change is safe), then
	 * the ordinary order-9 compound free carries it - via a short pcp
	 * transit whose stored pcppage migratetype preserves CMA - onto its
	 * block's CMA freelist.
	 */
	if (cma_capable &&
	    READ_ONCE(pool_want_with_cma) > newt && CMA_SUBBLKS == 1) {
		mutex_lock(&cma_mutex);
		flip = true;
		flip_budget = READ_ONCE(pool_want_with_cma) - newt -
			      cma_pool_cma_2mb();
	}

	/*
	 * v10 §7 class order (SUBBLKS > 1): drop class-C strays to buddy
	 * first (their blocks can never complete from what we hold), flip
	 * class-A groups whole into the reservoir second, and only then let
	 * the generic drain below take what remains - by then that is class B
	 * (avail members whose lent-out siblings may still return and turn
	 * the block into class A), deliberately dropped last.
	 */
	if (cma_capable && CMA_SUBBLKS > 1) {
		int budget;

		mutex_lock(&cma_mutex);
		while (atomic_read(&pool_count) > target_avail) {	/* pass C */
			unsigned long vpfn = pb_find_class_c_avail();
			struct page *p;

			if (!vpfn)
				break;
			p = pool_extract_pfn(vpfn);
			if (!p) {
				pb_track(vpfn, 0, PB_AVAIL);	/* stale bit */
				continue;
			}
			__free_pages(p, PAGE_ORDER);
			cond_resched();
		}
		budget = READ_ONCE(pool_want_with_cma) - newt - cma_pool_cma_2mb();
		while (atomic_read(&pool_count) > target_avail) {	/* pass A */
			unsigned long key = pb_find_full_avail();
			struct page *subs[8];
			int got, j, ret;

			if (!key)
				break;
			got = pool_extract_block(key, subs, CMA_SUBBLKS);
			if (!got)
				break;	/* index/pool disagree: generic drain mops up */
			ret = -EAGAIN;
			if (got == CMA_SUBBLKS && budget >= CMA_SUBBLKS &&
			    READ_ONCE(pool_want_with_cma) > newt &&
			    cma_floor_ok(1))
				ret = cma_commit_compound_block(
					key << cma_pb_order, subs);
			if (ret == 0) {
				budget -= CMA_SUBBLKS;
				flipped += CMA_SUBBLKS;
				continue;
			}
			if (ret == -EIO)
				cma_capable = false;
			for (j = 0; j < got; j++)	/* over budget/floor: buddy */
				__free_pages(subs[j], PAGE_ORDER);
			cond_resched();
		}
		mutex_unlock(&cma_mutex);
	}

	/* free outside the lock to avoid the page allocator under it. */
	for (;;) {
		unsigned long flags;
		int n = 0, i;

		raw_spin_lock_irqsave(&pool_lock, flags);
		while (n < 32 && atomic_read(&pool_count) > target_avail) {
			int idx = atomic_read(&pool_count) - 1;

			batch[n++] = page_pool[idx];
			page_pool[idx] = NULL;
			atomic_set(&pool_count, idx);
		}
		raw_spin_unlock_irqrestore(&pool_lock, flags);

		for (i = 0; i < n; i++) {
			struct page *pg = batch[i];

			pb_track(page_to_pfn(pg), 0, PB_AVAIL);	/* leaving avail */
			if (flip && flip_budget > 0) {
				unsigned long pfn = page_to_pfn(pg);
				int mt = cma_pb_mt(pfn);

				/* §9 whitelist + headroom floor, like every flip */
				if (cma_mt_flippable(mt) &&
				    cma_blocks_n < POOL_SIZE_MAX &&
				    cma_floor_ok(1)) {
					kapi.k_set_pageblock_migratetype(pg,
							migrate_cma_val);
					if (cma_pb_mt(pfn) == migrate_cma_val) {
						__free_pages(pg, PAGE_ORDER);
						cma_blocks[cma_blocks_n++] = pfn;
						flip_budget--;
						flipped++;
						continue;
					}
					/* readback failed: unflip, fall through */
					kapi.k_set_pageblock_migratetype(pg,
							MIGRATE_MOVABLE);
				}
			}
			__free_pages(pg, PAGE_ORDER);
		}
		if (n == 0)
			break;
		cond_resched();
	}

	if (flip) {
		/* Flush the pcp transit so CmaFree/pool_cma agree right away
		 * (a following stage-in grab drains internally anyway; this is
		 * for the accounting the GUI and the floor guard read). */
		if (flipped && kapi.k_drain_all_pages)
			kapi.k_drain_all_pages(NULL);
		mutex_unlock(&cma_mutex);
	}

	WRITE_ONCE(pool_total, newt);
	if (pb_enabled()) {	/* §3: resize is a limbo trigger point too */
		mutex_lock(&cma_mutex);
		cma_limbo_process();
		mutex_unlock(&cma_mutex);
	}
	pr_info("pool shrink: want=%d avail=%d served=%d flipped_to_cma=%d pool_cma=%d\n",
		newt, atomic_read(&pool_count), served_count, flipped,
		cma_pool_cma_2mb());
}

/*
 * pool_want: the single target knob. Settable at insmod and at runtime via the
 * same file. At insmod (before the pool is built) we just record the value;
 * after init, a write resizes live (shrink frees now, grow raises target).
 */
static int pool_want_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	if (v < 0)
		v = 0;
	if (v > pool_size_max)
		v = pool_size_max;
	if (READ_ONCE(pool_want_with_cma) > 0)
		v = cma_align_2mb(v);	/* whole-pageblock accounting while the
					 * reservoir is enabled (§2) */

	if (!READ_ONCE(pool_ready)) {
		WRITE_ONCE(pool_want, v);	/* boot: init will allocate toward it */
	} else if (atomic_read(&acquire_running)) {
		/*
		 * Refuse to resize while the acquire worker runs: it is migrating
		 * pages *into* the pool, and a concurrent resize would race it over
		 * pool_total/pool_count/page_pool[]. Keep it simple and lock-free by
		 * requiring the module to be quiescent first - write 0 to 'acquire'
		 * to interrupt the worker, then set pool_want once it has stopped.
		 */
		return -EBUSY;
	} else {
		pool_do_resize(v);		/* runtime: live resize */
		cma_want_follow(v);		/* grow past with_cma: total follows (§5) */
	}
	return 0;
}

static int pool_want_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(pool_want));
}

static const struct kernel_param_ops pool_want_ops = {
	.set = pool_want_set,
	.get = pool_want_get,
};
module_param_cb(pool_want, &pool_want_ops, NULL, 0600);
MODULE_PARM_DESC(pool_want,
	"Target pages (insmod + runtime): grow raises target, shrink frees now, 0 soft-disables");

/* ================================================================== */
/*  CMA reservoir engine (v10)                                        */
/*                                                                    */
/*  Blocks live in exactly one of two states: HELD (every page ours,  */
/*  refcount-normalized to order-0 singles, label being inspected or  */
/*  flipped) or COMMITTED (label CMA, every page free in buddy on the */
/*  block's own CMA freelist, base pfn recorded in cma_blocks[]).     */
/*  Migratetype flips happen ONLY on fully-held blocks and ONLY in    */
/*  process context under cma_mutex - the free hook never flips and   */
/*  never frees (§9), which is what keeps label and freelist          */
/*  consistent at every instant.                                      */
/* ================================================================== */

/* Free CMA pages on the pgdat the pool lives on. zone_page_state reads the
 * same folded (slightly lagging) counters /proc/meminfo's CmaFree uses, but
 * through our cached zone pointer - global_zone_page_state would add a
 * vm_zone_stat dependency the symbol-safety check forbids. Phones are
 * single-node, so one pgdat is the whole story; a second node would only make
 * the floor guard conservative in the wrong direction, and none of the target
 * devices has one. */
static unsigned long cma_free_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long sum = 0;
	int i;

	if (!acq_zone)
		return 0;
	pgdat = acq_zone->zone_pgdat;
	for (i = 0; i < MAX_NR_ZONES; i++)
		sum += zone_page_state(&pgdat->node_zones[i], NR_FREE_CMA_PAGES);
	return sum;
}

/* Headroom floor (§9): OK to flip @nblocks more pageblocks to CMA? CMA-free
 * memory only serves movable allocations, so what must stay above the floor is
 * available-minus-CMA - the budget kernel/unmovable allocations can still
 * reach. This is a different brake from acquire_mem_floor_mb (anti-livelock
 * for the sweep); both exist, each on its own path. */
static bool cma_floor_ok(int nblocks)
{
	unsigned long floor = (unsigned long)READ_ONCE(cma_reservoir_floor_mb)
				<< (20 - PAGE_SHIFT);
	unsigned long need = (unsigned long)nblocks << cma_pb_order;
	long noncma = (long)si_mem_available() - (long)cma_free_pages();

	return noncma > (long)(floor + need);
}

/* Guardianship total in 2MB pages: held pool (avail + lent out) + reservoir.
 * The §9 total invariant compares this against pool_want_with_cma. */
static int cma_guardianship_2mb(void)
{
	return atomic_read(&pool_count) + READ_ONCE(served_count) +
	       cma_pool_cma_2mb();
}

/* External-source gate (§6/§9): with the reservoir enabled, EXTERNAL memory
 * may only be grabbed while total guardianship is below pool_want_with_cma -
 * past that, a pool shortfall must be staged in from the reservoir, or fresh
 * grabs would push the total over the target. v9 semantics (feature off) are
 * untouched: 0/unset always allows. */
static bool cma_external_ok(void)
{
	int wc = READ_ONCE(pool_want_with_cma);

	return wc <= 0 || cma_guardianship_2mb() < wc;
}

/* FREE non-CMA pages on the pgdat (NR_FREE_PAGES - NR_FREE_CMA_PAGES): what
 * an unmovable allocation can take RIGHT NOW, without reclaim.
 * si_mem_available is the wrong ruler for "can the pool prefill still
 * allocate": it counts reclaimable pagecache that order-9 GFP_KERNEL often
 * cannot actually compact out (measured: prefill got 0/256 while MemAvailable
 * showed 3.4G non-CMA). */
static unsigned long cma_free_noncma_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long freep = 0;
	int i;

	if (!acq_zone)
		return 0;
	pgdat = acq_zone->zone_pgdat;
	for (i = 0; i < MAX_NR_ZONES; i++)
		freep += zone_page_state(&pgdat->node_zones[i], NR_FREE_PAGES);
	return freep - min(freep, cma_free_pages());
}

/*
 * Normalize a raw pfn range WE fully own to independent refcount-1 order-0
 * pages, whatever mix it arrived as (alloc_pages(order) = head 1 / tails 0;
 * alloc_contig_range = singles already). The same refcount surgery
 * rebuild_order9_compound already does, pointed the other way: singles are
 * the working form for label flips and partial frees.
 */
static void cma_span_make_singles(unsigned long pfn, unsigned long nr)
{
	unsigned long i;

	for (i = 0; i < nr; i++)
		set_page_count(pfn_to_page(pfn + i), 1);
}

/*
 * Free a singles-held range into the buddy freelists as order-8 chunks.
 * Order 8 is deliberate: it is above PAGE_ALLOC_COSTLY_ORDER and is not the
 * THP order, so free_unref_page never parks it in a pcp list - every chunk
 * goes straight through __free_one_page onto the freelist of the block's
 * CURRENT label. That immediacy is load-bearing three times over: the
 * first-block +512 accounting check reads NR_FREE_CMA_PAGES right after the
 * free; the floor guard and the GUI read CmaFree and must see reservoir
 * builds/demolitions at once; and a demolition grab must find the pages IN
 * buddy, not parked in pcp. Adjacent chunks coalesce back up on their own.
 * pfn/nr are pageblock-derived so the 256-page alignment always holds. The
 * free hook ignores these frees (order != 9).
 */
static void cma_span_free_to_buddy(unsigned long pfn, unsigned long nr)
{
	const unsigned long step = 1UL << 8;
	unsigned long i, j;

	for (i = 0; i < nr; i += step) {
		struct page *head = pfn_to_page(pfn + i);

		for (j = 1; j < step; j++)
			set_page_count(head + j, 0);	/* order-8 free wants tails at 0 */
		__free_pages(head, 8);
		cond_resched();
	}
}

/*
 * Flip one FULLY-HELD, singles-normalized pageblock to CMA and free it into
 * its own (now CMA) freelist, recording it in cma_blocks[]. Label whitelist
 * first (§9): accept {UNMOVABLE(0), MOVABLE(1), RECLAIMABLE(2)} - a
 * grab/steal often leaves an UNMOVABLE residue on a block that is entirely
 * ours, and holding every page makes the flip safe regardless of label.
 * Reject everything else without needing its runtime value: CMA (someone
 * else's carveout), HIGHATOMIC (the nr_reserved_highatomic ledger would go
 * stale), ISOLATE (an isolation in flight), >= 6 (vendor CHP extensions,
 * guarded by CHP_BUG_ON panics). 0/1/2 as raw integers is safe: they are the
 * first three enumerators on every supported kernel. No steal can relabel the
 * block between check and flip - steals happen when free pages of the block
 * are allocated, and it has none while held. cma_mutex held.
 * Returns 0 = committed; -EPERM = label not flippable (caller swaps
 * candidates, block left untouched and still held); -EIO = readback after the
 * flip failed = systemic, caller must stop the feature (block unflipped,
 * still held).
 */
static int cma_block_commit(unsigned long base)
{
	int mt = cma_pb_mt(base);

	if (!cma_mt_flippable(mt))
		return -EPERM;
	if (cma_blocks_n >= POOL_SIZE_MAX)
		return -EPERM;			/* can't record: don't flip */
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);
	if (cma_pb_mt(base) != migrate_cma_val) {
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		pr_err("cma: flip readback failed at pfn=%lx (wrote %d, read %d)\n",
		       base, migrate_cma_val, cma_pb_mt(base));
		return -EIO;
	}
	cma_span_free_to_buddy(base, CMA_PB_NR);
	cma_blocks[cma_blocks_n++] = base;
	return 0;
}

/*
 * Take one reservoir block back out of circulation: CMA-mode contig grab
 * (migrates app squatters out - they are movable by construction, nothing
 * unmovable can have entered), flip back to MOVABLE while every page is held,
 * free into the movable freelists. Returns 0 = restored (caller must drop the
 * cma_blocks[] entry), -errno = grab failed this time (transient pin /
 * memory pressure: entry stays, caller may retry later). cma_mutex held.
 */
static int cma_block_demolish(unsigned long base)
{
	int ret;

	if (cma_pb_mt(base) != migrate_cma_val) {
		/* Our label is gone - something relabeled a block we own the
		 * bookkeeping for. Never grab through a foreign label (the
		 * isolation undo would stamp OUR migratetype over theirs);
		 * drop the entry and shout. Should never happen. */
		pr_err("cma: reservoir block pfn=%lx lost its CMA label (mt=%d), dropping entry\n",
		       base, cma_pb_mt(base));
		return 0;
	}
	ret = kapi.k_alloc_contig_range_cma(base, base + CMA_PB_NR,
					    GFP_KERNEL | __GFP_NOWARN);
	if (ret)
		return ret;
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
	cma_span_free_to_buddy(base, CMA_PB_NR);
	return 0;
}

/* Racy free-page estimate for one block (no locks: page_count==0 covers buddy,
 * pcp and in-flight frees well enough to RANK blocks by grab cost - the only
 * consumer). */
static u16 cma_block_free_estimate(unsigned long base)
{
	unsigned long pfn, end = base + CMA_PB_NR;
	unsigned int freeish = 0;

	for (pfn = base; pfn < end; pfn++)
		if (page_count(pfn_to_page(pfn)) == 0)
			freeish++;
	return (u16)freeish;
}

#define CMA_EST_SKIP	0xFFFFU		/* demolition failed this pass: don't re-pick */

/*
 * Demolish reservoir blocks until guardianship fits under @target_2mb, or the
 * whole reservoir when @all. Emptiest-first (§5): the fewer squatters a block
 * has, the cheaper the grab-back - estimated once up front, then a linear
 * arg-max per round (worst case ~12k^2 int compares, fine for a rare admin
 * write). A block whose grab fails is skipped for the rest of THIS call;
 * the caller decides whether to run another pass. Returns blocks restored.
 * cma_mutex held, process context.
 */
static int cma_reservoir_demolish(int target_2mb, bool all)
{
	int i, done = 0;

	for (i = 0; i < cma_blocks_n; i++) {
		cma_est[i] = all ? 0 : cma_block_free_estimate(cma_blocks[i]);
		cond_resched();
	}

	while (cma_blocks_n > 0) {
		int pick = -1;
		u16 best = 0;

		if (!all && cma_guardianship_2mb() <= target_2mb)
			break;
		for (i = 0; i < cma_blocks_n; i++) {
			if (cma_est[i] == CMA_EST_SKIP)
				continue;
			if (pick < 0 || cma_est[i] > best) {
				pick = i;
				best = cma_est[i];
			}
		}
		if (pick < 0)
			break;			/* every remaining block resisted */
		if (cma_block_demolish(cma_blocks[pick]) == 0) {
			/* swap-with-last in BOTH arrays */
			cma_blocks[pick] = cma_blocks[cma_blocks_n - 1];
			cma_est[pick] = cma_est[cma_blocks_n - 1];
			WRITE_ONCE(cma_blocks_n, cma_blocks_n - 1);
			done++;
		} else {
			cma_est[pick] = CMA_EST_SKIP;
		}
		cond_resched();
	}
	return done;
}

/*
 * §6 stage-in (the commit-2 slice that works without the pb-hash): move
 * reservoir blocks into the held pool while the pool reserve (avail + served)
 * is short of pool_want. Whole-pageblock unit: CMA-mode grab (migrates app
 * squatters out - movable by construction), flip MOVABLE, rebuild as SUBBLKS
 * order-9 pool compounds, push into the pool, drop the cma_blocks[] entry.
 * Guardianship is conserved 1:1, so the caller's "guardianship >=
 * pool_want_with_cma" source-order check holds across the whole run.
 * Emptiest-first: fewer squatters = cheaper grab = pool fills fastest.
 * Interruptible via acquire_running like every acquire loop. A block whose
 * grab fails is skipped for this call. cma_mutex held, process context.
 * Returns blocks staged in.
 */
static int cma_stage_in(void)
{
	int i, staged = 0;

	for (i = 0; i < cma_blocks_n; i++) {
		cma_est[i] = cma_block_free_estimate(cma_blocks[i]);
		cond_resched();
	}

	while (atomic_read(&acquire_running) && cma_blocks_n > 0 &&
	       atomic_read(&pool_count) + READ_ONCE(served_count) <
			READ_ONCE(pool_want)) {
		int pick = -1;
		u16 best = 0;
		unsigned long base;

		if (atomic_read(&pool_count) + CMA_SUBBLKS > READ_ONCE(pool_size_max))
			break;			/* pool array capacity */
		for (i = 0; i < cma_blocks_n; i++) {
			if (cma_est[i] == CMA_EST_SKIP)
				continue;
			if (pick < 0 || cma_est[i] > best) {
				pick = i;
				best = cma_est[i];
			}
		}
		if (pick < 0)
			break;			/* every remaining block resisted */
		base = cma_blocks[pick];
		if (cma_pb_mt(base) != migrate_cma_val) {
			pr_err("cma: reservoir block pfn=%lx lost its CMA label (mt=%d), dropping entry\n",
			       base, cma_pb_mt(base));
			goto drop;		/* never grab through a foreign label */
		}
		if (kapi.k_alloc_contig_range_cma(base, base + CMA_PB_NR,
						  GFP_KERNEL | __GFP_NOWARN)) {
			cma_est[pick] = CMA_EST_SKIP;
			continue;
		}
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		for (i = 0; i < CMA_SUBBLKS; i++) {
			struct page *head =
				pfn_to_page(base + ((unsigned long)i << PAGE_ORDER));

			rebuild_order9_compound(head);
			if (pool_push_grow(head))
				atomic_inc(&total_refilled);
			else	/* capacity race: let it go whole */
				__free_pages(head, PAGE_ORDER);
		}
		staged++;
drop:
		cma_blocks[pick] = cma_blocks[cma_blocks_n - 1];
		cma_est[pick] = cma_est[cma_blocks_n - 1];
		WRITE_ONCE(cma_blocks_n, cma_blocks_n - 1);
		cond_resched();
	}
	if (staged)
		pr_info("cma: staged %d block(s) into pool, avail=%d pool_cma=%d\n",
			staged, atomic_read(&pool_count), cma_pool_cma_2mb());
	return staged;
}

/* Reservoir deficit (§6): the feature is live and total guardianship has not
 * reached the total target - Phase R's run condition. */
static bool cma_reservoir_deficit(void)
{
	return cma_capable && READ_ONCE(pool_want_with_cma) > 0 &&
	       cma_guardianship_2mb() < READ_ONCE(pool_want_with_cma);
}


/*
 * Rebuild the pb-hash from scratch out of the pool array, the served table
 * and the limbo pool (§3: derived index, not history - THIS is the proof).
 * Run at acquire start: cheap insurance against overflow drift. Concurrent
 * serves/reclaims during the rebuild leave a bit or two stale; the next
 * rebuild corrects them, and nothing downstream treats the index as truth.
 * Process context; chunked so no raw lock is held long.
 */
static void pb_rebuild(void)
{
	unsigned long flags, pfns[64];
	int i, n, start;

	if (!pb_enabled())
		return;
	raw_spin_lock_irqsave(&pb_lock, flags);
	pb_reset_locked();
	pb_overflow = 0;
	raw_spin_unlock_irqrestore(&pb_lock, flags);

	start = 0;
	for (;;) {			/* avail pool, 64 slots per lock hold */
		raw_spin_lock_irqsave(&pool_lock, flags);
		n = 0;
		for (i = start; i < atomic_read(&pool_count) && n < 64; i++)
			if (page_pool[i])
				pfns[n++] = page_to_pfn(page_pool[i]);
		start = i;
		raw_spin_unlock_irqrestore(&pool_lock, flags);
		for (i = 0; i < n; i++)
			pb_track(pfns[i], PB_AVAIL, 0);
		if (n < 64)
			break;
		cond_resched();
	}

	for (start = 0; start < (int)SERVED_MAX; start++) {	/* served table */
		u16 nd;

		raw_spin_lock_irqsave(&served_lock, flags);
		n = 0;
		for (nd = served_bucket[start]; nd != SERVED_NULL && n < 64;
		     nd = served_nodes[nd].next)
			pfns[n++] = served_nodes[nd].pfn;
		raw_spin_unlock_irqrestore(&served_lock, flags);
		for (i = 0; i < n; i++)
			pb_track(pfns[i], PB_SERVED, 0);
		if ((start & 1023) == 0)
			cond_resched();
	}

	raw_spin_lock_irqsave(&limbo_lock, flags);		/* limbo pool */
	n = 0;
	for (i = 0; i < limbo_n && n < 64; i++)
		pfns[n++] = page_to_pfn(limbo_pages[i]);
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	for (i = 0; i < n; i++)
		pb_track(pfns[i], PB_LIMBO, 0);
}

/*
 * Remove every avail member of pageblock @pb from the pool array into @subs.
 * One pool_lock hold over the whole array (~100us worst case at 12k entries):
 * acceptable for the rare process-context callers (exchange, shrink groups),
 * and a chunked removal scan would race the stack's own push/pop reindexing.
 */
static int pool_extract_block(unsigned long pb, struct page **subs, int max)
{
	unsigned long flags;
	int i = 0, got = 0;

	raw_spin_lock_irqsave(&pool_lock, flags);
	while (i < atomic_read(&pool_count)) {
		struct page *p = page_pool[i];

		if (p && got < max &&
		    (page_to_pfn(p) >> cma_pb_order) == pb) {
			int last = atomic_read(&pool_count) - 1;

			subs[got++] = p;
			page_pool[i] = page_pool[last];
			page_pool[last] = NULL;
			atomic_set(&pool_count, last);
			continue;	/* re-examine the swapped-in slot */
		}
		i++;
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	for (i = 0; i < got; i++)
		pb_track(page_to_pfn(subs[i]), 0, PB_AVAIL);
	return got;
}

/* Remove ONE specific avail page from the pool (Phase Q seat swap). */
static struct page *pool_extract_pfn(unsigned long pfn)
{
	unsigned long flags;
	struct page *p = NULL;
	int i;

	raw_spin_lock_irqsave(&pool_lock, flags);
	for (i = 0; i < atomic_read(&pool_count); i++) {
		if (page_pool[i] && page_to_pfn(page_pool[i]) == pfn) {
			int last = atomic_read(&pool_count) - 1;

			p = page_pool[i];
			page_pool[i] = page_pool[last];
			page_pool[last] = NULL;
			atomic_set(&pool_count, last);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&pool_lock, flags);
	if (p)
		pb_track(pfn, 0, PB_AVAIL);
	return p;
}

/*
 * Flip one whole pageblock whose SUBBLKS sub-blocks are all HELD as order-9
 * compounds (extracted from the pool, or pool + limbo) into the reservoir.
 * Same whitelist/readback contract as cma_block_commit; the compounds are
 * freed AFTER the flip through the ordinary order-9 path (the stored pcppage
 * migratetype carries CMA through the pcp transit). Floor is the caller's.
 * cma_mutex held. 0 committed; -EPERM label; -EIO systemic (block NOT freed
 * in either failure - pages stay with the caller).
 */
static int cma_commit_compound_block(unsigned long base, struct page **subs)
{
	int mt = cma_pb_mt(base), i;

	if (!cma_mt_flippable(mt))
		return -EPERM;
	if (cma_blocks_n >= POOL_SIZE_MAX)
		return -EPERM;
	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);
	if (cma_pb_mt(base) != migrate_cma_val) {
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), MIGRATE_MOVABLE);
		pr_err("cma: group flip readback failed at pfn=%lx\n", base);
		return -EIO;
	}
	for (i = 0; i < CMA_SUBBLKS; i++)
		__free_pages(subs[i], PAGE_ORDER);
	cma_blocks[cma_blocks_n++] = base;
	return 0;
}

/* Limbo intake cap (§6): compute BEFORE accepting - strays are only worth
 * holding while the pool has cma_able groups to exchange them against. */
static int cma_limbo_intake_cap(void)
{
	int cap = READ_ONCE(pb_full_avail) * CMA_SUBBLKS;

	return min(cap, LIMBO_MAX);
}

/* An avail member of a class-C pageblock (§7: guardianship incomplete - some
 * sibling is neither pooled, lent, nor in limbo). These are the first to go
 * on a shrink: they can never complete into a flippable block by themselves.
 * Returns the member's pfn, or 0. */
static unsigned long pb_find_class_c_avail(void)
{
	unsigned long flags, pfn = 0;
	unsigned int i;
	u8 full = pb_full_mask();

	for (i = 0; i < PB_HASH_MAX && !pfn; i++) {
		u8 av, guard;

		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		guard = av | pb_nodes[i].served_mask | pb_nodes[i].limbo_mask;
		if (av && guard != full)
			pfn = (pb_nodes[i].pb << cma_pb_order) +
			      ((unsigned long)__ffs(av) << PAGE_ORDER);
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}
	return pfn;
}

/* Scan for a full-avail pageblock (exchange material). Returns the pb key,
 * or 0 (no pageblock 0 on these devices - DRAM starts well above it). */
static unsigned long pb_find_full_avail(void)
{
	unsigned long flags, key = 0;
	unsigned int i;
	u8 full = pb_full_mask();

	for (i = 0; i < PB_HASH_MAX && !key; i++) {
		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		if (pb_nodes[i].avail_mask == full)
			key = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}
	return key;
}

/*
 * §6 Phase R exchange: pull one full-avail group out of the pool, flip it to
 * CMA, and refill the vacated seats with limbo strays 1:1 - net effect: the
 * reservoir gains a whole block, the pool trades clean group members for
 * strays without changing size, and limbo drains. Loops while there is
 * deficit, material and floor headroom. cma_mutex held.
 */
static void cma_limbo_exchange(void)
{
	int guard = LIMBO_MAX + 8;

	if (!pb_enabled())
		return;
	while (guard-- > 0 && cma_reservoir_deficit() &&
	       READ_ONCE(limbo_n) > 0 && READ_ONCE(pb_full_avail) > 0 &&
	       cma_floor_ok(1)) {
		struct page *subs[8];
		unsigned long key = pb_find_full_avail();
		int got, i, ret;

		if (!key)
			break;
		got = pool_extract_block(key, subs, CMA_SUBBLKS);
		if (got < CMA_SUBBLKS) {
			/* stale entry (raced a serve): give the pages back */
			for (i = 0; i < got; i++)
				if (!pool_push(subs[i]))
					__free_pages(subs[i], PAGE_ORDER);
			continue;
		}
		ret = cma_commit_compound_block(key << cma_pb_order, subs);
		if (ret) {
			for (i = 0; i < got; i++)
				if (!pool_push(subs[i]))
					__free_pages(subs[i], PAGE_ORDER);
			if (ret == -EIO) {
				cma_capable = false;
				break;
			}
			continue;	/* -EPERM: label not ours to flip */
		}
		for (i = 0; i < CMA_SUBBLKS; i++) {	/* refill seats 1:1 */
			struct page *lp = limbo_del_idx(0);

			if (!lp)
				break;
			pb_track(page_to_pfn(lp), 0, PB_LIMBO);
			if (!pool_push(lp))
				__free_pages(lp, PAGE_ORDER);
		}
		cond_resched();
	}
}

/* Bump the age of limbo entry @idx; returns the new age or -1 if gone. */
static int limbo_age_bump(int idx)
{
	unsigned long flags;
	int age = -1;

	raw_spin_lock_irqsave(&limbo_lock, flags);
	if (idx >= 0 && idx < limbo_n)
		age = ++limbo_age[idx];
	raw_spin_unlock_irqrestore(&limbo_lock, flags);
	return age;
}

#define LIMBO_MAX_AGE	3	/* process passes before a stray is given up on */

/*
 * §3 limbo outcomes, run at existing process-context trigger points (acquire
 * end, resize end) - no new worker. Per entry: whole block now assembled from
 * avail+limbo members and a deficit exists -> flip it whole; the pool has an
 * open seat -> promote; grown old without its siblings -> genuinely free (a
 * stray we cannot serve and cannot complete is dead weight). cma_mutex held.
 */
static void cma_limbo_process(void)
{
	int i = 0;

	if (!pb_enabled())
		return;
	while (i < READ_ONCE(limbo_n)) {
		unsigned long flags, pfn;
		struct page *p;
		u8 av, sv, lb;

		raw_spin_lock_irqsave(&limbo_lock, flags);
		if (i >= limbo_n) {
			raw_spin_unlock_irqrestore(&limbo_lock, flags);
			break;
		}
		p = limbo_pages[i];
		raw_spin_unlock_irqrestore(&limbo_lock, flags);
		pfn = page_to_pfn(p);

		/* Complete block sitting in avail+limbo, and a deficit: flip it
		 * whole - the "組齊 -> 整塊翻 CMA" outcome. */
		pb_peek(pfn, &av, &sv, &lb);
		if (cma_reservoir_deficit() && cma_floor_ok(1) && !sv && lb &&
		    (u8)(av | lb) == pb_full_mask()) {
			unsigned long key = pfn >> cma_pb_order;
			struct page *subs[8];
			int got, j, ret;

			got = pool_extract_block(key, subs, CMA_SUBBLKS);
			/* pull the block's limbo members (including @p) */
			for (j = 0; j < READ_ONCE(limbo_n) && got < CMA_SUBBLKS;) {
				struct page *lp;

				raw_spin_lock_irqsave(&limbo_lock, flags);
				lp = (j < limbo_n) ? limbo_pages[j] : NULL;
				raw_spin_unlock_irqrestore(&limbo_lock, flags);
				if (!lp)
					break;
				if ((page_to_pfn(lp) >> cma_pb_order) == key) {
					lp = limbo_del_idx(j);
					if (lp) {
						pb_track(page_to_pfn(lp), 0, PB_LIMBO);
						subs[got++] = lp;
					}
					continue;	/* j now holds swapped entry */
				}
				j++;
			}
			ret = (got == CMA_SUBBLKS) ?
				cma_commit_compound_block(key << cma_pb_order, subs) :
				-EAGAIN;
			if (ret) {
				/* give members back: pool if seated, else free */
				for (j = 0; j < got; j++)
					if (!pool_push(subs[j]))
						__free_pages(subs[j], PAGE_ORDER);
				if (ret == -EIO) {
					cma_capable = false;
					return;
				}
			}
			continue;	/* entry i changed either way: re-examine */
		}

		/* Open pool seat: promote the stray into it. */
		if (atomic_read(&pool_count) + READ_ONCE(served_count) <
		    READ_ONCE(pool_want)) {
			p = limbo_del_idx(i);
			if (!p)
				continue;
			pb_track(page_to_pfn(p), 0, PB_LIMBO);
			if (!pool_push_grow(p))
				__free_pages(p, PAGE_ORDER);
			continue;	/* slot i holds the swapped-in entry */
		}

		/* Aged out: its siblings never showed - stop hoarding. */
		if (limbo_age_bump(i) > LIMBO_MAX_AGE) {
			p = limbo_del_idx(i);
			if (!p)
				continue;
			pb_track(page_to_pfn(p), 0, PB_LIMBO);
			__free_pages(p, PAGE_ORDER);
			continue;
		}
		i++;
	}
}

/*
 * Grab the FREE missing sibling windows of @any_pfn's pageblock with targeted
 * movable-mode contig ranges and push them into the pool. §6 pairing: called
 * right after grab_free lands a block (its buddy neighbors are often still
 * free - grab them before anyone else fragments the block) and from the
 * pairing pass over half-full pb entries. Returns windows gained.
 */
static int cma_grab_missing_siblings(unsigned long any_pfn, int max_grabs)
{
	unsigned long base, w;
	u8 av, sv, lb, missing, full;
	int b, got = 0;

	if (!pb_enabled() || !kapi.k_alloc_contig_range ||
	    !kapi.k_prep_compound_page)
		return 0;
	full = pb_full_mask();
	base = any_pfn & ~(CMA_PB_NR - 1);
	pb_peek(any_pfn, &av, &sv, &lb);
	missing = full & ~(av | sv | lb);
	if (!missing)
		return 0;
	for (b = 0; b < CMA_SUBBLKS && got < max_grabs; b++) {
		struct page *head;

		if (!(missing & (1U << b)))
			continue;
		if (atomic_read(&pool_count) + READ_ONCE(served_count) >=
		    READ_ONCE(pool_want) || !cma_external_ok())
			break;
		w = base + ((unsigned long)b << PAGE_ORDER);
		if (!pfn_valid(w) || cma_pb_mt(w) == migrate_cma_val)
			break;		/* never contig-grab through a CMA label */
		if (kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
					      GFP_KERNEL | __GFP_NOWARN |
					      __GFP_NORETRY))
			continue;
		head = pfn_to_page(w);
		rebuild_order9_compound(head);
		if (!pool_push_grow(head)) {
			__free_pages(head, PAGE_ORDER);
			break;
		}
		atomic_inc(&total_refilled);
		got++;
	}
	return got;
}

/*
 * §6 pairing pass (Phase 1.5, SUBBLKS > 1): walk the pb-hash for blocks the
 * pool already partly owns and fetch their free siblings with targeted grabs
 * - far cheaper than sweeping for brand-new blocks, and every success turns a
 * stray into (eventual) exchange material. Bounded per acquire run.
 */
static void cma_pair_fill(void)
{
	unsigned int i;
	int budget = 256;

	if (!pb_enabled())
		return;
	for (i = 0; i < PB_HASH_MAX && budget > 0; i++) {
		unsigned long flags, pb;
		u8 av, sv, lb, full = pb_full_mask();

		if (!atomic_read(&acquire_running) || !cma_external_ok() ||
		    atomic_read(&pool_count) + READ_ONCE(served_count) >=
			READ_ONCE(pool_want))
			break;
		if ((i & 0xff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		sv = pb_nodes[i].served_mask;
		lb = pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!(av | sv | lb) || (u8)(av | sv | lb) == full)
			continue;	/* free node or already complete */
		budget -= CMA_SUBBLKS;
		cma_grab_missing_siblings(pb << cma_pb_order, CMA_SUBBLKS);
	}
}

/*
 * Complete a just-swept window's pageblock by grabbing every OTHER window of
 * the block (§6 Phase R "湊得齊" attempt). Only legal when no sibling is in
 * guardianship (those cases go to limbo and complete via exchange). On
 * success the WHOLE block is singles-held. On failure the partial grabs are
 * freed and the original window is untouched. cma_mutex held.
 */
static int cma_try_complete_block(unsigned long win)
{
	unsigned long base = win & ~(CMA_PB_NR - 1), w;
	unsigned long got_wins[8];
	u8 av, sv, lb;
	int b, got = 0, i;

	pb_peek(win, &av, &sv, &lb);
	if (av | sv | lb)
		return -EBUSY;		/* siblings guarded: limbo/exchange path */
	for (b = 0; b < CMA_SUBBLKS; b++) {
		w = base + ((unsigned long)b << PAGE_ORDER);
		if (w == win)
			continue;
		if (!pfn_valid(w) ||
		    kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
					      GFP_KERNEL | __GFP_NOWARN)) {
			for (i = 0; i < got; i++)
				cma_span_free_to_buddy(got_wins[i],
						       1UL << PAGE_ORDER);
			return -EAGAIN;
		}
		got_wins[got++] = w;
	}
	return 0;
}

/*
 * §6 Phase R consumer: feed one swept-and-assembled 2MB window (singles-held)
 * to the reservoir. Whole block assemblable -> flip it in directly, never
 * transiting the pool. Not assemblable (guarded or straggler siblings) ->
 * park in limbo up to the exchange capacity. Anything else -> free back.
 * Returns 0 consumed, -ENOSPC floor (caller stops Phase R), -EIO systemic,
 * -EAGAIN window freed back.
 */
static int cma_sweep_window_to_reservoir(unsigned long win)
{
	unsigned long base = win & ~(CMA_PB_NR - 1);
	int ret;

	mutex_lock(&cma_mutex);
	if (!cma_floor_ok(1)) {
		mutex_unlock(&cma_mutex);
		cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
		return -ENOSPC;
	}
	if (CMA_SUBBLKS == 1) {
		ret = cma_block_commit(base);	/* window IS the block */
		if (ret == -EIO)
			cma_capable = false;
		mutex_unlock(&cma_mutex);
		if (ret)
			cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
		return ret ? (ret == -EIO ? -EIO : -EAGAIN) : 0;
	}

	ret = cma_try_complete_block(win);
	if (ret == 0) {
		ret = cma_block_commit(base);	/* all windows singles-held */
		if (ret == -EIO)
			cma_capable = false;
		mutex_unlock(&cma_mutex);
		if (ret)
			cma_span_free_to_buddy(base, CMA_PB_NR);
		return ret ? (ret == -EIO ? -EIO : -EAGAIN) : 0;
	}
	/* stray: park for the exchange if it is worth holding */
	if (READ_ONCE(limbo_n) < cma_limbo_intake_cap()) {
		struct page *head = pfn_to_page(win);

		rebuild_order9_compound(head);
		if (limbo_add(head)) {
			pb_track(win, PB_LIMBO, 0);
			mutex_unlock(&cma_mutex);
			return 0;
		}
		/* raced full: back to singles and out */
		cma_span_make_singles(win, 1UL << PAGE_ORDER);
	}
	mutex_unlock(&cma_mutex);
	cma_span_free_to_buddy(win, 1UL << PAGE_ORDER);
	return -EAGAIN;
}

/* Victim search for the Phase Q seat swap: the least-complete pageblock
 * (fewest guarded sub-blocks) other than @exclude that still has an avail
 * member. Returns one avail member's pfn, or 0. Complete blocks are never
 * victims - they are the cma_able capital Phase Q exists to grow. */
static unsigned long pb_find_worst_avail(unsigned long exclude)
{
	unsigned long flags, best_pfn = 0;
	unsigned int i, best_pop = 0xff;

	for (i = 0; i < PB_HASH_MAX; i++) {
		unsigned long pb;
		unsigned int pop;
		u8 av, guard;

		if ((i & 0x3ff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		guard = av | pb_nodes[i].served_mask | pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!av || pb == exclude || guard == pb_full_mask())
			continue;
		pop = pb_popcount8(guard);
		if (pop < best_pop) {
			best_pop = pop;
			best_pfn = (pb << cma_pb_order) +
				   ((unsigned long)__ffs(av) << PAGE_ORDER);
			if (pop == 1)
				break;	/* cannot get worse */
		}
	}
	return best_pfn;
}

/*
 * §6 Phase Q (quality): counts are met but some avail sub-blocks sit in
 * pageblocks the module will never own whole (foreign stragglers). Upgrade:
 * targeted-grab the missing windows of partly-owned blocks; each success
 * REALLY frees one least-complete stray (process context may genuinely free)
 * and seats the new sibling instead - pool size unchanged, cma_able rising.
 * Also sweeps the limbo pool. Returns true if anything improved; the caller
 * stops on false ("quality converged"). SUBBLKS > 1 only.
 */
static bool cma_phase_q(void)
{
	unsigned int i;
	int budget = 64;
	bool progress = false;

	if (!pb_enabled())
		return false;
	for (i = 0; i < PB_HASH_MAX && budget > 0; i++) {
		unsigned long flags, pb, base, w;
		u8 av, sv, lb, missing, full = pb_full_mask();
		int b;

		if (!atomic_read(&acquire_running))
			break;
		if ((i & 0xff) == 0)
			cond_resched();
		raw_spin_lock_irqsave(&pb_lock, flags);
		av = pb_nodes[i].avail_mask;
		sv = pb_nodes[i].served_mask;
		lb = pb_nodes[i].limbo_mask;
		pb = pb_nodes[i].pb;
		raw_spin_unlock_irqrestore(&pb_lock, flags);
		if (!av || (u8)(av | sv | lb) == full)
			continue;
		missing = full & ~(av | sv | lb);
		base = pb << cma_pb_order;
		for (b = 0; b < CMA_SUBBLKS && budget > 0; b++) {
			struct page *head, *victim;
			unsigned long vpfn;

			if (!(missing & (1U << b)))
				continue;
			w = base + ((unsigned long)b << PAGE_ORDER);
			if (!pfn_valid(w) || cma_pb_mt(w) == migrate_cma_val)
				break;
			if (kapi.k_alloc_contig_range(w, w + (1UL << PAGE_ORDER),
						      GFP_KERNEL | __GFP_NOWARN)) {
				budget--;
				continue;
			}
			head = pfn_to_page(w);
			rebuild_order9_compound(head);
			/* seat swap: really free one least-complete stray */
			vpfn = pb_find_worst_avail(pb);
			victim = vpfn ? pool_extract_pfn(vpfn) : NULL;
			if (victim) {
				__free_pages(victim, PAGE_ORDER);
				if (!pool_push(head))
					__free_pages(head, PAGE_ORDER);
				progress = true;
			} else if (!pool_push_grow(head)) {
				__free_pages(head, PAGE_ORDER);
				budget = 0;	/* no seats, no victims: done */
			} else {
				progress = true;
			}
			budget--;
		}
	}
	mutex_lock(&cma_mutex);
	cma_limbo_process();	/* 順手: promote/complete/evict strays */
	mutex_unlock(&cma_mutex);
	return progress;
}

/* Accept the first-block +512 accounting check when at least 3/4 of it shows:
 * the per-cpu vmstat fold our order-8 frees force can carry up to ~125 pages
 * of unrelated residue either way (stat threshold cap), while the failure
 * being tested for - migrate_cma_val not actually meaning MIGRATE_CMA - shows
 * a delta of ~0. 384 splits those cleanly. */
#define CMA_VERIFY_DELTA_MIN	(3L << (PAGE_ORDER - 2))
#define CMA_VERIFY_CAND_MAX	8

/*
 * First-block verification (§4): the ONE place the module proves, on live
 * memory, that its three preflight-derived beliefs hold before it will label
 * anything CMA: (a) pageblock_order_val is the kernel's real pageblock order,
 * (b) migrate_cma_val is really MIGRATE_CMA (accounting moves with it),
 * (c) CONTIG_RANGE_CMA can grab a block back. Runs UNCONDITIONALLY at every
 * init (cma_boot_build) and nowhere else - its outcome folds into cma_capable
 * as a boot constant, so there is no first-run runtime state to manage.
 *
 * The span is TWO pageblocks at DOUBLE alignment, fully held. That shape is
 * what makes an off-by-one-level pageblock_order_val belief SAFE to probe: if
 * the belief is one too small, the flip covers the whole held span and the
 * base+PB_NR readback (expected MOVABLE, reads CMA) catches it; if one too
 * big, the flip covers only the first real block and the base+PB_NR-1
 * readback (expected CMA, reads MOVABLE) catches it. Either way nothing
 * outside memory we hold was ever relabeled, and the flip-back restores it
 * whole. One allocation of order pb+1 provides the span where the kernel
 * allows it; otherwise (order-10 pageblocks on 6.1, whose max alloc order is
 * 10) one pageblock is allocated and its 2x-alignment sibling is grabbed with
 * a movable-mode contig range.
 *
 * Candidates may carry ANY whitelisted label ({0,1,2}, like every other
 * flip): the boundary checks compare against RECORDED pre-values, not against
 * a hardcoded MOVABLE, so steal residue does not defeat them. This matters in
 * the field: after the module itself has churned GFP_KERNEL blocks through
 * buddy for a while, movable high-order allocations come back with UNMOVABLE
 * residue labels almost every time (measured: a runtime enable on the OnePlus
 * exhausted eight strictly-MOVABLE candidates and silently fell back to v9).
 * A candidate with a non-whitelisted label is HELD until the hunt ends:
 * freeing it at once would just hand the same block back on the next try.
 *
 * On success the span becomes reservoir blocks #1 and #2 (zero waste). On a
 * pre-write mismatch: nothing was written, swap candidates (bounded, then
 * pr_warn + feature stays unverified). On any post-write failure: systemic -
 * restore the recorded label if still clean, otherwise ABANDON the first
 * block CMA-labeled (its label and freelist stay mutually consistent; the
 * memory remains app-usable movable; it merely refuses unmovable allocations
 * until reboot), pr_err, feature off.
 *
 * Returns 0 verified; -EAGAIN candidates exhausted; -EIO systemic failure.
 * cma_mutex held.
 */
static int cma_verify_first_span(void)
{
	const gfp_t mov_gfp = GFP_HIGHUSER_MOVABLE | __GFP_NOWARN | __GFP_RETRY_MAYFAIL;
	const unsigned long pb_nr = CMA_PB_NR, span_nr = 2 * CMA_PB_NR;
	unsigned long rej_pfn[CMA_VERIFY_CAND_MAX];
	unsigned long rej_nr[CMA_VERIFY_CAND_MAX];
	unsigned long base = 0;
	int nrej = 0, tries, ret = -EAGAIN;
	int pre0, pre2;
	struct zone *z;
	long before, delta;

	for (tries = 0; tries < CMA_VERIFY_CAND_MAX && nrej < CMA_VERIFY_CAND_MAX;
	     tries++) {
		struct page *p;
		unsigned long pfn, cand;

		if (cma_pb_order + 1 <= GH_MAX_ALLOC_ORDER) {
			/* one naturally 2x-aligned allocation = the whole span */
			p = alloc_pages(mov_gfp, cma_pb_order + 1);
			if (!p)
				break;
			cand = page_to_pfn(p);
			if (!cma_mt_flippable(cma_pb_mt(cand)) ||
			    !cma_mt_flippable(cma_pb_mt(cand + pb_nr))) {
				cma_span_make_singles(cand, span_nr);
				rej_pfn[nrej] = cand;
				rej_nr[nrej++] = span_nr;
				continue;
			}
			cma_span_make_singles(cand, span_nr);
			base = cand;
			break;
		}
		/*
		 * pb+1 exceeds the alloc limit (order-10 pageblocks, 6.1): hold
		 * single pageblocks one by one and PAIR any two that are each
		 * other's 2x-alignment buddy - the stash doubles as the pairing
		 * pool, both halves are then already held and NO foreign memory
		 * is ever contig-grabbed. Adjacent blocks freed together (a
		 * prior rmmod, a balloon exit) sit near each other on the
		 * freelists, so consecutive allocations pair quickly. Fall back
		 * to contig-grabbing the not-yet-held sibling only when it
		 * still sits free/movable outside (measured: on churned memory
		 * that grab alone fails eight times out of eight - pairing is
		 * what makes a runtime-insmod verification viable at pb=10).
		 */
		p = alloc_pages(mov_gfp, cma_pb_order);
		if (!p)
			break;
		pfn = page_to_pfn(p);
		cand = pfn & ~(span_nr - 1);	/* 2x-aligned span base */
		{
			unsigned long sib = (cand == pfn) ? pfn + pb_nr
							  : cand;
			int j, pair = -1;

			for (j = 0; j < nrej; j++)
				if (rej_nr[j] == pb_nr && rej_pfn[j] == sib) {
					pair = j;
					break;
				}
			if (pair >= 0 && cma_mt_flippable(cma_pb_mt(pfn)) &&
			    cma_mt_flippable(cma_pb_mt(sib))) {
				/* both halves held: consume the stash entry */
				rej_pfn[pair] = rej_pfn[nrej - 1];
				rej_nr[pair] = rej_nr[nrej - 1];
				nrej--;
				cma_span_make_singles(pfn, pb_nr);
				base = cand;	/* stashed half is singles already */
				break;
			}
			if (pfn_valid(sib) && pfn_valid(sib + pb_nr - 1) &&
			    page_zone(pfn_to_page(sib)) == page_zone(p) &&
			    cma_mt_flippable(cma_pb_mt(pfn)) &&
			    cma_mt_flippable(cma_pb_mt(sib)) &&
			    !kapi.k_alloc_contig_range(sib, sib + pb_nr,
					GFP_KERNEL | __GFP_NOWARN)) {
				cma_span_make_singles(cand, span_nr);
				base = cand;
				break;
			}
			cma_span_make_singles(pfn, pb_nr);
			rej_pfn[nrej] = pfn;
			rej_nr[nrej++] = pb_nr;
		}
	}
	if (!base)
		goto out;	/* -EAGAIN: no flippable span obtainable */

	/* Pre-write reads (non-destructive): RECORD the boundary labels the
	 * post-write checks compare against. pre0 is base's REAL block's label
	 * (whatever the real pageblock order is) - the restore value. */
	pre0 = cma_pb_mt(base);
	pre2 = cma_pb_mt(base + pb_nr);
	if (!cma_mt_flippable(pre0) ||
	    !cma_mt_flippable(cma_pb_mt(base + pb_nr - 1)) ||
	    !cma_mt_flippable(pre2)) {
		cma_span_free_to_buddy(base, span_nr);
		goto out;	/* raced relabel: treat as exhausted */
	}

	kapi.k_set_pageblock_migratetype(pfn_to_page(base), migrate_cma_val);

	/* Post-write boundary reads: base+PB_NR-1 catches a too-BIG order
	 * belief (the real flip fell short of our believed block), base+PB_NR
	 * catches a too-SMALL one (the real flip overflowed into the second
	 * half - its label must still read exactly what was recorded). */
	if (cma_pb_mt(base + pb_nr - 1) != migrate_cma_val ||
	    cma_pb_mt(base + pb_nr) != pre2) {
		pr_err("cma: pageblock_order_val=%d verification FAILED (post-write mt %d/%d, expected %d/%d) - feature off\n",
		       pageblock_order_val,
		       cma_pb_mt(base + pb_nr - 1), cma_pb_mt(base + pb_nr),
		       migrate_cma_val, pre2);
		kapi.k_set_pageblock_migratetype(pfn_to_page(base), pre0);
		cma_span_free_to_buddy(base, span_nr);
		ret = -EIO;
		goto out;
	}

	/* Semantic check: one 2MB freed into the flipped block must move the
	 * zone's CMA-free accounting by (about) +512 - proving migrate_cma_val
	 * IS the accounting migratetype, not merely a writable label. */
	z = page_zone(pfn_to_page(base));
	before = (long)zone_page_state(z, NR_FREE_CMA_PAGES);
	cma_span_free_to_buddy(base, 1UL << PAGE_ORDER);
	delta = (long)zone_page_state(z, NR_FREE_CMA_PAGES) - before;
	if (delta < CMA_VERIFY_DELTA_MIN) {
		pr_err("cma: migrate_cma_val=%d verification FAILED (CMA-free delta %ld after +512 free) - feature off\n",
		       migrate_cma_val, delta);
		goto abandon_first;
	}

	/* End-to-end CMA-mode grab of exactly what was freed. */
	if (kapi.k_alloc_contig_range_cma(base, base + (1UL << PAGE_ORDER),
					  GFP_KERNEL | __GFP_NOWARN)) {
		pr_err("cma: CONTIG_RANGE_CMA grab-back verification FAILED - feature off\n");
		goto abandon_first;
	}

	/* Verified. First half (fully held again) is reservoir block #1... */
	cma_span_free_to_buddy(base, pb_nr);
	cma_blocks[cma_blocks_n++] = base;
	/* ...second half goes through the standard commit = block #2 (zero
	 * waste). Its label was just screened flippable, so -EPERM is all but
	 * impossible; -EIO here would mean the setter works at base but not at
	 * base+pb_nr - treat as systemic and undo block #1 too. */
	if (cma_block_commit(base + pb_nr) != 0) {
		cma_span_free_to_buddy(base + pb_nr, pb_nr);
		if (cma_block_demolish(base) == 0)
			cma_blocks_n--;
		ret = -EIO;
		goto out;
	}
	ret = 0;
	pr_info("cma: first-block verification passed (pb_order=%d migrate_cma=%d pfn=%lx)\n",
		cma_pb_order, migrate_cma_val, base);
	goto out;

abandon_first:
	/*
	 * 2MB of the first block already sits on its labeled freelist, so an
	 * unflip would break label/freelist consistency. Leave the label, free
	 * the rest of the first block into it (consistent), free the still-
	 * MOVABLE second half normally, and forget the block. Worst case one
	 * stray CMA-labeled pageblock until reboot - inert and app-usable.
	 */
	pr_err("cma: abandoning one CMA-labeled pageblock at pfn=%lx\n", base);
	cma_span_free_to_buddy(base + (1UL << PAGE_ORDER),
			       pb_nr - (1UL << PAGE_ORDER));
	cma_span_free_to_buddy(base + pb_nr, pb_nr);
	ret = -EIO;
out:
	while (nrej-- > 0)
		cma_span_free_to_buddy(rej_pfn[nrej], rej_nr[nrej]);
	/* Candidates exhausted is a QUIET failure mode by design (nothing was
	 * written), but the operator must be able to see why an enable flow
	 * silently stayed on the v9 path - the exact trap a runtime enable on
	 * a churned system fell into before this warning existed. */
	if (ret == -EAGAIN)
		pr_warn("cma: first-block verification found no usable candidate span (feature stays unverified)\n");
	return ret;
}

#define CMA_BUILD_REJECTS	16

/*
 * Build the reservoir toward @target_blocks pageblocks (the first-block
 * verification has already run at init - see cma_boot_build). Per block, the
 * cheap path: allocate one whole pageblock, whitelist, flip, single readback,
 * free in place.
 *
 * The per-block allocation is deliberately UNMOVABLE (GFP_KERNEL), not
 * movable: a movable request is ALLOC_CMA-eligible, and once the growing
 * reservoir dominates the zone's free memory the allocator starts handing the
 * builder its own CMA blocks back (label 3 -> whitelist reject), starving the
 * build - measured on the OnePlus 6.12: movable builds stalled at 1684/3840
 * blocks with the reject stash full of our own pages. GFP_KERNEL can never be
 * served from a CMA freelist, at the cost of an UNMOVABLE steal-residue label
 * on the block - which is exactly what the {0,1,2} whitelist exists to accept
 * (the flip is safe regardless of label because every page is held).
 *
 * Rejected candidates (foreign-CMA/highatomic/CHP labels) are held to the end
 * - freeing one immediately would hand the same block right back. Every early
 * stop is a pr_warn with the reason (§10). cma_mutex held, process context.
 */
static void cma_reservoir_build(int target_blocks, unsigned long reserve_pages)
{
	const gfp_t blk_gfp = GFP_KERNEL | __GFP_NOWARN | __GFP_RETRY_MAYFAIL;
	struct page *rej[CMA_BUILD_REJECTS];
	const char *stop = NULL;
	int nrej = 0, i, ret;

	while (cma_blocks_n < target_blocks) {
		struct page *p;
		unsigned long base;
		int mt;

		if (!cma_floor_ok(1)) {
			stop = "headroom floor (cma_reservoir_floor_mb)";
			break;
		}
		/* Leave @reserve_pages of FREE non-CMA memory for whoever runs
		 * after the build (the boot prefill): the build itself consumes
		 * exactly that pool, and si_mem_available can't see the
		 * difference (measured: an unreserved build left the prefill
		 * 0/256 with MemAvailable still reading gigabytes). */
		if (reserve_pages &&
		    cma_free_noncma_pages() < reserve_pages + 2 * CMA_PB_NR) {
			stop = "leaving free memory for the pool prefill";
			break;
		}
		p = alloc_pages(blk_gfp, cma_pb_order);
		if (!p) {
			stop = "pageblock allocation failed";
			break;
		}
		base = page_to_pfn(p);
		mt = cma_pb_mt(base);
		if (!cma_mt_flippable(mt)) {	/* §9 whitelist */
			rej[nrej++] = p;
			if (nrej >= CMA_BUILD_REJECTS) {
				stop = "too many unflippable candidates";
				break;
			}
			continue;
		}
		cma_span_make_singles(base, CMA_PB_NR);
		ret = cma_block_commit(base);
		if (ret) {
			/* label can't change while fully held, so this is the
			 * -EIO readback path: systemic. Free the held block
			 * (label restored/unchanged) and stop the feature. */
			cma_span_free_to_buddy(base, CMA_PB_NR);
			cma_capable = false;
			stop = "flip readback failed";
			break;
		}
		cond_resched();
	}

	for (i = 0; i < nrej; i++)
		__free_pages(rej[i], cma_pb_order);
	if (stop)
		pr_warn("cma: reservoir build stopped: %s (%d/%d blocks)\n",
			stop, cma_blocks_n, target_blocks);
}

/*
 * Boot-time capability latch, want alignment and reservoir build - called
 * from module_init AFTER kapi_init and BEFORE the pool prefill (§10):
 * reservoir blocks are whole pageblocks and want the cleanest memory this
 * boot will ever offer; the prefill's order-9 allocations then draw from what
 * remains. GFP_KERNEL prefill can never draw from the blocks just committed
 * (unmovable allocations cannot enter CMA freelists - the property the whole
 * reservoir stands on).
 *
 * The first-block verification runs HERE, EVERY init, regardless of whether a
 * reservoir was asked for - deliberately: capability becomes a boot constant
 * settled while memory is at its cleanest, and no runtime enable flow ever
 * needs a lazy first-run verification again (a runtime verify on churned
 * memory was measured failing candidate hunts that boot-time trivially
 * passes). The two blocks the protocol seeds cost ~one contig grab to trim
 * back out when no reservoir is wanted.
 */
static void cma_boot_build(void)
{
	int target_blocks, ret;

	cma_capable = kapi_can_cma();
	if (pool_want_with_cma < 0)
		pool_want_with_cma = 0;
	if (!cma_capable) {
		if (pool_want_with_cma)
			pr_warn("cma: pool_want_with_cma=%d requested but unavailable (syms=%d migrate_cma_val=%d pageblock_order_val=%d) - v9 path\n",
				pool_want_with_cma,
				!!(kapi.k_set_pageblock_migratetype &&
				   kapi.k_get_pfnblock_flags_mask &&
				   kapi.k_alloc_contig_range_cma),
				migrate_cma_val, pageblock_order_val);
		pool_want_with_cma = 0;
		return;
	}
	cma_pb_order = pageblock_order_val;
	if (pb_enabled()) {			/* arm the completeness index */
		unsigned long flags;

		raw_spin_lock_irqsave(&pb_lock, flags);
		pb_reset_locked();
		raw_spin_unlock_irqrestore(&pb_lock, flags);
	}

	/* Clamp + align both targets (§2/§5): want <= with_cma <= size_max,
	 * whole pageblocks each while enabled (0 stays the no-reservoir value). */
	pool_want = cma_align_2mb(pool_want);
	if (pool_want_with_cma > 0) {
		if (pool_want_with_cma < pool_want)
			pool_want_with_cma = pool_want;
		if (pool_want_with_cma > pool_size_max)
			pool_want_with_cma = pool_size_max;
		pool_want_with_cma = cma_align_2mb(pool_want_with_cma);
	}
	target_blocks = pool_want_with_cma > 0 ?
		(pool_want_with_cma - pool_want) / CMA_SUBBLKS : 0;

	mutex_lock(&cma_mutex);
	ret = cma_floor_ok(2) ? cma_verify_first_span() : -EAGAIN;
	if (ret) {
		cma_capable = false;		/* boot constant: no retry */
		cma_pb_order = -1;
		if (pool_want_with_cma)
			pr_warn("cma: verification failed (%d), pool_want_with_cma=%d falls back to v9\n",
				ret, pool_want_with_cma);
		pool_want_with_cma = 0;
		mutex_unlock(&cma_mutex);
		return;
	}
	if (cma_blocks_n < target_blocks) {
		/* Reserve the prefill's budget (pool_want order-9 pages) plus
		 * a 64MB compaction slack in FREE non-CMA memory, so building
		 * the reservoir first cannot starve the prefill that follows. */
		cma_reservoir_build(target_blocks,
				    ((unsigned long)pool_want << PAGE_ORDER) +
				    (64UL << (20 - PAGE_SHIFT)));
	}
	/* Verification seeded two blocks: trim whatever exceeds the target
	 * (all of it when no reservoir is wanted) - the fresh blocks are fully
	 * free, so the grab-backs are instant. */
	if (cma_blocks_n > target_blocks)
		cma_reservoir_demolish(target_blocks * CMA_SUBBLKS, false);
	mutex_unlock(&cma_mutex);

	if (cma_blocks_n)
		pr_info("cma: reservoir ready: %d pageblock(s) x %d MB = %d MB (target %d x 2MB)\n",
			cma_blocks_n, 2 * CMA_SUBBLKS,
			cma_pool_cma_2mb() * 2,
			pool_want_with_cma - pool_want);
}

/*
 * pool_want_with_cma (§5): the total guardianship target.
 *   write big:   target only - construction is exclusively acquire's job.
 *   write small: clamped to pool_want below; excess reservoir is demolished
 *                NOW, emptiest blocks first.
 *   write 0:     disable - demolish the whole reservoir; capability and
 *                verification results survive, so re-enabling does not
 *                re-test.
 * Writing pool_want above with_cma pulls with_cma along (cma_want_follow);
 * the reverse direction clamps here. Boot-time writes only record: init
 * clamps, aligns and builds.
 */
static int pool_want_with_cma_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v))
		return -EINVAL;
	if (v < 0)
		v = 0;

	if (!READ_ONCE(pool_ready)) {
		WRITE_ONCE(pool_want_with_cma, v);
		return 0;
	}
	if (v == 0) {
		mutex_lock(&cma_mutex);
		WRITE_ONCE(pool_want_with_cma, 0);
		cma_reservoir_demolish(0, true);
		if (cma_blocks_n)
			pr_warn("cma: %d block(s) resisted demolition, still reservoir\n",
				cma_blocks_n);
		mutex_unlock(&cma_mutex);
		return 0;
	}
	if (!cma_capable)
		return -ENOSYS;

	v = max(v, READ_ONCE(pool_want));
	v = min(v, pool_size_max);
	v = cma_align_2mb(v);
	mutex_lock(&cma_mutex);
	WRITE_ONCE(pool_want_with_cma, v);
	cma_reservoir_demolish(v, false);	/* no-op unless shrinking */
	mutex_unlock(&cma_mutex);
	return 0;
}

static int pool_want_with_cma_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(pool_want_with_cma));
}

static const struct kernel_param_ops pool_want_with_cma_ops = {
	.set = pool_want_with_cma_set,
	.get = pool_want_with_cma_get,
};
module_param_cb(pool_want_with_cma, &pool_want_with_cma_ops, NULL, 0600);
MODULE_PARM_DESC(pool_want_with_cma,
	"Total guardianship target in 2MB pages (pool + CMA reservoir); 0 = reservoir off (v9 behavior)");

static int pool_cma_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", cma_pool_cma_2mb());
}

static const struct kernel_param_ops pool_cma_ops = {
	.get = pool_cma_get,
};
module_param_cb(pool_cma, &pool_cma_ops, NULL, 0400);
MODULE_PARM_DESC(pool_cma, "CMA reservoir size in 2MB-page equivalents (read-only)");

static int pool_avail_cma_able_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", cma_avail_cma_able_2mb());
}

static const struct kernel_param_ops pool_avail_cma_able_ops = {
	.get = pool_avail_cma_able_get,
};
module_param_cb(pool_avail_cma_able, &pool_avail_cma_able_ops, NULL, 0400);
MODULE_PARM_DESC(pool_avail_cma_able,
	"Avail pool pages flippable to CMA as whole pageblocks (read-only)");

/*
 * §11 cma_usage: GUI occupancy view of the reservoir. The module is the ONLY
 * party that can produce it - the reservoir is no registered struct cma (so
 * debugfs cannot see it) and global CmaFree mixes in the vendor carveouts.
 * Racy-by-design snapshot (pages move while we scan; GUI use tolerates it),
 * buddy-order / folio_nr_pages jumps + cond_resched keep it cheap, and a ~1s
 * cache keeps a polling GUI from rescanning gigabytes. used_anon_mb doubles
 * as the acquire cost estimate: roughly how much must migrate out before a VM
 * start.
 */
static unsigned long cma_usage_v[5];	/* pages: total/free/used/anon/file */
static int cma_usage_b[3];		/* blocks: free/partial/full */
static unsigned long cma_usage_jiffies;	/* 0 = no snapshot yet */

static int cma_usage_get(char *buf, const struct kernel_param *kp)
{
	unsigned long free_p = 0, anon_p = 0, file_p = 0, total_p;
	int bfree = 0, bpart = 0, bfull = 0, i, n;

	mutex_lock(&cma_mutex);
	if (cma_usage_jiffies &&
	    time_before(jiffies, cma_usage_jiffies + HZ))
		goto emit;		/* ~1s cache: numbers, not a string -
					 * snprintf would be a new undefined
					 * symbol; sysfs_emit is already ours */

	for (i = 0; i < cma_blocks_n; i++) {
		unsigned long pfn = cma_blocks[i];
		unsigned long end = pfn + CMA_PB_NR, bf = 0;

		while (pfn < end) {
			struct page *p = pfn_to_page(pfn);
			unsigned long step = 1;

			if (PageBuddy(p)) {
				unsigned int order =
					(unsigned int)READ_ONCE(page_private(p));

				if (order > GH_MAX_ALLOC_ORDER)
					order = 0;	/* racy read: distrust */
				step = min(1UL << order, end - pfn);
				bf += step;
			} else if (page_count(p) == 0) {
				bf++;		/* pcp-parked / in-flight free */
			} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
				struct folio *folio = page_folio(p);
				unsigned long fn = folio_nr_pages(folio);

				step = fn ? min(fn, end - pfn) : 1;
				if (folio_test_anon(folio))
					anon_p += step;
				else if (folio_test_lru(folio))
					file_p += step;
#else
				/* pre-folio kernels cannot run the reservoir
				 * (no Gunyah, feature compile-gated) - keep
				 * the scan compiling with page-flag spellings */
				struct page *head = compound_head(p);
				unsigned long fn = compound_nr(head);

				step = fn ? min(fn, end - pfn) : 1;
				if (PageAnon(head))
					anon_p += step;
				else if (PageLRU(head))
					file_p += step;
#endif
			}
			pfn += step;
		}
		free_p += bf;
		if (bf >= CMA_PB_NR)
			bfree++;
		else if (bf == 0)
			bfull++;
		else
			bpart++;
		cond_resched();
	}
	/* feature-off / empty: no shift by an unvalidated order */
	total_p = cma_blocks_n ? (unsigned long)cma_blocks_n << cma_pb_order : 0;
	cma_usage_v[0] = total_p;
	cma_usage_v[1] = free_p;
	cma_usage_v[2] = total_p - min(free_p, total_p);
	cma_usage_v[3] = anon_p;
	cma_usage_v[4] = file_p;
	cma_usage_b[0] = bfree;
	cma_usage_b[1] = bpart;
	cma_usage_b[2] = bfull;
	cma_usage_jiffies = jiffies ? jiffies : 1;
emit:
	n = sysfs_emit(buf,
		       "reservoir_mb=%lu\nfree_mb=%lu\nused_mb=%lu\nused_anon_mb=%lu\nused_file_mb=%lu\nblocks_free=%d\nblocks_partial=%d\nblocks_full=%d\n",
		       cma_usage_v[0] >> (20 - PAGE_SHIFT),
		       cma_usage_v[1] >> (20 - PAGE_SHIFT),
		       cma_usage_v[2] >> (20 - PAGE_SHIFT),
		       cma_usage_v[3] >> (20 - PAGE_SHIFT),
		       cma_usage_v[4] >> (20 - PAGE_SHIFT),
		       cma_usage_b[0], cma_usage_b[1], cma_usage_b[2]);
	mutex_unlock(&cma_mutex);
	return n;
}

static const struct kernel_param_ops cma_usage_ops = {
	.get = cma_usage_get,
};
module_param_cb(cma_usage, &cma_usage_ops, NULL, 0400);
MODULE_PARM_DESC(cma_usage,
	"Reservoir occupancy snapshot: free/used/anon/file MB + block states (read-only, ~1s cached)");

/* ================================================================== */
/*  Aggressive acquire (GUI-only)                                      */
/* ================================================================== */

/* Resolve a non-exported symbol via a throwaway kprobe (module already uses
 * kprobes). Returns NULL if the symbol is absent. */
static void *resolve_kfunc(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	void *addr = NULL;

	if (register_kprobe(&kp) == 0) {
		addr = (void *)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

/* Resolve the first name in a NULL-terminated list that exists (handles the
 * *_noprof rename dance). */
static void *resolve_first(const char * const *names)
{
	void *addr = NULL;
	int i;

	for (i = 0; names[i] && !addr; i++)
		addr = resolve_kfunc(names[i]);
	return addr;
}

/* resolve_kfunc(), but honor the disable_kapi ABI guard: a symbol the userspace
 * preflight flagged incompatible resolves to NULL, so it is never called. */
static void *resolve_gated(const char *name)
{
	if (kapi_sym_disabled(name)) {
		pr_info("kapi: %s left NULL (disable_kapi ABI guard)\n", name);
		return NULL;
	}
	return resolve_kfunc(name);
}

/* Warn on unknown disable_kapi tokens (typo guard). The known set is the
 * generated KAPI_SYMBOLS list, so it stays in lockstep with the ABI table. */
static void kapi_validate_disable(void)
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
		/* Whole-token match without strlen: a matched strncmp implies
		 * known[i] has >= tlen chars, so [tlen] is a valid read (the
		 * NUL iff the lengths match too). strlen(known[i]) here quietly
		 * became an out-of-line libcall on the 5.10 toolchain once the
		 * symbol list outgrew clang's unroll threshold - a new
		 * undefined symbol the ABI check forbids. */
		for (i = 0; i < ARRAY_SIZE(known); i++)
			if (!strncmp(p, known[i], tlen) &&
			    known[i][tlen] == '\0') {
				hit = true;
				break;
			}
		if (!hit)
			pr_warn("kapi: disable_kapi token \"%.*s\" not a known symbol\n",
				(int)tlen, p);
		p += tlen;
	}
}

/* Populate the kapi object once at init. Missing (or guard-disabled) stay NULL. */
static void kapi_init(void)
{
	static const char * const acp[] = {	/* alloc_contig_pages (id=1) */
		"alloc_contig_pages_noprof", "alloc_contig_pages", NULL };
	static const char * const acr[] = {	/* alloc_contig_range (id=2/3) */
		"alloc_contig_range_noprof", "alloc_contig_range", NULL };

	kapi_validate_disable();

	/* alloc_contig_pages/alloc_contig_range resolve a *_noprof/base fallback pair, so gate
	 * on the logical (base) name the guard/preflight use, not the variant. */
	kapi.k_alloc_contig_pages		= kapi_sym_disabled("alloc_contig_pages") ?
						NULL : resolve_first(acp);
	kapi.k_prep_compound_page		= resolve_gated("prep_compound_page");
	kapi.k_lru_add_drain_all		= resolve_gated("lru_add_drain_all");
	kapi.k_drop_slab			= resolve_gated("drop_slab");
	kapi.k_drain_all_pages		= resolve_gated("drain_all_pages");
	kapi.k_mem_cgroup_from_task	= resolve_gated("mem_cgroup_from_task");

	/* Divergent-contract symbols: raw pointer into kraw, normalizing shim
	 * into kapi. Resolve failure/guard leaves both NULL, so the NULL checks
	 * and kapi_can_* checklists work unchanged on the kapi side. */
	kraw.k_alloc_contig_range		= kapi_sym_disabled("alloc_contig_range") ?
						NULL : resolve_first(acr);
	kapi.k_alloc_contig_range		= kraw.k_alloc_contig_range ?
						kapi_alloc_contig_range_shim : NULL;
	kraw.k_folio_isolate_lru		= resolve_gated("folio_isolate_lru");
	kapi.k_folio_isolate_lru		= kraw.k_folio_isolate_lru ?
						kapi_folio_isolate_lru_shim : NULL;
	kraw.k_reclaim_pages		= resolve_gated("reclaim_pages");
	kapi.k_reclaim_pages		= kraw.k_reclaim_pages ?
						kapi_reclaim_pages_shim : NULL;
	kraw.k_try_to_free_mem_cgroup_pages =
				resolve_gated("try_to_free_mem_cgroup_pages");
	kapi.k_try_to_free_mem_cgroup_pages = kraw.k_try_to_free_mem_cgroup_pages ?
						kapi_try_to_free_memcg_shim : NULL;

	/* CMA reservoir setter/reader: resolved only on [6.1, 6.16). Below 6.1
	 * there is no Gunyah to serve; from 6.16 the migratetype rework
	 * (MIGRATE_ISOLATE becomes a standalone bit) changes pageblock-flag
	 * semantics out from under both symbols and 6.18 makes the setter
	 * static - so the feature auto-disables to the v9 path there, exactly
	 * like a failed resolution would. The CMA-mode contig shim additionally
	 * needs the preflight-supplied migratetype value. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
	kapi.k_set_pageblock_migratetype = resolve_gated("set_pageblock_migratetype");
	kapi.k_get_pfnblock_flags_mask	 = resolve_gated("get_pfnblock_flags_mask");
#endif
	kapi.k_alloc_contig_range_cma	 = (kraw.k_alloc_contig_range &&
					    migrate_cma_val >= 0) ?
						kapi_alloc_contig_range_cma_shim : NULL;

	if (disable_kapi && *disable_kapi)
		pr_info("kapi: ABI guard active, disable_kapi=\"%s\"\n", disable_kapi);
	pr_info("kapi: id1=%d id2=%d id3=%d A(sysreclaim)=%d drain=%d drop_slab=%d cma=%d\n",
		kapi_can_v1(), kapi_can_sweep(), kapi_can_evict_b(),
		kapi_has_sys_reclaim(), !!kapi.k_lru_add_drain_all, !!kapi.k_drop_slab,
		kapi_can_cma());
}

/*
 * Resolve the zone the smart acquire (id=2) sweeps. Prefer an existing pool
 * page; otherwise (empty pool) borrow the zone a GFP_KERNEL order-0 allocation
 * would use (ZONE_NORMAL, where order-9 movable memory lives). Process context
 * only. Returns NULL if it cannot be determined (id=2 then stays disabled).
 */
static struct zone *acq_resolve_zone(void)
{
	struct page *p;
	struct zone *z = NULL;

	if (atomic_read(&pool_count) > 0 && page_pool[0])
		return page_zone(page_pool[0]);

	p = alloc_pages(GFP_KERNEL | __GFP_NOWARN, 0);
	if (p) {
		z = page_zone(p);
		__free_pages(p, 0);
	}
	return z;
}

/*
 * Fast path shared by both acquire modes: grab a free order-9 block straight
 * from the buddy allocator (splitting a free order-10, plus one light
 * compaction pass). This is instant and needs no migration - crucial because
 * the migration allocators (alloc_contig_*) do NOT harvest buddy free blocks,
 * so without this the pool ignores whatever contiguous 2MB blocks are already
 * free. __GFP_NORETRY keeps it cheap: when buddy has no free order-9/10 it fails
 * fast and the caller falls back to migration. __GFP_COMP yields a ready compound
 * page (refcount 1) - no prep_compound_page needed.
 */
static bool acquire_grab_free(bool strong)
{
	/*
	 * strong=false: __GFP_NORETRY - cheap, grabs an already-free order-9/10
	 * (split), light effort. strong=true: __GFP_RETRY_MAYFAIL - runs
	 * compaction + reclaim to assemble a block from freed movable pages (the
	 * in-kernel equivalent of "compact_memory then alloc"). Never OOM-kills.
	 */
	gfp_t gfp = GFP_KERNEL | __GFP_COMP | __GFP_NOWARN |
		    (strong ? __GFP_RETRY_MAYFAIL : __GFP_NORETRY);
	struct page *p = alloc_pages(gfp, PAGE_ORDER);

	if (!p)
		return false;
	if (!pool_push_grow(p)) {
		__free_pages(p, PAGE_ORDER);
		return false;
	}
	atomic_inc(&total_refilled);
	/* §6 pairing: the block just entered the index - grab its still-free
	 * siblings NOW, while buddy adjacency makes them likely free too
	 * (no-op on SUBBLKS == 1). */
	cma_grab_missing_siblings(page_to_pfn(p), CMA_SUBBLKS);
	return true;
}

/*
 * B: evict one pageblock-aligned 2MB window's own reclaimable folios to zram, so
 * the window frees up for alloc_contig without needing migration destinations.
 * Refcount protocol mirrors madvise(MADV_PAGEOUT): folio_try_get ->
 * kapi.k_folio_isolate_lru -> list_add -> folio_put; kapi.k_reclaim_pages then pages
 * them out and drops the isolation refs. A race just skips a folio or makes the later
 * alloc_contig return -EBUSY. Bounded to <=512 folios and yields every 64 pages,
 * so it cannot livelock the way system-wide reclaim can. Returns the number of
 * folios queued (>=0), or -1 if B is unavailable (folio API <5.16 or unresolved).
 */
static int reclaim_block_b(unsigned long start_pfn)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
	const unsigned long end = start_pfn + (1UL << PAGE_ORDER);
	unsigned long pfn = start_pfn;
	LIST_HEAD(folios);
	int queued = 0;

	if (!kapi.k_folio_isolate_lru || !kapi.k_reclaim_pages)
		return -1;

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
		fnr = folio_nr_pages(folio);		/* re-read under our ref */
		if (kapi.k_folio_isolate_lru(folio)) {
			list_add(&folio->lru, &folios);
			queued++;
		}
		folio_put(folio);
		pfn += fnr ? fnr : 1;
		if ((pfn & 0x3f) == 0)
			cond_resched();
	}
	if (!list_empty(&folios))
		kapi.k_reclaim_pages(&folios);		/* pages out; drops the isolation refs */
	return queued;
#else
	return -1;	/* folio API absent (<5.16): B unavailable */
#endif
}

/*
 * Reclaim ~nr pages from the kworker's (root) memcg, allowing swap to zram, so
 * compaction has room to assemble 2MB blocks. Bounded by nr. Returns pages
 * reclaimed, or 0 if unavailable. Keep nr SMALL per call and check headroom
 * before calling - driving the system to near-OOM makes reclaim livelock into
 * an RCU stall (see acquire_worker_v3).
 */
static unsigned long reclaim_system_a(unsigned long nr)
{
	struct mem_cgroup *memcg;

	if (!kapi_has_sys_reclaim())
		return 0;
	memcg = kapi.k_mem_cgroup_from_task(current);	/* kworker -> root memcg */
	if (!memcg)
		return 0;
	return kapi.k_try_to_free_mem_cgroup_pages(memcg, nr);
}

/*
 * acquire id=1 (original): fill the pool toward pool_want using
 * alloc_contig_pages(), which *migrates* movable pages to assemble 2MB blocks
 * even when the buddy allocator has none. Heavy (migration) but only ever run
 * from the GUI button, never at boot, so a worst-case stall/abort is
 * reboot-recoverable and can't bootloop.
 *
 * alloc_contig_pages returns nr_pages individual refcount-1 pages; turn the
 * range into one order-9 compound page (prep_compound_page sets structure,
 * then zero the tail refcounts) so it is indistinguishable from an
 * alloc_pages(__GFP_COMP) page for serving and for __free_pages on release.
 */
static void acquire_worker_v1(void)
{
	const int nr = 1 << PAGE_ORDER;
	const char *reason = NULL;
	int got = 0, fail_score = 0;

	if (!kapi_can_v1())
		return;

	/*
	 * Fill until the reserve (available + served-out) reaches pool_want, so
	 * pages already lent to VMs count toward the target. Exit when:
	 *   - acquire_running is cleared (GUI wrote 0 to interrupt - pool_want is
	 *     left intact so the remaining deficit still shows as "waiting"), or
	 *   - the cumulative fail score reaches ACQUIRE_MAX_FAILS (each migration
	 *     failure adds 1, each success subtracts ACQUIRE_FAIL_DECAY, floored
	 *     at 0), the normal "system can't migrate any more" give-up.
	 * msleep throttles CPU.
	 */
	while (atomic_read(&acquire_running) && cma_external_ok() &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want) &&
	       fail_score < ACQUIRE_MAX_FAILS) {
		struct page *p = kapi.k_alloc_contig_pages(nr,
				GFP_KERNEL | __GFP_NOWARN, numa_node_id(), NULL);

		if (!p) {
			fail_score++;
			msleep(ACQUIRE_DELAY_MS);
			continue;
		}
		fail_score -= ACQUIRE_FAIL_DECAY;
		if (fail_score < 0)
			fail_score = 0;

		rebuild_order9_compound(p);

		if (!pool_push_grow(p)) {
			__free_pages(p, PAGE_ORDER);
			reason = "pool capacity full";
			break;			/* pool array full */
		}
		atomic_inc(&total_refilled);
		got++;
		msleep(ACQUIRE_DELAY_MS);	/* let the system breathe */
	}

	/* Why the loop ended, for the GUI (see acquire_stop_reason). */
	if (!reason) {
		if (!atomic_read(&acquire_running))
			reason = "stopped by user";
		else if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want))
			reason = "reached target";
		else
			reason = "migration exhausted";	/* fail_score hit the cap */
	}
	WRITE_ONCE(acquire_stop_reason, reason);

	pr_info("acquire(v1) done: +%d pages, avail=%d capacity=%d want=%d\n",
		got, atomic_read(&pool_count), pool_total, READ_ONCE(pool_want));
}

/*
 * Feasibility gate for one pageblock-aligned 2MB window - the authoritative
 * "will this assemble" test, run BEFORE any migration/eviction so a doomed
 * window is skipped rather than white-kicked. A window is assemblable iff every
 * page is free, on the LRU (anon/file: migratable & B-reclaimable), or
 * __PageMovable (driver-movable), and none is reserved or longterm-pinned.
 *
 * Migratetype is NOT a gate anymore: Unmovable/Reclaimable pageblocks are tried
 * too - many hold no actual straggler, just the label, and that ~7.6GB is where
 * the theoretical headroom lives. Only CMA (FOLL_LONGTERM trap), isolated, and
 * highatomic blocks are refused outright. The per-page straggler test decides
 * the rest, and it also naturally rejects our OWN pool pages (in-use, not LRU,
 * not movable) so the sweep never re-grabs the reserve. compound_head resolves
 * THP tails to the head. REQUIRES a prior lru_add_drain_all() so pagevec-resident
 * folios read as LRU; a rare transient false positive just skips a good window.
 */
static bool block_candidate(struct zone *z, unsigned long pfn)
{
	const unsigned long end = pfn + (1UL << PAGE_ORDER);
	struct page *first;
	unsigned long i;
	int mt;

	if (!pfn_valid(pfn))
		return false;
	first = pfn_to_page(pfn);
	if (page_zone(first) != z)
		return false;
	mt = get_pageblock_migratetype(first);
	if (is_migrate_cma(mt) || mt == MIGRATE_HIGHATOMIC
#ifdef CONFIG_MEMORY_ISOLATION
	    || mt == MIGRATE_ISOLATE
#endif
	   )
		return false;			/* CMA/isolated/atomic: never touch */

	for (i = pfn; i < end; i++) {
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

/* Keep at least this much memory available (MB) for the rest of the system.
 * Reclaiming past the floor is what livelocks into an RCU stall -> panic/reboot:
 * the freed pages are instantly re-faulted, every CPU spins in reclaim, and 21 s
 * without a quiescent state trips the watchdog. Staying above the floor keeps
 * reclaim making forward progress, so worst case is "gave up early", not a hang.
 * Tunable: raise it if the sweep leaves the system too tight; the historical
 * safe value was 1024. Read fresh each window, so a live write takes effect
 * mid-sweep. */
static unsigned int acquire_mem_floor_mb = 512;
module_param(acquire_mem_floor_mb, uint, 0600);
MODULE_PARM_DESC(acquire_mem_floor_mb,
	"id=2/3 sweep stops when si_mem_available() drops below this many MB (default 512)");

#define ACQUIRE_MEM_FLOOR_PAGES \
	((unsigned long)READ_ONCE(acquire_mem_floor_mb) << (20 - PAGE_SHIFT))
#define ACQUIRE_A_STRIDE	8	/* A: system reclaim once per this many failed windows */

/* Reclaim strategy the shared sweep uses when migration alone can't assemble a
 * window: B evicts that window's OWN folios (targeted, needs folio API 5.16+);
 * A does a strided bounded system-wide reclaim to create migration headroom
 * (coarse, but works wherever try_to_free_mem_cgroup_pages resolves). */
enum acquire_assist { ASSIST_B, ASSIST_A };

/* Drop reclaimable slab (dentry/inode) once at sweep start. That slab is NOT on
 * the LRU, so neither B nor A can reclaim it, yet a single dentry page poisons a
 * whole window (block_candidate sees a straggler). Shrinking it up front unpoisons
 * those windows so they become assemblable - the module doing what a userspace
 * `echo 2 > drop_caches` does. Global side-effect (filesystem re-lookups are
 * slower afterwards), so it is toggleable; default on. */
static int acquire_drop_slab = 1;
module_param(acquire_drop_slab, int, 0600);
MODULE_PARM_DESC(acquire_drop_slab,
	"id=2/3: drop reclaimable slab at sweep start to unpoison windows (default 1)");

/*
 * The shared acquire sweep (id=2 uses ASSIST_A, id=3 uses ASSIST_B): one FULL
 * sweep of the zone from the persistent cursor, wrapping once. Per window: try
 * cheap ASYNC migration; if that fails and the window has no unmovable
 * straggler, apply the reclaim assist and retry SYNC.
 *   ASSIST_B: evict the window's OWN folios to zram (reclaim_block_b) - it then
 *             frees in place, no migration destinations needed. Bounded to one
 *             2MB window; never strips the system.
 *   ASSIST_A: strided, bounded system-wide reclaim (reclaim_system_a) to free
 *             migration destinations elsewhere; the SYNC retry migrates the
 *             window into them. One batch every ACQUIRE_A_STRIDE failed windows.
 *
 * No failure cap: a window that still won't assemble (transient contention, an
 * undetectable pin) is passed over - the sweep keeps going. SPARSEMEM holes in
 * the zone span are skipped a whole section at a time, so both the runtime and
 * the reported scan count track real present memory, not the (much larger) span.
 * It exits only when (a) the whole zone span has been traversed once, (b)
 * pool_want is met, (c) the user
 * writes acquire=0, or (d) the memory floor is hit (the sole safety brake -
 * never reclaim toward OOM; this is what keeps A from the RCU-stall reboot the
 * earlier unbounded A-loop hit). The cursor is persistent and advanced every
 * window, so a user-/floor-stopped partial sweep resumes where it left off.
 * lru_add_drain_all() up front makes PageLRU (hence the straggler test) accurate.
 */
static void acquire_sweep(enum acquire_assist assist)
{
	const unsigned long nr = 1UL << PAGE_ORDER;
	struct zone *z = acq_zone;
	unsigned long zstart, zend, pfn, span_pages, present_blocks;
	unsigned long examined = 0, advanced = 0, evicted = 0, since_a = 0;
	const char *reason = NULL;
	int got = 0, got_r = 0, i;
	bool floor_hit = false;

	if (!z || !kapi_can_sweep())
		return;

	zstart = ALIGN(z->zone_start_pfn, nr);
	zend = zone_end_pfn(z);
	if (zend <= zstart)
		return;
	if (scan_cursor < zstart || scan_cursor >= zend)
		scan_cursor = zstart;
	span_pages = zend - zstart;			/* full span, holes included */
	/* Honest denominator: real present memory in 2MB blocks (e.g. 16G RAM,
	 * not the ~48G span). Scanning/reporting against the span reads as a
	 * falsely-high "scanned 48G" when two thirds of it are holes. */
	present_blocks = READ_ONCE(z->present_pages) >> PAGE_ORDER;
	if (!present_blocks)
		present_blocks = 1;

	if (kapi.k_lru_add_drain_all)
		kapi.k_lru_add_drain_all();		/* make PageLRU accurate for the straggler test */

	/*
	 * §6 outer condition: the sweep serves TWO consumers in fill order -
	 * the pool while its reserve is short of pool_want (external intake
	 * capped by cma_external_ok), then the RESERVOIR while guardianship is
	 * short of pool_want_with_cma (Phase R: assembled windows flip to CMA
	 * or park in limbo, never transiting the pool). Feature off reduces
	 * both terms to the v9 condition exactly.
	 */
	while (atomic_read(&acquire_running) &&
	       ((cma_external_ok() &&
		 atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) ||
		cma_reservoir_deficit())) {
		struct page *head;
		int ret;

		cond_resched();

		if (advanced >= span_pages) {
			reason = cma_reservoir_deficit() ?
				 "cma sources exhausted" :
				 "scanned all present memory";
			break;			/* traversed the whole span once: done */
		}
		pfn = scan_cursor;

#ifdef CONFIG_SPARSEMEM
		/* Skip SPARSEMEM holes a whole section at a time. A zone's span
		 * (zone_start_pfn..zone_end_pfn) can include large absent sections
		 * (non-contiguous DRAM, vendor carveouts); an absent section has NO
		 * valid pfn, so stepping one 2MB window at a time would burn up to
		 * PAGES_PER_SECTION/nr iterations per section on block_candidate's
		 * pfn_valid() failure for nothing. Jump the cursor to the next section
		 * and count the gap as span *advanced* only - NOT as an *examined*
		 * window - so both the runtime and the reported scan count reflect
		 * real present memory. (present_pages only *counts* present memory; it
		 * can't say WHERE the holes are, so pfn_valid drives the skip.) */
		if (!pfn_valid(pfn)) {
			unsigned long next = ALIGN(pfn + 1, PAGES_PER_SECTION);

			advanced += next - pfn;
			scan_cursor = (next >= zend) ? zstart : next;
			continue;
		}
#endif
		examined++;			/* present windows actually looked at */
		advanced += nr;
		scan_cursor = pfn + nr;		/* advance + persist BEFORE work so a */
		if (scan_cursor >= zend)	/* stop here resumes at the next window */
			scan_cursor = zstart;

		/* block_candidate is now the full feasibility gate: a window that
		 * would white-kick (straggler / reserved / pinned) is rejected here,
		 * BEFORE any migration or eviction - so we never evict for nothing.
		 * It also skips our own pool pages. */
		if (!block_candidate(z, pfn))
			continue;

		/* Cheap first: ASYNC migration (window may be mostly free already). */
		ret = kapi.k_alloc_contig_range(pfn, pfn + nr,
			GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);

		if (ret != 0) {
			/* Straggler-free but occupied. The only brake: never reclaim
			 * the system toward OOM. */
			if (si_mem_available() < ACQUIRE_MEM_FLOOR_PAGES + nr) {
				floor_hit = true;
				reason = "low-memory floor";
				break;
			}
			if (assist == ASSIST_B) {
				/* Evict this window's own folios, then it frees in place. */
				i = reclaim_block_b(pfn);
				if (i < 0) {
					reason = "evict-B unavailable";
					break;		/* B unavailable (use id=2/A instead) */
				}
				evicted += (unsigned long)i;
			} else {		/* ASSIST_A */
				/* Strided system-wide reclaim -> migration headroom that
				 * this and the next few windows migrate into. */
				if (++since_a >= ACQUIRE_A_STRIDE) {
					since_a = 0;
					evicted += reclaim_system_a(nr * 8);
				}
			}
			ret = kapi.k_alloc_contig_range(pfn, pfn + nr,
				GFP_KERNEL | __GFP_NOWARN);	/* SYNC retry */
		}
		msleep(ACQUIRE_DELAY_MS);
		if (ret != 0)
			continue;			/* transient/pinned: pass over it */

		if (atomic_read(&pool_count) + served_count <
		    READ_ONCE(pool_want) && cma_external_ok()) {
			/* pool first (§6 fill order) */
			head = pfn_to_page(pfn);
			rebuild_order9_compound(head);
			if (!pool_push_grow(head)) {
				__free_pages(head, PAGE_ORDER);
				reason = "pool capacity full";
				break;
			}
			atomic_inc(&total_refilled);
			got++;
			if (pb_enabled())	/* pairing: buddies often still free */
				cma_grab_missing_siblings(pfn, CMA_SUBBLKS);
		} else if (cma_reservoir_deficit()) {
			/* Phase R: the window feeds the reservoir */
			ret = cma_sweep_window_to_reservoir(pfn);
			if (ret == -ENOSPC) {
				floor_hit = true;
				reason = "cma headroom floor";
				break;
			}
			if (ret == -EIO) {
				reason = "cma flip failed (systemic)";
				break;
			}
			if (ret == 0)
				got_r++;
		} else {
			/* both targets met between the check and the grab */
			cma_span_free_to_buddy(pfn, nr);
		}
	}

	/* No break took a specific reason -> the while-condition ended it: the user
	 * interrupted (acquire=0) or the target was reached. Surface it for the GUI. */
	if (!reason)
		reason = atomic_read(&acquire_running) ?
			 (READ_ONCE(pool_want_with_cma) > 0 ?
			  "reached target(with_cma)" : "reached target")
						       : "stopped by user";
	WRITE_ONCE(acquire_stop_reason, reason);

	pr_info("acquire(%s) done: +%d pool +%d reservoir-fed, avail=%d want=%d pool_cma=%d evicted=%luMB scanned=%lu/%lu blocks (present=%luMB span=%luMB) floor_hit=%d\n",
		assist == ASSIST_B ? "id3/B" : "id2/A", got, got_r,
		atomic_read(&pool_count),
		READ_ONCE(pool_want), cma_pool_cma_2mb(),
		evicted >> (20 - PAGE_SHIFT),
		examined, present_blocks,
		(present_blocks << PAGE_ORDER) >> (20 - PAGE_SHIFT),
		span_pages >> (20 - PAGE_SHIFT), floor_hit);
}

static void acquire_worker(struct work_struct *w)
{
	/*
	 * v10 Phase S (before everything): when total guardianship already
	 * meets pool_want_with_cma, the pool's shortfall comes OUT OF THE
	 * RESERVOIR first (§6 source order) - a targeted CMA-mode grab per
	 * block, seconds instead of a sweep, and no external memory is taken
	 * that would push guardianship past the total target. Only when
	 * guardianship is BELOW the total target do the external phases below
	 * lead. Stage-in conserves guardianship 1:1, so checking once is
	 * checking always.
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0) {
		mutex_lock(&cma_mutex);
		if (cma_guardianship_2mb() >= READ_ONCE(pool_want_with_cma))
			cma_stage_in();
		mutex_unlock(&cma_mutex);
	}

	/*
	 * v10 Phase 1.5 prep (SUBBLKS > 1): refresh the derived index (cheap
	 * insurance against overflow drift), then the pairing pass - targeted
	 * grabs of free siblings of blocks the pool already partly owns (§6).
	 * Both no-ops on order-9 devices.
	 */
	pb_rebuild();
	cma_pair_fill();

	/*
	 * Phase 0 (all modes): drop reclaimable slab (dentry/inode) once. That slab
	 * is not on the LRU so no acquire path can reclaim it, yet a single dentry
	 * page poisons a whole window - freeing it up front unpoisons windows for
	 * every mode (including v1's alloc_contig_pages and the Phase 1 fast-path).
	 * Skipped when Phase S already met the target: dropping the system's
	 * dentry/inode cache is a real cost, pointless with no deficit left.
	 */
	if (atomic_read(&pool_count) + served_count < READ_ONCE(pool_want) &&
	    READ_ONCE(acquire_drop_slab) && kapi.k_drop_slab)
		kapi.k_drop_slab();

	/*
	 * Phase 1 (all modes): harvest already-free order-9/10 blocks from buddy
	 * - instant, no migration. Drains what's cheaply available (now including
	 * blocks the slab drop just freed) before paying for migration in Phase 2.
	 * cma_external_ok caps EXTERNAL intake at the with_cma total (§9); a
	 * remaining pool shortfall past that point is Phase S2's job.
	 */
	while (atomic_read(&acquire_running) && cma_external_ok() &&
	       atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		if (!acquire_grab_free(false))
			break;
		cond_resched();
	}

	/*
	 * v10 Phase S2: pool still short after the cheap external grabs -
	 * cannibalize the reservoir BEFORE the expensive sweep (§6 fill order:
	 * pool first, reservoir later). This is what makes acquire fast when
	 * assembled blocks sit in cma_blocks[] but guardianship is below the
	 * total target (e.g. a prefill that fell short at load): VM serving
	 * capacity must not wait minutes of zone sweep for memory the module
	 * already guards. The reservoir deficit this leaves behind is Phase
	 * R's to rebuild.
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0 &&
	    atomic_read(&pool_count) + served_count < READ_ONCE(pool_want)) {
		mutex_lock(&cma_mutex);
		cma_stage_in();
		mutex_unlock(&cma_mutex);
	}

	/* Phase 2: migration for the fragmented remainder, plus a reclaim assist:
	 * id=3 = per-block evict (B), id=2 = system-wide reclaim (A), id=1 = old.
	 * The sweep also runs Phase R (reservoir fill) per its §6 outer
	 * condition; mode 1 remains pool-only legacy. */
	if (READ_ONCE(acquire_mode) == 3)
		acquire_sweep(ASSIST_B);	/* per-block evict to zram */
	else if (READ_ONCE(acquire_mode) == 2)
		acquire_sweep(ASSIST_A);	/* strided system-wide reclaim */
	else
		acquire_worker_v1();

	/*
	 * v10 tail: exchange limbo strays against full-avail pool groups (the
	 * Phase R "待配滿/掃畢 -> 交換" step), settle limbo outcomes, then
	 * Phase Q - when every count is met but part of the pool sits in
	 * blocks we will never own whole, trade those strays for completable
	 * siblings until nothing improves ("quality converged").
	 */
	if (cma_capable && READ_ONCE(pool_want_with_cma) > 0) {
		mutex_lock(&cma_mutex);
		cma_limbo_exchange();
		cma_limbo_process();
		mutex_unlock(&cma_mutex);
		if (pb_enabled() && atomic_read(&acquire_running) &&
		    atomic_read(&pool_count) + served_count >=
			READ_ONCE(pool_want) &&
		    cma_avail_cma_able_2mb() < atomic_read(&pool_count)) {
			int rounds = 8;
			bool prog = true;

			while (prog && rounds-- > 0 &&
			       atomic_read(&acquire_running))
				prog = cma_phase_q();
			if (!prog && atomic_read(&acquire_running))
				WRITE_ONCE(acquire_stop_reason,
					   "quality converged");
		}
	}
	atomic_set(&acquire_running, 0);
}

/*
 * acquire: 0 = stop the running worker, 1 = original acquire (alloc_contig_pages
 * blind scan), 2 = shared sweep with system-wide reclaim assist (A), 3 = shared
 * sweep with per-block evict assist (B). 2 and 3 run the same acquire_sweep()
 * skeleton, differing only in how they free space when migration alone fails.
 */
static int acquire_set(const char *val, const struct kernel_param *kp)
{
	int v;

	if (kstrtoint(val, 10, &v) || v < 0 || v > 3)
		return -EINVAL;
	if (v == 0) {
		/* Interrupt: the worker exits at its next check; pool_want stays. */
		atomic_set(&acquire_running, 0);
		return 0;
	}
	/* Required-symbol checklists (option 1): a mode whose required set is
	 * incomplete is refused here rather than half-run. id=3 requires B on top of
	 * the sweep - without it id=3 would just be id=2, so point the user at id=2. */
	if (v == 1 && !kapi_can_v1())
		return -ENOSYS;
	if (v == 2 && (!kapi_can_sweep() || !acq_zone))
		return -ENOSYS;
	if (v == 3 && (!kapi_can_evict_b() || !acq_zone))
		return -ENOSYS;
	/* Nothing to do if the pool reserve meets want AND total guardianship
	 * meets pool_want_with_cma (§6 outer condition - a reservoir deficit
	 * alone is enough for acquire to have Phase R work to do). */
	if (atomic_read(&pool_count) + served_count >= READ_ONCE(pool_want) &&
	    !cma_reservoir_deficit()) {
		WRITE_ONCE(acquire_stop_reason, "already at target");
		return 0;
	}
	if (atomic_cmpxchg(&acquire_running, 0, 1) != 0)
		return -EBUSY;
	WRITE_ONCE(acquire_mode, v);
	WRITE_ONCE(acquire_stop_reason, "acquiring");
	/*
	 * Fire-and-return: the heavy migration runs in the kworker and the write
	 * comes back immediately. Blocking here would hold the per-module param
	 * mutex for the whole acquire, stalling every other get/set (refill_stat
	 * polling, etc.) and the GUI. Instead the worker sets acquire_running=0
	 * when done; userspace polls refill_stat's acquire_active to watch live
	 * progress and detect completion.
	 */
	schedule_work(&acquire_work);
	return 0;
}

static const struct kernel_param_ops acquire_ops = {
	.set = acquire_set,
};
module_param_cb(acquire, &acquire_ops, NULL, 0200);
MODULE_PARM_DESC(acquire,
	"0=stop, 1=alloc_contig_pages, 2=sweep+system-reclaim(A), 3=sweep+per-block-evict(B)");

/* ================================================================== */
/*  Module init / exit                                                */
/* ================================================================== */

static int __init hugepage_reserve_init(void)
{
	int i, ret;

	/*
	 * Effective cap: keep min(ram/2, SYSTEM_RESERVE) away from the pool -
	 * half the RAM on small systems, SYSTEM_RESERVE on big ones - and offer
	 * the rest, up to the POOL_SIZE_MAX_RAM array bound. Computed before any
	 * pool_want clamp below and before pool_ready, so every later write site
	 * sees the final value.
	 */
	{
		unsigned long ram = totalram_pages() << PAGE_SHIFT;	/* bytes */
		unsigned long keep = min(ram / 2, SYSTEM_RESERVE);
		unsigned long pool_size_max_ram = min(ram - keep, POOL_SIZE_MAX_RAM);

		pool_size_max = (int)(pool_size_max_ram >> (PAGE_SHIFT + PAGE_ORDER));
	}

	if (pool_want < 0)
		pool_want = 1024;
	if (pool_want > pool_size_max)
		pool_want = pool_size_max;

	if (refill_delay_ms < 1000)
		refill_delay_ms = 1000;

	/* "Grace already expired": jiffies wraps from INITIAL_JIFFIES, so a zero
	 * init would leave reconcile in no-purge mode for half the wrap period. */
	last_destroy_jiffies = jiffies - msecs_to_jiffies(RECONCILE_GRACE_MS) - 1;

	pr_info("allocating up to %d x 2MB = %d MB (limit %d x 2MB = %d MB)\n",
		pool_want, pool_want * 2, pool_size_max, pool_size_max * 2);

	INIT_WORK(&vm_owner_sweep_work, vm_owner_sweep_worker);
	INIT_DELAYED_WORK(&refill_work, refill_worker);
	INIT_DELAYED_WORK(&pcp_drain_work, pcp_drain_worker);
	INIT_WORK(&acquire_work, acquire_worker);
	served_init();

	/* Resolve every optional kernel symbol into the kapi object; a miss only
	 * disables/degrades a feature, never fails module load. */
	kapi_init();

	/*
	 * v10: resolve the sweep zone early (the reservoir floor guard reads
	 * CMA-free counts off its pgdat), then build the CMA reservoir BEFORE
	 * the pool prefill - whole pageblocks want the cleanest memory this
	 * boot will ever offer, and the prefill can't draw from committed
	 * blocks anyway (GFP_KERNEL never allocates from CMA freelists). The
	 * zone is re-resolved after the prefill as before; a pool page's zone
	 * is the more authoritative answer once one exists.
	 */
	acq_zone = acq_resolve_zone();
	cma_boot_build();

	/* Prepare kretprobe struct */
	kretp.handler       = ret_handler;
	kretp.entry_handler = entry_handler;
	kretp.data_size     = 0;
	kretp.maxactive     = 20;

	/* Pre-allocate order-9 compound pages toward the target (give up per-page
	 * when even compaction/reclaim can't produce one; never OOM-kills). */
	for (i = 0; i < pool_want; i++) {
		page_pool[i] = alloc_pages(GFP_KERNEL | __GFP_COMP |
					   __GFP_NOWARN | __GFP_RETRY_MAYFAIL,
					   PAGE_ORDER);
		if (!page_pool[i]) {
			pr_info("stopped at %d/%d\n",
				i, pool_want);
			break;
		}
		if ((i + 1) % 50 == 0) {
			cond_resched();
			if ((i + 1) % 100 == 0)
				pr_info("allocated %d/%d ...\n",
					i + 1, pool_want);
		}
	}

	pool_total = i;			/* capacity = what we actually got */
	/* pool_want stays = the requested target (may exceed capacity) */
	atomic_set(&pool_count, pool_total);
	for (i = 0; i < pool_total; i++)	/* v10: index the prefill */
		pb_track(page_to_pfn(page_pool[i]), PB_AVAIL, 0);

	/*
	 * An empty pool is a valid load state (boot-time memory too fragmented,
	 * or pool_want=0 soft-disable): the hooks stay armed but idle while
	 * pool_count is 0, and capacity grows later via acquire (or a pool_want
	 * write followed by acquire).
	 */
	if (pool_total == 0 && pool_want > 0)
		pr_warn("started empty; grow via acquire toward target %d\n",
			pool_want);

	pr_info("pool ready: %d x 2MB = %d MB (target %d)\n",
		pool_total, pool_total * 2, pool_want);

	/* Cache the zone the smart acquire (id=2) sweeps. */
	acq_zone = acq_resolve_zone();
	if (acq_zone)
		scan_cursor = ALIGN(acq_zone->zone_start_pfn, 1UL << PAGE_ORDER);

	/* From here, pool_want writes resize live instead of just recording. */
	WRITE_ONCE(pool_ready, true);

	/*
	 * Register kretprobe at init - must be active before any VM ioctl.
	 * entry_handler filters efficiently: order, pool count, then mm.
	 */
	if (register_kretp_with_fallback() == 0) {
		hook_active = true;
	} else {
		pr_warn("kretprobe registration failed\n");
	}

	/* Register VM creation kprobe */
	{
		static const char * const syms[] = {
			"gh_dev_vm_mgr_ioctl",
			"gunyah_dev_vm_mgr_ioctl",
			NULL,
		};
		int j;

		vm_detect_kp.pre_handler = vm_detect_pre_handler;
		for (j = 0; syms[j]; j++) {
			vm_detect_kp.symbol_name = syms[j];
			ret = register_kprobe(&vm_detect_kp);
			if (ret == 0) {
				detect_active = true;
				pr_info("watching for VM creation (%s)\n",
					syms[j]);
				break;
			}
		}
		if (!detect_active)
			pr_warn("VM creation symbol not found (ret=%d)\n", ret);
	}

	/* Register VM destruction kprobe */
	{
		static const char * const syms[] = {
			"gh_vm_free",
			"gunyah_vm_release",
			NULL,
		};
		int j;

		vm_destroy_kp.pre_handler = vm_destroy_pre_handler;
		for (j = 0; syms[j]; j++) {
			vm_destroy_kp.symbol_name = syms[j];
			ret = register_kprobe(&vm_destroy_kp);
			if (ret == 0) {
				vm_destroy_active = true;
				pr_info("watching for VM destruction (%s)\n",
					syms[j]);
				break;
			}
		}
		if (!vm_destroy_active)
			pr_warn("VM destruction symbol not found (ret=%d), no auto-refill\n", ret);
	}

	/*
	 * Register the free-path vendor hook for VM-shutdown reclaim. Catches
	 * every order-9 page reaching the buddy allocator (the folio/batch paths
	 * the old __free_pages kprobe missed), so served pages a VM frees are
	 * recovered into the pool instead of leaking to buddy and fragmenting.
	 */
	if (!READ_ONCE(reclaim_want)) {
		pr_info("free-path reclaim disabled by reclaim_enable=0\n");
	} else {
		ret = free_hook_register();
		if (ret == 0)
			pr_info("free-path reclaim active (android_vh_free_one_page_bypass)\n");
		else if (ret == -ENODEV)
			pr_warn("prep_compound_page unresolved, free-path reclaim disabled\n");
		else if (ret == -ENOENT)
			pr_warn("android_vh_free_one_page_bypass absent on this kernel, free-path reclaim disabled\n");
		else
			pr_warn("free_one_page hook registration failed (ret=%d), VM-return pages will leak to buddy\n",
				ret);
	}

	return 0;
}

static void __exit hugepage_reserve_exit(void)
{
	int i, remaining;

	/* Stop live resizes racing with teardown */
	WRITE_ONCE(pool_ready, false);

	/*
	 * Signal any in-flight acquire loop to stop before waiting for it: both
	 * acquire workers re-check acquire_running every iteration, so this bounds
	 * the cancel_work_sync wait to the current window instead of a whole
	 * zone sweep (v2 has no fail-score give-up).
	 */
	atomic_set(&acquire_running, 0);

	/*
	 * v10: restore the CMA reservoir first (§10) - grab each block back,
	 * flip it MOVABLE, free it - while the module's bookkeeping is fully
	 * alive. Retried a few times: a block can be transiently ungrabbable
	 * (racing movable allocation mid-migration). A block that still
	 * resists is left CMA-labeled: its label and freelist stay mutually
	 * consistent, the memory stays app-usable (movable), it merely
	 * refuses unmovable allocations until reboot - noisy, never harmful.
	 */
	mutex_lock(&cma_mutex);
	{
		int pass;

		for (pass = 0; pass < 3 && cma_blocks_n; pass++) {
			cma_reservoir_demolish(0, true);
			if (cma_blocks_n)
				msleep(100);
		}
	}
	if (cma_blocks_n)
		pr_err("cma: %d reservoir block(s) left CMA-labeled at exit\n",
		       cma_blocks_n);
	mutex_unlock(&cma_mutex);

	/* v10: free the limbo strays (held order-9 compounds). */
	{
		struct page *lp;

		while ((lp = limbo_del_idx(0)))
			__free_pages(lp, PAGE_ORDER);
	}

	/*
	 * Remove all probes FIRST, cancel works second. The create/destroy
	 * kprobes queue works (refill, owner sweep, pcp drain), so cancelling
	 * works while the probes are still armed lets a VM event in the gap
	 * re-queue an already-cancelled work - which would then run after this
	 * function freed the pool and dropped the owner mms (use-after-free).
	 * unregister_kprobe/tracepoint_synchronize_unregister wait for in-
	 * flight handlers, so after this block no new work can appear.
	 */
	mutex_lock(&hook_mutex);
	if (hook_active) {
		unregister_kretprobe(&kretp);
		hook_active = false;
	}
	free_hook_unregister();		/* under hook_mutex, like the toggle */
	mutex_unlock(&hook_mutex);

	if (detect_active)
		unregister_kprobe(&vm_detect_kp);
	if (vm_destroy_active)
		unregister_kprobe(&vm_destroy_kp);

	/*
	 * Cancel works in dependency order: refill_worker's tail schedules
	 * vm_owner_sweep_work, so the sweep must be cancelled AFTER refill has
	 * been flushed - cancelling it first lets an in-flight refill re-arm
	 * it behind its own cancel, with the same use-after-free result.
	 */
	cancel_work_sync(&acquire_work);
	cancel_delayed_work_sync(&refill_work);
	cancel_delayed_work_sync(&pcp_drain_work);
	cancel_work_sync(&vm_owner_sweep_work);

	/* Release tracked VM owner mms */
	{
		int j, count = atomic_read(&vm_owner_count);

		for (j = 0; j < count; j++) {
			if (vm_owners[j].mm)
				mmdrop(vm_owners[j].mm);
		}
		atomic_set(&vm_owner_count, 0);
	}

	/* Free remaining pool pages */
	remaining = atomic_read(&pool_count);
	for (i = 0; i < remaining; i++)
		__free_pages(page_pool[i], PAGE_ORDER);

	pr_info("freed %d/%d pages (served=%d, refilled=%d)\n",
		remaining, pool_total,
		atomic_read(&total_served), atomic_read(&total_refilled));
}

module_init(hugepage_reserve_init);
module_exit(hugepage_reserve_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wasdwasd0105");
MODULE_AUTHOR("lateautumn");
MODULE_AUTHOR("BigfootACA");
MODULE_AUTHOR("HuJK");
MODULE_DESCRIPTION("Pre-allocate pages for Gunyah with VM lifecycle-aware pool recycling");
