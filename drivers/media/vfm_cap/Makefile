# SPDX-License-Identifier: GPL-2.0+
#
# Makefile for vfm_cap out-of-tree kernel module
#

MODULE_NAME = vfm_cap

obj-m += $(MODULE_NAME).o

# If the module grows to multiple files, use:
# $(MODULE_NAME)-objs := vfm_cap_main.o vfm_cap_v4l2.o vfm_cap_vfm.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
