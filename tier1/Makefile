# Tier-1 QEMU integration rig. Needs a kernel build tree (KDIR).
KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m := gh_test_companion.o

modules exerciser: ;
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
exerciser: exerciser.c
	$(CC) -O2 -Wall -o exerciser exerciser.c
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || true
	rm -f exerciser
.PHONY: modules exerciser clean
