// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include "meson_vpu_pipeline.h"

#define T7_OSD_HDR2_BYPASS_CTRL			(BIT(16) | BIT(15) | BIT(14))
#define T7_OSD1_HDR_IN_SIZE			0x1a5a
#define T7_OSD1_HDR2_CTRL			0x38a0
#define T7_OSD1_HDR2_MATRIXI_COEF00_01		0x38a2
#define T7_OSD1_HDR2_MATRIXI_EN_CTRL		0x38db
#define T7_OSD1_HDR2_MATRIXO_EN_CTRL		0x38dc
#define T7_OSD3_HDR2_CTRL			0x5b50
#define T7_OSD3_HDR2_MATRIXI_COEF00_01		0x5b52
#define T7_OSD3_HDR2_MATRIXI_EN_CTRL		0x5b8b
#define T7_OSD3_HDR2_MATRIXO_EN_CTRL		0x5b8c

static void t7_osd_hdr_bypass(struct rdma_reg_ops *reg_ops, u32 hdr_ctrl,
				      u32 matrixi_base, u32 matrixi_en_ctrl,
				      u32 matrixo_en_ctrl)
{
	reg_ops->rdma_write_reg(matrixi_base + 0x0, 0x00ba0273);
	reg_ops->rdma_write_reg(matrixi_base + 0x1, 0x003f1f9a);
	reg_ops->rdma_write_reg(matrixi_base + 0x2, 0x1ea801c0);
	reg_ops->rdma_write_reg(matrixi_base + 0x3, 0x01c01e6a);
	reg_ops->rdma_write_reg(matrixi_base + 0x4, 0x00001fd8);
	reg_ops->rdma_write_reg(matrixi_base + 0x5, 0x0);
	reg_ops->rdma_write_reg(matrixi_base + 0x6, 0x0);
	reg_ops->rdma_write_reg(matrixi_base + 0x7, 0x0);
	reg_ops->rdma_write_reg(matrixi_base + 0x8, 0x00400200);
	reg_ops->rdma_write_reg(matrixi_base + 0x9, 0x00000200);
	reg_ops->rdma_write_reg(matrixi_base + 0xa, 0x0);
	reg_ops->rdma_write_reg(matrixi_base + 0xb, 0x0);
	reg_ops->rdma_write_reg(matrixi_base + 0x18, 0x0);
	reg_ops->rdma_write_reg(matrixi_en_ctrl, 0x1);
	reg_ops->rdma_write_reg(matrixo_en_ctrl, 0x0);
	reg_ops->rdma_write_reg(hdr_ctrl, T7_OSD_HDR2_BYPASS_CTRL);
}

static struct hdr_reg_s osd_hdr_reg[MESON_MAX_HDRS] = {
	{
		VPP_OSD1_IN_SIZE,
	},
	{
		VPP_OSD2_IN_SIZE,
	}
};

static struct hdr_reg_s t7_osd_hdr_reg[MESON_MAX_HDRS] = {
	{
		VPP_OSD1_IN_SIZE,//TO check
	},
	{
		T7_HDR2_IN_SIZE,
	}
};

static int hdr_check_state(struct meson_vpu_block *vblk,
			   struct meson_vpu_block_state *state,
		struct meson_vpu_pipeline_state *mvps)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);

	if (state->checked)
		return 0;

	state->checked = true;

	//vpu_block_check_input(vblk, state, mvps);

	MESON_DRM_BLOCK("%s check_state called.\n", hdr->base.name);
	return 0;
}

static void hdr_set_state(struct meson_vpu_block *vblk,
			  struct meson_vpu_block_state *state,
			  struct meson_vpu_block_state *old_state)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);
	//struct meson_vpu_hdr_state *hdr_state = to_hdr_state(state);

	MESON_DRM_BLOCK("%s set_state called.\n", hdr->base.name);
}

static void s7d_hdr_set_state(struct meson_vpu_block *vblk,
			  struct meson_vpu_block_state *state,
			  struct meson_vpu_block_state *old_state)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);
	struct meson_vpu_pipeline *pipeline = hdr->base.pipeline;
	struct hdr_reg_s *reg = hdr->reg;
	struct rdma_reg_ops *reg_ops = state->sub->reg_ops;
	struct meson_vpu_pipeline_state *mvps;
	u32 hsize, vsize;

	mvps = priv_to_pipeline_state(pipeline->obj.state);

	hsize = mvps->plane_info[vblk->index].src_w;
	vsize = mvps->plane_info[vblk->index].src_h;

	reg_ops->rdma_write_reg(reg->vpp_osd_in_size, hsize | (vsize << 16));
	MESON_DRM_BLOCK("%s set_state called.\n", hdr->base.name);
}

