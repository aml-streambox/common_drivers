obj-y	+=	common/
obj-y	+=	frame_provider/
obj-y	+=	stream_input/
obj-y	+=	media_sync/
obj-$(CONFIG_AMLOGIC_MEDIA_V4L_DEC)	+=	amvdec_ports/

subdir-ccflags-y += -include $(srctree)/$(COMMON_DRIVERS_DIR)/include/linux/amlogic/aml_kernel_compat.h
subdir-ccflags-y += -Wno-error=incompatible-pointer-types
subdir-ccflags-y += -Wno-error=missing-prototypes -Wno-frame-larger-than
