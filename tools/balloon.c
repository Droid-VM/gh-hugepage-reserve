/*
 * balloon - anon-memory pressure instrument for the CMA reservoir probe.
 * Plain CLI, no daemon, no boot hook, NO VERDICT: it applies pressure,
 * measures, prints, exits. The caller (the DroidVM app) owns both the
 * judgment (compare cma_diff_kb against held_mb) and the safety policy.
 *
 * It allocates and TOUCHES anon memory in 100MB steps - memset is the point:
 * malloc alone takes no physical pages, faulting them in walks the exact
 * do_anonymous_page path real app memory takes, vendor gfp tagging included.
 * That is the allocation class whose CMA eligibility is being measured, and
 * why this must be a userspace tool.
 *
 * Stop condition: MemAvailable < floor_mb (argv[1], default 1536), or
 * allocation failure. NOTE for the caller: on a kernel that does NOT let
 * anon consume CMA, CmaFree keeps counting into MemAvailable while being
 * unusable - the floor can fire very late or not at all there, so the caller
 * should supervise (timeout / watch meminfo / kill) rather than trust the
 * floor alone on unknown vendors.
 *
 * Usage:   balloon [floor_mb=1536]
 * stdout:  cma_before_kb= / cma_after_kb= / cma_diff_kb= / held_mb= /
 *          mem_avail_before_kb= / mem_avail_after_kb= / stop_reason=
 * exit:    0 = measured and printed, 2 = /proc/meminfo unreadable
 *
 * Memory is released by process exit. Build (static, for the module package):
 *   aarch64-linux-gnu-gcc -static -O2 -o balloon balloon.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STEP_MB	100L

static long meminfo_kb(const char *key)
{
	char line[256];
	long val = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, strlen(key))) {
			sscanf(line + strlen(key), "%ld", &val);
			break;
		}
	}
	fclose(f);
	return val;
}

int main(int argc, char **argv)
{
	long floor_mb = argc > 1 ? atol(argv[1]) : 1536;
	long cma_before, cma_after, avail_before, got = 0;
	const char *stop = "floor";

	cma_before = meminfo_kb("CmaFree:");
	avail_before = meminfo_kb("MemAvailable:");
	if (cma_before < 0 || avail_before < 0)
		return 2;

	for (;;) {
		char *p;

		if (meminfo_kb("MemAvailable:") / 1024 < floor_mb)
			break;
		p = malloc((size_t)STEP_MB << 20);
		if (!p) {
			stop = "alloc_fail";
			break;
		}
		memset(p, 0x5a, (size_t)STEP_MB << 20);
		got += STEP_MB;
	}

	cma_after = meminfo_kb("CmaFree:");
	printf("cma_before_kb=%ld\n", cma_before);
	printf("cma_after_kb=%ld\n", cma_after);
	printf("cma_diff_kb=%ld\n", cma_before - cma_after);
	printf("held_mb=%ld\n", got);
	printf("mem_avail_before_kb=%ld\n", avail_before);
	printf("mem_avail_after_kb=%ld\n", meminfo_kb("MemAvailable:"));
	printf("stop_reason=%s\n", stop);
	return 0;
}
