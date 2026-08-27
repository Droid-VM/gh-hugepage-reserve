# gh_hugepage_reserve

**English** · [繁體中文](README_zh.md) · [简体中文](README_cn.md)

A 2MB-hugepage pool kernel module for Gunyah VMs. **`POOL_DESIGN.md` is the
single source of truth** — this README is a map; details and evidence (including
the measurements the design rests on) live there, and every §n below points at
one of its sections.

Ships as a loadable `.ko` (KernelSU / APatch / Magisk friendly). Supported KMIs:
`android14-6.1` … `android16-6.12`, including 6.6 vendor kernels (e.g. SM8750).
Older KMIs (5.10 / 5.15) get a functionless placeholder so the Magisk package
still installs.

---

### Disclaimer — use at your own risk

> This module patches core memory-management behaviour from inside the kernel:
> it resolves unexported symbols at runtime, and deliberately migrates and
> reclaims system memory. A mismatch with your kernel, a vendor-patched mm, or
> plain bad luck can cause **crashes, reboots, data loss, a device that runs out
> of memory, or anything else we did not expect**. It is developed and tested on
> a small set of devices and kernels; anything else is uncharted. No warranty of
> any kind is provided, express or implied — the authors are not liable for any
> damage to your device or data. Keep a way to remove the module offline
> (recovery / safe mode / `adb`) before installing, and back up your data first.

### Prerequisite

Guest RAM must actually be 2MB shmem THP, or there is no order-9 allocation to
intercept:

```sh
echo advise > /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

Installed as a Magisk/KernelSU module this is applied for you at every boot
(`package/module/service.sh`, at late_start so it wins over vendor defaults).
The DroidVM app also sets it per VM launch, and crosvm must request huge pages.

---

## What it is for

On a Gunyah phone (demand-paging guest), a VM's guest RAM is allocated from the
kernel in 2MB (order-9) units. After some uptime the system is inevitably
fragmented — measured: **84% of all 2MB windows hold an unmovable hard
straggler** (median ~50 per window, §preface) — and by then it is too late to
assemble 2MB blocks. **Early boot is the only moment you can get them all.**

So the module acquires and guards a batch of 2MB pages at boot, for the long
term:

- **When a VM wants them**: intercept the VM's 2MB allocations and hand out pool
  pages instead; take them back precisely when the VM closes, ready for the next
  VM.
- **When no VM is using them**: guarded pages above the pool's share are flipped
  whole-block into a `MIGRATE_CMA` **reservoir** and lent to the system (page
  cache / mTHP), so the memory is not sitting idle; when the pool needs them
  back, the borrowers are migrated out.

Two user-facing targets: `pool_want` (the pool's share, I1) and
`pool_want_with_cma` (total guardianship, I2). The external consumers are the
management app (DroidVM), `load.sh`, and `serve_test`; the sysfs surface is an
existing contract (§10).

---

## Mechanism

### At a glance

Function only, no detail:

1. **insmod builds the pool synchronously**: `module_init` drives the pool to
   target in its own context before returning — the load script and the app rely
   on "when insmod returns, the pool is built".
2. **serve**: a hook intercepts the VM's order-9 allocations and hands out a
   pool page; if the pool is empty or the caller is not a VM, the original
   allocation passes through untouched.
3. **reclaim**: when the VM lets a page go, the free hook takes it back into the
   pool by pfn; after VM shutdown the release worker actively recovers lent
   pages, and gives up tracking anything still missing after the grace period
   (10s).
4. **CMA reservoir**: guarded pages above the pool's share are flipped
   whole-block into CMA and lent to the system; when the pool runs short the
   borrowers are migrated out and the block is taken back. The `unlock_cma`
   side-car widens plain movable allocations into CMA so the reservoir is
   actually borrowed from.
5. **sysfs control / observation**: write the two want targets, press `acquire`
   for an active harvest (including the heavy whole-zone sweep + evict modes),
   read `refill_stat` / `cma_usage` and the other counters.

### In detail

With the invariants. The core concept is **custody**: a 2MB slot either belongs
to us or belongs to the system — the whole design has only that one boundary.

**Page states** (§1) — every 2MB slot is in exactly one of:

| state | meaning |
|---|---|
| `not_useable` | non-RAM / carveout / vendor CMA / ZONE_MOVABLE; decided once at insmod, never participates |
| `external` | the system's (outside custody) |
| `avail` | standing by in the pool |
| `served` | lent to a VM (GUP pin + one protective reference of ours) |
| `released` | the VM let go, destination undecided (waiting for the free hook, or for the give-up timer) |
| `cand` | being assembled by collect_cma; cannot be served or shed |
| `verify` | scratch window for CMA parameter verification (within one call) |
| `cma` | the reservoir, lent to the system |

**Accounting and invariants** (§1):

```
custody   = avail + served + released + cand + verify
held      = avail + served + released + verify
held_cma  = held + cma

I1  held     → pool_want               (a convergence target, not an instantaneous invariant)
I2  held_cma → effective pool_want_with_cma
W   always pool_want <= pool_want_with_cma <= pool_size_max, or with_cma == 0 (CMA disabled);
    when S>1 both are multiples of S (S = how many 2MB slots make up one CMA block)
P   0 <= pool_total <= configured_total <= pool_size_max
    (pool_total = "proven capacity": what was actually obtained and may be refilled in
     the background — not an alias for held)
