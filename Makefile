# SPDX-License-Identifier: GPL-2.0-only

obj-m := g502-drv.o
g502-drv-y := g502.o
ccflags-y += -I$(M)/include -Werror -Wall -O2 -c -D__KERNEL__ \
				-Wno-error=unused-function -Wno-error=comment -DMODULE -DDEBUG

all:
	$(MAKE) -C /lib/modules/$$(uname -r)/build M=$(CURDIR) modules

clean:
	$(MAKE) -C /lib/modules/$$(uname -r)/build M=$(CURDIR) clean
