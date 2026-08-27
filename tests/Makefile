CFLAGS ?= -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined
harness: harness.c shim.h mock_kapi.c.inc ../parts/gh_defs.h ../parts/gh_owner.c.inc ../parts/gh_pool.c.inc ../parts/gh_release.c.inc ../parts/gh_adjust.c.inc
	$(CC) $(CFLAGS) -o $@ harness.c
race: race_harness.c shim.h mock_kapi.c.inc ../parts/gh_defs.h ../parts/gh_owner.c.inc ../parts/gh_pool.c.inc ../parts/gh_release.c.inc ../parts/gh_adjust.c.inc
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=thread -o $@ race_harness.c -lpthread
test: harness race
	./harness
	TSAN_OPTIONS=halt_on_error=0 ./race
clean:
	rm -f harness harness_ng race
.PHONY: test clean
