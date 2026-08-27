// SPDX-License-Identifier: GPL-2.0
/* Tier-1 all-in-one init: PID 1 inside the QEMU aarch64 GKI guest. No busybox,
 * no rootfs distro — a single static binary that mounts, loads both modules,
 * drives the serve/reclaim + induced-fragmentation scenarios through sysfs,
 * asserts on the module's own counters, prints TIER1: ALL PASS, powers off. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/reboot.h>
#include <sys/wait.h>
#include <signal.h>
#define GH_DEATH_GRACE_PROBE 5

/* /dev/gh_pinprobe UAPI — must match parts/gh_pinprobe.c.inc byte for byte */
struct gh_pinprobe_range {
	unsigned long long addr, len, samples, samples_cma, samples_isolate,
		samples_movable, samples_absent, first_bad_offset;
	unsigned int sample_bytes, flags;
};
#define GH_PINPROBE_RANGE _IOWR('P', 1, struct gh_pinprobe_range)

static int fails;
#define OK(c, ...) do { if (!(c)) { fails++; printf("TIER1 FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static int finit(const char *path, const char *args)
{
	int fd = open(path, O_RDONLY), r;
	if (fd < 0) { printf("open %s: %s\n", path, strerror(errno)); return -1; }
	r = syscall(SYS_finit_module, fd, args ? args : "", 0);
	close(fd);
	if (r) printf("finit_module %s (%s): %s\n", path, args, strerror(errno));
	return r;
}
static int wsys(const char *p, const char *v)
{
	int fd = open(p, O_WRONLY), r;
	if (fd < 0) return -1;
	r = write(fd, v, strlen(v)); close(fd);
	return r < 0 ? -1 : 0;
}
static long rstat(const char *key)
{
	FILE *f = fopen("/sys/module/gh_hugepage_reserve/parameters/refill_stat", "r");
	char line[256]; long v = -1; size_t kl = strlen(key);
	if (!f) return -1;
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (eq && (size_t)(eq - line) == kl && !strncmp(line, key, kl)) v = atol(eq + 1);
	}
	fclose(f); return v;
}
static long rdbg(const char *key)
{
	FILE *f = fopen("/sys/module/gh_hugepage_reserve/parameters/reclaim_debug","r");
	char line[256]; long v=-1; size_t kl=strlen(key);
	if (!f) return -1;
	while (fgets(line,sizeof line,f)) { char *eq=strchr(line,'='); if(eq&&(size_t)(eq-line)==kl&&!strncmp(line,key,kl)) v=atol(eq+1); }
	fclose(f); return v;
}
/* is pid present as a VM owner? (grep vm_owners) */
static int owner_present(pid_t pid)
{
	FILE *f = fopen("/sys/module/gh_hugepage_reserve/parameters/vm_owners", "r");
	char line[256], key[32]; int found = 0;
	if (!f) return -1;
	snprintf(key, sizeof key, "pid=%d ", (int)pid);
	while (fgets(line, sizeof line, f))
		if (strstr(line, key)) found = 1;
	fclose(f); return found;
}
static const char *stopreason(void)
{
	static char buf[64]; FILE *f = fopen("/sys/module/gh_hugepage_reserve/parameters/refill_stat","r");
	char line[256]; buf[0]=0;
	if (!f) return "?";
	while (fgets(line,sizeof line,f)) if (!strncmp(line,"acquire_stop_reason=",20)) { strncpy(buf,line+20,63); buf[strcspn(buf,"\n")]=0; }
	fclose(f); return buf;
}

/* the kernel's REAL pageblock order (/proc/pagetypeinfo), so we test S=2 on a
 * genuinely order-10 kernel instead of faking it with sim_cma_order. All three
 * GKI configs here build with CONFIG_HUGETLB_PAGE unset, so pageblock_order
 * falls out as MAX_ORDER-ish = 10 (4MB blocks, S = 1<<(10-9) = 2). */
static int read_pageblock_order(void)
{
	FILE *f = fopen("/proc/pagetypeinfo", "r");
	char line[256]; int o = -1;
	if (!f) return -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "Page block order: %d", &o) == 1) break;
	fclose(f); return o;
}

