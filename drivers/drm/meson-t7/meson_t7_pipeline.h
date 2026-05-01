/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_PIPELINE_H__
#define __MESON_T7_PIPELINE_H__

#include <linux/types.h>

#define MESON_T7_GFCD_ODD_SIZE	0

struct meson_t7_vpu_pipeline {
	u8 osd_version;
	u32 osd_axi_sel;
	u32 num_osds;
	u32 num_video;
	u32 num_postblend;
};

#endif /* __MESON_T7_PIPELINE_H__ */
