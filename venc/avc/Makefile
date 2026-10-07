obj-m := amvenc_avc.o
amvenc_avc-y := encoder.o compat.o
# see ../Makefile for why ~/kbuild
KDIR ?= $(HOME)/kbuild

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
