// SPDX-License-Identifier: GPL-2.0
/*
 * gh_test_companion — Tier-1 QEMU test rig for gh_hugepage_reserve.
 *
 * Stands in for the three things a real Gunyah phone provides, so the main
 * module's kernel-integration layer can be exercised on a generic kernel with
 * NO Gunyah and NO device:
 *
 *   1. VM-lifecycle stubs   — noinline functions the main module kprobes
 *      (point its vm_create_sym / vm_destroy_sym / vm_reclaim_sym params at
 *      gh_test_vm_create / _destroy / _reclaim). A sysfs write "fires" them.
 *   2. A FOLL_LONGTERM pinner — pins a served page range for the VM's "life",
 *      the property that makes migration fail (so give-up / stuck-pin paths
 *      run deterministically).
 *   3. A CONTROLLABLE UNMOVABLE FRAGMENTATION FIELD — the interesting part.
 *      We own a large contiguous region and plant kernel (unmovable) order-0
 *      stragglers at scripted 2MB-window offsets, so most windows have a hard
 *      straggler and can never be assembled, while a scripted subset stays
 *      clean. That is what forces the module to give up on non-cma-able blocks
 *      and preferentially assemble the cma-able ones. Without induced
 *      fragmentation, contiguous hugepages come free and that logic never runs.
 *
 * All three are driven from sysfs under /sys/kernel/gh_test/.
 * On-device/CI verified only (needs a kernel build; not compiled by mock CI).
 */
#define pr_fmt(fmt) "gh_test: " fmt
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mm_types.h>
#include <linux/sched/mm.h>
#include <linux/ioctl.h>

#define GH_IOCTL_TYPE	'G'
#define GH_CREATE_VM	_IO(GH_IOCTL_TYPE, 0x0)
#define PAGE_ORDER	9
#define PAGES_2MB	(1UL << PAGE_ORDER)

/* ── 1. VM-lifecycle stubs (kprobe targets) ──────────────
 * The main module's vm_create handler reads arg index 1 (the ioctl cmd) and
 * matches GH_CREATE_VM. So the stub takes cmd as its 2nd argument. noinline +
 * the asm barrier keep the call real and the args in registers. */
noinline void gh_test_vm_create(void *file, unsigned int cmd, unsigned long arg);
noinline void gh_test_vm_create(void *file, unsigned int cmd, unsigned long arg)
{ asm volatile("" :: "r"(file), "r"(cmd), "r"(arg) : "memory"); }
noinline void gh_test_vm_destroy(void *vm) { barrier(); }
noinline void gh_test_vm_reclaim(void *parcel) { barrier(); }
EXPORT_SYMBOL(gh_test_vm_create);
EXPORT_SYMBOL(gh_test_vm_destroy);
EXPORT_SYMBOL(gh_test_vm_reclaim);

/* ── 3. Fragmentation field ──────────────────────────────
 * Own a big contiguous region, free it all back to buddy except one unmovable
 * order-0 page per POISONED 2MB window. The pattern knob decides which windows
 * are poisoned relative to the S-block grouping so you can build:
 *   pattern=all     every 2MB window straggled  -> nothing assemblable
 *   pattern=none    clean                        -> everything assemblable
 *   pattern=block   straggle whole S-blocks, alternating: half the believed
 *                   blocks fully clean (assemblable -> can become cma_able),
 *                   half fully poisoned (skip). Tests the "prefer assemblable
 *                   block" choice.
 *   pattern=split   straggle ONE sub-block of every S-block: NO believed block
 *                   can complete (every block mixed) -> module must give up on
 *                   all of them and report quality-converged / not assemble.
 */
static struct page *frag_region;		/* head of the owned contiguous run */
static unsigned long frag_base_pfn, frag_nr;	/* the run, in pages */
static struct page **frag_stragglers;		/* kept-allocated poison pages */
static int frag_nstrag;
static int frag_sub_per_block = 2;		/* S for the split/block patterns */