U   CMA block consistency: the S slots of a block are all CMA or all non-CMA
Q   minimise custody pages that are not part of a cma_able block (fewest orphan fragments)
G   bookkeeping consistency: state / lists / counters agree (pool_check verifies when debug=1)
```

Counting `released` inside `held` is deliberate: a released page is very likely
back within microseconds; excluding it would make the deficit spike the instant a
VM shuts down and send the worker out to buy replacements for pages that are
already on their way home. **The deficit only really opens at the moment we give
up (purge).**

**The four pillars** (§preface):

1. **cma_able = a custody predicate**: it holds when every sub-slot of a CMA
   block is inside custody. No per-block flag is stored — it is derived on the
   fly by scanning the S adjacent slots (one cacheline when S≤4).
2. **cma_ready = all sub-slots AVAIL**: the only legal thing to flip into CMA. A
   served page is a `FOLL_LONGTERM` pin, and flipping a block that contains a pin
   into CMA is a dead end (longterm GUP migrates before pinning).
3. **Promotion/demotion happen only at the custody boundary**, all in worker
   (sleepable) context; the atomic hot paths are O(S) worst case.
4. **Two-level addressing, hole-immune**: a chunked absolute-coordinate table
   (same construction as `mem_section`); ARM's huge physical-address holes cost
   one NULL pointer in the top array.

**serve** (§3.2/§4): a kretprobe on `__alloc_pages` return; four fast filters
(order ≠ 9 / pool empty / not an owner / gfp without `__GFP_MOVABLE`) must all
pass before the transfer. The transfer does owner validation + AVAIL→SERVED as
one step under the fixed lock order pid_lock(read) → pool_lock, and takes
`get_page()` before leaving AVAIL — that protective reference is what stops the
order-9 compound from being split or migrated while it is out on loan. Pages are
taken in three tiers: fragments first (`avail_non`) → then `cma_able` → and only
last does it break a whole `cma_ready` block.

**free return** (§3.2): when the free hook (tracepoint) hits one of our RELEASED
pages, it compares held↔want and held_cma↔want_cma **inside the same pool_lock**
and decides KEEP (rebuild the order-9 compound, move to AVAIL, bypass the
original free) or DROP (move to EXT and let the original free continue into
buddy). Splitting that into a query call plus an edge call would be a TOCTOU, so
it is a single event api.

**release** (§6): the release worker classifies each SERVED page three ways —
`refcount==1 && !mapping` → mark RELEASED first, then `put_page` outside the
lock; `refcount>1` → the pin is still held, look again next round;
`page_count==0` → orphan (the hook missed the free), just mark RELEASED and let
the existing sweep/purge exits handle it. Letting go of a served page is the EVENT
consequence of pid death plus a short 3s grace (a live owner is never touched,
whatever its vm_count — vm_count is only the serve gate; the grace lets the exit
path's own frees come home as a collect, and the clock starts only at death and
is never cleared); `released_at + GRACE=10s` governs only the RELEASED purge.

**CMA reservoir** (§3.2):

- flip (avail→cma) only ever takes a whole `cma_ready` block, and requires
  `cma_params_state == VERIFIED` — before the first flip,
  `pool_verify_cma_params()` uses a fully-owned verification window to check the
  pageblock_order boundary, the CmaFree delta and a CMA-mode grab for real; a
  structural failure is the terminal state UNAVAILABLE.
- stage_in (cma→avail) and drop (cma→ext) both have to grab the whole block with
  contig first and migrate the borrowers out, and **can fail** (pin / writeback).
  A failed block is skipped for this run, **keeps its CMA label and stays counted
  in pool_cma** — books and labels must agree, and **"just flip the label"** is
  forbidden (the free pages are still on the CMA freelist; flipping the label
  alone drifts the CmaFree accounting and makes the pages invisible to unmovable
  allocations).
- The gfp iron rule for acquire: GFP_KERNEL family, never movable and never
  `__GFP_CMA` — otherwise, once `unlock_cma` has widened things, acquire would
  fish pages out of our own reservoir, and a movable page would be migrated away
  by GUP before the `FOLL_LONGTERM` pin.

**The concurrency contract** (§2 — the structural reasons this is race-safe):

- **Atomic per call, no cross-call invariants**: every pool api completes its
  bookkeeping atomically under pool_lock; there is no structure that needs
  repairing afterwards, so the ABA family has nowhere to live.
- **APIs defend themselves**: a caller's condition checks are all advisory and
  taken outside the lock; every public api re-validates its preconditions inside
  the lock and no-ops if they do not hold. It never trusts the caller.
- **Work lists hold pfn values, re-validated at the consumption point**: scan
  sets store values, not pointers; the slot table is static and the memmap always
  exists, so dangling is structurally impossible — only staleness can happen, and
  that is arbitrated by the in-lock re-check at the point of use.
- **Iteration is an index-ordered table scan, never a link walk**: the whole
  problem class "the next the cursor points at got moved concurrently" does not
  exist.
- Two hard rules: (a) **never `put_page` / `__free_pages` under the lock** (the
  free tracepoint re-enters pool_lock); (b) **change state before letting go**
  (release marks RELEASED before putting, shed marks EXT before freeing — the
  reverse order is a distinct kind of data loss in each case).

---

## Architecture

### Overall

Three layers: **consumer / worker / pool api + kapi**. Consumers are grouped by
*what they call*, and each group touches exactly one layer; the full diagram and
the description of every edge is §0.1.

```
consumer layer (grouped by call target; insmod/rmmod excepted — they touch every layer)
┌────────────────────────┐ ┌───────────────────┐ ┌───────────────────┐ ┌────────────┐
│ -> pool api            │ │ -> release worker │ │ -> adjust worker  │ │ -> kapi    │
│ serve hook (atomic)    │ │ vm_shutdown       │ │ sysfs want shrink │ │ unlock_cma │
│ free hook  (atomic)    │ │ vm_unshare        │ │ sysfs acquire / 0 │ │ pinprobe   │
│ sysfs reads, vm_boot   │ │ pid-exit hooks    │ │                   │ │ (side-car) │
└───────────┬────────────┘ └─────────┬─────────┘ └─────────┬─────────┘ └──────┬─────┘
            │                        ▼                     ▼                  │
            │            ┌───────────────────────┐   ┌────────────────────┐   │
            │            │ release_worker (1s×5) │──►│ adjust_worker      │   │
            │            │ served→released,      │   │ (10ms×MAX)         │   │
            │            │ timed give-up, ownerGC│   │ 7-phase pipeline   │   │
            │            └───────────┬───────────┘   └─────────┬──────────┘   │
            ▼                        ▼                         ▼              │
┌─────────────────────────────────────────────────────────────────────┐       │
│ pool api   public: pool_{src}_to_{dst}(one function per edge)       │       │
│                    + queries / actions                              │       │
│            private: pool_private_{slot,classify,promote,demote,...} │       │
└──────────────────────────────────┬──────────────────────────────────┘       │
                                   ▼                                          │
