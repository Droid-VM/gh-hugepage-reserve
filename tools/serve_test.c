/*
 * serve_test - exercise the module's serve + free-hook reclaim loop WITHOUT a
 * real VM, by doing exactly the three syscall-level things crosvm does that
 * the module hooks:
 *
 *   1. ioctl(GH_CREATE_VM) on /dev/gunyah  -> VM-creation kprobe tracks our mm
 *      (fires on function ENTRY, so even a driver error still arms tracking;
 *      a successful create also gives us a vm fd whose close fires the
 *      VM-destroy kprobe at the end).
 *   2. touch 2MB-aligned offsets of a MADV_HUGEPAGE'd memfd -> order-9 shmem
 *      faults in OUR (tracked) process -> the __alloc_pages kretprobe serves
 *      pool pages.
 *   3. fallocate(PUNCH_HOLE) whole 2MB chunks -> the huge folio frees ->
 *      android_vh_free_one_page_bypass reclaims it back into the pool.
 *
 * Takes and returns run in SHUFFLED chunk order over several rounds, then the
 * vm fd is closed (destroy detection -> pcp drain + scavenge) and the memfd
 * torn down (the "VM exit" free burst). After each phase the module's own
 * counters are printed, so the pool's self-recomposition is visible inline:
 * pool_avail should fall by the takes, rise back on the holes, and land at
 * its starting value at the end with served=0 and take_fail/cma_leak still 0.
 *
 * Usage: serve_test [size_mb=256] [rounds=3] [seed=1]
 * Build: aarch64-linux-gnu-gcc -static -O2 -o serve_test serve_test.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#define GH_CREATE_VM	_IO('G', 0x0)
#define HP		(2UL << 20)
#define PARAMS		"/sys/module/gh_hugepage_reserve/parameters/"

static long kv(const char *file, const char *key)
{
	char buf[2048], *p;
	long val = -1;
	int fd = open(file, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, key);
	if (p)
		val = atol(p + strlen(key));
	return val;
}

static void stats(const char *tag)
{
	printf("[%-12s] pool_avail=%ld served=%ld total_served=%ld total_refilled=%ld take_fail=%ld cma_leak=%ld pool_cma=%ld cma_able=%ld\n",
	       tag,
	       kv(PARAMS "refill_stat", "pool_avail="),
	       kv(PARAMS "refill_stat", "served="),
	       kv(PARAMS "refill_stat", "total_served="),
	       kv(PARAMS "refill_stat", "total_refilled="),
	       kv(PARAMS "reclaim_debug", "take_fail="),
	       kv(PARAMS "reclaim_debug", "cma_leak="),
	       kv(PARAMS "refill_stat", "pool_cma="),
	       kv(PARAMS "refill_stat", "pool_avail_cma_able="));
	fflush(stdout);
}

static unsigned long rng_state;
static unsigned long rng(void)
{
	rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
	return rng_state >> 33;
}

static void shuffle(int *a, int n)
{
	int i, j, t;

	for (i = n - 1; i > 0; i--) {
		j = (int)(rng() % (unsigned long)(i + 1));
		t = a[i];
		a[i] = a[j];
		a[j] = t;
	}
}

int main(int argc, char **argv)
{
	long size_mb = argc > 1 ? atol(argv[1]) : 256;
	int rounds = argc > 2 ? atoi(argv[2]) : 3;
	int nchunks = (int)(size_mb / 2), r, i, gfd, vmfd = -1, memfd;
	size_t size = (size_t)nchunks * HP;
	char *present, *base, *raw;
	int *perm;

	rng_state = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;

	stats("baseline");

	/* 1. become a tracked "VM owner" */
	gfd = open("/dev/gunyah", O_RDWR | O_CLOEXEC);
	if (gfd < 0) {
		printf("open /dev/gunyah: %s\n", strerror(errno));
		return 2;
	}
	vmfd = ioctl(gfd, GH_CREATE_VM, 0);
	printf("GH_CREATE_VM -> %d%s%s (tracking armed either way)\n", vmfd,
	       vmfd < 0 ? " errno=" : "", vmfd < 0 ? strerror(errno) : "");
	usleep(300 * 1000);
	stats("tracked");

	/* 2. guest-RAM stand-in: memfd + MADV_HUGEPAGE, 2MB-aligned mapping */
	memfd = memfd_create("serve_test", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, (off_t)size)) {
		printf("memfd: %s\n", strerror(errno));
		return 2;
	}
	raw = mmap(NULL, size + HP, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (raw == MAP_FAILED) {
		printf("reserve mmap: %s\n", strerror(errno));
		return 2;
	}
	base = (char *)(((unsigned long)raw + HP - 1) & ~(HP - 1));
	base = mmap(base, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, memfd, 0);
	if (base == MAP_FAILED) {
		printf("file mmap: %s\n", strerror(errno));
		return 2;
	}
	madvise(base, size, MADV_HUGEPAGE);

	perm = calloc((size_t)nchunks, sizeof(int));
	present = calloc((size_t)nchunks, 1);
	for (i = 0; i < nchunks; i++)
		perm[i] = i;

	for (r = 0; r < rounds; r++) {
		char tag[32];
		int holes = 0;

		/* take: fault every absent chunk, shuffled order */
		shuffle(perm, nchunks);
		for (i = 0; i < nchunks; i++) {
			if (present[perm[i]])
				continue;
			*(volatile char *)(base + (size_t)perm[i] * HP) = 1;
			present[perm[i]] = 1;
		}
		usleep(500 * 1000);
		snprintf(tag, sizeof(tag), "r%d take", r);
		stats(tag);

		/* return: punch ~half the chunks whole, shuffled order */
		shuffle(perm, nchunks);
		for (i = 0; i < nchunks && holes < nchunks / 2; i++) {
			if (!present[perm[i]])
				continue;
			if (fallocate(memfd,
				      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				      (off_t)perm[i] * (off_t)HP, (off_t)HP)) {
				printf("punch: %s\n", strerror(errno));
				break;
			}
			present[perm[i]] = 0;
			holes++;
		}
		sleep(2);	/* give pcp-parked frees a moment to drain */
		snprintf(tag, sizeof(tag), "r%d hole*%d", r, holes);
		stats(tag);
	}

	/* 3. "VM shutdown": close the vm fd first (destroy detection: grace
	 * window + pcp drain + refill), then drop all remaining guest RAM. */
	if (vmfd >= 0)
		close(vmfd);
	close(gfd);
	usleep(300 * 1000);
	stats("vm closed");

	munmap(base, size);
	close(memfd);		/* frees every remaining huge folio */
	sleep(5);		/* PCP_DRAIN_DELAY(3s) + margin */
	stats("final");
	printf("expect: final pool_avail == baseline, served==0, take_fail==0, cma_leak==0\n");
	return 0;
}