static void frag_release(void)
{
	unsigned long w;
	int i;

	if (frag_stragglers) {
		for (i = 0; i < frag_nstrag; i++)
			if (frag_stragglers[i])
				__free_pages(frag_stragglers[i], 0);
		kfree(frag_stragglers);
		frag_stragglers = NULL;
		frag_nstrag = 0;
	}
	if (frag_region) {
		/* free every 2MB window not already broken into a straggler */
		for (w = 0; w < frag_nr; w += PAGES_2MB)
			__free_pages(frag_region + w, PAGE_ORDER);
		frag_region = NULL;
	}
}

static bool window_poisoned(const char *pat, unsigned long widx)
{
	int s = frag_sub_per_block;

	if (!strcmp(pat, "none"))  return false;
	if (!strcmp(pat, "all"))   return true;
	if (!strcmp(pat, "block")) return ((widx / s) & 1) == 1;	/* alt whole blocks */
	if (!strcmp(pat, "split")) return (widx % s) == 0;	/* one sub per block */
	return false;
}

static int frag_build(unsigned long mb, const char *pat)
{
	unsigned long nr = (mb << 20) >> PAGE_SHIFT, w, widx = 0;
	int order = get_order(mb << 20), i = 0, maxstrag;

	frag_release();
	/* one big contiguous run so window pfns are contiguous and scriptable */
	frag_region = alloc_pages(GFP_KERNEL | __GFP_COMP | __GFP_NOWARN, order);
	if (!frag_region) {
		pr_warn("frag: alloc_pages order %d failed\n", order);
		return -ENOMEM;
	}
	split_page(frag_region, order);		/* order-0 refcounts across the run */
	frag_base_pfn = page_to_pfn(frag_region);
	frag_nr = nr;
	maxstrag = (int)(nr / PAGES_2MB) + 1;
	frag_stragglers = kcalloc(maxstrag, sizeof(*frag_stragglers), GFP_KERNEL);
	if (!frag_stragglers) { frag_release(); return -ENOMEM; }

	for (w = 0; w + PAGES_2MB <= frag_nr; w += PAGES_2MB, widx++) {
		struct page *win = frag_region + w;
		unsigned long k;

		if (window_poisoned(pat, widx)) {
			/* keep ONE page (an unmovable kernel alloc: refcount held,
			 * not on LRU, not __PageMovable) — a hard straggler — and
			 * free the other 511 back to buddy */
			frag_stragglers[i++] = win;	/* head stays allocated */
			for (k = 1; k < PAGES_2MB; k++)
				__free_pages(win + k, 0);
		} else {
			for (k = 0; k < PAGES_2MB; k++)
				__free_pages(win + k, 0);
		}
	}
	frag_nstrag = i;
	pr_info("frag: %lu MB region at pfn %lx, %d straggled window(s), pattern=%s\n",
		mb, frag_base_pfn, frag_nstrag, pat);
	return 0;
}

/* ── 2. FOLL_LONGTERM pinner ──────────────────────────────
 * Pin a user range for the VM's "life". pin_user_pages(FOLL_LONGTERM) is what
 * makes folio_maybe_dma_pinned() true, so migration of these pages fails —
 * exactly the Gunyah guest-RAM property. Unpin releases it. A range left
 * pinned across a "vm_destroy" fire reproduces the stuck-pin give-up path. */
static struct page **pin_pages;
static long pin_n;
static unsigned long pin_addr, pin_len;

static void do_unpin(void)
{
	if (pin_pages) {
		unpin_user_pages(pin_pages, pin_n);
		kvfree(pin_pages);
		pin_pages = NULL;
		pin_n = 0;
	}
}
static int do_pin(unsigned long addr, unsigned long len)
{
	long np = (len + PAGE_SIZE - 1) >> PAGE_SHIFT, got;

	do_unpin();
	pin_pages = kvcalloc(np, sizeof(*pin_pages), GFP_KERNEL);
	if (!pin_pages)
		return -ENOMEM;
	got = pin_user_pages_fast(addr, np, FOLL_LONGTERM | FOLL_WRITE, pin_pages);
	if (got <= 0) {
		kvfree(pin_pages);
		pin_pages = NULL;
		return got < 0 ? (int)got : -EFAULT;
	}
	pin_n = got;
	pin_addr = addr; pin_len = len;
	pr_info("pinned %ld page(s) FOLL_LONGTERM at %lx\n", got, addr);
	return 0;
}

