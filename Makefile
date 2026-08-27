# gh_hugepage_reserve — top-level build.
#   make test    — build & run the userspace mock harness (CI; no kernel needed)
#   make module  — build the kernel .ko (needs KDIR=/path/to/kernel/build)
.PHONY: test module clean
test:
	$(MAKE) -C tests test
clean:
	$(MAKE) -C tests clean
	rm -f *.o *.ko *.mod* .*.cmd modules.order Module.symvers

obj-m := gh_hugepage_reserve.o
# unity build: 'public' api is file-scope, cross-part calls have no separate
# prototypes; these warnings are noise here (and -Werror would fail on them).
ccflags-y += -Wno-missing-prototypes -Wno-unused-function
KDIR ?= /lib/modules/$(shell uname -r)/build
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
