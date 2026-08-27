// SPDX-License-Identifier: GPL-2.0
/*
 * Tier-1 userspace exerciser. Drives the main module through the companion:
 *   1. fire a "VM create" so the pool starts tracking this pid as an owner
 *   2. allocate guest-RAM-like memory (movable order-9 THP via memfd) so the
 *      main module's serve kretprobe replaces the buddy pages with pool pages
 *   3. optionally ask the companion to FOLL_LONGTERM-pin it (VM "alive")
 *   4. read /sys/module/gh_hugepage_reserve/parameters/refill_stat and assert
 *
 * Success/failure is judged from the module's own counters — the scaffold does
 * not need precise pfn control, only to make the interesting paths run.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/memfd.h>

#define GB_SYS  "/sys/kernel/gh_test/"
#define MOD_SYS "/sys/module/gh_hugepage_reserve/parameters/"

static int wsys(const char *dir, const char *f, const char *val)
{
	char p[256]; int fd, r;
	snprintf(p, sizeof p, "%s%s", dir, f);
	fd = open(p, O_WRONLY);
	if (fd < 0) { perror(p); return -1; }
	r = write(fd, val, strlen(val));
	close(fd);
	return r < 0 ? -1 : 0;
}
static long rstat(const char *key)
{
	FILE *fp = fopen(MOD_SYS "refill_stat", "r");
	char line[256]; long v = -1;
	if (!fp) { perror("refill_stat"); return -1; }
	while (fgets(line, sizeof line, fp)) {
		char *eq = strchr(line, '=');
		if (eq && !strncmp(line, key, eq - line) && (size_t)(eq - line) == strlen(key))
			v = atol(eq + 1);
	}
	fclose(fp);
	return v;
}

int main(int argc, char **argv)
{
	size_t len = 64UL << 20;			/* 64MB of "guest RAM" */
	void *buf;
	int fd;

	/* 1. become a tracked VM owner */
	if (wsys(GB_SYS, "fire_create", "1")) return 2;

	/* 2. movable THP-backed memory; touch it so pages fault in as order-9 */
	fd = memfd_create("guest_ram", 0);
	if (fd < 0 || ftruncate(fd, len)) { perror("memfd"); return 2; }
	buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 2; }
	madvise(buf, len, MADV_HUGEPAGE);
	memset(buf, 1, len);				/* fault in -> serve kretprobe */

	printf("after serve: served=%ld total_served=%ld avail=%ld\n",
	       rstat("served"), rstat("total_served"), rstat("pool_avail"));

	/* 3. pin FOLL_LONGTERM (VM alive) then fire destroy -> stuck-pin path */
	{
		char cmd[64];
		snprintf(cmd, sizeof cmd, "%lx %zu", (unsigned long)buf, len);
		wsys(GB_SYS, "pin", cmd);
	}
	wsys(GB_SYS, "fire_destroy", "1");
	sleep(2);
	printf("VM down, still pinned: served=%ld active_vms=%ld stop=%s\n",
	       rstat("served"), rstat("active_vms"), "(see refill_stat)");

	/* 4. unpin -> the give-up/return path completes over the release rounds */
	wsys(GB_SYS, "unpin", "1");
	sleep(12);					/* > GRACE + a few rounds */
	printf("after unpin: served=%ld avail=%ld total_refilled=%ld\n",
	       rstat("served"), rstat("pool_avail"), rstat("total_refilled"));

	munmap(buf, len); close(fd);
	return rstat("served") == 0 ? 0 : 1;		/* all pages accounted */
}
