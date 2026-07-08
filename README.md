# Gunyah-Hugepage-Reserve

A `.ko` kernel module that keeps a pool of pre-assembled **2 MB
(order-9) compound pages** and hands them to Gunyah VM (crosvm) guest RAM, so VMs
get contiguous THP-backed memory even on a fragmented system - then **recovers**
those pages when the VM returns them.

Ships as a loadable module (KernelSU / APatch / Magisk friendly). Supported KMIs:
`android15-6.1` ... `android16-6.12`, including 6.6 vendor kernels (e.g. SM8750).

### **Disclaimer - use at your own risk.** 


> This module patches core memory-management behavior from inside the kernel:  
> It resolves unexported symbols at runtime, and deliberately migrates/reclaims system memory.  
> A mismatch with your kernel, a vendor-patched mm, or plain bad luck can cause
> **crashes, reboots, data loss, or a device that runs out of memory, or anything else we didn't expect**.  
> It is developed and tested on a small set of devices and kernels; anything else is uncharted.  
> No warranty of any kind is provided, express or implied - the authors are not liable for any damage with
> your device or data.  
> Keep a way to remove the module offline (recovery / safe mode / `adb`) before installing,
> and back up your data first.  


### **Prerequisite** - guest RAM must actually be 2 MB shmem THP, or there is nothing
order-9 to intercept:

