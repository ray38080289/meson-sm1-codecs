obj-m := hk1_venc.o
# ~/kbuild = copy of linux-headers-6.18.38-ophub with modpost rebuilt for bookworm
# glibc and -fmin-function-alignment swapped for gcc 12 (kernel was built by gcc 15).
KDIR ?= $(HOME)/kbuild

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