/* ── sysfs control surface ───────────────────────────────
 *   fire_create / fire_destroy / fire_reclaim  (echo 1)
 *   frag         "<MB> <pattern>"   (all|none|block|split)
 *   frag_s       "<n>"              sub-blocks per believed block (match sim_cma_order)
 *   pin          "<hexaddr> <len>"  ;  unpin (echo 1)
 */
static struct kobject *gh_test_kobj;

static ssize_t fire_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t n)
{
	if (!strcmp(a->attr.name, "fire_create"))  gh_test_vm_create(NULL, GH_CREATE_VM, 0);
	if (!strcmp(a->attr.name, "fire_destroy")) gh_test_vm_destroy(NULL);
	if (!strcmp(a->attr.name, "fire_reclaim")) gh_test_vm_reclaim(NULL);
	return n;
}
static ssize_t frag_store(struct kobject *k, struct kobj_attribute *a,
			  const char *buf, size_t n)
{
	unsigned long mb; char pat[16];

	if (sscanf(buf, "%lu %15s", &mb, pat) != 2)
		return -EINVAL;
	return frag_build(mb, pat) ? -EIO : (ssize_t)n;
}
static ssize_t frag_s_store(struct kobject *k, struct kobj_attribute *a,
			    const char *buf, size_t n)
{ int v; if (kstrtoint(buf, 10, &v) || v < 1 || v > 8) return -EINVAL; frag_sub_per_block = v; return n; }
static ssize_t pin_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t n)
{
	unsigned long addr, len;

	if (sscanf(buf, "%lx %lu", &addr, &len) != 2)
		return -EINVAL;
	return do_pin(addr, len) ? -EIO : (ssize_t)n;
}
static ssize_t unpin_store(struct kobject *k, struct kobj_attribute *a,
			   const char *buf, size_t n)
{ do_unpin(); return n; }

static struct kobj_attribute a_create  = __ATTR(fire_create,  0200, NULL, fire_store);
static struct kobj_attribute a_destroy = __ATTR(fire_destroy, 0200, NULL, fire_store);
static struct kobj_attribute a_reclaim = __ATTR(fire_reclaim, 0200, NULL, fire_store);
static struct kobj_attribute a_frag    = __ATTR(frag,   0200, NULL, frag_store);
static struct kobj_attribute a_frag_s  = __ATTR(frag_s, 0200, NULL, frag_s_store);
static struct kobj_attribute a_pin     = __ATTR(pin,    0200, NULL, pin_store);
static struct kobj_attribute a_unpin   = __ATTR(unpin,  0200, NULL, unpin_store);
static struct attribute *gh_test_attrs[] = {
	&a_create.attr, &a_destroy.attr, &a_reclaim.attr,
	&a_frag.attr, &a_frag_s.attr, &a_pin.attr, &a_unpin.attr, NULL,
};
static const struct attribute_group gh_test_grp = { .attrs = gh_test_attrs };

static int __init gh_test_init(void)
{
	gh_test_kobj = kobject_create_and_add("gh_test", kernel_kobj);
	if (!gh_test_kobj)
		return -ENOMEM;
	if (sysfs_create_group(gh_test_kobj, &gh_test_grp)) {
		kobject_put(gh_test_kobj);
		return -ENOMEM;
	}
	pr_info("ready (sysfs /sys/kernel/gh_test/)\n");
	return 0;
}
static void __exit gh_test_exit(void)
{
	do_unpin();
	frag_release();
	sysfs_remove_group(gh_test_kobj, &gh_test_grp);
	kobject_put(gh_test_kobj);
}
module_init(gh_test_init);
module_exit(gh_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tier-1 QEMU test rig for gh_hugepage_reserve (VM stubs, longterm pin, fragmenter)");