static void t7_hdr_set_state(struct meson_vpu_block *vblk,
			  struct meson_vpu_block_state *state,
			  struct meson_vpu_block_state *old_state)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);
	struct meson_vpu_pipeline *pipeline = hdr->base.pipeline;
	struct hdr_reg_s *reg = hdr->reg;
	struct rdma_reg_ops *reg_ops = state->sub->reg_ops;
	struct meson_vpu_pipeline_state *mvps;
	u32 hsize, vsize;

	mvps = priv_to_pipeline_state(pipeline->obj.state);

	if (vblk->index == HDR1_INDEX) {
		hsize = mvps->scaler_param[MESON_OSD1].output_width;
		vsize = mvps->scaler_param[MESON_OSD1].output_height;

		MESON_DRM_BLOCK("%s set_state,input size:%u,%u.\n",
				hdr->base.name, hsize, vsize);
		reg_ops->rdma_write_reg(reg->vpp_osd_in_size,
			hsize | (vsize << 16));
		reg_ops->rdma_write_reg(T7_OSD1_HDR_IN_SIZE,
			hsize | (vsize << 16));
		t7_osd_hdr_bypass(reg_ops, T7_OSD1_HDR2_CTRL,
			T7_OSD1_HDR2_MATRIXI_COEF00_01,
			T7_OSD1_HDR2_MATRIXI_EN_CTRL,
			T7_OSD1_HDR2_MATRIXO_EN_CTRL);
	} else if (vblk->index == HDR2_INDEX) {
		hsize = mvps->scaler_param[MESON_OSD3].output_width;
		vsize = mvps->scaler_param[MESON_OSD3].output_height;

		MESON_DRM_BLOCK("%s set_state,input size:%u,%u.\n", hdr->base.name, hsize, vsize);
		reg_ops->rdma_write_reg(reg->vpp_osd_in_size, hsize | (vsize << 16));
		t7_osd_hdr_bypass(reg_ops, T7_OSD3_HDR2_CTRL,
			T7_OSD3_HDR2_MATRIXI_COEF00_01,
			T7_OSD3_HDR2_MATRIXI_EN_CTRL,
			T7_OSD3_HDR2_MATRIXO_EN_CTRL);
	}
	MESON_DRM_BLOCK("%s set_state called.\n", hdr->base.name);
}

static void hdr_enable(struct meson_vpu_block *vblk,
		       struct meson_vpu_block_state *state)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);

	MESON_DRM_BLOCK("%s enable called.\n", hdr->base.name);
}

static void hdr_disable(struct meson_vpu_block *vblk,
			struct meson_vpu_block_state *state)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);

	MESON_DRM_BLOCK("%s disable called.\n", hdr->base.name);
}

static void hdr_init(struct meson_vpu_block *vblk)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);

	hdr->reg = &osd_hdr_reg[vblk->index];
	MESON_DRM_BLOCK("%s hw_init called.\n", hdr->base.name);
}

static void t7_hdr_init(struct meson_vpu_block *vblk)
{
	struct meson_vpu_hdr *hdr = to_hdr_block(vblk);

	hdr->reg = &t7_osd_hdr_reg[vblk->index];
	MESON_DRM_BLOCK("%s hw_init called.\n", hdr->base.name);
}

struct meson_vpu_block_ops hdr_ops = {
	.check_state = hdr_check_state,
	.update_state = hdr_set_state,
	.enable = hdr_enable,
	.disable = hdr_disable,
};

struct meson_vpu_block_ops s7d_hdr_ops = {
	.check_state = hdr_check_state,
	.update_state = s7d_hdr_set_state,
	.enable = hdr_enable,
	.disable = hdr_disable,
	.init = hdr_init,
};

struct meson_vpu_block_ops t7_hdr_ops = {
	.check_state = hdr_check_state,
	.update_state = t7_hdr_set_state,
	.enable = hdr_enable,
	.disable = hdr_disable,
	.init = t7_hdr_init,
};

static int db_check_state(struct meson_vpu_block *vblk,
			     struct meson_vpu_block_state *state,
		struct meson_vpu_pipeline_state *mvps)
{
	struct meson_vpu_db *mvd = to_db_block(vblk);

	if (state->checked)
		return 0;

	state->checked = true;

	//vpu_block_check_input(vblk, state, mvps);

	MESON_DRM_BLOCK("%s check_state called.\n", mvd->base.name);
	return 0;
}

static void db_set_state(struct meson_vpu_block *vblk,
			    struct meson_vpu_block_state *state,
			    struct meson_vpu_block_state *old_state)
{
	struct meson_vpu_db *mvd = to_db_block(vblk);

	MESON_DRM_BLOCK("%s set_state called.\n", mvd->base.name);
}

static void db_enable(struct meson_vpu_block *vblk,
			 struct meson_vpu_block_state *state)
{
	struct meson_vpu_db *mvd = to_db_block(vblk);

	MESON_DRM_BLOCK("%s enable called.\n", mvd->base.name);
}

static void db_disable(struct meson_vpu_block *vblk,
			  struct meson_vpu_block_state *state)
{
	struct meson_vpu_db *mvd = to_db_block(vblk);

	MESON_DRM_BLOCK("%s disable called.\n", mvd->base.name);
}

struct meson_vpu_block_ops db_ops = {
	.check_state = db_check_state,
	.update_state = db_set_state,
	.enable = db_enable,
	.disable = db_disable,
};