```sh
echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

When installed as a Magisk/KernelSU module this is applied for you at every boot
(`service.sh`, at late_start so it wins over vendor defaults). The DroidVM app
also sets it per VM launch, and crosvm must request huge pages.

**Core invariant while running:** `pool_avail + served == pool_total`.
If the sum drops below `pool_total`, pages leaked (were freed without being
recovered).

The module has three mechanisms: **serve**, **reclaim**, and **acquire**.

---

## 1. Serve - hand crosvm a pooled 2 MB page, and track the VM process

**Goal:** when a tracked VM allocates a 2 MB guest page, give it a page from the
reserve pool instead of letting it fight the fragmented buddy allocator.

This is two hooks: one learns **which processes are VMs**, the other **serves**
those VMs from the pool.

### 1a. Tracking - which processes are VMs

A **kprobe on `gunyah_dev_vm_mgr_ioctl(GH_CREATE_VM)`** records the caller's
`mm_struct` in the tracked-owner set (each owner carries a `vm_count`). It *only*
tracks processes that created a VM - it never touches any page. This set is the
scope: it's why the pool goes to VMs and not to every order-9 allocation on the
system, and it must exist *before* serving (you can't tell "is this crosvm?" from
an allocation alone).

### 1b. Serving - hand a tracked VM a pooled page

An always-on **kretprobe on `__alloc_pages`** (resolved as `__alloc_pages_noprof`
/ `__alloc_pages` / `__alloc_pages_nodemask`; toggle **`hook_enable`**). The
`entry_handler` filters fast - it acts only when `order == 9`, the pool is
non-empty, **and** `current->mm` is a tracked owner (from 1a); everything else
falls straight through.

When it fires, `pool_serve` does the hand-out **and** the page-owner registration
as one atomic step: pop a pool page, write its holder `pfn -> tgid` into the
served-page table under the same lock, swap it in for the allocator's buddy page,
and free that original (so memory isn't consumed twice). A page **never** leaves
the pool without a tracking entry - if the served table is full it declines to
serve and leaves the buddy page, so it can't hand out a page it couldn't later
reclaim (this keeps `pool_avail + served == pool_total`).

So the **holder** of each page (`pfn -> current tgid`, purpose-agnostic, updated on
re-serve) is recorded here, at serve time - not in 1a. That table drives the
read-only attribution files (`served_summary`, `vm_owners`, `reconcile`).

---

## 2. Reclaim - put a VM's returned 2 MB back into the pool (two paths)

When a VM shuts down, its 2 MB guest pages come back to the kernel. Two
independent paths return them to the pool; both can be on at once.

### 2a. Direct free-path hook - precise, leak-free (`reclaim_enable`, default on)

- Rides the vendor hook **`android_vh_free_one_page_bypass`**. Because that hook
  is not present on every KMI (and not monotonic by version), it is **located at
  runtime by name** (`for_each_kernel_tracepoint` + `tracepoint_probe_register`)
  rather than compiled in - so it works where present and silently disables where
  absent.
- Callback `gh_free_one_page_cb`: the instant an order-9 page **we served**
  (matched by `pfn` in the served table) is about to re-enter the buddy allocator
  (VM shutdown / memfd truncate), it is intercepted, **re-frozen as a compound
  page**, and dropped straight back into the pool. The *exact same page* returns -
  no re-allocation, no fragmentation, no leak.
- Toggle: **`reclaim_enable`**. On KMIs lacking the hook it reads `0` even after
  writing `1`.
- **Post-destroy scavenge:** 3 s after a VM destroy the module flushes the
  per-CPU page lists (so order-9 frees parked there reach the hook) and then
  physically re-acquires any served page whose free the hook missed (e.g. a
  THP split frees 512 order-0 pages the order-9 gate never sees): a targeted
  `alloc_contig_range` on exactly that window recovers the same physical
  block while its fragments are still coalesced in buddy. `reconcile` runs
  the same scavenge, so stale entries are retired by taking the page back,
  not by forgetting it.

### 2b. Detect-shutdown + delayed refill - the older alloc-back path (`refill_enable`, default on)

- A kprobe on VM release (`gunyah_vm_release` / `gh_vm_free`) detects a VM
  tearing down and, after **`refill_delay_ms`** (default 5000 ms), schedules work
  that **re-allocates** 2 MB blocks from buddy (with compaction/reclaim, retrying
  on fragmentation) to top the pool back up to `pool_total`.
- Unlike 2a it does not catch the returned page; it waits and allocates fresh
  blocks. Params: **`refill_enable`**, **`refill_delay_ms`**, **`manual_refill`**
  (write `1` to fire one refill now).
- On VM destroy the module also sweeps owners **per-entry**: it releases only
  owners whose process is dead (`mm_users == 0`) or whose last VM closed - a
  sibling VM dying (e.g. one crosvm OOM-killed) never drops the others.

> Set `refill_enable=0` to leave recovery purely to the precise free-path hook
> (2a) - useful for verifying leak-free behaviour, since the pool then refills
> **only** from pages a VM actually returned, never from fresh `alloc_pages`.

---

## 3. Acquire - build the pool: grab at boot, then fill on demand

**Boot grab (insmod).** At load, the module allocates up to `pool_want` x 2 MB
via `alloc_pages` (with compaction) into the pool. Memory is unfragmented at
boot, so this is the reliable way to get a large reserve.

- **`pool_want`** - the single target (pages). Set at insmod and at runtime:
  grow raises the target, shrink frees the excess immediately, `0` soft-disables.
  Capped at `pool_size_max`, computed from system RAM at insmod as
  `min(ram - min(ram/2, 6G), 24G)` - i.e. the system keeps 6 GB (or half the
  RAM on small devices) - and readable via the `pool_size_max` sysfs
  parameter. A shrink (including `0`) is fully
  reversible for pages lent out at the time: the free hook re-pools a returning
  page iff the pool holds fewer than the target *at return time*, so raising
  the target back before the VMs exit recovers them as usual.

**On-demand fill (`acquire`, write-only).** Grow the pool toward `pool_want` when
the buddy allocator has no free order-9 left. Fire-and-return: the work runs in a
kworker - poll `refill_stat`'s `acquire_active` for completion, `echo 0` to stop
(the scan cursor is saved and resumes next time).

Every `acquire` (1/2/3) runs the same three phases:

- **Phase 0 - drop slab** (all modes): `drop_slab()` once - frees reclaimable
  slab (dentry/inode). That slab isn't on the LRU so no reclaim path can move
  it, yet a single slab page poisons a whole 2 MB window; dropping it unpoisons
  windows for every mode. Toggle **`acquire_drop_slab`** (default 1; global
  side-effect - filesystem re-lookups are slower afterwards).
- **Phase 1 - free grab** (all modes): harvest already-free order-9/10 blocks
  from buddy via `alloc_pages` - instant, no migration (includes blocks the
  slab drop just freed).
- **Phase 2 - assemble** (per mode): assemble the fragmented remainder using
  the strategy selected by the written value:
  - `echo 1` - **original**: `alloc_contig_pages` - blind full-zone scan.
  - `echo 2` - **sweep + A (system-wide reclaim)**: cursor sweep +
    `alloc_contig_range`; when migration alone can't assemble a window, a
    **strided, bounded system-wide reclaim** (`try_to_free_mem_cgroup_pages`)
    frees migration destinations elsewhere. Coarse, but works wherever that
    symbol resolves.
  - `echo 3` - **sweep + B (per-block evict)**: same sweep; when migration
    fails, **evict the window's own folios to zram** (`folio_isolate_lru` +
    `reclaim_pages`) so it frees in place - targeted, minimal eviction. Needs
    the folio API (5.16+).

**The 2/3 sweep** is one full pass over the whole zone from a **persistent
cursor** (a stopped/partial sweep resumes next trigger). Its feasibility gate
(`block_candidate`) decides, **before any eviction**, whether a window can ever
assemble and skips the rest - so it never "white-kicks" (evicts for nothing). It
rejects windows holding:

- an unmovable **straggler** (slab / page table / kernel / vmalloc page),
- a **reserved** or **longterm-pinned** page (e.g. another live VM's guest RAM),
- CMA / isolated / highatomic pageblocks,
- our **own pool pages** (so the sweep never re-grabs the reserve).

A **memory floor** (`acquire_mem_floor_mb`, default 512 MB, writable at runtime)
is the sole safety brake: the sweep stops once `si_mem_available()` drops below
it, so reclaim never drives the system toward OOM and acquire fails gracefully
instead of hanging. Raise it if the sweep leaves the system too tight (1024 was
the historical value). `drop_slab` + `lru_add_drain_all` up front make the gate
accurate.

> On-demand acquire is capped by physical fragmentation (unmovable pageblocks): a
> window with even one truly-unmovable page can never be assembled at runtime. The
> reliable large reserve is the **boot grab**; on-demand `acquire` is a best-effort
> top-up. It is always interruptible (`echo 0`) and never OOMs (the floor).

Write return codes: `-EINVAL` (bad value), `-ENOSYS` (a required symbol didn't
resolve - try a lower mode), `-EBUSY` (already running), `0` (started / already at
target).

---

## 4. CMA reservoir (v10, experimental) - give idle reserve back to apps

With `pool_want_with_cma > pool_want`, the difference is kept as a **CMA
reservoir**: whole pageblocks the module labels `MIGRATE_CMA` and leaves *free
in buddy*. While no VM needs them, app (movable) allocations use that memory
like any other - it is not "held" at all. But unmovable allocations can never
enter a CMA block, so the blocks stay assemblable: before a VM starts, a
targeted CMA-mode `alloc_contig_range` migrates the movable squatters out and
rebuilds 2 MB pages in seconds, instead of fighting the fragmentation wall
(measured: ~10 s for 8 GB via the reservoir vs. a sweep stalling at ~30%).

`pool_want_with_cma=0` (default) disables all of it - v9 behavior exactly.
The feature needs two preflight values the packaging scripts pass at insmod
(`migrate_cma_val` from BTF, `pageblock_order_val` from `/proc/pagetypeinfo`),
resolves the pageblock setter/reader from kallsyms, and self-verifies on the
first block before labeling anything (order + accounting + grab-back checks);
any missing piece quietly falls back to the v9 path. Available on 6.1-6.12;
from 6.16 the kernel's migratetype rework removes the interfaces and the
feature auto-disables. A `cma_reservoir_floor_mb` headroom floor (default
1024 MB) refuses flips that would starve the kernel's unmovable budget.

Writing `pool_want` above `pool_want_with_cma` pulls the total target along, so
a legacy management app that only knows `pool_want` still drives the whole
elastic loop. Writing `pool_want_with_cma` smaller demolishes the excess
reservoir immediately (emptiest blocks first); writing `0` demolishes all of
it. `rmmod` restores every block to `MOVABLE` before the pool is freed.

---

## Parameter reference

All under `/sys/module/gh_hugepage_reserve/parameters/`.

| File                   | Mode | Group   | Purpose                                             |
| ---------------------- | ---- | ------- | --------------------------------------------------- |
| `hook_enable`          | 0600 | serve   | Alloc-side serve kretprobe on/off                   |
| `reclaim_enable`       | 0600 | reclaim | Direct free-path recovery hook on/off (2a)          |
| `refill_enable`        | 0600 | reclaim | Detect-shutdown -> alloc-back refill on/off (2b)    |
| `refill_delay_ms`      | 0600 | reclaim | Delay after VM shutdown before alloc-back refill    |
| `manual_refill`        | 0200 | reclaim | Write `1` to trigger one alloc-back refill          |
| `pool_want`            | 0600 | acquire | Target pool size (pages)                            |
| `pool_size_max`        | 0400 | acquire | RAM-derived cap on `pool_want` (pages), read-only   |
| `acquire`              | 0200 | acquire | `0` stop / `1` old / `2` sweep+A / `3` sweep+B      |
| `acquire_drop_slab`    | 0600 | acquire | Drop reclaimable slab at sweep start (default 1)    |
| `acquire_mem_floor_mb` | 0600 | acquire | Sweep stops below this `MemAvailable` (default 512) |
| `pool_want_with_cma`   | 0600 | cma     | Total target incl. reservoir (pages); `0` = off     |
| `cma_reservoir_floor_mb` | 0600 | cma   | Refuse flips below this non-CMA available (MB)      |
| `migrate_cma_val`      | 0400 | cma     | `MIGRATE_CMA` value from preflight; `-1` = off      |
| `pageblock_order_val`  | 0400 | cma     | Pageblock order from preflight; `-1` = off          |
| `pool_cma`             | 0400 | cma     | Reservoir size (2 MB-page equivalents), read-only   |
| `pool_avail_cma_able`  | 0400 | cma     | Avail pages flippable as whole pageblocks           |
| `cma_usage`            | 0400 | cma     | Reservoir occupancy snapshot (free/anon/file, ~1s)  |
| `pool_avail`           | 0400 | info    | Pages currently in the pool                         |
| `refill_stat`          | 0400 | info    | Full status + counters                              |
| `served_summary`       | 0400 | info    | Reconciled served-page summary                      |
| `vm_owners`            | 0400 | info    | Per-VM-owner attribution (pid / pages / comm)       |
| `reconcile`            | 0200 | info    | Write `1` to recompute `served_summary`             |
| `reclaim_debug`        | 0400 | info    | Free-path reclaim forensic counters                 |

`refill_stat` reports one `key=value` per line: `state`, `pool_avail`,
`pool_total`, `served`, `pool_want`, `total_served`, `total_refilled`,
`active_vms`, `acquire_active`, `refill_enable`, `free_reclaim`,
`pool_want_with_cma`, `pool_cma`, `pool_avail_cma_able`, `cma_pb_order`.
`cma_pb_order=-1` means the whole CMA side is off for this boot (missing
symbols/preflight values, or the boot-time first-block verification failed).

The app-side consumability probe is `tools/balloon.c` (static aarch64 CLI,
shipped in the package): a pure pressure instrument - it anon-balloons until
`MemAvailable < floor_mb` (argv), prints `cma_before_kb/cma_after_kb/
cma_diff_kb/held_mb/stop_reason` and exits. The app writes `pool_want=0`
first, runs it, judges the numbers itself, and records its verdict as
`cma_probe_result=` in `settings.prop` - app-owned state the kernel module
never sees.
`reclaim_debug` gained `cma_leak` - a tripwire counting CMA-labeled pages that
tried to enter the pool; it must stay 0.

---

## Usage

```sh
# Load with a boot reserve (memory is unfragmented at boot - most reliable).
insmod gh_hugepage_reserve.ko pool_want=1024        # 1024 x 2MB = 2 GB

# Top up on demand later (per-block evict), watch progress.
echo 3 > .../parameters/acquire
while grep -q 'acquire_active=1' .../parameters/refill_stat; do sleep 1; done
cat .../parameters/refill_stat

# Verify leak-free recovery in isolation (pool refills ONLY from returned pages).
insmod gh_hugepage_reserve.ko pool_want=256 refill_enable=0
# ... run a VM, use it, shut it down, wait a few seconds ...
cat .../parameters/refill_stat        # pool_avail returns; pool_avail + served == pool_total
```

(where `...` is `/sys/module/gh_hugepage_reserve/parameters`)
