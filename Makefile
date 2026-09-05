# SPDX-License-Identifier: GPL-2.0
obj-m := hd60s.o
hd60s-y := hd60s-core.o hd60s-video.o hd60s-audio.o hd60s-parse.o \
	   hd60s-rev4.o hd60s-mstar.o hd60s-modes.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
