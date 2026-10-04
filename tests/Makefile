CFLAGS ?= -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined
harness: harness.c shim.h mock_kapi.c.inc ../parts/gh_defs.h ../parts/gh_owner.c.inc ../parts/gh_pool.c.inc ../parts/gh_release.c.inc ../parts/gh_adjust.c.inc
	$(CC) $(CFLAGS) -o $@ harness.c
race: race_harness.c shim.h mock_kapi.c.inc ../parts/gh_defs.h ../parts/gh_owner.c.inc ../parts/gh_pool.c.inc ../parts/gh_release.c.inc ../parts/gh_adjust.c.inc
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=thread -o $@ race_harness.c -lpthread
static_key: static_key_harness.c ../parts/gh_static_key.h
	$(CC) $(CFLAGS) -DCONFIG_JUMP_LABEL -o $@ static_key_harness.c
static_key_no_jump: static_key_harness.c ../parts/gh_static_key.h
	$(CC) $(CFLAGS) -o $@ static_key_harness.c
test: harness race static_key static_key_no_jump
	./harness
	./static_key
	./static_key_no_jump
	TSAN_OPTIONS=halt_on_error=0 ./race
clean:
	rm -f harness harness_ng race static_key static_key_no_jump
.PHONY: test clean