┌─────────────────────────────────────────────────────────────────────┐       │
│ kapi   alloc_pages / alloc_contig_range / set_pageblock_migratetype │◄──────┘
│        walk_system_ram_range / evict_range / drop_slab / drain / …  │
└─────────────────────────────────────────────────────────────────────┘
```

Key points:

- **The hooks touch only the pool api** and make no policy decisions; **the
  workers are the only two drivers of the state machine**, and every transition
  goes through a public api. Every release round fires `adjust_try(RELEASE)` —
  that is the edge background refill travels on.
- **Kernel symbols live in the kapi layer and nowhere else**; `unlock_cma` is a
  side-car that uses only kapi and read-only pool queries, and never touches the
  pool, the workers or the state machine (§9).
- **insmod/rmmod are the only consumers that touch every layer**: kapi resolve,
  slot table construction, hook attach/detach, worker start/stop, synchronous
  teardown. The fixed order is §8 (unload iron rule: detach hooks → stop workers
  → hand back references).

The build is a unity build: `gh_hugepage_reserve.c` includes `parts/` in order.
The pool object and both workers are environment-agnostic, and the same sources
also compile in userspace against `tests/shim.h` — that is the CI mock harness
(§3.3), which replays 34 scenarios against a deterministic fake buddy. kapi /
hooks / sysfs / unlock_cma are verified on real kernels only.

### kernel api (kapi)

`parts/gh_kapi.c.inc` (contract in §3.3). **The only layer in the whole module
that touches kernel symbols**; upwards it offers one coarse adapter table,
`gh_kapi`:

```c
bool kapi.cap(feature);                        /* CONTIG_RANGE / DROP_SLAB / ... */
struct page *kapi.alloc_try(order, strong);
int  kapi.contig_range(start, end, noretry);   /* half-open pfn range */
bool kapi.candidate_range(start, end);         /* pure-read feasibility probe */
void kapi.evict_range(mode, start, end);
bool kapi.cma_floor_ok(nblocks);
void kapi.drop_slab(); kapi.drain_pages(); kapi.lru_add_drain_all();
/* … full table in parts/gh_defs.h, struct gh_kapi */
```

Design rules:

- **kapi is the module's own stable ABI**: call sites always use the normalised
  `kapi.op()`. The real symbols that diverge per kernel version (names,
  signatures, argument/return semantics) live in a private `kraw` struct plus
  shims, and never leak upwards.
- **Resolution happens at load time** (a throwaway kprobe finds the address);
  what is missing stays NULL and its `cap()` returns false — the whole feature is
  refused at its precondition, so execution never gets half-way in and calls
  through a mistyped pointer (a kCFI type-id mismatch is a panic).
- **ABI guard**: `load.sh`'s `kapi_check` pre-validates signatures against
  `/sys/kernel/btf`, and incompatible symbols are blacklisted through the
  `disable_kapi` parameter; `abi/kapi_abi.tsv` is the symbol registry.
- **Folio flag interpretation lives only here** (the real implementations of
  candidate/evict). The pool object's direct contact with pages is confined to a
  small set of redefinable primitives which the test build swaps out through the
  shim — that is exactly what makes the whole kapi layer replaceable by the mock
  backend, and what lets pool/worker run in userspace CI.

### pool api

`parts/gh_pool.c.inc`. **The pool is the object** (C has no objects, so the pool
api *is* the object): the slot table, the three avail lists, the counters, the
cursors, the scan sets and their locks are **all inside the pool**. Hooks,
workers and sysfs only call public apis and take back values (pfn, counts,
bools); they never hold `pool_lock` and never look at `top[]` or a slot.

Naming convention (§3): public is `pool_{src}_to_{dst}`, **one function per
edge**, with variants of the same edge expressed as a mode argument; private is
`pool_private_xxx`, callable only from inside the pool. Every public api
re-validates its own preconditions inside the lock and no-ops if they fail.

The state machine (one edge = one public api, §3.1):

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                  external                                    │
└──┬─────────▲──────▲─────────┬─────▲───────────────────▲───────────────▲──────┘
   │acquire! │shed* │drop!    │grab!│flush              │expired/rmmod* │purge*
   ▼         │      │         │     │                   │               │
┌────────────┴─┐    │         │     │             ┌─────┴──────┐        │
│    avail     ├────┼─────────┼─────┼─serve──────►│   served   │        │
│              │◄───┼ret/sweep┼─────┼─────┐       └─────┬──────┘        │
│              │◄───┼──┐      │     │     │             │release        │
└──┬──────▲────┘    │  │      ▼     │     │             ▼               │
   ▼flip  │stage_in!│  │promote     │     │       ┌────────────┐        │
┌─────────┴────┐    │  │  ┌─────────┴──┐  └───────┤  released  ├────────┘
│     cma      ├────┘  └──┤    cand    │          └────────────┘
└──────────────┘          └────────────┘

*  = custody exit (block demotion point)     ! = this edge has an api that can fail
```

| edge | api | caller |
|---|---|---|
| acquire (ext→avail) | `pool_ext_to_avail(how, …)` — the five ways are modes | adjust |
| serve (avail→served) | `pool_avail_to_served(pid)` | serve hook (atomic) |
| release (served→released) | `pool_served_to_released()` | release worker |
| return (released→avail/ext) | `pool_released_return(pg)` — KEEP/DROP/MISS event api | free hook (atomic) |
| sweep (released→avail) | `pool_released_to_avail_sweep(pfn)` | adjust |
| purge (released→ext) | `pool_released_to_ext(EXPIRED/ALL)` | adjust / rmmod |
| expired (served→ext) | `pool_served_to_ext(EXPIRED/ALL)` | release / rmmod |
| flip (avail→cma) | `pool_avail_to_cma(n)` | adjust |
| stage_in (cma→avail) | `pool_cma_to_avail(n)` | adjust |
| drop (cma→ext) | `pool_cma_to_ext(n/ALL)` | adjust / rmmod |
| shed (avail→ext) | `pool_avail_to_ext(n)` | adjust / rmmod |
| grab (ext→cand) / flush | `pool_ext_to_cand(pfn)` / `pool_cand_to_ext()` | adjust |
| promote (cand→avail) | no public api — `pool_private_promote` runs it internally | — |

