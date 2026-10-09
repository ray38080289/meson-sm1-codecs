# Build the three kernel modules and the venc CLI against the running kernel.
#   make [KDIR=<kernel build dir>]
KDIR ?= /lib/modules/$(shell uname -r)/build
MODULES := vdec venc-h264 venc-hevc

all: $(MODULES) venc-h264/venc

$(MODULES):
	$(MAKE) -C $(KDIR) M=$(CURDIR)/$@ modules

venc-h264/venc: venc-h264/venc.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	for m in $(MODULES); do $(MAKE) -C $(KDIR) M=$(CURDIR)/$$m clean; done
	rm -f venc-h264/venc

.PHONY: all clean $(MODULES)
