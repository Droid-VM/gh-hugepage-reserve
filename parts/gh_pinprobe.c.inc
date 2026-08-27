// SPDX-License-Identifier: GPL-2.0
/*
 * gh_pinprobe — /dev/gh_pinprobe: a read-only "would this range fail a
 * FOLL_LONGTERM pin?" probe. Kernel-only part; the mock harness never includes
 * it.
 *
 * Why the reserve owns this: every gunyah memory transfer (boot LEND of guest
 * RAM, boot SHARE of the GPU pools, runtime SHARE of a blob) ends in
 * pin_user_pages_fast(FOLL_LONGTERM). A page in a CMA / isolate / ZONE_MOVABLE
 * pageblock cannot be pinned that way — the kernel migrates it out first, and
 * when there is nothing to migrate into, that step fails deep inside the
 * hypervisor call (observed: a multi-minute whole-host stall ending in the
 * kernel OOM-killing crosvm, and a "qcom_scm: Assign memory protection call
 * failed -22" that reset the phone). The cheap question at the decision point is
 * not "pin it" but "is any of it in a pageblock that would need migrating?" — a
 * property of the pages, readable without a long-term reference and without
 * migrating anything.
 *
 * This probe used to live in gh_unmovable.ko. It belongs here: this module is
 * the component that owns the CMA state — it already resolves the pageblock
 * migratetype reader through the version-flattening kapi, it knows the RUNTIME
 * MIGRATE_CMA value (preflight/BTF, not a compile-time enum — the enumerator is
 * vendor-arranged and differs across these kernels), and it is loaded whenever a
 * VM runs. The ABI is byte-identical to the original so the crosvm client
 * (hypervisor/src/gunyah/pin.rs) needs no change: same /dev/gh_pinprobe path,
 * same 'P' magic, same struct gh_pinprobe_range.
 *
 * Sampling: migratetype is a per-pageblock property (>= 2MB on every config
 * here) and the caller's regions are built out of 2MB folios, so one sample per
 * 2MB covers every pageblock. The GUP is FOLL_NOFAULT — "look, do not populate":
 * get_user_pages() with no flags would fault an absent page in (one 4KB page per
 * 2MB sample, order-0, which the reserve's hook does not intercept), so the
 * probe would be measuring memory it had itself just created, and could land it
 * in CMA. Absent pages are counted separately, never faulted in. FOLL_NOFAULT is
 * 5.17+ and public on every KMI this builds for (6.1/6.6/6.12); if a future
 * kernel refuses it, every sample reads absent — "I could not tell", not a false
 * all-clear — and we say so once.
 */

#define GH_PINPROBE_IOC_MAGIC 'P'

/* struct gh_pinprobe_range — the UAPI. Byte-for-byte identical to the original
 * gh_unmovable definition; the userspace client mirrors this layout. */
struct gh_pinprobe_range {
	__u64 addr;		/* in:  user VA (page aligned) */
	__u64 len;		/* in:  bytes */
	__u64 samples;		/* out: pageblock samples taken */
	__u64 samples_cma;	/* out: samples in a MIGRATE_CMA pageblock */
	__u64 samples_isolate;	/* out: samples in a MIGRATE_ISOLATE pageblock */
	__u64 samples_movable;	/* out: samples in ZONE_MOVABLE */
	__u64 samples_absent;	/* out: samples with no page present */
	__u64 first_bad_offset;	/* out: byte offset of the first unpinnable sample, ~0 if none */
	__u32 sample_bytes;	/* out: distance between samples */
	__u32 flags;		/* in/out: reserved, must be 0 */
};

#define GH_PINPROBE_RANGE _IOWR(GH_PINPROBE_IOC_MAGIC, 1, struct gh_pinprobe_range)

#define GHP_SAMPLE_BYTES   (2UL << 20)	/* one sample per 2MB */
#define GHP_FOLL_LOOK_ONLY FOLL_NOFAULT	/* look, never populate/migrate */

/* Returns true if a page in this pageblock would force migration before a
 * FOLL_LONGTERM pin. Classification goes through the kapi so it is correct on
 * every KMI: get_pfnblock_mt is the resolved migratetype reader, gh_migrate_cma
 * is the runtime MIGRATE_CMA value (-1 when CMA is off, so nothing matches), and
 * in_zone_movable is the resolved ZONE_MOVABLE test. */
static bool gh_pp_classify(unsigned long pfn, struct gh_pinprobe_range *r)
{
	bool bad = false;
	int mt = (int)gh_kapi.get_pfnblock_mt(pfn);

	if (gh_migrate_cma >= 0 && mt == gh_migrate_cma) {
		r->samples_cma++;
		bad = true;
	}
#ifdef CONFIG_MEMORY_ISOLATION
	if (mt == MIGRATE_ISOLATE) {	/* transient; the last enumerator, stable across these builds */
		r->samples_isolate++;
		bad = true;
	}
#endif
	if (gh_kapi.in_zone_movable(pfn)) {
		r->samples_movable++;
		bad = true;
	}
	return bad;
}

static long gh_pp_probe(unsigned long arg)
{
	struct gh_pinprobe_range r;
	unsigned long off, end;

	if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
		return -EFAULT;
	if (!r.len || r.flags || (r.addr & ~PAGE_MASK))
		return -EINVAL;

	end = r.len;
	r.samples = 0;
	r.samples_cma = 0;
	r.samples_isolate = 0;
	r.samples_movable = 0;
	r.samples_absent = 0;
	r.first_bad_offset = ~0ULL;
	r.sample_bytes = GHP_SAMPLE_BYTES;

	for (off = 0; off < end; off += GHP_SAMPLE_BYTES) {
		unsigned long va = (unsigned long)r.addr + off;
		struct page *page = NULL;
		int got;

		if (fatal_signal_pending(current))
			return -EINTR;

		/* Look, never populate, never migrate (see the header). */
		got = get_user_pages_fast(va, 1, GHP_FOLL_LOOK_ONLY, &page);
		if (got == -EINVAL)
			pr_warn_once("pinprobe: kernel rejected FOLL_NOFAULT; every sample reads absent\n");
		if (got != 1) {
			r.samples_absent++;
			r.samples++;
			continue;
		}
		r.samples++;
		if (gh_pp_classify(page_to_pfn(page), &r) && r.first_bad_offset == ~0ULL)
			r.first_bad_offset = off;
		put_page(page);
		cond_resched();
	}

	if (copy_to_user((void __user *)arg, &r, sizeof(r)))
		return -EFAULT;
	return 0;
}

static long gh_pp_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case GH_PINPROBE_RANGE:
		return gh_pp_probe(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations gh_pp_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= gh_pp_ioctl,
	.compat_ioctl	= gh_pp_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice gh_pp_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "gh_pinprobe",
	.fops  = &gh_pp_fops,
	.mode  = 0666,
};

static bool gh_pp_registered;

/* Register /dev/gh_pinprobe. A failure here must not take the reserve down (the
 * probe is an optional optimisation for the VMM), so it is a warning, not an
 * error — mirrors how gh_unmovable treated it. */
static void gh_pinprobe_register(void)
{
	int ret = misc_register(&gh_pp_dev);

	if (ret) {
		pr_warn("/dev/gh_pinprobe unavailable: %d\n", ret);
		return;
	}
	gh_pp_registered = true;
	pr_info("/dev/gh_pinprobe ready (longterm-pinnability probe)\n");
}

static void gh_pinprobe_unregister(void)
{
	if (gh_pp_registered) {
		misc_deregister(&gh_pp_dev);
		gh_pp_registered = false;
	}
}