Internal structure (§0.2): two-level addressing
`top[off>>30][(off>>21)&511]` (O(1) worst case; a hole is a NULL pointer). Only
the three avail lists are maintained (`avail_non` / `avail_cma_able` /
`avail_cma_ready`) — the only consumer that needs an atomic "give me the next
usable one" is serve; every other state uses an index-ordered table scan or an
O(1) scalar threshold (released's `(count, oldest)`). The block properties
cma_able/cma_ready are not stored in a field, they are derived by scanning the S
adjacent slots. Zero `kmalloc` at runtime — every scan set is preallocated at
insmod.

The owner registry belongs to the same pool facade but uses its own `pid_lock`
(rwlock): an entry lives until its pid dies, and the serve hot path first
lock-lessly filters with `pool_owner_maybe()`, taking the fixed pid→pool lock
order only on a hit.

### Consumers

#### hooks

`parts/gh_hooks.c.inc` (§4). The principle: **each hook makes exactly one
pool/facade call and makes no policy decision**; the policy (keep or give up,
target comparison) is encapsulated in the api being called.

| hook | attach point | action |
|---|---|---|
| serve | kretprobe on `__alloc_pages` return | all four fast filters pass → `pool_avail_to_served(pid)`; NULL lets the original allocation through |
| free | tracepoint `android_vh_free_one_page_bypass` | order-9 only → `pool_released_return(page)`; set bypass on KEEP |
| vm_boot | kprobe (the `GH_CREATE_VM` ioctl path) | `pool_owner_add(current)` |
| vm_shutdown / unshare | kprobe (vendor symbols; overridable by parameter) | `release_vm_shutdown/unshare(current)` (the facade updates the owner and arms release) |
| pid death | tracepoint `sched_process_exit` (core; always present on GKI) | group_dead and in the owner table → `release_vm_exit(current)` |

The serve hook's four filters are ordered by cost: order ≠ 2MB → pool empty →
not an owner (lockless) → gfp without `__GFP_MOVABLE` (measured: dma_heap's
unmovable order-9 allocations never come back to buddy; without this filter we
lose 2 pages per VM). serve and free both run in **atomic** context and only
touch O(1)–O(S) pool apis. `hook_enable` / `reclaim_enable` toggle them
independently and read back the *actual* attach state.

#### workers

`parts/gh_release.c.inc` / `parts/gh_adjust.c.inc` (§5–§7). Hooks run in atomic
context and can only do O(1)–O(S) quick moves; every slow action — asking the
system for pages, migration, whole-table scans, verification — needs sleepable
context and is concentrated in these two workers. They are the only two drivers
of the state machine.

**The shared model: the run counter** (§5)

A worker is not a resident thread but a delayed_work on a workqueue: wake up →
do one small slice → save progress → schedule the next wake-up. The reason for
slicing: pool_lock is only held briefly inside a slice, so the serve/free hooks
are never blocked behind a worker's slow action.

The `run` field in the ctx is both a **switch** and a **watchdog**: each round
decrements it and finishes at zero; anyone may write it at any time — writing 0
means stop (at most one more round completes), writing N starts or extends it.
Every other ctx field is written only by the worker itself from init to finish;
outsiders who want to change the work always **hand off** (`next_profile`) and
never edit the ctx in place — the worker may be doing a slow action outside the
lock and will still write its ctx back at the end of the round.

**release_worker — "get back the pages the VM let go"** (one round per second)

Three VM events (vm_shutdown / vm_unshare / pid death) each arm 5 rounds; the
event facade also updates the owner table (attributed vm_count decrement — pure
serve-gate maintenance — and the dead mark). Five rounds is a guaranteed floor, not a
ceiling. Each round does five things:

1. **Collect (served→released)**: index-scan the whole table for SERVED and
   classify each page by refcount —
   - `refcount==1` (only our own protective reference is left) = the VM really
     let go → mark RELEASED and stamp inside the lock, then `put_page` outside
     it. After the put the page takes the normal free path, and as it flows
     through the free hook `pool_released_return` decides whether to take it back
     or let it go. "Mark before releasing" is a hard rule: in the other order the
     free hook would not recognise the page and it would be lost.
   - `refcount>1` = the VM's/Gunyah's pin is not released yet → do not touch it
     this round, look again next round.
   - `page_count==0` = **orphan**: the free already happened but the hook missed
     it (pcp lag, or an attach gap), and our reference evaporated with the free →
     do not put, just mark RELEASED and hand it to the existing sweep (take it
     back) / purge (give up) exits. No new path is opened.
2. **Death give-up (served→ext EXPIRED)**: for each SERVED, look up the owner —
   a LIVE owner is never touched, whatever its vm_count (vm_count is only the
   serve gate); owner dead (marked) or gone from the table = stop tracking: mark
   EXT, drop our reference, record purge_log and `owner.abandoned++`. From then
   on the page lives with whoever still pins it, and is none of the module's
   business. Giving up is the EVENT consequence of pid death plus DEATH_GRACE
   (3s) — a one-way clock started only at death, never cleared (the old
   idle+10s clock raced a VM restart 1→0→1 into false give-ups; the grace lets
   the exit path's own frees come home as a collect).
3. **`drain_pages` on round 3**: an order-9 free parks in the per-CPU cache
   (pcp) first and does not flow through the hook immediately — measured ~2 pages
   lost per VM. Round 3 (≈3s, after the shutdown free storm has passed) drains
   once to force them out.
4. **Early finish / final round**: "no page can still come back (released==0) and
   no page is waiting to be unpinned (pending==0)" → set run to 0 and finish
   early. The final round drains again, collects once more, and does owner GC —
   clearing only entries that are dead *and* hold no pages; an entry with
   vm_count==0 whose pid is still alive is **kept**, because the app must be able
   to see a suspicious pid that closed its VM but did not give the pages back
   (D-state detection, §10).
5. **Wake adjust**: `adjust_try(RELEASE)` every round. During the wait, held
   still includes served/released, so there is no deficit on the books and only
   `precise` actually has work; once DROP/EXPIRED genuinely opens a deficit,
   cheap/full/stage_in start moving — the condition system sequences this by
   itself, so there is no need for two profiles.

**Self-extension**: rounds exhausted but pending>0 (pages still waiting on unpin
or grace) → `run=1` for one more round. It is bounded: after at most 10 seconds
of grace EXPIRED takes them, and RELEASED converges through purge, so it cannot
extend forever.

The design core: **give-up is an event plus a short grace (pid death + 3s),
purge is a clock (released_at + 10s), the rounds are just the vehicle** — so a
high-frequency `manual_release`, or several events arming rounds repeatedly,
only cause a few extra harmless scans and never bring either forward.

**adjust_worker — "drive the pool to target"** (one round per 10ms)

Who triggered it decides how painful an authorisation it gets (profile, §5.1):

| profile | trigger | authorised acquisition strength |
|---|---|---|
| `INSMOD` | boot; module_init drives it synchronously to completion | cheap + full |
| `RELEASE` | every release worker round | precise + stage_in (+ cheap + full, gated by `refill_enable`) |
| `SHRINK` | a want target was lowered | stage_in only |
| `USER` | the user pressed `acquire` | everything (the only one including main) |

The profile only decides which strengths open inside the ACQUIRE phase
(**policy**); the seven-phase skeleton (**mechanism**) is not subject to the
profile and every run walks it in order. RELEASE's target snapshot is
additionally capped to `pool_total` (proven capacity) — background refill only
refills what was historically actually obtained, so writing a big number into a
knob does not quietly authorise background reclaim.

**One run = a single pass through the pipeline, no loop**: each phase advances
only when it completes naturally (which may span many rounds, each doing one
small slice), and the run is done when the pipeline ends; not reaching the target
is a partial, recorded in stop_reason, and retrying belongs to the next trigger.
Acquire gets exactly one chance per run — a gap opened by a later shed never
loops back to trigger an earlier phase, so the "grab → cannot complete → drop →
grab again" oscillation is structurally impossible.

Fixed actions at the start of every round: first purge expired released pages (an
O(1) threshold blocks it until GRACE has actually passed — this does not wait for
the phase cursor, which may sit in main for a long time); then **recompute five
numbers, never cached** (the pool situation is changed by hooks at any moment):

```
held           = avail + served + released     how much is in custody now
diff_want      = want − held                   pool share short (+) / over (−) by
new_page_need  = want_cma − (held + cma)       total guardianship still short → must ask the system
reserve_target = want_cma − want               how big the reservoir should be
cma_excess     = cma − reserve_target          how much the reservoir is over by
```

The seven phases, in order:

1. **PREPARE**: all five numbers on target → finish immediately (an idle run
   exits in O(1)). A USER run with a gap pays for one `drop_slab` + pcp drain
   (dentry/inode caches are not on the LRU so no harvest can reclaim them, yet
   one dentry page poisons a whole 2MB window; the order-0 pages it drops land in
   pcp, and without a drain they never merge into buddy's higher orders, leaving
   the window that just opened invisible to the rest of the harvest). One table
   pass builds this run's scan sets: the precise list (our own released pages
   still sitting in buddy) and the main list (all EXT windows: gaps first, then
   resuming from the last cursor and wrapping once). Only if the reservoir is
   over target does it pay for one CMA occupancy probe + bucket sort (cheapest
   blocks to move first).
2. **ACQUIRE** — the only phase governed by the profile. Five strengths ordered
   by "who feels the pain", dropping to the next only when one is done or stuck:
   - **precise**: contig-grab back our own released pages (they are still sitting
     unused in buddy). Free, hurts nobody, always first.
   - **cheap**: pick up what buddy already has (NORETRY; give up instantly if it
     is not there). Try a **whole block** first (S × 2MB at once, natively
     flippable into the reservoir) — big blocks are plentiful at boot, and this
     is how pool + reservoir get built in one go. One whole-block failure demotes
     permanently to single pages for the rest of the run (exhausted big blocks do
     not grow back on their own).
   - **stage_in (cma→avail)**: pool short but total guardianship sufficient (the
     pages are in the reservoir) → take whole blocks back from our own reservoir,
     migrating the borrowers (page cache etc.) out. Medium pain, and only our own
     people feel it. Can fail (a borrower is pinned); hit points `cta_hp` bound
     it: any progress refills to 8, eight consecutive zero-progress batches give
     up.
   - **full**: `alloc_pages` with RETRY_MAYFAIL — let the kernel do its own
     compaction + reclaim (possibly swapping). Heavier than cheap, lighter than
     main; the kernel manages it, so it needs no user authorisation. Stop on
     first failure: the kernel already tried its best and asking again gets the
     same answer.
   - **main**: heavy pressure, USER only. Two styles: `acquire=1` is CONTIG_ANY
     (blind grab, the kernel picks the location; hit points `main_hp` +3 on
     success, −1 per consecutive failure, zero = "migration exhausted");
     `acquire=2/3` is CONTIG_AT, a **whole-zone sweep** — one window per round
     along the main list: first a pure read of 512 `struct page`s for a
     feasibility verdict, and **no eviction unless it passes the gate**
     ("never white-kick": measured, 84% of windows hold a straggler that cannot
     be moved, and without this gate we would be throwing away page cache for
     nothing on 84% of windows); past the gate try the cheap async grab, and only
     then evict (mode A: one system-wide memcg reclaim per 8 failed windows;
     mode B: evict only that window's own folios) and try a sync grab once,
     moving on either way. `mem_available` is checked before every window and
     anything below `acquire_mem_floor_mb` (512MB) brakes immediately (measured:
     without the brake, an RCU stall to the point of reboot). The cursor
     "advances before acting": if a run is interrupted, the next one resumes
     after the break point instead of retrying the same batch.
   - **End-of-round eager flip**: regardless of strength, every round checks in
     passing — pool share full, reservoir not full, a ready block available →
     flip one block into CMA. Earlier is better: an avail page is completely
     unusable to the system, while a cma page can at least be borrowed.
3. **VERIFY_CMA**: CMA parameters still PENDING and this is an INSMOD/USER run →
   verify once for real using a fully-owned window (pageblock boundary, CmaFree
   accounting, CMA-mode grab-back). Only after passing does it become VERIFIED
   and flipping is allowed; a structural failure permanently disables CMA and the
   run falls back to pure pool mode.
4. **COLLECT_CMA**: authorisation follows main. For blocks that are only a few
   slots short of complete, grab the gap windows (same feasibility gate, eviction
   allowed); what is grabbed goes into the cand isolation area and **not into the
   pool** — putting it straight into the pool would push held above want and wake
   shed up to dismantle our own scaffolding. Block complete → promote: cand
   becomes AVAIL, its siblings rejoin, and an equal number of fragment pages is
   freed to conserve the total (held unchanged, quality Q improved). Any cand
   still incomplete at the end of the run is flushed back to the system (its
   block cannot be completed anyway, so keeping it is pointless).
5. **AVAIL_TO_CMA**: flip the ready blocks above the pool's share in batches
   (≤8 blocks per round) until the reservoir reaches `reserve_target`. Each flip
   asks `cma_floor_ok` first — every flipped block takes 2MB×S of room away from
   the unmovable working set (~3.5GB, which cannot enter CMA), so nothing is
   flipped below the floor (default 1024MB). If this phase flipped anything it
   drains once so the CmaFree accounting catches up (the GUI reads it).
6. **CMA_FREE**: want_cma was lowered → give the excess reservoir blocks back to
   the system. Per block: grab the whole block back (migrating the borrowers,
   **which can fail**) → only with the whole block in hand flip the label back to
   MOVABLE → free. A block that cannot be grabbed keeps its CMA label and stays
   counted (books and labels must agree, never "just flip the label"); hit points
   `drop_hp` reaching zero means "cma sources stuck", and the next trigger probes
   and tries again. One exception makes way: if the pool is still short and this
   run still has stage_in authorisation, this phase is skipped so the excess is
   left for stage_in to convert rather than released.
7. **AVAIL_FREE**: last, genuinely surplus avail pages (≤32 per round) go back to
   the system. avail already empty but the books still over (all the surplus is
   in a VM's hands) → do not wait, just finish — when they come back, the free
   hook's DROP gate will let them go against the then-current target, or a later
   trigger's run will shed them; without this exit the run would spin to the
   watchdog limit (20 minutes).

**Preemption and hand-off** (`adjust_try`): no run in flight → start immediately.
A USER run in flight → nobody may preempt it (what the user pressed wins; a
second press returns -EBUSY and needs `acquire=0` to cancel first). A background
run in flight → the new trigger writes run to 0 (interrupt) and registers
`next_profile` (highest priority wins: USER > SHRINK > RELEASE > INSMOD) and
returns immediately; when the worker wraps up it sees next_profile and restarts
under the new profile. Hand-off latency is ≤ one round (10ms), and the ctx is
written only by the worker from beginning to end.

**Finishing** (`adjust_finish`): every path that brings run to zero goes through
this one function, with no branches — incomplete cands are returned to buddy, the
run-local scan sets are discarded, a USER run records its stop_reason (the app
displays it verbatim: reached target / migration exhausted / low-memory floor /
…), and a pending next_profile is handed off. `ADJUST_RUN_MAX=120000`
(×10ms ≈ 20 minutes) is a watchdog, not a loop length: a normal run ends
naturally when the pipeline is done.

---

## Source layout, build and test

| file | layer | tested |
|---|---|---|
| `parts/gh_defs.h` | shared types, kapi adapter contract, enums | — |
| `parts/gh_owner.c.inc` | owner registry (pid-lifetime entries; vm_count is only the serve gate) | mock |
| `parts/gh_pool.c.inc` | **the pool object**: all state + locks live here | mock |
| `parts/gh_release.c.inc` | release worker (served→released, timed give-up) | mock |
| `parts/gh_adjust.c.inc` | adjust worker (the 7-phase acquire/flip pipeline) | mock |
| `parts/gh_kernel_env.h` | kernel primitive glue (locks, clock, page ops) | on-device |
| `parts/gh_kapi.c.inc` | kernel kapi backend (symbol resolve, version shims) | on-device |
| `parts/gh_hooks.c.inc` | kretprobe / kprobe / tracepoint attach | on-device |
| `parts/gh_sysfs.c.inc` | sysfs / module params (the ABI, §10) | on-device |
| `parts/gh_unlock_cma.c.inc` | movable→CMA bypass levers (§9) | on-device |
| `parts/gh_pinprobe.c.inc` | `/dev/gh_pinprobe` read-only pin-feasibility probe | on-device |
| `tests/` | mock harness: shim, fake buddy, scenarios | CI |
| `tier1/` | QEMU integration rig (companion `.ko` + exerciser) | CI |

```sh
make test              # Tier-0: userspace mock harness (ASan+UBSan, then TSan); no kernel
make module KDIR=...   # the kernel .ko (against a GKI build tree)
make -C tier1 ...      # Tier-1: QEMU integration rig (see tier1/README.md)
./build.sh             # all KMIs in Docker + the Magisk/KernelSU package
```

Testing has three tiers. **Tier 0** (`tests/`, this repo's CI) proves the pool
logic with no kernel: `make test` builds `tests/harness.c` and replays 34
scenarios covering serve/return, event-driven death give-up + grace, the serve
gate across a VM restart, u32-timestamp wrap, two-owner mm-less attribution,
orphan/purge, S=2 block classification, the CMA reservoir (flip/stage-in/drop/
verify including the pageblock-order off-by-one), whole-zone sweep with eviction,
shrink, profile preemption/hand-off and the hook-less (temp-root) release→sweep
loop, plus a 4-phase TSan race harness (serve/return vs workers, overlapping
teardowns, serve mid-acquire, and the operator resizing/acquiring/cancelling
into the storm). **Tier 1** (`tier1/`, QEMU, run locally) compiles the real `.ko` and
fires its kprobes/tracepoints on real pages under *induced* fragmentation — no
device, no Gunyah. **Tier 2** is on-device (real `FOLL_LONGTERM`, real
fragmentation, vendor quirks).

---

## sysfs / parameter reference

All files live under `/sys/module/gh_hugepage_reserve/parameters/`. The names,
formats and error codes are a **contract** with three external consumers (§10):
the management app (polls `refill_stat` line by line, writes the two wants,
presses `acquire`, writes `reconcile` before stats reads (one app spans both
module generations: required by pre-v12, a no-op on v12), compares
`stop_reason` strings, draws
`cma_usage`), `load.sh` (preflight arguments + the fallback ladder), and
`serve_test`.

### Targets and control

| file | mode | purpose |
|---|---|---|
| `pool_want` | 0600 | pool share target (2MB pages) |
| `pool_want_with_cma` | 0600 | total guardianship target incl. reservoir; `0` disables CMA |
| `acquire` | 0200 | `0` cancel / `1` CONTIG_ANY / `2` sweep + system-wide reclaim (mode A) / `3` sweep + per-window evict (mode B) |
| `reconcile` | 0200 | legacy-compat no-op — accepted for old apps; stats are live, there is nothing to reconcile |
| `manual_release` | 0200 | write `1` → one extra release round; writing often never shortens a grace period |
| `manual_refill` | 0200 | write `1` → `adjust_try(RELEASE)` |
| `hook_enable` | 0600 | serve kretprobe on/off; reads back the **actual** attach state |
| `reclaim_enable` | 0600 | free tracepoint on/off; reads back the actual attach state |

Write rules (§4): both wants are clamped to `pool_size_max` and, when CMA is
usable and S>1, aligned up to a multiple of S; coupling keeps
`want <= want_cma` (writing a bigger want raises with_cma; writing a with_cma
below want raises it to want — it never shrinks want). Lowering either value
clamps `pool_total` and fires `adjust_try(SHRINK)` asynchronously; *raising* a
value only records it — someone still has to press `acquire`.

Error codes: `-EINVAL` (bad value), `-ENODEV` (insmod not finished),
`-EBUSY` (for the wants: ANY adjust run is in flight; for `acquire`: a USER run
is), `-ENOSYS` (a required symbol did not resolve — for `acquire`: 1 needs
contig_pages, 2 needs contig_range, 3 additionally needs folio_isolate_lru +
reclaim_pages; for `pool_want_with_cma`: CMA is UNAVAILABLE). `-EBUSY` on want
writes while a run is in flight is deliberate, not defensive: every run chases
the target it snapshotted at start, and the only way to change targets is
`acquire=0` (stop the running run) → write → press `acquire` again;
`acquire_active=1` in `refill_stat` is exactly the "writes are blocked" signal.

### Observation

| file | mode | contents |
|---|---|---|
| `refill_stat` | 0400 | 17 `key=value` lines (below) |
| `pool_avail` | 0400 | pages standing by in the pool |
| `pool_cma` | 0400 | reservoir size (in 2MB-page equivalents) |
| `pool_avail_cma_able` | 0400 | avail pages belonging to a cma_able block (0 unless VERIFIED) |
| `pool_size_max` | 0400 | RAM-derived cap on both wants |
| `reclaim_debug` | 0400 | `o9_seen`, `del_hit`, `del_miss`, `gate_drop`, `skip_unmovable`, `reject`, `orphan`, `purged`, `in_hook`, `in_sweep`, `in_cma`, `in_user`, `in_refill` |
| `vm_owners` | 0400 | one line per owner: pid / vm_count / served / abandoned / comm |
| `served_summary` | 0400 | tracked / live / orphan counts, computed by the release round |
| `purge_log` | 0400 | pfn / refcount / current state of given-up pages (needs `debug=1`) |
| `cma_usage` | 0400 | reservoir occupancy (free/anon/file MB, block tri-state), ~1s cache |

`refill_stat` fields: `state`, `pool_avail`, `pool_total` (proven capacity,
§5.1), `served` (= served + released), `pool_want`, `total_served`,
`total_refilled`, `active_vms`, `acquire_active`, `acquire_mode`,
`acquire_stop_reason`, `refill_enable`, `free_reclaim` (the free hook's real
state), `pool_want_with_cma`, `pool_cma`, `pool_avail_cma_able`, `cma_pb_order`.
`acquire_active` reflects whether the adjust worker has a run in flight at all
(**any profile**, release-triggered background refills and SHRINK included); =1
also means want writes return -EBUSY and the CMA bypass pauses lending.
`acquire_mode` / `acquire_stop_reason` reflect **USER runs only** — a background
run never overwrites them. `cma_pb_order=-1` means the whole CMA side is off for
this boot (missing symbols/preflight values, or verification failed).

`acquire_stop_reason` is a fixed string set the app compares verbatim: `idle` /
`acquiring` / `already at target` / `pool capacity full` / `cma headroom floor` /
`cma flip failed (systemic)` / `stopped by user` / `reached target` /
`reached target,with_cma` / `migration exhausted` / `cma sources exhausted` /
`scanned all present memory` / `low-memory floor` / `evict-B unavailable` /
`quality converged` / `cma sources stuck`.

`vm_owners` doubles as a stuck-VM detector. `abandoned` counts pages let go when
the pid DIED still holding them; for a live pid it is always 0 (there is no
abandon-while-alive anymore). The live-pid stuck signal is vm_count==0 with
served>0 persisting: crosvm's teardown is slow or stuck, and the pages are still
on the SERVED books — the app can read `/proc/<pid>/stat` and flag a D state.

### Movable→CMA levers (§9)

| file | mode | purpose |
|---|---|---|
| `moveable_to_cma_vender_already_allowed` | 0400 | `1` = the vendor kernel already redirects movable→CMA, so a lever write is a no-op |
| `moveable_to_cma_gfp_cma_hook` | 0600 | surgical, preferred: a vendor-hook probe that ORs `ALLOC_CMA` into plain movable requests |
| `moveable_to_cma_restrict_cma_redirect_disabled` | 0600 | global: flips the kernel's `restrict_cma_redirect` static key; `1` = movable may migrate in |

Without one of these the reservoir is "guarded, but nobody can borrow it": a
stock kernel routes only `__GFP_CMA`-tagged allocations to the CMA freelist,
while the app's real working set (page cache, mTHP anon) carries no such tag.
Both levers are off by default, gated on the reservoir being built and harvesting
being quiet (prefill finished and NO adjust run in flight — any profile can reach
CMA-touching stages, not just a user-pressed acquire), and refuse to grant CMA
below `cma_bypass_floor_mb`.

### Module parameters

| parameter | mode | default | purpose |
|---|---|---|---|
| `pool_want` / `pool_want_with_cma` | 0600 | 0 | as above; also settable at insmod |
| `system_reserve_mb` | 0400 | 6144 (floor 64) | RAM left to the system when computing `pool_size_max = min(ram − min(ram/2, system_reserve_mb), table cap)` |
| `system_reserve_mb_default` | 0400 | computed | **This device's** default reserve, `min(RAM/2, 6144)`, settled at init — not the built-in constant, which the RAM/2 cap makes wrong on small phones (8GB: the 6144 default keeps only 4096, and configuring anything above 4096 is a no-op). A configured value overwrites `system_reserve_mb`, so this is the only place the default survives; the app needs it as the threshold for "you are lowering the reserve" |
| `migrate_cma_val` | 0400 | −1 | preflight: the runtime `MIGRATE_CMA` value (from BTF) |
| `pageblock_order_val` | 0400 | −1 | preflight: pageblock order (from `/proc/pagetypeinfo`) |
| `disable_kapi` | 0400 | — | preflight: comma-separated blacklist of symbols whose BTF signature drifted |
| `cma_reservoir_floor_mb` | 0600 | 512 | refuse a flip below this much non-CMA headroom |
| `acquire_mem_floor_mb` | 0600 | 512 | the sweep brakes below this `MemAvailable` |
| `cma_bypass_floor_mb` | 0600 | 256 | the movable→CMA hook stops granting below this free CMA |
| `acquire_drop_slab` | 0600 | 1 | drop reclaimable slab once at the start of a USER run |
| `refill_enable` | 0600 | 1 | gates only RELEASE's cheap+full; free return, precise, stage_in and EXPIRED run regardless |
| `debug` | 0644 | 0 | enable `pool_check` per round and the `purge_log` file |
| `sim_cma_order` | 0400 | 0 | test only: force the S>1 bookkeeping paths on an S==1 device |
| `vm_create_sym` / `vm_destroy_sym` / `vm_reclaim_sym` | 0400 | — | override the kprobe target symbol names |
| `refill_delay_ms` | 0600 | 0 | **accepted and ignored** — legacy loader compatibility (rejecting it would break the insmod line and drop `load.sh` to a lower rung) |

Any of the three preflight values missing leaves CMA UNAVAILABLE; all present
only reaches PENDING — flipping still requires passing verification.

### `load.sh` / `settings.prop` contract

`package/module/load.sh` is the single source of truth for "how to insmod this
module" (both the boot path and the app's runtime re-enable run it), and it
degrades through a ladder of insmod lines so an older `.ko` that lacks a
parameter still loads. Its ladder deliberately relies on a strict kernel
*rejecting* an unknown parameter — which is why the module **must never define a
parameter named `pool_target`** (the historical name carried in the ladder line).

`settings.prop` keys: `pool_want`, `pool_want_with_cma`, `cma_movable_lever`
(`hook` | `flag` → which of the two lever files to arm at insmod),
`system_reserve_mb` (only passed when set), `cma_reservoir_floor_mb` (only
passed when set — it rides the v10 CMA argument group rather than a new top
rung, being a v10-era parameter, and it has to be an insmod argument rather than
a later write to its 0600 file: the synchronous prefill inside insmod already
builds the reservoir and consults the floor on every flip, so a write after
insmod returns is one boot too late; note that the parameter has existed since
v10, so its presence does *not* prove this package's `load.sh` feeds it — the
app must compare the readback after a reload, or gate on `module.prop`'s
versionCode), and the boot-acquire trio
`boot_acquire` (0–3, default 0: once the module is present, `load.sh` writes that
mode to `acquire`, stepping down 3→2→1 on `-ENOSYS`; failure is non-fatal),
`boot_acquire_runs` (default 1) and `boot_acquire_wait` (seconds, default 0).
All three are **loader policy, not module parameters** — it is just a USER run,
so the app shows its progress in `refill_stat` and `acquire=0` cancels it. Their
module params are writable (0600) *because* the module is blind to them: a write
changes no behaviour, and it gives a saved setting somewhere visible before the
next load. Anything the module acts on stays read-only — `system_reserve_mb`
sizes a table built once, so a later write could only lie.

The trio is what a temp-root device has instead of a button. A temp-root *soft*
reboot restarts Android's userspace without taking the kernel down, so this
script runs again with the `.ko` still loaded: the acquire is therefore gated on
the module being **present**, not on this insmod having succeeded (it returns
EEXIST there), because userspace-down-and-no-GUI-yet is the freest memory the
module will ever see and the only window heavy pressure can use. `runs` presses
that USER run more than once, each after the previous finished — one run is a
single one-way pass, so windows its own eviction freed behind the sweep cursor
are only reachable by the next run, and a run with nothing to do exits in O(1).
`wait` is how many seconds `load.sh` may block in post-fs-data waiting for them,
which is what actually holds zygote back while the sweep works; keep it under
the root manager's post-fs-data timeout (Magisk: 40s), and anything left over
finishes in the background. Blocking is a boot-path concern, so only the
`GH_BOOT=1` caller (post-fs-data.sh) may spend that budget — the app's runtime
"Enable" runs the same script, presses the same runs and never blocks. The defaults (0 / 1 / 0) are the old behaviour, and
because they live in `settings.prop` on `/data` they are re-read on every soft
reboot.

### `/dev/gh_pinprobe`

A read-only "would this range fail a `FOLL_LONGTERM` pin?" probe
(`GH_PINPROBE_RANGE`, `'P'` magic, `struct gh_pinprobe_range`), ABI-identical to
the original in `gh_unmovable.ko`, so the crosvm client needs no change. Every
Gunyah memory transfer ends in `pin_user_pages_fast(FOLL_LONGTERM)`, and a page
in a CMA / isolate / ZONE_MOVABLE pageblock cannot be pinned that way — the
kernel migrates it out first, and when there is nowhere to migrate to, the
failure lands deep inside the hypervisor call. The probe answers the cheap
question ("is any of this in a pageblock that would need migrating?") by sampling
one page per 2MB with `FOLL_NOFAULT`, never faulting anything in. This module
owns it because it owns the CMA state and knows the runtime `MIGRATE_CMA` value.

---

## Usage

```sh
# Load with a boot reserve (memory is unfragmented at boot — by far the most reliable).
insmod gh_hugepage_reserve.ko pool_want=1024                    # 1024 × 2MB = 2GB

# Same, plus a 2GB CMA reservoir lent back to the system while no VM needs it.
insmod gh_hugepage_reserve.ko pool_want=1024 pool_want_with_cma=2048 \
       migrate_cma_val=... pageblock_order_val=...              # load.sh fills these in

# Top up on demand later (mode B: per-window evict), and watch progress.
echo 3 > .../parameters/acquire
while grep -q 'acquire_active=1' .../parameters/refill_stat; do sleep 1; done
grep -E 'pool_avail|pool_cma|acquire_stop_reason' .../parameters/refill_stat

# Change the target while a user acquire is running: cancel first, then rewrite.
echo 0    > .../parameters/acquire
echo 2048 > .../parameters/pool_want
echo 3    > .../parameters/acquire

# Verify leak-free recovery in isolation (the pool then refills ONLY from pages a
# VM actually returned, never from fresh alloc_pages).
insmod gh_hugepage_reserve.ko pool_want=256 refill_enable=0
# ... run a VM, use it, shut it down, wait ~10s ...
cat .../parameters/refill_stat
cat .../parameters/reclaim_debug     # in_hook should account for the returns; reject/orphan ≈ 0
```

(where `...` is `/sys/module/gh_hugepage_reserve/parameters`)

---

## Status

The mock-testable core (pool object + workers) is complete and CI-green (30
scenarios + a 4-phase TSan race harness). The kernel backend (kapi / hooks / sysfs /
unlock_cma / pinprobe + root) compiles clean (zero warnings) against
android14-6.1 / android15-6.6 / android16-6.12, and Tier-1 passes on all three in
QEMU (2026-08-28): kapi fully resolved, CMA verify reaches VERIFIED (including a
live DEFERRED retry), all six hooks attach, scenarios A–E pass, clean unload.

Remaining work is Tier-2: the on-device regression on the three target devices
(real `FOLL_LONGTERM`, vendor quirks, kCFI), plus the two `NOTE(on-device)`
confirmations (owner write-lock irq context; the kapi coarse-adapter caveats).