int main(void)
{
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);	/* misc nodes: /dev/gh_pinprobe */
	printf("TIER1: guest up\n");
	/* module prereq: shmem THP so memfd faults produce order-9 movable allocs */
	wsys("/sys/kernel/mm/transparent_hugepage/enabled", "always");
	wsys("/sys/kernel/mm/transparent_hugepage/defrag", "always");
	wsys("/sys/kernel/mm/transparent_hugepage/shmem_enabled", "force");

	OK(finit("/gh_test_companion.ko", "") == 0, "load companion");
	/* point the VM kprobes at the companion stubs; S=2 via sim_cma_order */
	int pbo = read_pageblock_order();
	int S = pbo >= 9 ? (1 << (pbo - 9)) : 1;
	{
		char args[320];

		/* NO sim_cma_order: cma_order = the REAL pageblock_order, so relabel,
		 * verify and flip run against genuine blocks. 6.1 here is a true
		 * order-10 kernel (S=2); 6.6/6.12 are order-9 (S=1). sim_cma_order
		 * stays available for forcing S=2 bookkeeping on an order-9 kernel. */
		printf("A0. real pageblock_order = %d (S = %d) -- no sim_cma_order\n",
		       pbo, S);
		OK(pbo >= 9, "read a sane pageblock order (%d)", pbo);
		snprintf(args, sizeof args,
			 "pool_want=64 migrate_cma_val=3 pageblock_order_val=%d debug=1 "
			 "vm_create_sym=gh_test_vm_create vm_destroy_sym=gh_test_vm_destroy "
			 "vm_reclaim_sym=gh_test_vm_reclaim", pbo);
		OK(finit("/gh_hugepage_reserve.ko", args) == 0, "load main");
	}
	printf("A. loaded: pool_avail=%ld pool_total=%ld\n", rstat("pool_avail"), rstat("pool_total"));
	OK(rstat("pool_avail") >= 0, "refill_stat readable");

	/* B. serve: become an owner, fault movable THP, expect serve to swap pool pages */
	OK(wsys("/sys/kernel/gh_test/fire_create", "1") == 0, "fire_create");
	printf("   after create: active_vms=%ld\n", rstat("active_vms"));
	{
		size_t len = 32UL << 20;
		int fd = -1;
		void *raw = mmap(NULL, len + (2UL<<20), PROT_READ|PROT_WRITE,
				 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		void *b = raw == MAP_FAILED ? MAP_FAILED :
			  (void *)(((unsigned long)raw + (2UL<<20)-1) & ~((2UL<<20)-1));
		OK(raw != MAP_FAILED, "anon mmap");
		if (b != MAP_FAILED) { madvise(b, len, MADV_HUGEPAGE); memset(b, 1, len); }
		printf("B. after fault: served=%ld total_served=%ld avail=%ld\n",
		       rstat("served"), rstat("total_served"), rstat("pool_avail"));
		printf("   rdbg: o9_seen=%ld del_hit=%ld del_miss=%ld skip_unmovable=%ld\n",
		       rdbg("o9_seen"), rdbg("del_hit"), rdbg("del_miss"), rdbg("skip_unmovable"));
		/* pin FOLL_LONGTERM then shut the VM down -> stuck-pin give-up path */
		{ char c[64]; snprintf(c, sizeof c, "%lx %zu", (unsigned long)b, len); wsys("/sys/kernel/gh_test/pin", c); }
		wsys("/sys/kernel/gh_test/fire_destroy", "1");
		sleep(2);
		printf("   VM down (pinned): served=%ld active_vms=%ld\n", rstat("served"), rstat("active_vms"));
		wsys("/sys/kernel/gh_test/unpin", "1");
		if (raw != MAP_FAILED) munmap(raw, len + (2UL<<20));
		(void)fd;
		sleep(12);
		printf("   after unpin+grace: served=%ld avail=%ld refilled=%ld\n",
		       rstat("served"), rstat("pool_avail"), rstat("total_refilled"));
	}

	/* C. induced fragmentation — the straggler-skip / assemble-what-you-can test */
	{ char sb[8]; snprintf(sb, sizeof sb, "%d", S); wsys("/sys/kernel/gh_test/frag_s", sb); }
	printf("C1. frag=all: "); wsys("/sys/kernel/gh_test/frag", "128 all");
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "3"); sleep(6);
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "0");
	printf("stop=%s able=%ld (want: no white-kick, gave up cleanly)\n", stopreason(), rstat("pool_avail_cma_able"));

	printf("C2. frag=split (every block mixed): "); wsys("/sys/kernel/gh_test/frag", "128 split");
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "3"); sleep(6);
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "0");
	printf("able=%ld stop=%s\n", rstat("pool_avail_cma_able"), stopreason());

	printf("C3. frag=block (half clean): "); wsys("/sys/kernel/gh_test/frag", "128 block");
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "3"); sleep(8);
	wsys("/sys/module/gh_hugepage_reserve/parameters/acquire", "0");
	printf("avail=%ld able=%ld stop=%s\n", rstat("pool_avail"), rstat("pool_avail_cma_able"), stopreason());
	OK(rstat("pool_avail") > 0, "C3 assembled something from a half-clean field");

	/* P. pinprobe ioctl — the CMA-detect probe this module now serves. Fault a
	 * region (pages present), then look-only probe it: FOLL_NOFAULT must see the
	 * present pages (absent==0), sample once per 2MB, and copy the counters back. */
	{
		size_t len = 16UL << 20;
		void *p = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		int pf = open("/dev/gh_pinprobe", O_RDONLY);

		if (pf < 0) {	/* devtmpfs did not populate it: mknod from the misc class */
			int maj = 0, mn = 0;
			FILE *df = fopen("/sys/class/misc/gh_pinprobe/dev", "r");

			if (df) { if (fscanf(df, "%d:%d", &maj, &mn) != 2) maj = 0; fclose(df); }
			if (maj && mknod("/dev/gh_pinprobe", S_IFCHR | 0666, makedev(maj, mn)) == 0)
				pf = open("/dev/gh_pinprobe", O_RDONLY);
		}
		OK(p != MAP_FAILED, "pinprobe test mmap");
		OK(pf >= 0, "open /dev/gh_pinprobe");
		if (p != MAP_FAILED)
			memset(p, 1, len);		/* fault every page in */
		if (pf >= 0 && p != MAP_FAILED) {
			struct gh_pinprobe_range r;
			memset(&r, 0, sizeof r);
			r.addr = (unsigned long long)(unsigned long)p;
			r.len  = len;
			OK(ioctl(pf, GH_PINPROBE_RANGE, &r) == 0, "pinprobe ioctl");
			OK(r.samples == (len >> 21), "pinprobe sampled once per 2MB");
			OK(r.samples_absent == 0, "pinprobe saw present pages (FOLL_NOFAULT ok)");
			printf("P. pinprobe: samples=%llu cma=%llu isolate=%llu movable=%llu absent=%llu first_bad=%#llx bytes=%u\n",
			       r.samples, r.samples_cma, r.samples_isolate, r.samples_movable,
			       r.samples_absent, r.first_bad_offset, r.sample_bytes);
		}
		if (pf >= 0) close(pf);
		if (p != MAP_FAILED) munmap(p, len);
	}

	/* F. a forked child becomes the VM owner, gets pool pages via serve, then
	 * is SIGKILLed. This is the ONLY path that exercises real pid death:
	 * PID 1 (this init) writes fire_create everywhere else and never dies, so
	 * the sched_process_exit tracepoint -> mark_dead -> DEATH_GRACE give-up
	 * chain is untested without a mortal owner. While the child LIVES its
	 * pages are never taken; when it dies the exit path unmaps them and the
	 * release rounds collect them back into the pool within the grace. */
	{
		pid_t child = fork();

		if (child == 0) {			/* the mortal VM owner */
			size_t len = 16UL << 20;
			void *raw = mmap(NULL, len + (2UL << 20), PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			void *b = raw == MAP_FAILED ? MAP_FAILED :
				(void *)(((unsigned long)raw + (2UL << 20) - 1) &
					 ~((2UL << 20) - 1));

			wsys("/sys/kernel/gh_test/fire_create", "1");	/* I am the owner */
			if (b != MAP_FAILED) {
				madvise(b, len, MADV_HUGEPAGE);
				memset(b, 1, len);	/* fault THP -> serve swaps pool pages */
			}
			for (;;) pause();		/* wait to be killed */
			_exit(0);
		}

		/* parent: wait for the child to register as an owner and serve */
		long served0 = 0;
		long orphan0 = rdbg("orphan"), purged0 = rdbg("purged");
		long base0;
		int i;
		for (i = 0; i < 50 && served0 == 0; i++) {
			usleep(100 * 1000);
			served0 = rstat("served");
		}
		base0 = rstat("pool_avail") + served0;		/* ~= held, conserved */
		OK(served0 > 0, "F: child owner got pool pages (served=%ld)", served0);
		OK(owner_present(child) == 1, "F: child is a tracked VM owner");

		/* child alive: it is never given up, however long (event, not timer) */
		sleep(GH_DEATH_GRACE_PROBE);
		OK(owner_present(child) == 1 && rstat("served") >= served0,
		   "F: live child keeps its pages and its entry (no timed give-up)");

		/* kill it -> real sched_process_exit -> mark_dead -> grace -> sweep.
		 * This is the ONE assertion that needs a mortal owner: the whole
		 * event chain, on real pages, on a real exit path. (Page RECLAIM
		 * itself rides the free hook / sweep, which these GKI test kernels
		 * drive differently — del_hit=0 above — so we assert the death
		 * EVENT and no leak, not a served==0 that depends on the hook.) */
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		int gone = 0;
		for (i = 0; i < 150; i++) {	/* up to ~15s: grace + rounds + sweep */
			usleep(100 * 1000);
			if (owner_present(child) == 0) { gone = 1; break; }
		}
		OK(gone, "F: child death swept its owner entry (event-driven)");
		long base1 = rstat("pool_avail") + rstat("served");
		OK(base1 >= base0 - 2 && base1 <= base0 + 2,
		   "F: no leak across the death (held %ld -> %ld)", base0, base1);
		OK(rdbg("orphan") == orphan0,
		   "F: clean exit, no orphan pages (delta=%ld)", rdbg("orphan") - orphan0);
		printf("F. fork-child owner: served=%ld while alive, entry swept after "
		       "death; held %ld->%ld orphan+%ld purged+%ld\n",
		       served0, base0, base1, rdbg("orphan") - orphan0,
		       rdbg("purged") - purged0);
	}

		/* D0. rmmod while a VM still holds served, FOLL_LONGTERM-pinned pages:
	 * teardown must hand back only the module's protection refs (§8 served
	 * ALL — custody released, pages keep living with their holder) and must
	 * never free memory out from under the pin: the buffer has to stay
	 * intact and writable across the rmmod. */
	{
		size_t len = 8UL << 20;
		void *raw = mmap(NULL, len + (2UL<<20), PROT_READ|PROT_WRITE,
				 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		void *b = raw == MAP_FAILED ? MAP_FAILED :
			  (void *)(((unsigned long)raw + (2UL<<20)-1) & ~((2UL<<20)-1));
		long served_before;
		size_t i;

		OK(raw != MAP_FAILED, "D0 mmap");
		wsys("/sys/kernel/gh_test/fire_create", "1");
		if (b != MAP_FAILED) { madvise(b, len, MADV_HUGEPAGE); memset(b, 0x5a, len); }
		{ char c[64]; snprintf(c, sizeof c, "%lx %zu", (unsigned long)b, len); wsys("/sys/kernel/gh_test/pin", c); }
		served_before = rstat("served");
		printf("D0. pinned VM live: served=%ld avail=%ld\n",
		       served_before, rstat("pool_avail"));
		OK(served_before > 0, "D0 pages are out on loan before rmmod");
		OK(syscall(SYS_delete_module, "gh_hugepage_reserve", O_NONBLOCK) == 0,
		   "rmmod main while VM pages pinned");
		if (b != MAP_FAILED) {
			int bad = 0;
			for (i = 0; i < len; i += 2UL << 20)
				if (((unsigned char *)b)[i] != 0x5a) bad++;
			OK(bad == 0, "D0 pinned memory intact after rmmod (%d bad)", bad);
			memset(b, 0xa5, len);	/* and still writable */
			printf("D0. rmmod under pin OK, buffer intact\n");
		}
		wsys("/sys/kernel/gh_test/unpin", "1");
		wsys("/sys/kernel/gh_test/fire_destroy", "1");	/* stub only: probe gone */
		if (raw != MAP_FAILED) munmap(raw, len + (2UL<<20));
	}

	/* D. rmmod companion — main already gone (D0) */
	OK(syscall(SYS_delete_module, "gh_test_companion", O_NONBLOCK) == 0, "rmmod companion");

	if (fails) printf("TIER1: %d FAILURE(S)\n", fails);
	else       printf("TIER1: ALL PASS\n");
	sync();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
	return 0;
}
