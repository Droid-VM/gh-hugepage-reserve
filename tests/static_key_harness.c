/* Exercise the production static-key reader without a kernel or symbol exports. */
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>

struct static_key { struct { int counter; } enabled; };
#define atomic_read(value) ((value)->counter)
#include "../parts/gh_static_key.h"

int main(void)
{
	static const struct { int count; bool want; } cases[] = {
		{ 0, false }, { 1, true }, { 2, true }, { INT_MAX, true },
#ifdef CONFIG_JUMP_LABEL
		{ -1, true }, { INT_MIN, true },
#else
		{ -1, false }, { INT_MIN, false },
#endif
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct static_key key = { .enabled = { .counter = cases[i].count } };
		if (gh_static_key_enabled(&key) != cases[i].want) {
			fprintf(stderr, "FAIL: static key count %d expected enabled=%d\n",
				cases[i].count, cases[i].want);
			return 1;
		}
	}
	puts("PASS: static-key state, including negative transition counts");
	return 0;
}
