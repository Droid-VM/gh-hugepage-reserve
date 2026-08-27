# Tier-1 QEMU integration rig

The mock harness (`tests/`) proves the pool **logic** with no kernel. This rig
proves the **kernel-integration layer** — that the module compiles, its
kprobes/tracepoints fire on real `struct page`s, the refcount protocol works,
`alloc_contig_range` really migrates, and the straggler-skip / CMA-assembly
choices behave under *induced* fragmentation — all in QEMU, with **no Gunyah
and no device**.

## Pieces

- **`gh_test_companion.ko`** stands in for the phone:
  - VM-lifecycle stubs (`gh_test_vm_create/_destroy/_reclaim`) that the main
    module kprobes via its `vm_*_sym=` params — so the probes fire on a kernel
    that has no Gunyah.
  - a **FOLL_LONGTERM pinner** (`pin`/`unpin`) — the property that makes
    migration of guest RAM fail, so the stuck-pin give-up path runs
    deterministically.
  - a **controllable unmovable fragmentation field** (`frag "<MB> <pattern>"`):
    owns a large contiguous region and keeps one unmovable kernel page per
    *poisoned* 2MB window. Patterns:
    - `none` — clean; hugepages come free (the logic under test never runs).
    - `all` — every window straggled; nothing assembles (asserts the sweep
      does **not** white-kick — no eviction for windows that can't assemble).
    - `split` — one sub-block of **every** believed S-block straggled; **no**
      block can complete → the module must give up on all of them.
    - `block` — alternate whole S-blocks straggled; **half the blocks are fully
      clean** → those assemble into cma_able while the poisoned half is skipped.
      This is the "prefer the assemblable block, give up on non-cma-able" test.
      Set `frag_s` to match `sim_cma_order` (S = 1 << (sim_cma_order-9)).
- **`exerciser`** (userspace) — fires a VM create, faults in movable THP so the
  serve kretprobe swaps in pool pages, pins/unpins, and asserts on the module's
  own `refill_stat` counters.
- **`tier1_init.c`** — PID-1 in-guest test plan: A load, B serve+pin+shutdown,
  C induced-fragmentation sweep, P pinprobe ioctl, **F fork-child owner killed
  (the only real pid-death path — PID 1 never dies, so this is what exercises
  sched_process_exit → mark_dead → DEATH_GRACE give-up), D0 rmmod-under-pin, D
  rmmod**.
- **`run-qemu.sh`** / **`mkinitramfs.sh`** — boot + package for CI.

Real order per kernel, no sim: `tier1_init` reads the true `pageblock_order`
from `/proc/pagetypeinfo` and passes it as `pageblock_order_val` with **no**
`sim_cma_order`, and sizes the fragmentation field (`frag_s`) to the real
S = 1<<(order-9). These GKI configs build with `CONFIG_HUGETLB_PAGE` unset,
so the order comes out of MAX_ORDER — and it differs by build: **android14-6.1
is a genuine order-10 kernel (S=2)**, while android15-6.6 / android16-6.12 are
order-9 (S=1). So relabel / CMA verify / flip run against real blocks on every
kernel, and 6.1 exercises the whole S=2 path (cma_able, gap-repair, sub-block
assembly) with no simulation. (`sim_cma_order` is still there to force S=2
bookkeeping onto an order-9 kernel — an x86 host, or 6.6/6.12 — but the pool's
S=2 logic is also covered by the mock harness's S=2 scenarios.)

## Run

**Local-only by decision** — CI runs Tier-0 (mock) plus the GKI compile matrix;
the QEMU runtime is too heavy for CI and needs a built GKI Image anyway. Run it
here, either via `run-local.sh` (DDK kdir + prebuilt Image, default
android15-6.6) or by hand against a kernel build tree:

```sh
make -C .. module KDIR=$KDIR          # main .ko
make -C . modules exerciser KDIR=$KDIR
bash mkinitramfs.sh
BZIMAGE=$KDIR/arch/x86/boot/bzImage INITRAMFS=$PWD/initramfs.cpio.gz ./run-qemu.sh
```

## What Tier-1 still cannot prove

Real Gunyah `FOLL_LONGTERM` semantics on *guest* stage-2 mappings, real device
fragmentation, vendor pageblock/CMA quirks, kCFI on the target's clang. Those
stay Tier-2 (on-device).
