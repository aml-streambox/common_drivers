// SPDX-License-Identifier: GPL-2.0+
/*
 * vfm_cap.c - VFM Capture Tee Module (Phase 2c - Zero-Copy)
 *
 * Inserts into the VFM chain as:
 *   vdin0 -> vfm_cap -> deinterlace -> amvideo
 *
 * Provides /dev/video_cap as a V4L2 capture device with:
 *   - True zero-copy DMA-buf export of vdin0's CMA frame buffers
 *   - Per-frame reference counting (display path + DMA-buf consumers)
 *   - V4L2 flow control (QBUF/DQBUF) with DMA-buf fd sideband
 *   - Transparent passthrough to display path
 *   - V4L2_EVENT_SOURCE_CHANGE for signal detection (Phase 2a)
 *   - 10-bit format awareness (Phase 2b)
 *
 * Zero-copy data flow:
 *   vdin0 ISR -> vfm_cap (wrap in cap_frame, pass through to display)
 *   deliver_work -> create DMA-buf from CMA phys addr -> attach to vb2 buf
 *   consumer: DQBUF -> ioctl(VFM_CAP_IOC_GET_DMABUF) -> import fd -> use -> QBUF
 *
 * Copyright (C) 2026 StreamBox
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>
#include <drm/drm_fourcc.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/media/codec_mm/codec_mm.h>

#include "vfm_cap.h"

/* ========== Module Parameters ========== */

static char *provider_name = "vdin0";
module_param(provider_name, charp, 0444);
MODULE_PARM_DESC(provider_name, "Upstream VFM provider name (default: vdin0)");

static char *receiver_name = "deinterlace";
module_param(receiver_name, charp, 0444);
MODULE_PARM_DESC(receiver_name, "Downstream VFM receiver name (default: deinterlace)");

unsigned int vfm_cap_max_consumers = VFM_CAP_MAX_CONSUMERS;
module_param_named(max_consumers, vfm_cap_max_consumers, uint, 0644);
MODULE_PARM_DESC(max_consumers, "Max simultaneous V4L2 consumers (default: 4)");

int vfm_cap_video_nr = -1;
module_param_named(video_nr, vfm_cap_video_nr, int, 0444);
MODULE_PARM_DESC(video_nr, "V4L2 video device number (-1=auto)");

unsigned int vfm_cap_debug;
module_param_named(debug, vfm_cap_debug, uint, 0644);
MODULE_PARM_DESC(debug, "Debug level (0=off, 1=info, 2=verbose)");

/* ========== Global device instance ========== */

static struct vfm_cap_dev *g_vfm_cap_dev;

/* Forward declaration - defined after provider/receiver ops */
static const struct vframe_operations_s vfm_cap_vf_provider_ops;

/* ========== Frame Pool Management ========== */

/**
 * frame_pool_init() - Initialize the frame pool
 *
 * Sets all slots to unused.
 */
static void frame_pool_init(struct vfm_cap_dev *dev)
{
	int i;

	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		dev->frame_pool[i].vf = NULL;
		dev->frame_pool[i].index = i;
		dev->frame_pool[i].in_use = false;
		dev->frame_pool[i].phy_addr = 0;
		dev->frame_pool[i].buf_size = 0;
		atomic_set(&dev->frame_pool[i].refcount, 0);
		INIT_LIST_HEAD(&dev->frame_pool[i].list);
		INIT_LIST_HEAD(&dev->frame_pool[i].pending_node);
	}
}

/**
 * frame_pool_recycle_all() - Recycle all in-use frames back to vdin0
 *
 * Must be called with ready_lock held.
 * Properly returns all held vframes to upstream before pool reinit.
 * Also clears prev_v4l2_frame (dropping its held reference).
 */
static void frame_pool_recycle_all(struct vfm_cap_dev *dev)
{
	int i;

	/* Clear the held-back V4L2 frame pointer first — the loop below
	 * will recycle the underlying vframe along with all others.
	 * No need to decrement refcount separately since we're about to
	 * force-reset all refcounts to 0. */
	dev->prev_v4l2_frame = NULL;

	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		struct cap_frame *frame = &dev->frame_pool[i];

		if (!frame->in_use)
			continue;

		if (frame->vf) {
			vf_put(frame->vf, dev->recv_name);
			vf_notify_provider(dev->recv_name,
					   VFRAME_EVENT_RECEIVER_PUT, NULL);
			frame->vf = NULL;
		}
		frame->in_use = false;
		atomic_set(&frame->refcount, 0);
		list_del_init(&frame->list);
		list_del_init(&frame->pending_node);
	}

	INIT_LIST_HEAD(&dev->ready_list);
	INIT_LIST_HEAD(&dev->pending_list);
}

/**
 * frame_pool_alloc() - Get a free cap_frame slot
 *
 * Must be called with ready_lock held.
 * Returns NULL if pool is exhausted.
 */
static struct cap_frame *frame_pool_alloc(struct vfm_cap_dev *dev)
{
	int i;

	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		if (!dev->frame_pool[i].in_use) {
			dev->frame_pool[i].in_use = true;
			return &dev->frame_pool[i];
		}
	}
	return NULL;
}

/**
 * frame_release() - Release a cap_frame back to vdin0 when refcount hits 0
 *
 * Called when atomic_dec_and_test(&frame->refcount) succeeds.
 * Puts the vframe back to upstream and marks the slot free.
 */
static void frame_release(struct vfm_cap_dev *dev, struct cap_frame *frame)
{
	if (frame->vf) {
		vfm_cap_dbg(1, "recycling vframe idx=%u slot=%d to %s\n",
			     frame->vf->index, frame->index, dev->recv_name);
		vf_put(frame->vf, dev->recv_name);
		vf_notify_provider(dev->recv_name,
				   VFRAME_EVENT_RECEIVER_PUT, NULL);
		frame->vf = NULL;
	}
	frame->in_use = false;
}

/**
 * frame_put() - Decrement refcount; release if zero
 *
 * Safe to call from any context. Uses the ready_lock to protect pool state.
 */
static void frame_put(struct vfm_cap_dev *dev, struct cap_frame *frame)
{
	unsigned long flags;

	if (atomic_dec_and_test(&frame->refcount)) {
		spin_lock_irqsave(&dev->ready_lock, flags);
		list_del_init(&frame->list);
		frame_release(dev, frame);
		spin_unlock_irqrestore(&dev->ready_lock, flags);
	}
}

/* ========== DMA-buf Export Ops (Zero-Copy) ========== */

/*
 * These implement struct dma_buf_ops for exporting vdin0's CMA frame
 * buffers directly to userspace consumers as DMA-buf file descriptors.
 *
 * Pattern follows Amlogic's dmabuf_manage.c:
 *   phys_to_page(paddr) -> sg_set_page() -> dma_buf_export()
 *
 * The CMA memory is identity-mapped (no IOMMU on A311D2), so
 * sg_dma_address is set directly to the physical address.
 */

static int vfm_cap_dmabuf_attach(struct dma_buf *dbuf,
				 struct dma_buf_attachment *attachment)
{
	struct vfm_cap_dmabuf *priv = dbuf->priv;
	struct vfm_cap_dmabuf_attach *attach;
	struct sg_table *sgt;
	struct page *page;
	int ret;

	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		return -ENOMEM;

	sgt = &attach->sgt;
	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(attach);
		return ret;
	}

	page = phys_to_page(priv->paddr);
	sg_set_page(sgt->sgl, page, PAGE_ALIGN(priv->size), 0);

	attach->dma_dir = DMA_NONE;
	attachment->priv = attach;
	return 0;
}

static void vfm_cap_dmabuf_detach(struct dma_buf *dbuf,
				  struct dma_buf_attachment *attachment)
{
	struct vfm_cap_dmabuf_attach *attach = attachment->priv;

	if (!attach)
		return;

	sg_free_table(&attach->sgt);
	kfree(attach);
	attachment->priv = NULL;
}

static struct sg_table *vfm_cap_dmabuf_map(struct dma_buf_attachment *attachment,
					   enum dma_data_direction dma_dir)
{
	struct vfm_cap_dmabuf_attach *attach = attachment->priv;
	struct vfm_cap_dmabuf *priv = attachment->dmabuf->priv;
	struct sg_table *sgt = &attach->sgt;

	if (attach->dma_dir == dma_dir)
		return sgt;

	/*
	 * Identity mapping: CMA physical address IS the DMA address.
	 * No IOMMU on A311D2, so skip dma_map_sg and set directly.
	 */
	sgt->sgl->dma_address = priv->paddr;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
	sgt->sgl->dma_length = PAGE_ALIGN(priv->size);
#else
	sgt->sgl->length = PAGE_ALIGN(priv->size);
#endif

	attach->dma_dir = dma_dir;
	return sgt;
}

static void vfm_cap_dmabuf_unmap(struct dma_buf_attachment *attachment,
				 struct sg_table *sgt,
				 enum dma_data_direction dma_dir)
{
	/* No-op: identity-mapped, no dma_map_sg to undo */
}

static void vfm_cap_dmabuf_release(struct dma_buf *dbuf)
{
	struct vfm_cap_dmabuf *priv = dbuf->priv;

	if (priv) {
		/*
		 * Release the cap_frame reference held by this DMA-buf.
		 * If this was the last reference, the vframe is recycled
		 * back to vdin0.
		 */
		if (priv->frame && priv->dev) {
			vfm_cap_dbg(2, "dmabuf release: frame %u refcount=%d\n",
				     priv->frame->index,
				     atomic_read(&priv->frame->refcount));
			frame_put(priv->dev, priv->frame);
		}
		kfree(priv);
	}
}

static int vfm_cap_dmabuf_mmap(struct dma_buf *dbuf,
			       struct vm_area_struct *vma)
{
	struct vfm_cap_dmabuf *priv = dbuf->priv;

	if (!priv || !priv->paddr)
		return -EINVAL;

	/*
	 * Map the CMA physical pages into userspace with write-combine.
	 * This is used for debug/test only (e.g., demo program reads
	 * one frame). Normal consumers use the DMA-buf fd for hardware
	 * import (Vulkan, encoder, etc.) and never mmap.
	 */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(phys_to_page(priv->paddr)),
			       PAGE_ALIGN(priv->size),
			       vma->vm_page_prot);
}

static const struct dma_buf_ops vfm_cap_dmabuf_ops = {
	.attach       = vfm_cap_dmabuf_attach,
	.detach       = vfm_cap_dmabuf_detach,
	.map_dma_buf  = vfm_cap_dmabuf_map,
	.unmap_dma_buf = vfm_cap_dmabuf_unmap,
	.release      = vfm_cap_dmabuf_release,
	.mmap         = vfm_cap_dmabuf_mmap,
};

/* ========== AFBC Repack DMA-buf Ops (Zero-Copy Layout Fix) ========== */

/*
 * vdin AFBC CMA layout:  [body][header][table]
 * Standard ARM AFBC:     [header][body]
 *
 * These ops export a DMA-buf with a 2-entry scatter-gather list that
 * presents the header region first and body region second.  The Mali
 * Bifrost GPU MMU maps the SG entries into contiguous GPU virtual
 * address space, so the GPU sees standard [header][body] layout.
 *
 * Zero-copy: no pixel data is copied or moved.  We just re-order
 * the physical page references in the SG table.
 */

static int vfm_cap_afbc_dmabuf_attach(struct dma_buf *dbuf,
				       struct dma_buf_attachment *attachment)
{
	struct vfm_cap_afbc_dmabuf *priv = dbuf->priv;
	struct vfm_cap_dmabuf_attach *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
	int ret;

	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		return -ENOMEM;

	sgt = &attach->sgt;
	ret = sg_alloc_table(sgt, 2, GFP_KERNEL);
	if (ret) {
		kfree(attach);
		return ret;
	}

	/* SG entry 0: header (comes first in standard AFBC) */
	sg = sgt->sgl;
	sg_set_page(sg, phys_to_page(priv->head_paddr),
		    PAGE_ALIGN(priv->head_size), 0);

	/* SG entry 1: body (comes second in standard AFBC) */
	sg = sg_next(sg);
	sg_set_page(sg, phys_to_page(priv->body_paddr),
		    PAGE_ALIGN(priv->body_size), 0);

	attach->dma_dir = DMA_NONE;
	attachment->priv = attach;
	return 0;
}

static struct sg_table *vfm_cap_afbc_dmabuf_map(
				struct dma_buf_attachment *attachment,
				enum dma_data_direction dma_dir)
{
	struct vfm_cap_dmabuf_attach *attach = attachment->priv;
	struct vfm_cap_afbc_dmabuf *priv = attachment->dmabuf->priv;
	struct sg_table *sgt = &attach->sgt;
	struct scatterlist *sg;

	if (attach->dma_dir == dma_dir)
		return sgt;

	/* Identity mapping: CMA physical address IS the DMA address */
	sg = sgt->sgl;
	sg_dma_address(sg) = priv->head_paddr;
	sg_dma_len(sg) = PAGE_ALIGN(priv->head_size);

	sg = sg_next(sg);
	sg_dma_address(sg) = priv->body_paddr;
	sg_dma_len(sg) = PAGE_ALIGN(priv->body_size);

	sgt->nents = 2;

	attach->dma_dir = dma_dir;
	return sgt;
}

static void vfm_cap_afbc_dmabuf_release(struct dma_buf *dbuf)
{
	struct vfm_cap_afbc_dmabuf *priv = dbuf->priv;

	if (priv) {
		if (priv->frame && priv->dev) {
			vfm_cap_dbg(2, "afbc dmabuf release: frame %u refcount=%d\n",
				     priv->frame->index,
				     atomic_read(&priv->frame->refcount));
			frame_put(priv->dev, priv->frame);
		}
		kfree(priv);
	}
}

static int vfm_cap_afbc_dmabuf_mmap(struct dma_buf *dbuf,
				     struct vm_area_struct *vma)
{
	/* AFBC repacked buffers are not intended for CPU mmap.
	 * Consumers should use GPU import only. */
	return -ENODEV;
}

static const struct dma_buf_ops vfm_cap_afbc_dmabuf_ops = {
	.attach        = vfm_cap_afbc_dmabuf_attach,
	.detach        = vfm_cap_dmabuf_detach,  /* reuse: same sg_free logic */
	.map_dma_buf   = vfm_cap_afbc_dmabuf_map,
	.unmap_dma_buf = vfm_cap_dmabuf_unmap,   /* reuse: no-op */
	.release       = vfm_cap_afbc_dmabuf_release,
	.mmap          = vfm_cap_afbc_dmabuf_mmap,
};

/**
 * vfm_cap_export_frame_dmabuf() - Export a cap_frame as a DMA-buf
 *
 * For AFBC frames: creates a repacked DMA-buf with a 2-entry SG table
 * presenting [header][body] in standard ARM AFBC order (zero-copy).
 *
 * For linear frames: creates a single-entry DMA-buf pointing to the
 * CMA buffer as-is.
 *
 * Returns a dma_buf pointer, or ERR_PTR on failure.
 */
static struct dma_buf *vfm_cap_export_frame_dmabuf(struct vfm_cap_dev *dev,
					   struct cap_frame *frame)
{
	struct dma_buf *dbuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (!frame || !frame->vf)
		return ERR_PTR(-EINVAL);

	/*
	 * AFBC path: repack as [header][body] via 2-entry SG table.
	 * This makes the DMA-buf importable by Mali's Vulkan driver
	 * which expects standard ARM AFBC layout.
	 */
	if (frame->is_afbc && frame->comp_head_addr && frame->comp_body_addr &&
	    frame->comp_head_size > 0 && frame->comp_body_size > 0) {
		struct vfm_cap_afbc_dmabuf *apriv;

		apriv = kzalloc(sizeof(*apriv), GFP_KERNEL);
		if (!apriv)
			return ERR_PTR(-ENOMEM);

		apriv->head_paddr = frame->comp_head_addr;
		apriv->head_size = frame->comp_head_size;
		apriv->body_paddr = frame->comp_body_addr;
		apriv->body_size = frame->comp_body_size;
		apriv->frame = frame;
		apriv->dev = dev;

		exp_info.ops = &vfm_cap_afbc_dmabuf_ops;
		exp_info.size = PAGE_ALIGN(frame->comp_head_size) +
				PAGE_ALIGN(frame->comp_body_size);
		exp_info.flags = O_RDONLY | O_CLOEXEC;
		exp_info.priv = apriv;
		exp_info.exp_name = "vfm_cap_afbc";

		dbuf = dma_buf_export(&exp_info);
		if (IS_ERR(dbuf)) {
			kfree(apriv);
			return dbuf;
		}

		atomic_inc(&frame->refcount);

		vfm_cap_dbg(1,
			     "exported AFBC frame %u as repacked dmabuf "
			     "(head=%pa/%zu body=%pa/%zu total=%zu refcount=%d)\n",
			     frame->index,
			     &frame->comp_head_addr, frame->comp_head_size,
			     &frame->comp_body_addr, frame->comp_body_size,
			     (size_t)exp_info.size,
			     atomic_read(&frame->refcount));

		return dbuf;
	}

	/* Linear path: single contiguous CMA buffer */
	{
		struct vfm_cap_dmabuf *priv;

		if (!frame->phy_addr)
			return ERR_PTR(-EINVAL);

		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return ERR_PTR(-ENOMEM);

		priv->paddr = frame->phy_addr;
		priv->size = frame->buf_size;
		priv->frame = frame;
		priv->dev = dev;

		exp_info.ops = &vfm_cap_dmabuf_ops;
		exp_info.size = PAGE_ALIGN(frame->buf_size);
		exp_info.flags = O_RDONLY | O_CLOEXEC;
		exp_info.priv = priv;
		exp_info.exp_name = "vfm_cap";

		dbuf = dma_buf_export(&exp_info);
		if (IS_ERR(dbuf)) {
			kfree(priv);
			return dbuf;
		}

		atomic_inc(&frame->refcount);

		vfm_cap_dbg(2, "exported frame %u as dmabuf (paddr=0x%llx size=%zu refcount=%d)\n",
			     frame->index, (u64)frame->phy_addr, frame->buf_size,
			     atomic_read(&frame->refcount));

		return dbuf;
	}
}

static size_t vfm_cap_afbc_header_size(unsigned int width, unsigned int height)
{
	return PAGE_ALIGN((roundup(width, 64) * roundup(height, 64)) / 32);
}

static size_t vfm_cap_afbc_table_size(size_t body_size)
{
	return PAGE_ALIGN((body_size * 4) / PAGE_SIZE);
}

/* ========== Format Detection from vframe ========== */

/**
 * vfm_cap_bitdepth_y() - Extract Y-channel bit depth from vf->bitdepth
 *
 * Returns 8, 9, 10, or 12 based on the BITDEPTH_YMASK field.
 */
static unsigned int vfm_cap_bitdepth_y(u32 bitdepth)
{
	switch (bitdepth & BITDEPTH_YMASK) {
	case BITDEPTH_Y10:
		return 10;
	case BITDEPTH_Y12:
		return 12;
	case BITDEPTH_Y9:
		return 9;
	default:
		return 8;
	}
}

/**
 * vfm_cap_update_format() - Detect format from incoming vframe
 *
 * Called when we get a new frame. Updates the device format state
 * so V4L2 consumers can query it. Handles both 8-bit and 10-bit
 * Amlogic native formats.
 *
 * Returns true if format changed (for SOURCE_CHANGE event generation).
 */
static bool vfm_cap_update_format(struct vfm_cap_dev *dev,
				   struct vframe_s *vf)
{
	u32 w, h, pixfmt;
	u32 bpl, size;
	u32 bd_raw, bd_y;
	unsigned long flags;
	bool changed = false;

	if (!vf)
		return false;

	w = vf->width;
	h = vf->height;
	bd_raw = vf->bitdepth;
	bd_y = vfm_cap_bitdepth_y(bd_raw);

	/*
	 * Detect pixel format from vframe type flags and bitdepth.
	 *
	 * 10-bit detection hierarchy:
	 *   1. FULL_PACK_422_MODE set in bitdepth → AML_YUV422_10BIT (40-bit packed)
	 *   2. Y10 + VIU_422 → AML_YUV422_10BIT (same fourcc, different stride)
	 *   3. Y10 + NV21/NV12 → NV21/NV12 (12-bit container, 3 bytes/pixel)
	 *   4. 8-bit → standard V4L2 formats
	 */
	if (bd_y >= 10 && (bd_raw & FULL_PACK_422_MODE)) {
		/* Amlogic 40-bit packed YUV422 10-bit */
		pixfmt = V4L2_PIX_FMT_AML_YUV422_10BIT;
	} else if (bd_y >= 10 && (vf->type & VIDTYPE_VIU_422)) {
		/* 10-bit 422 in non-full-pack mode — report same fourcc */
		pixfmt = V4L2_PIX_FMT_AML_YUV422_10BIT;
	} else if (vf->type & VIDTYPE_VIU_NV21) {
		pixfmt = V4L2_PIX_FMT_NV21;
	} else if (vf->type & VIDTYPE_VIU_NV12) {
		pixfmt = V4L2_PIX_FMT_NV12;
	} else if (vf->type & VIDTYPE_VIU_422) {
		pixfmt = V4L2_PIX_FMT_UYVY;
	} else {
		pixfmt = V4L2_PIX_FMT_NV21; /* default assumption */
	}

	spin_lock_irqsave(&dev->fmt_spin, flags);
	if (dev->fmt_valid && dev->width == w && dev->height == h &&
	    dev->pixelformat == pixfmt && dev->bitdepth == bd_raw) {
		spin_unlock_irqrestore(&dev->fmt_spin, flags);
		return false; /* no change */
	}

	/* Something changed */
	changed = dev->fmt_valid; /* only report change after first valid format */

	dev->width = w;
	dev->height = h;
	dev->pixelformat = pixfmt;
	dev->bitdepth = bd_raw;
	dev->bitdepth_y = bd_y;

	switch (pixfmt) {
	case V4L2_PIX_FMT_AML_YUV422_10BIT:
		/*
		 * Amlogic 40-bit packed: 2 pixels in 5 bytes.
		 * bytesperline = width * 5 / 2 (width always even from vdin).
		 */
		dev->num_planes = 1;
		bpl = w * 5 / 2;
		size = bpl * h;
		dev->bytesperline[0] = bpl;
		dev->sizeimage[0] = PAGE_ALIGN(size);
		break;
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV12:
		dev->num_planes = 1;
		if (bd_y >= 10) {
			/*
			 * 10-bit in 12-bit containers (NV21/NV12 10-bit):
			 * Y: 2 bytes/pixel, UV: 2 bytes/pixel, 420 subsampling
			 * Total: w*2*h + w*2*h/2 = w*h*3
			 */
			bpl = w * 2;
			size = w * h * 3;
		} else {
			/* 8-bit: Y plane + interleaved UV plane */
			bpl = w;
			size = w * h * 3 / 2;
		}
		dev->bytesperline[0] = bpl;
		dev->sizeimage[0] = PAGE_ALIGN(size);
		break;
	case V4L2_PIX_FMT_UYVY:
		dev->num_planes = 1;
		bpl = w * 2;
		size = bpl * h;
		dev->bytesperline[0] = bpl;
		dev->sizeimage[0] = PAGE_ALIGN(size);
		break;
	default:
		dev->num_planes = 1;
		bpl = w;
		size = w * h * 3 / 2;
		dev->bytesperline[0] = bpl;
		dev->sizeimage[0] = PAGE_ALIGN(size);
		break;
	}

	dev->colorspace = V4L2_COLORSPACE_DEFAULT;
	dev->field = (vf->type & VIDTYPE_INTERLACE) ?
		     V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	dev->fmt_valid = true;

	spin_unlock_irqrestore(&dev->fmt_spin, flags);

	vfm_cap_info("format updated: %ux%u pixfmt=0x%x bd=0x%x(%ubit) size=%u\n",
		     w, h, pixfmt, bd_raw, bd_y, dev->sizeimage[0]);

	return changed;
}

/* ========== Signal Monitoring (Phase 2a) ========== */

/**
 * vfm_cap_build_signal_info() - Populate signal_info from current device state
 *
 * Must be called with fmt_spin held.
 */
static void vfm_cap_build_signal_info(struct vfm_cap_dev *dev,
				       struct vfm_cap_signal_info *info,
				       u32 status)
{
	memset(info, 0, sizeof(*info));
	info->status = status;

	if (status == VFM_CAP_SIG_STATUS_STABLE && dev->fmt_valid) {
		info->width = dev->width;
		info->height = dev->height;
		info->color_format = dev->pixelformat;
		info->bitdepth = dev->bitdepth_y;
		info->signal_type = dev->last_signal_type;
		info->is_interlaced = (dev->field == V4L2_FIELD_INTERLACED);
		/* fps and hdr_status will be filled when we have that info */
	}
}

/**
 * vfm_cap_queue_source_change() - Queue V4L2_EVENT_SOURCE_CHANGE to all subscribers
 *
 * @changes: bitmask of V4L2_EVENT_SRC_CH_* flags (typically V4L2_EVENT_SRC_CH_RESOLUTION)
 */
static void vfm_cap_queue_source_change(struct vfm_cap_dev *dev, u32 changes)
{
	struct v4l2_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = V4L2_EVENT_SOURCE_CHANGE;
	ev.u.src_change.changes = changes;

	/*
	 * Also embed our extended signal_info in the event data.
	 * The standard src_change.changes field tells standard consumers
	 * what changed, while our signal_info gives extended details
	 * to vfm_cap-aware consumers.
	 *
	 * Note: v4l2_event.u is a union. src_change uses only 4 bytes.
	 * We copy signal_info starting at offset 4 within the 64-byte
	 * data area, leaving src_change.changes intact.
	 * Actually, u.data[64] and u.src_change share the same union space.
	 * src_change.changes is at offset 0 (4 bytes). Our signal_info
	 * would overwrite it. Instead, we just use the standard field
	 * and consumers query G_FMT + our sysfs for extended info.
	 */
	v4l2_event_queue(&dev->vdev, &ev);

	vfm_cap_info("SOURCE_CHANGE event queued (changes=0x%x)\n", changes);
}

/**
 * vfm_cap_drain_pending() - Flush pending V4L2 frames during signal loss
 *
 * Called from sm_poll_work (process context) when signal goes unstable.
 * Removes all frames from pending_list and drops their V4L2 (held) refs.
 *
 * We do all refcount manipulation under ready_lock to avoid races with
 * PROVIDER_RESET / frame_pool_recycle_all() which may run concurrently
 * from ISR context and force-reset all refcounts.
 */
static void vfm_cap_drain_pending(struct vfm_cap_dev *dev)
{
	struct cap_frame *frame, *tmp;
	unsigned long flags;
	LIST_HEAD(drain_list);
	int count = 0;

	spin_lock_irqsave(&dev->ready_lock, flags);
	list_splice_init(&dev->pending_list, &drain_list);
	/* Also release the held-back frame — signal is lost/unstable */
	if (dev->prev_v4l2_frame) {
		list_add_tail(&dev->prev_v4l2_frame->pending_node,
			      &drain_list);
		dev->prev_v4l2_frame = NULL;
	}

	/*
	 * Drop the V4L2/pending ref for each frame while still under lock.
	 * This avoids a race with frame_pool_recycle_all() (called from ISR
	 * context on PROVIDER_RESET) which force-resets all refcounts to 0.
	 * If we dropped refs outside the lock, recycle_all could run between
	 * the splice and the dec, corrupting the refcount.
	 *
	 * We inline the refcount-dec + conditional release here because
	 * frame_put() takes ready_lock internally and would deadlock.
	 */
	list_for_each_entry_safe(frame, tmp, &drain_list, pending_node) {
		list_del_init(&frame->pending_node);
		if (atomic_dec_and_test(&frame->refcount)) {
			/* Last ref — recycle vframe back to vdin0 */
			frame_release(dev, frame);
		}
		count++;
	}
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	if (count > 0)
		vfm_cap_dbg(1, "drained %d pending frames\n", count);
}

/**
 * vfm_cap_sm_poll_work() - Periodic signal state machine monitor
 *
 * Polls tvin_get_sm_status() every VFM_CAP_SM_POLL_MS to detect
 * signal state transitions. On STABLE→!STABLE, sets draining mode.
 * On !STABLE→STABLE, queues V4L2_EVENT_SOURCE_CHANGE.
 */
static void vfm_cap_sm_poll_work(struct work_struct *work)
{
	struct vfm_cap_dev *dev = container_of(work, struct vfm_cap_dev,
					       sm_poll_work.work);
	enum vfm_cap_sm_status new_state;
	enum vfm_cap_sm_status old_state;
	int sm_val;

	if (!dev->sm_polling)
		return;

	sm_val = tvin_get_sm_status(0);
	new_state = (enum vfm_cap_sm_status)sm_val;
	old_state = dev->sm_state;

	if (new_state != old_state) {
		vfm_cap_info("SM state: %d -> %d\n", old_state, new_state);
		dev->sm_state = new_state;

		if (old_state == VFM_SM_STABLE && new_state != VFM_SM_STABLE) {
			/*
			 * Signal lost or unstable. Enter drain mode:
			 * - Stop adding new frames to pending_list
			 * - Flush any frames already pending V4L2 delivery
			 * - Let deliver_work finish naturally
			 */
			vfm_cap_info("signal lost, entering drain mode\n");
			dev->draining = true;

			/* Drain pending V4L2 frames */
			vfm_cap_drain_pending(dev);
			flush_work(&dev->deliver_work);

			/* Queue NOSIG or NOTSUP event */
			if (new_state == VFM_SM_NOSIG || new_state == VFM_SM_NULL) {
				unsigned long flags;

				spin_lock_irqsave(&dev->fmt_spin, flags);
				vfm_cap_build_signal_info(dev, &dev->sig_info,
							  VFM_CAP_SIG_STATUS_NOSIG);
				dev->fmt_valid = false;
				spin_unlock_irqrestore(&dev->fmt_spin, flags);

				vfm_cap_queue_source_change(dev,
					V4L2_EVENT_SRC_CH_RESOLUTION);
			} else if (new_state == VFM_SM_NOTSUP) {
				unsigned long flags;

				spin_lock_irqsave(&dev->fmt_spin, flags);
				vfm_cap_build_signal_info(dev, &dev->sig_info,
							  VFM_CAP_SIG_STATUS_NOTSUP);
				dev->fmt_valid = false;
				spin_unlock_irqrestore(&dev->fmt_spin, flags);

				vfm_cap_queue_source_change(dev,
					V4L2_EVENT_SRC_CH_RESOLUTION);
			}

		} else if (old_state != VFM_SM_STABLE &&
			   new_state == VFM_SM_STABLE) {
			/*
			 * Signal became stable. The new format will be
			 * detected on the next VFRAME_READY (vfm_cap_update_format
			 * returns true for the change). We exit drain mode
			 * so new frames flow to V4L2 consumers.
			 *
			 * We don't queue SOURCE_CHANGE here because we don't
			 * know the new format yet. It will be queued from
			 * vfm_cap_recv_event_cb on the first VFRAME_READY
			 * after format is detected.
			 */
			vfm_cap_info("signal stable, exiting drain mode\n");
			dev->draining = false;
		}
	}

	/* Reschedule */
	if (dev->sm_polling)
		schedule_delayed_work(&dev->sm_poll_work,
				      msecs_to_jiffies(VFM_CAP_SM_POLL_MS));
}

/* ========== V4L2 Consumer Frame Delivery (Workqueue) ========== */

/**
 * vfm_cap_deliver_work() - Deliver pending frames to V4L2 consumers
 *
 * Runs in workqueue context (process context).
 * Drains the pending_list and creates DMA-buf exports for each frame,
 * attaching them to V4L2 buffers for consumer retrieval via
 * VFM_CAP_IOC_GET_DMABUF.
 *
 * Zero-copy: no frame data is copied. The vb2 MMAP buffer is used
 * only as a flow-control token. The actual frame data is accessed
 * by the consumer through the DMA-buf fd.
 */
static void vfm_cap_deliver_work(struct work_struct *work)
{
	struct vfm_cap_dev *dev = container_of(work, struct vfm_cap_dev,
					       deliver_work);
	struct cap_frame *frame, *tmp;
	struct vfm_cap_consumer *cons;
	struct vfm_cap_buffer *buf;
	struct vb2_v4l2_buffer *vb;
	unsigned long flags;
	LIST_HEAD(local_pending);
	int delivered;

	/*
	 * Move all pending frames to a local list under the spinlock
	 * so we minimize time holding the lock.
	 */
	spin_lock_irqsave(&dev->ready_lock, flags);
	list_splice_init(&dev->pending_list, &local_pending);
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	if (list_empty(&local_pending))
		return;

	list_for_each_entry_safe(frame, tmp, &local_pending, pending_node) {
		list_del_init(&frame->pending_node);
		delivered = 0;

		if (!frame->vf || !frame->phy_addr) {
			vfm_cap_dbg(2, "deliver: frame %u skip (no vf/phy)\n",
				     frame->index);
			frame_put(dev, frame);
			continue;
		}

		/*
		 * Deliver to each streaming consumer that has a queued buffer.
		 * consumer_lock mutex is safe here (workqueue = process ctx).
		 */
		mutex_lock(&dev->consumer_lock);
		list_for_each_entry(cons, &dev->consumers, list) {
			struct dma_buf *dbuf;

			if (!cons->streaming)
				continue;

			spin_lock_irqsave(&cons->queue_lock, flags);
			if (list_empty(&cons->queued_list)) {
				spin_unlock_irqrestore(&cons->queue_lock, flags);
				continue;
			}

			buf = list_first_entry(&cons->queued_list,
					       struct vfm_cap_buffer, list);
			list_del_init(&buf->list);
			spin_unlock_irqrestore(&cons->queue_lock, flags);

			buf->frame = frame;
			vb = &buf->vb;

			/*
			 * Zero-copy: export the vdin0 CMA buffer as DMA-buf.
			 * The consumer retrieves the fd via VFM_CAP_IOC_GET_DMABUF.
			 * No memcpy — the DMA-buf points directly to vdin0's
			 * CMA physical memory.
			 */
			dbuf = vfm_cap_export_frame_dmabuf(dev, frame);
			if (IS_ERR(dbuf)) {
				vfm_cap_err("dmabuf export failed: %ld\n",
					    PTR_ERR(dbuf));
				buf->dbuf = NULL;
				buf->dbuf_fd = -1;
			} else {
				buf->dbuf = dbuf;
				buf->dbuf_fd = -1; /* fd created on demand in ioctl */
			}

			/*
			 * Set minimal payload in the vb2 buffer.
			 * The actual data size is reported via the DMA-buf.
			 */
			vb2_set_plane_payload(&vb->vb2_buf, 0,
					      frame->buf_size);
			vb->vb2_buf.timestamp = ktime_get_ns();
			vb->sequence = cons->frame_count++;
			vb->field = dev->field;

			vb2_buffer_done(&vb->vb2_buf, VB2_BUF_STATE_DONE);
			delivered++;

			vfm_cap_dbg(2, "deliver: frame %u -> cons %u seq=%llu (zero-copy)\n",
				     frame->index, cons->id,
				     cons->frame_count - 1);
		}
		mutex_unlock(&dev->consumer_lock);

		if (delivered > 0)
			atomic64_inc(&dev->stat_delivered);

		/* Release the pending-list reference on this frame */
		frame_put(dev, frame);
	}
}

/* ========== VFM Provider Ops (downstream interface) ========== */
/*
 * These ops are called by the downstream receiver (deinterlace)
 * to get frames from us.
 */

static struct vframe_s *vfm_cap_vf_peek(void *op_arg)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)op_arg;
	struct cap_frame *frame;
	unsigned long flags;

	spin_lock_irqsave(&dev->ready_lock, flags);
	if (list_empty(&dev->ready_list)) {
		spin_unlock_irqrestore(&dev->ready_lock, flags);
		return NULL;
	}
	frame = list_first_entry(&dev->ready_list, struct cap_frame, list);
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	return frame->vf;
}

static struct vframe_s *vfm_cap_vf_get(void *op_arg)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)op_arg;
	struct cap_frame *frame;
	struct vframe_s *vf;
	unsigned long flags;

	spin_lock_irqsave(&dev->ready_lock, flags);
	if (list_empty(&dev->ready_list)) {
		spin_unlock_irqrestore(&dev->ready_lock, flags);
		return NULL;
	}
	frame = list_first_entry(&dev->ready_list, struct cap_frame, list);
	list_del_init(&frame->list);
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	vf = frame->vf;

	vfm_cap_dbg(2, "downstream get vf idx=%u, refcount=%d\n",
		     vf ? vf->index : 0xFFFF,
		     atomic_read(&frame->refcount));

	return vf;
}

/**
 * vfm_cap_vf_put() - Downstream (deinterlace) returns a frame
 *
 * This decrements the display path's reference. If no V4L2 consumers
 * held the frame (or they already released), this recycles the buffer
 * back to vdin0.
 */
static void vfm_cap_vf_put(struct vframe_s *vf, void *op_arg)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)op_arg;
	struct cap_frame *frame;
	int i;

	if (!vf)
		return;

	/* Find the cap_frame for this vframe */
	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		if (dev->frame_pool[i].in_use &&
		    dev->frame_pool[i].vf == vf) {
			frame = &dev->frame_pool[i];
			vfm_cap_dbg(2, "downstream put vf idx=%u\n",
				     vf->index);
			frame_put(dev, frame);
			return;
		}
	}

	/*
	 * Frame not found in our pool - this shouldn't happen normally.
	 * Pass it directly back to upstream as a safety measure.
	 */
	vfm_cap_err("put: unknown vframe idx=%u, passing through\n",
		    vf->index);
	vf_put(vf, dev->recv_name);
	vf_notify_provider(dev->recv_name, VFRAME_EVENT_RECEIVER_PUT, NULL);
}

static int vfm_cap_prov_event_cb(int type, void *data, void *private_data)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)private_data;

	if (type & VFRAME_EVENT_RECEIVER_PUT) {
		vfm_cap_dbg(2, "prov_event: RECEIVER_PUT\n");
	} else if (type & VFRAME_EVENT_RECEIVER_GET) {
		vfm_cap_dbg(2, "prov_event: RECEIVER_GET\n");
	} else if (type & VFRAME_EVENT_RECEIVER_FRAME_WAIT) {
		vfm_cap_dbg(2, "prov_event: RECEIVER_FRAME_WAIT\n");
		/* Downstream is hungry - check if we have frames */
		if (!list_empty(&dev->ready_list))
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_VFRAME_READY,
					   NULL);
	}
	return 0;
}

static int vfm_cap_vf_states(struct vframe_states *states, void *op_arg)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)op_arg;
	int ready_count = 0;
	int in_use_count = 0;
	struct cap_frame *f;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&dev->ready_lock, flags);
	list_for_each_entry(f, &dev->ready_list, list)
		ready_count++;
	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		if (dev->frame_pool[i].in_use)
			in_use_count++;
	}
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	states->vf_pool_size = VFM_CAP_POOL_SIZE;
	states->buf_recycle_num = 0;
	states->buf_free_num = VFM_CAP_POOL_SIZE - in_use_count;
	states->buf_avail_num = ready_count;
	return 0;
}

static const struct vframe_operations_s vfm_cap_vf_provider_ops = {
	.peek     = vfm_cap_vf_peek,
	.get      = vfm_cap_vf_get,
	.put      = vfm_cap_vf_put,
	.event_cb = vfm_cap_prov_event_cb,
	.vf_states = vfm_cap_vf_states,
};

/* ========== VFM Receiver Event Callback (upstream interface) ========== */
/*
 * Called by the upstream provider (vdin0) to notify us of events.
 * This runs in ISR context when VFRAME_READY fires.
 */

static int vfm_cap_recv_event_cb(int type, void *data, void *private_data)
{
	struct vfm_cap_dev *dev = (struct vfm_cap_dev *)private_data;
	struct vframe_s *vf;
	struct cap_frame *frame;
	unsigned long flags;
	int num_streaming = 0;
	struct vfm_cap_consumer *cons;

	if (type == VFRAME_EVENT_PROVIDER_START) {
		/*
		 * Upstream says "start streaming". Register our provider
		 * to connect to downstream (deinterlace).
		 * This follows the amlvideo.c pattern exactly.
		 *
		 * Standalone mode: if no downstream receiver exists in
		 * the VFM chain (headless: path is "vdin0 vfm_cap" only),
		 * skip provider registration entirely. Frames will be
		 * managed purely for V4L2 consumers.
		 *
		 * If we previously registered as a provider (from a prior
		 * PROVIDER_START with a different VFM path), unregister
		 * first to avoid duplicate-name errors in vf_reg_provider().
		 */
		vfm_cap_info("PROVIDER_START: checking downstream for '%s' "
			     "(was standalone=%d, vfm_started=%d)\n",
			     dev->prov_name, dev->standalone,
			     dev->vfm_started);

		/* Clean up stale provider registration from previous path */
		if (dev->vfm_started && !dev->standalone) {
			vfm_cap_info("PROVIDER_START: unreg stale provider "
				     "before re-evaluation\n");
			vf_unreg_provider(&dev->vf_prov);
		}

		spin_lock_irqsave(&dev->ready_lock, flags);
		frame_pool_recycle_all(dev);
		frame_pool_init(dev);
		dev->prev_v4l2_frame = NULL;
		spin_unlock_irqrestore(&dev->ready_lock, flags);

		if (vf_get_receiver(dev->prov_name)) {
			dev->standalone = false;
			vfm_cap_info("TEE mode: downstream receiver found\n");
			vf_provider_init(&dev->vf_prov, dev->prov_name,
					 &vfm_cap_vf_provider_ops, dev);
			vf_reg_provider(&dev->vf_prov);
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_START, NULL);
		} else {
			dev->standalone = true;
			vfm_cap_info("STANDALONE mode: no downstream receiver, "
				     "frames managed for V4L2 only\n");
		}
		dev->vfm_started = true;

	} else if (type == VFRAME_EVENT_PROVIDER_UNREG) {
		/*
		 * Upstream is going away. Unregister our provider.
		 *
		 * NOTE: We must NOT rely on vf_get_receiver() here because
		 * the VFM map entry may already be removed by the time this
		 * event fires (e.g. when tvpath is removed then re-added
		 * with a different chain). If the map is gone,
		 * vf_get_receiver() returns NULL and we'd skip the unreg,
		 * leaving a stale provider registration that blocks future
		 * vf_reg_provider() calls (duplicate name check).
		 *
		 * Instead, unconditionally unregister if we registered
		 * (i.e. vfm_started && !standalone).
		 */
		vfm_cap_info("PROVIDER_UNREG: unregistering provider "
			     "(standalone=%d)\n", dev->standalone);

		if (!dev->standalone) {
			vfm_cap_info("PROVIDER_UNREG: unreg provider '%s'\n",
				     dev->prov_name);
			vf_unreg_provider(&dev->vf_prov);
		}

		/* Cancel any pending V4L2 delivery work */
		cancel_work_sync(&dev->deliver_work);

		/* Flush the frame pool - release any held frames */
		spin_lock_irqsave(&dev->ready_lock, flags);
		frame_pool_recycle_all(dev);
		frame_pool_init(dev);
		spin_unlock_irqrestore(&dev->ready_lock, flags);

		dev->prev_v4l2_frame = NULL;
		dev->vfm_started = false;

	} else if (type == VFRAME_EVENT_PROVIDER_REG) {
		vfm_cap_info("PROVIDER_REG\n");
		dev->vfm_started = false;

	} else if (type == VFRAME_EVENT_PROVIDER_QUREY_STATE) {
		struct vframe_states states;
		int downstream_state;
		int pool_in_use = 0;
		int pool_free = 0;
		int i_qs;

		vfm_cap_vf_states(&states, dev);

		/* Count truly in-use pool slots for diagnostics */
		for (i_qs = 0; i_qs < VFM_CAP_POOL_SIZE; i_qs++) {
			if (dev->frame_pool[i_qs].in_use)
				pool_in_use++;
		}
		pool_free = VFM_CAP_POOL_SIZE - pool_in_use;

		/*
		 * We are ACTIVE if:
		 * 1. We have frames in ready_list for downstream, OR
		 * 2. We have free pool slots to accept new frames, OR
		 * 3. Our downstream is active (will free slots by consuming)
		 *
		 * Previously we only checked (1), which caused vdin0 to
		 * stop sending VFRAME_READY if the pool was exhausted
		 * (all slots in_use but none in ready_list).
		 */
		if (states.buf_avail_num > 0 || pool_free > 0)
			return RECEIVER_ACTIVE;

		/* Also check downstream (tee mode only) */
		if (!dev->standalone) {
			downstream_state = vf_notify_receiver(dev->prov_name,
					       VFRAME_EVENT_PROVIDER_QUREY_STATE,
					       NULL);
			if (downstream_state == RECEIVER_ACTIVE)
				return RECEIVER_ACTIVE;
		}

		/*
		 * We're returning INACTIVE: pool is 100% exhausted and
		 * downstream is also not consuming. Log this critical state.
		 */
		vfm_cap_err("QUREY_STATE: INACTIVE! pool %d/%d in_use, "
			    "ready=%d, downstream=%s — vdin0 will stop "
			    "sending frames!\n",
			    pool_in_use, VFM_CAP_POOL_SIZE,
			    states.buf_avail_num,
			    downstream_state == RECEIVER_ACTIVE ?
			    "ACTIVE" : "INACTIVE");

		return RECEIVER_INACTIVE;

	} else if (type == VFRAME_EVENT_PROVIDER_VFRAME_READY) {
		/*
		 * A new frame is available from upstream (vdin0).
		 * This is the hot path - called from vdin0 ISR context
		 * (hard IRQ, irqs disabled by isr_lock).
		 *
		 * We do only lock-safe operations here:
		 * 1. Get the vframe from upstream (vf_get is IRQ-safe)
		 * 2. Update format state (spinlock-protected)
		 * 3. Wrap in cap_frame, add to ready_list (for downstream)
		 * 4. Optionally add to pending_list (for V4L2 workqueue)
		 * 5. Notify downstream
		 * 6. Schedule workqueue for V4L2 delivery (memcpy)
		 */
		bool fmt_changed;
		bool want_v4l2;

		vf = vf_peek(dev->recv_name);
		if (!vf)
			return 0;

		vf = vf_get(dev->recv_name);
		if (!vf)
			return 0;

		atomic64_inc(&dev->stat_frames);

		/*
		 * If we were loaded into an already-running pipeline
		 * (e.g. rmmod + insmod while vdin0 is streaming),
		 * we never received VFRAME_EVENT_PROVIDER_START.
		 * Detect this and self-register the VFM provider so
		 * frames can flow through the tee to downstream.
		 *
		 * Standalone mode: skip provider registration (no downstream).
		 */
		if (!dev->vfm_started) {
			unsigned long pflags;

			vfm_cap_info("late start: provider not registered, "
				     "self-registering (standalone=%d)\n",
				     dev->standalone);

			spin_lock_irqsave(&dev->ready_lock, pflags);
			frame_pool_recycle_all(dev);
			frame_pool_init(dev);
			dev->prev_v4l2_frame = NULL;
			spin_unlock_irqrestore(&dev->ready_lock, pflags);

			if (vf_get_receiver(dev->prov_name)) {
				dev->standalone = false;
				vfm_cap_info("late start: TEE mode "
					     "(downstream found)\n");
				vf_provider_init(&dev->vf_prov,
						 dev->prov_name,
						 &vfm_cap_vf_provider_ops,
						 dev);
				vf_reg_provider(&dev->vf_prov);
				vf_notify_receiver(dev->prov_name,
					VFRAME_EVENT_PROVIDER_START,
					NULL);
			} else {
				dev->standalone = true;
				vfm_cap_info("late start: STANDALONE mode "
					     "(no downstream)\n");
			}
			dev->vfm_started = true;
		}

		/* Update format on first frame or format change */
		fmt_changed = vfm_cap_update_format(dev, vf);

		/* Track signal_type for HDR/DV change detection */
		if (vf->signal_type != dev->last_signal_type) {
			vfm_cap_dbg(1, "signal_type changed: 0x%x -> 0x%x\n",
				     dev->last_signal_type, vf->signal_type);
			dev->last_signal_type = vf->signal_type;
			fmt_changed = true;
		}

		/*
		 * If format changed and we came from STABLE→STABLE
		 * (no SM transition, just format change within stable signal),
		 * queue SOURCE_CHANGE. This is safe from ISR context because
		 * v4l2_event_queue() uses spin_lock_irqsave internally.
		 *
		 * Flush the frame pool to prevent "unknown vframe idx" errors
		 * when resolution changes. Old frames from previous resolution
		 * cannot be matched when downstream returns them.
		 */
		if (fmt_changed) {
			unsigned long eflags;

			/* Recycle all frames from pool - they have old format */
			spin_lock_irqsave(&dev->ready_lock, eflags);
			frame_pool_recycle_all(dev);
			spin_unlock_irqrestore(&dev->ready_lock, eflags);

			spin_lock_irqsave(&dev->fmt_spin, eflags);
			vfm_cap_build_signal_info(dev, &dev->sig_info,
						  VFM_CAP_SIG_STATUS_STABLE);
			spin_unlock_irqrestore(&dev->fmt_spin, eflags);

			vfm_cap_queue_source_change(dev,
				V4L2_EVENT_SRC_CH_RESOLUTION);
		}

		/*
		 * Determine if we should deliver to V4L2 consumers.
		 * Skip V4L2 delivery if draining (signal lost/unstable).
		 * Always pass through to display path regardless.
		 */
		want_v4l2 = false;
		if (!dev->draining) {
			spin_lock(&dev->consumer_spin);
			list_for_each_entry(cons, &dev->consumers, list) {
				if (cons->streaming)
					num_streaming++;
			}
			spin_unlock(&dev->consumer_spin);
			want_v4l2 = (num_streaming > 0);
		}

		/* Allocate a cap_frame wrapper */
		spin_lock_irqsave(&dev->ready_lock, flags);
		frame = frame_pool_alloc(dev);
		if (!frame) {
			int in_use_cnt = 0, j;
			for (j = 0; j < VFM_CAP_POOL_SIZE; j++) {
				if (dev->frame_pool[j].in_use)
					in_use_cnt++;
			}
			spin_unlock_irqrestore(&dev->ready_lock, flags);
			vfm_cap_err("pool exhausted! %d/%d in_use, "
				    "dropping vf idx=%u (total drops=%lld)\n",
				    in_use_cnt, VFM_CAP_POOL_SIZE,
				    vf->index,
				    atomic64_read(&dev->stat_drops) + 1);
			/* Recycle frame back to vdin0 */
			vf_put(vf, dev->recv_name);
			vf_notify_provider(dev->recv_name,
					   VFRAME_EVENT_RECEIVER_PUT, NULL);
			atomic64_inc(&dev->stat_drops);
			return 0;
		}

		frame->vf = vf;
		frame->acquired_at = ktime_get();
		INIT_LIST_HEAD(&frame->list);
		INIT_LIST_HEAD(&frame->pending_node);

		/* Cache export layout for zero-copy DMA-buf export. */
		frame->is_afbc = !!(vf->type & VIDTYPE_COMPRESS);
		frame->comp_head_addr = vf->compHeadAddr;
		frame->comp_body_addr = vf->compBodyAddr;
		frame->comp_table_addr = 0;
		frame->comp_head_size = 0;
		frame->comp_table_size = 0;
		frame->comp_body_size = 0;
		frame->comp_width = 0;
		frame->comp_height = 0;
		frame->drm_modifier = 0;

		if (frame->is_afbc && vf->compBodyAddr && vf->compHeadAddr &&
		    vf->compHeadAddr > vf->compBodyAddr) {
			size_t body_size = vf->compHeadAddr - vf->compBodyAddr;
			size_t head_size = vfm_cap_afbc_header_size(vf->compWidth,
							     vf->compHeight);
			size_t table_size = vfm_cap_afbc_table_size(body_size);

			frame->phy_addr = vf->compBodyAddr;
			frame->buf_size = body_size + head_size + table_size;
			frame->comp_table_addr = vf->compHeadAddr + head_size;
			frame->comp_head_size = head_size;
			frame->comp_table_size = table_size;
			frame->comp_body_size = body_size;
			frame->comp_width = vf->compWidth;
			frame->comp_height = vf->compHeight;
			frame->drm_modifier = DRM_FORMAT_MOD_ARM_AFBC(
				AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
				AFBC_FORMAT_MOD_SPARSE |
				AFBC_FORMAT_MOD_SPLIT);
			vfm_cap_dbg(1,
				    "afbc frame idx=%u src=%ux%u coded=%ux%u body=%pa/%zu head=%pa/%zu table=%pa/%zu mod=%#llx\n",
				    vf->index, vf->width, vf->height,
				    frame->comp_width, frame->comp_height,
				    &frame->comp_body_addr, frame->comp_body_size,
				    &frame->comp_head_addr, frame->comp_head_size,
				    &frame->comp_table_addr, frame->comp_table_size,
				    frame->drm_modifier);
		} else {
			frame->phy_addr = vf->canvas0_config[0].phy_addr;
			frame->buf_size = dev->sizeimage[0];
			frame->is_afbc = false;
			frame->comp_head_addr = 0;
			frame->comp_table_addr = 0;
			frame->comp_body_addr = 0;
			frame->comp_head_size = 0;
			frame->comp_table_size = 0;
			frame->comp_body_size = 0;
			frame->comp_width = 0;
			frame->comp_height = 0;
		}

		/*
		 * Set refcount and manage frame ownership.
		 *
		 * Standalone mode: no display path, so no base refcount of 1.
		 * Frames are managed purely for V4L2. If no V4L2 consumers
		 * are streaming, recycle immediately.
		 *
		 * Tee mode: display path gets base refcount of 1.
		 *
		 * The V4L2 reference (+1) is NOT set on the current frame.
		 * Instead, we use a one-frame delay: the PREVIOUS frame
		 * (prev_v4l2_frame) is delivered to V4L2 consumers.
		 * This ensures V4L2/DMA-buf consumers only see fully-written
		 * frames, eliminating tearing from vdin0's VRR write phase.
		 *
		 * Additional refs are taken by vfm_cap_export_frame_dmabuf()
		 * when the consumer requests DMA-buf fds.
		 */
		if (dev->standalone) {
			/*
			 * Standalone: no downstream display path.
			 * Refcount is purely for V4L2 consumers.
			 */
			if (want_v4l2) {
				struct cap_frame *prev = dev->prev_v4l2_frame;

				/* refcount=1 for the held V4L2 ref */
				atomic_set(&frame->refcount, 1);
				dev->prev_v4l2_frame = frame;

				if (prev) {
					/*
					 * Previous frame fully written.
					 * Its refcount was set to 1 when held;
					 * that ref transfers to pending_list.
					 */
					list_add_tail(&prev->pending_node,
						      &dev->pending_list);
				}
			} else {
				/*
				 * No V4L2 consumers: recycle immediately.
				 * Don't add to any list — just put back.
				 */
				frame->in_use = false;
				spin_unlock_irqrestore(&dev->ready_lock,
						       flags);
				vf_put(vf, dev->recv_name);
				vf_notify_provider(dev->recv_name,
					VFRAME_EVENT_RECEIVER_PUT, NULL);
				frame->vf = NULL;
				/* Wake pollers, schedule work if pending */
				if (!list_empty(&dev->pending_list))
					schedule_work(&dev->deliver_work);
				wake_up_interruptible(&dev->wq);
				return 0;
			}
		} else {
			/*
			 * Tee mode: display path gets base refcount of 1.
			 */
			atomic_set(&frame->refcount, 1);

			/* Add to ready list for downstream to pick up */
			list_add_tail(&frame->list, &dev->ready_list);

			/*
			 * One-frame delay for V4L2 delivery (tearing fix):
			 *
			 * Instead of delivering the current frame (which may
			 * still be under active DMA write by vdin0), we
			 * deliver the PREVIOUS frame which is guaranteed
			 * complete.
			 *
			 * Flow:
			 *   Frame N arrives:
			 *     - prev_v4l2_frame (N-1) -> pending_list
			 *     - current frame N -> saved as prev_v4l2_frame
			 *   Frame N+1 arrives:
			 *     - prev_v4l2_frame (N) -> pending_list
			 *     - current frame N+1 -> saved as prev_v4l2_frame
			 *
			 * The first frame after start has no previous, so
			 * it's held and delivered when the second frame
			 * arrives (one-frame startup latency, acceptable
			 * for streaming).
			 */
			if (want_v4l2) {
				struct cap_frame *prev = dev->prev_v4l2_frame;

				/* Bump refcount for the held ref */
				atomic_inc(&frame->refcount);
				dev->prev_v4l2_frame = frame;

				if (prev) {
					list_add_tail(&prev->pending_node,
						      &dev->pending_list);
				}
			}
		}

		spin_unlock_irqrestore(&dev->ready_lock, flags);

		/* Notify downstream that a frame is ready (tee mode only) */
		if (!dev->standalone)
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_VFRAME_READY,
					   NULL);

		/* Schedule V4L2 delivery work if we queued a previous frame */
		if (want_v4l2 && !list_empty(&dev->pending_list))
			schedule_work(&dev->deliver_work);

		/* Wake up any polling consumers */
		wake_up_interruptible(&dev->wq);

	} else if (type == VFRAME_EVENT_PROVIDER_RESET) {
		vfm_cap_info("PROVIDER_RESET\n");

		spin_lock_irqsave(&dev->ready_lock, flags);
		frame_pool_recycle_all(dev);
		frame_pool_init(dev);
		spin_unlock_irqrestore(&dev->ready_lock, flags);

		/* Forward reset downstream (tee mode only) */
		if (!dev->standalone)
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_RESET, data);

	} else if (type == VFRAME_EVENT_PROVIDER_FR_HINT) {
		/* Pass through frame rate hint (tee mode only) */
		if (!dev->standalone)
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_FR_HINT, data);
	} else if (type == VFRAME_EVENT_PROVIDER_FR_END_HINT) {
		if (!dev->standalone)
			vf_notify_receiver(dev->prov_name,
					   VFRAME_EVENT_PROVIDER_FR_END_HINT,
					   data);
	}

	return 0;
}

static const struct vframe_receiver_op_s vfm_cap_vf_receiver_ops = {
	.event_cb = vfm_cap_recv_event_cb,
};

/* ========== vb2 Queue Operations ========== */

static int vfm_cap_queue_setup(struct vb2_queue *vq,
			       unsigned int *num_buffers,
			       unsigned int *num_planes,
			       unsigned int sizes[],
			       struct device *alloc_devs[])
{
	/*
	 * In zero-copy mode, vb2 buffers are flow-control tokens only.
	 * We allocate a minimal 4K page per buffer. The actual frame data
	 * is accessed via DMA-buf fd from VFM_CAP_IOC_GET_DMABUF.
	 */
	*num_planes = 1;
	sizes[0] = PAGE_SIZE;
	alloc_devs[0] = v4l_get_dev_from_codec_mm();

	vfm_cap_dbg(1, "queue_setup: %u buffers (flow-control tokens, %u bytes each)\n",
		    *num_buffers, sizes[0]);
	return 0;
}

static int vfm_cap_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vfm_cap_buffer *buf = to_vfm_cap_buffer(vbuf);

	buf->frame = NULL;
	buf->dbuf = NULL;
	buf->dbuf_fd = -1;
	return 0;
}

static int vfm_cap_buf_prepare(struct vb2_buffer *vb)
{
	/* Flow-control tokens: no size validation needed */
	return 0;
}

static void vfm_cap_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vfm_cap_buffer *buf = to_vfm_cap_buffer(vbuf);
	struct vfm_cap_consumer *cons = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	/*
	 * On re-queue (QBUF after DQBUF), clean up any DMA-buf
	 * that the consumer didn't close. This is a safety net;
	 * well-behaved consumers should close the fd before QBUF.
	 */
	if (buf->dbuf) {
		vfm_cap_dbg(1, "buf_queue: cleaning up unreleased dmabuf on buf %u\n",
			     vb->index);
		dma_buf_put(buf->dbuf);
		buf->dbuf = NULL;
	}
	buf->dbuf_fd = -1;
	buf->frame = NULL;

	spin_lock_irqsave(&cons->queue_lock, flags);
	list_add_tail(&buf->list, &cons->queued_list);
	spin_unlock_irqrestore(&cons->queue_lock, flags);
}

static int vfm_cap_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vfm_cap_consumer *cons = vb2_get_drv_priv(vq);

	vfm_cap_info("consumer %u start streaming (bufs=%u)\n",
		     cons->id, count);
	cons->streaming = true;
	cons->frame_count = 0;
	return 0;
}

static void vfm_cap_stop_streaming(struct vb2_queue *vq)
{
	struct vfm_cap_consumer *cons = vb2_get_drv_priv(vq);
	struct vfm_cap_buffer *buf, *tmp;
	unsigned long flags;

	vfm_cap_info("consumer %u stop streaming\n", cons->id);
	cons->streaming = false;

	/* Return all queued buffers to userspace with ERROR state */
	spin_lock_irqsave(&cons->queue_lock, flags);
	list_for_each_entry_safe(buf, tmp, &cons->queued_list, list) {
		list_del_init(&buf->list);
		/* Clean up any DMA-buf references */
		if (buf->dbuf) {
			dma_buf_put(buf->dbuf);
			buf->dbuf = NULL;
		}
		buf->dbuf_fd = -1;
		buf->frame = NULL;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&cons->queue_lock, flags);
}

static void vfm_cap_buf_finish(struct vb2_buffer *vb)
{
	/* No-op: DMA-buf cleanup happens in buf_queue (re-queue) or stop_streaming */
}

static const struct vb2_ops vfm_cap_vb2_ops = {
	.queue_setup     = vfm_cap_queue_setup,
	.buf_init        = vfm_cap_buf_init,
	.buf_prepare     = vfm_cap_buf_prepare,
	.buf_queue       = vfm_cap_buf_queue,
	.buf_finish      = vfm_cap_buf_finish,
	.start_streaming = vfm_cap_start_streaming,
	.stop_streaming  = vfm_cap_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ========== V4L2 ioctl Operations ========== */

static int vfm_cap_querycap(struct file *file, void *fh,
			    struct v4l2_capability *cap)
{
	strscpy(cap->driver, VFM_CAP_MODULE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "VFM Capture Tee", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vfm_cap", sizeof(cap->bus_info));
	return 0;
}

static int vfm_cap_enum_fmt(struct file *file, void *fh,
			    struct v4l2_fmtdesc *f)
{
	/* Supported formats: 8-bit + 10-bit Amlogic native */
	static const u32 formats[] = {
		V4L2_PIX_FMT_NV21,
		V4L2_PIX_FMT_NV12,
		V4L2_PIX_FMT_UYVY,
		V4L2_PIX_FMT_AML_YUV422_10BIT,
	};

	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	f->pixelformat = formats[f->index];
	return 0;
}

static int vfm_cap_g_fmt(struct file *file, void *fh,
			 struct v4l2_format *f)
{
	struct vfm_cap_consumer *cons = file->private_data;
	struct vfm_cap_dev *dev = cons->dev;
	struct v4l2_pix_format_mplane *mp = &f->fmt.pix_mp;
	unsigned long flags;

	spin_lock_irqsave(&dev->fmt_spin, flags);
	if (!dev->fmt_valid) {
		/* Return a sane default */
		mp->width = 1920;
		mp->height = 1080;
		mp->pixelformat = V4L2_PIX_FMT_NV21;
		mp->num_planes = 1;
		mp->plane_fmt[0].bytesperline = 1920;
		mp->plane_fmt[0].sizeimage = PAGE_ALIGN(1920 * 1080 * 3 / 2);
		mp->field = V4L2_FIELD_NONE;
		mp->colorspace = V4L2_COLORSPACE_DEFAULT;
	} else {
		mp->width = dev->width;
		mp->height = dev->height;
		mp->pixelformat = dev->pixelformat;
		mp->num_planes = dev->num_planes;
		mp->plane_fmt[0].bytesperline = dev->bytesperline[0];
		mp->plane_fmt[0].sizeimage = dev->sizeimage[0];
		mp->field = dev->field;
		mp->colorspace = dev->colorspace;
	}
	spin_unlock_irqrestore(&dev->fmt_spin, flags);

	return 0;
}

static int vfm_cap_s_fmt(struct file *file, void *fh,
			 struct v4l2_format *f)
{
	/*
	 * Phase 1: Format is determined by upstream vdin0 signal.
	 * S_FMT is a no-op that returns the current format.
	 * The consumer should use G_FMT to discover the format.
	 */
	return vfm_cap_g_fmt(file, fh, f);
}

static int vfm_cap_try_fmt(struct file *file, void *fh,
			   struct v4l2_format *f)
{
	return vfm_cap_g_fmt(file, fh, f);
}

static int vfm_cap_reqbufs(struct file *file, void *fh,
			   struct v4l2_requestbuffers *rb)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_reqbufs(&cons->vb2q, rb);
}

static int vfm_cap_querybuf(struct file *file, void *fh,
			    struct v4l2_buffer *b)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_querybuf(&cons->vb2q, b);
}

static int vfm_cap_qbuf(struct file *file, void *fh,
			struct v4l2_buffer *b)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_qbuf(&cons->vb2q,
			cons->dev->v4l2_dev.mdev, b);
}

static int vfm_cap_dqbuf(struct file *file, void *fh,
			 struct v4l2_buffer *b)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_dqbuf(&cons->vb2q, b, file->f_flags & O_NONBLOCK);
}

static int vfm_cap_expbuf(struct file *file, void *fh,
			  struct v4l2_exportbuffer *eb)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_expbuf(&cons->vb2q, eb);
}

static int vfm_cap_streamon(struct file *file, void *fh,
			    enum v4l2_buf_type type)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_streamon(&cons->vb2q, type);
}

static int vfm_cap_streamoff(struct file *file, void *fh,
			     enum v4l2_buf_type type)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_streamoff(&cons->vb2q, type);
}

static int vfm_cap_enum_input(struct file *file, void *fh,
			      struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;

	strscpy(inp->name, "VFM Capture", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int vfm_cap_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vfm_cap_s_input(struct file *file, void *fh, unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

/* ========== Custom ioctl: VFM_CAP_IOC_GET_DMABUF ========== */

/**
 * vfm_cap_ioctl_get_dmabuf() - Get DMA-buf fd for a DQBUF'd frame
 *
 * Called after VIDIOC_DQBUF to retrieve the DMA-buf file descriptor for
 * the zero-copy frame data. The fd points directly to vdin0's CMA buffer.
 *
 * Flow:
 *   1. Userspace calls DQBUF → gets buffer index + metadata
 *   2. Userspace calls VFM_CAP_IOC_GET_DMABUF with that index
 *   3. We find the vb2 buffer, get its dma_buf, create an fd
 *   4. Userspace imports fd into Vulkan/encoder/etc.
 *   5. Userspace closes fd, then calls QBUF to return the token
 *
 * The DMA-buf fd holds a reference on the cap_frame. Closing the fd
 * (or process exit) releases that reference, allowing the vframe to
 * be recycled back to vdin0.
 */
static int vfm_cap_ioctl_get_dmabuf(struct vfm_cap_consumer *cons,
				     struct vfm_cap_dmabuf_req *req)
{
	struct vb2_queue *vq = &cons->vb2q;
	struct vb2_buffer *vb;
	struct vb2_v4l2_buffer *vbuf;
	struct vfm_cap_buffer *buf;
	int fd;

	if (req->index >= vq->num_buffers)
		return -EINVAL;

	vb = vq->bufs[req->index];
	if (!vb)
		return -EINVAL;

	/*
	 * Only allow GET_DMABUF on buffers in DONE state (after DQBUF).
	 * The buffer transitions: QUEUED -> ACTIVE -> DONE -> DEQUEUED.
	 * After DQBUF the state is DEQUEUED. We check that we have a
	 * valid DMA-buf attached (set during deliver_work).
	 */
	vbuf = to_vb2_v4l2_buffer(vb);
	buf = to_vfm_cap_buffer(vbuf);

	if (!buf->dbuf) {
		vfm_cap_dbg(1, "get_dmabuf: buf %u has no dmabuf attached\n",
			     req->index);
		return -EINVAL;
	}

	/*
	 * If we already created an fd for this buffer (repeat call),
	 * return the cached fd. This avoids creating multiple fds for
	 * the same DMA-buf.
	 */
	if (buf->dbuf_fd >= 0) {
		req->fd = buf->dbuf_fd;
		req->size = buf->frame ? buf->frame->buf_size : 0;
		req->flags = (buf->frame && buf->frame->is_afbc) ?
			VFM_CAP_DMABUF_FLAG_AFBC : 0;
		req->drm_modifier = buf->frame ? buf->frame->drm_modifier : 0;
		req->comp_head_addr = buf->frame ? buf->frame->comp_head_addr : 0;
		req->comp_table_addr = buf->frame ? buf->frame->comp_table_addr : 0;
		req->comp_body_addr = buf->frame ? buf->frame->comp_body_addr : 0;
		req->comp_width = buf->frame ? buf->frame->comp_width : 0;
		req->comp_height = buf->frame ? buf->frame->comp_height : 0;
		req->comp_head_size = buf->frame ? buf->frame->comp_head_size : 0;
		req->comp_table_size = buf->frame ? buf->frame->comp_table_size : 0;
		req->comp_body_size = buf->frame ? buf->frame->comp_body_size : 0;
		req->reserved0 = 0;
		req->reserved1 = 0;
		req->reserved2 = 0;
		req->reserved3 = 0;
		return 0;
	}

	/*
	 * Create a new fd for the DMA-buf.
	 * dma_buf_fd() installs the fd in the calling process's fd table.
	 * The fd holds an additional reference on the dma_buf (via
	 * dma_buf_get/put), so the dma_buf stays alive until the consumer
	 * closes the fd, even if the vb2 buffer is re-queued.
	 */
	fd = dma_buf_fd(buf->dbuf, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		vfm_cap_err("dma_buf_fd failed: %d\n", fd);
		return fd;
	}

	/*
	 * dma_buf_fd() consumes our dma_buf reference — the fd now
	 * owns it. When userspace closes the fd, dma_buf refcount
	 * drops to 0 and dma_buf is freed. But we still hold
	 * buf->dbuf and will call dma_buf_put() on it in buf_queue
	 * cleanup. Take an extra reference so both the fd and our
	 * buf->dbuf each hold one independently.
	 */
	get_dma_buf(buf->dbuf);
	buf->dbuf_fd = fd;
	req->fd = fd;
	req->size = buf->frame ? buf->frame->buf_size : 0;
	req->flags = (buf->frame && buf->frame->is_afbc) ?
		VFM_CAP_DMABUF_FLAG_AFBC : 0;
	req->drm_modifier = buf->frame ? buf->frame->drm_modifier : 0;
	req->comp_head_addr = buf->frame ? buf->frame->comp_head_addr : 0;
	req->comp_table_addr = buf->frame ? buf->frame->comp_table_addr : 0;
	req->comp_body_addr = buf->frame ? buf->frame->comp_body_addr : 0;
	req->comp_width = buf->frame ? buf->frame->comp_width : 0;
	req->comp_height = buf->frame ? buf->frame->comp_height : 0;
	req->comp_head_size = buf->frame ? buf->frame->comp_head_size : 0;
	req->comp_table_size = buf->frame ? buf->frame->comp_table_size : 0;
	req->comp_body_size = buf->frame ? buf->frame->comp_body_size : 0;
	req->reserved0 = 0;
	req->reserved1 = 0;
	req->reserved2 = 0;
	req->reserved3 = 0;

	vfm_cap_dbg(1, "get_dmabuf: buf %u -> fd %d (size=%u)\n",
		     req->index, fd, req->size);
	return 0;
}

/**
 * vfm_cap_vidioc_default() - Handle custom ioctls
 *
 * Called by video_ioctl2 for any ioctl command not handled by the
 * standard v4l2_ioctl_ops callbacks. We use this for VFM_CAP_IOC_GET_DMABUF.
 */
static long vfm_cap_vidioc_default(struct file *file, void *fh,
				   bool valid_prio, unsigned int cmd,
				   void *arg)
{
	struct vfm_cap_consumer *cons = file->private_data;

	switch (cmd) {
	case VFM_CAP_IOC_GET_DMABUF:
		return vfm_cap_ioctl_get_dmabuf(cons,
						(struct vfm_cap_dmabuf_req *)arg);
	default:
		return -ENOTTY;
	}
}

/* ========== V4L2 Event Subscribe/Unsubscribe (Phase 2a) ========== */

static int vfm_cap_subscribe_event(struct v4l2_fh *fh,
				   const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case VFM_CAP_EVENT_VRR:
	case VFM_CAP_EVENT_ALLM:
	case VFM_CAP_EVENT_FPS:
		return v4l2_event_subscribe(fh, sub, 4, NULL);
	default:
		return -EINVAL;
	}
}

static int vfm_cap_unsubscribe_event(struct v4l2_fh *fh,
				     const struct v4l2_event_subscription *sub)
{
	return v4l2_event_unsubscribe(fh, sub);
}

static const struct v4l2_ioctl_ops vfm_cap_ioctl_ops = {
	.vidioc_querycap              = vfm_cap_querycap,
	.vidioc_enum_fmt_vid_cap      = vfm_cap_enum_fmt,
	.vidioc_g_fmt_vid_cap_mplane  = vfm_cap_g_fmt,
	.vidioc_s_fmt_vid_cap_mplane  = vfm_cap_s_fmt,
	.vidioc_try_fmt_vid_cap_mplane = vfm_cap_try_fmt,
	.vidioc_reqbufs               = vfm_cap_reqbufs,
	.vidioc_querybuf              = vfm_cap_querybuf,
	.vidioc_qbuf                  = vfm_cap_qbuf,
	.vidioc_dqbuf                 = vfm_cap_dqbuf,
	.vidioc_expbuf                = vfm_cap_expbuf,
	.vidioc_streamon              = vfm_cap_streamon,
	.vidioc_streamoff             = vfm_cap_streamoff,
	.vidioc_enum_input            = vfm_cap_enum_input,
	.vidioc_g_input               = vfm_cap_g_input,
	.vidioc_s_input               = vfm_cap_s_input,
	.vidioc_subscribe_event       = vfm_cap_subscribe_event,
	.vidioc_unsubscribe_event     = vfm_cap_unsubscribe_event,
	.vidioc_default               = vfm_cap_vidioc_default,
};

/* ========== V4L2 File Operations ========== */

static int vfm_cap_open(struct file *file)
{
	struct vfm_cap_dev *dev = video_drvdata(file);
	struct vfm_cap_consumer *cons;
	struct vb2_queue *vq;
	unsigned long flags;
	int ret;

	mutex_lock(&dev->consumer_lock);
	if (dev->num_consumers >= vfm_cap_max_consumers) {
		mutex_unlock(&dev->consumer_lock);
		vfm_cap_err("max consumers (%u) reached\n",
			    vfm_cap_max_consumers);
		return -EBUSY;
	}
	mutex_unlock(&dev->consumer_lock);

	cons = kzalloc(sizeof(*cons), GFP_KERNEL);
	if (!cons)
		return -ENOMEM;

	cons->dev = dev;
	cons->streaming = false;
	INIT_LIST_HEAD(&cons->queued_list);
	INIT_LIST_HEAD(&cons->list);
	mutex_init(&cons->lock);
	spin_lock_init(&cons->queue_lock);

	/* Initialize V4L2 file handle */
	v4l2_fh_init(&cons->fh, &dev->vdev);
	file->private_data = cons;

	/* Set up vb2 queue for this consumer */
	vq = &cons->vb2q;
	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vq->io_modes = VB2_MMAP | VB2_DMABUF;
	vq->drv_priv = cons;
	vq->buf_struct_size = sizeof(struct vfm_cap_buffer);
	vq->ops = &vfm_cap_vb2_ops;
	vq->mem_ops = &vb2_dma_contig_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->min_buffers_needed = 2;
	vq->lock = &cons->lock;
	vq->gfp_flags = GFP_DMA32;
	vq->dev = v4l_get_dev_from_codec_mm();

	ret = vb2_queue_init(vq);
	if (ret) {
		vfm_cap_err("vb2_queue_init failed: %d\n", ret);
		v4l2_fh_exit(&cons->fh);
		kfree(cons);
		return ret;
	}

	v4l2_fh_add(&cons->fh);

	mutex_lock(&dev->consumer_lock);
	spin_lock_irqsave(&dev->consumer_spin, flags);
	cons->id = dev->consumer_id_gen++;
	list_add_tail(&cons->list, &dev->consumers);
	dev->num_consumers++;
	spin_unlock_irqrestore(&dev->consumer_spin, flags);
	mutex_unlock(&dev->consumer_lock);

	vfm_cap_info("consumer %u opened (total: %u)\n",
		     cons->id, dev->num_consumers);
	return 0;
}

static int vfm_cap_release(struct file *file)
{
	struct vfm_cap_consumer *cons = file->private_data;
	struct vfm_cap_dev *dev = cons->dev;
	struct vfm_cap_buffer *buf, *tmp;
	unsigned long flags;

	vfm_cap_info("consumer %u closing\n", cons->id);

	/* Stop streaming if active */
	if (cons->streaming) {
		vb2_streamoff(&cons->vb2q,
			      V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		cons->streaming = false;
	}

	/* Return any remaining queued buffers, cleaning up DMA-bufs */
	spin_lock_irqsave(&cons->queue_lock, flags);
	list_for_each_entry_safe(buf, tmp, &cons->queued_list, list) {
		list_del_init(&buf->list);
		if (buf->dbuf) {
			dma_buf_put(buf->dbuf);
			buf->dbuf = NULL;
		}
		buf->dbuf_fd = -1;
		buf->frame = NULL;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&cons->queue_lock, flags);

	/* Cleanup vb2 queue */
	vb2_queue_release(&cons->vb2q);

	/* Remove from consumer list */
	mutex_lock(&dev->consumer_lock);
	spin_lock_irqsave(&dev->consumer_spin, flags);
	list_del(&cons->list);
	dev->num_consumers--;
	spin_unlock_irqrestore(&dev->consumer_spin, flags);
	mutex_unlock(&dev->consumer_lock);

	v4l2_fh_del(&cons->fh);
	v4l2_fh_exit(&cons->fh);

	vfm_cap_info("consumer %u closed (remaining: %u)\n",
		     cons->id, dev->num_consumers);

	/*
	 * Auto-drain: when the last V4L2 consumer closes, recycle any
	 * stuck pool frames back to vdin0.  With zero consumers there
	 * can be no V4L2/DMA-buf references, so every in_use frame is
	 * held only by the display path.  Returning them now prevents
	 * vdin0 write-buffer exhaustion (the "freeze after first run" bug).
	 */
	if (dev->num_consumers == 0) {
		/* Cancel pending delivery — no consumers to deliver to */
		cancel_work_sync(&dev->deliver_work);

		spin_lock_irqsave(&dev->ready_lock, flags);
		frame_pool_recycle_all(dev);
		spin_unlock_irqrestore(&dev->ready_lock, flags);

		vfm_cap_info("auto-drain: recycled all frames to vdin0\n");
	}

	kfree(cons);
	return 0;
}

static __poll_t vfm_cap_poll(struct file *file,
			     struct poll_table_struct *wait)
{
	struct vfm_cap_consumer *cons = file->private_data;
	__poll_t ret;

	ret = vb2_poll(&cons->vb2q, file, wait);

	/* Also check for pending V4L2 events */
	if (v4l2_event_pending(&cons->fh))
		ret |= EPOLLPRI;

	return ret;
}

static int vfm_cap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vfm_cap_consumer *cons = file->private_data;

	return vb2_mmap(&cons->vb2q, vma);
}

static const struct v4l2_file_operations vfm_cap_fops = {
	.owner          = THIS_MODULE,
	.open           = vfm_cap_open,
	.release        = vfm_cap_release,
	.poll           = vfm_cap_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vfm_cap_mmap,
};

/* ========== Sysfs Attributes ========== */

static ssize_t status_show(struct device *device,
			   struct device_attribute *attr, char *buf)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;

	if (!dev)
		return scnprintf(buf, PAGE_SIZE, "not initialized\n");

	return scnprintf(buf, PAGE_SIZE,
			 "vfm_started: %d\n"
			 "consumers: %u\n"
			 "fmt_valid: %d\n"
			 "sm_state: %d\n"
			 "draining: %d\n",
			 dev->vfm_started,
			 dev->num_consumers,
			 dev->fmt_valid,
			 dev->sm_state,
			 dev->draining);
}
static DEVICE_ATTR_RO(status);

static ssize_t signal_info_show(struct device *device,
				struct device_attribute *attr, char *buf)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;
	unsigned long flags;
	int ret;

	if (!dev || !dev->fmt_valid)
		return scnprintf(buf, PAGE_SIZE, "no signal\n");

	spin_lock_irqsave(&dev->fmt_spin, flags);
	ret = scnprintf(buf, PAGE_SIZE,
			"width: %u\n"
			"height: %u\n"
			"pixelformat: 0x%08x\n"
			"sizeimage: %u\n"
			"field: %u\n"
			"bitdepth: %u\n"
			"bitdepth_raw: 0x%x\n"
			"signal_type: 0x%x\n",
			dev->width, dev->height,
			dev->pixelformat,
			dev->sizeimage[0],
			dev->field,
			dev->bitdepth_y,
			dev->bitdepth,
			dev->last_signal_type);
	spin_unlock_irqrestore(&dev->fmt_spin, flags);
	return ret;
}
static DEVICE_ATTR_RO(signal_info);

static ssize_t stats_show(struct device *device,
			  struct device_attribute *attr, char *buf)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;

	if (!dev)
		return scnprintf(buf, PAGE_SIZE, "not initialized\n");

	return scnprintf(buf, PAGE_SIZE,
			 "frames_received: %lld\n"
			 "frames_dropped: %lld\n"
			 "frames_delivered: %lld\n",
			 atomic64_read(&dev->stat_frames),
			 atomic64_read(&dev->stat_drops),
			 atomic64_read(&dev->stat_delivered));
}
static DEVICE_ATTR_RO(stats);

static ssize_t pool_state_show(struct device *device,
			       struct device_attribute *attr, char *buf)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;
	unsigned long flags;
	int i, len = 0;
	int in_use_count = 0, ready_count = 0, pending_count = 0;
	struct cap_frame *f;

	if (!dev)
		return scnprintf(buf, PAGE_SIZE, "not initialized\n");

	spin_lock_irqsave(&dev->ready_lock, flags);

	/* Count ready and pending list entries */
	list_for_each_entry(f, &dev->ready_list, list)
		ready_count++;
	list_for_each_entry(f, &dev->pending_list, pending_node)
		pending_count++;

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "pool_size: %d\n"
			 "ready_list: %d\n"
			 "pending_list: %d\n"
			 "slot  in_use  refcount  vf_idx  phy_addr\n"
			 "----  ------  --------  ------  --------\n",
			 VFM_CAP_POOL_SIZE, ready_count, pending_count);

	for (i = 0; i < VFM_CAP_POOL_SIZE; i++) {
		struct cap_frame *frame = &dev->frame_pool[i];

		if (frame->in_use) {
			in_use_count++;
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%4d  %6d  %8d  %6u  0x%llx\n",
					 i, frame->in_use,
					 atomic_read(&frame->refcount),
					 frame->vf ? frame->vf->index : 0xFFFF,
					 (u64)frame->phy_addr);
		}
	}

	spin_unlock_irqrestore(&dev->ready_lock, flags);

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "in_use_total: %d / %d\n",
			 in_use_count, VFM_CAP_POOL_SIZE);

	return len;
}
static DEVICE_ATTR_RO(pool_state);

/**
 * pool_drain_store() - Force-release all stuck cap_frame slots
 *
 * Write any value to trigger. This is a last-resort debug tool that
 * force-recycles all in_use frames back to vdin0, regardless of refcount.
 * Only use when the pool is stuck and capture is frozen.
 */
static ssize_t pool_drain_store(struct device *device,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;
	unsigned long flags;

	if (!dev)
		return -ENODEV;

	vfm_cap_info("FORCE DRAIN: releasing all stuck cap_frames\n");

	/* Cancel any pending delivery work first */
	cancel_work_sync(&dev->deliver_work);

	spin_lock_irqsave(&dev->ready_lock, flags);
	frame_pool_recycle_all(dev);
	spin_unlock_irqrestore(&dev->ready_lock, flags);

	vfm_cap_info("FORCE DRAIN: complete\n");
	return count;
}
static DEVICE_ATTR_WO(pool_drain);

static struct attribute *vfm_cap_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_signal_info.attr,
	&dev_attr_stats.attr,
	&dev_attr_pool_state.attr,
	&dev_attr_pool_drain.attr,
	NULL,
};

static const struct attribute_group vfm_cap_attr_group = {
	.attrs = vfm_cap_attrs,
};

static const struct attribute_group *vfm_cap_attr_groups[] = {
	&vfm_cap_attr_group,
	NULL,
};

/* ========== Platform Device (for CMA allocation context) ========== */

static void vfm_cap_pdev_release(struct device *dev)
{
	/* No-op for static platform device */
}

static struct platform_device vfm_cap_pdev = {
	.name = VFM_CAP_MODULE_NAME,
	.id = -1,
	.dev = {
		.release = vfm_cap_pdev_release,
	},
};

/* ========== Module Init/Exit ========== */

static int __init vfm_cap_init(void)
{
	struct vfm_cap_dev *dev;
	struct video_device *vdev;
	int ret;

	vfm_cap_info("initializing (provider=%s, receiver=%s)\n",
		     provider_name, receiver_name);

	/* Register platform device for DMA context */
	ret = platform_device_register(&vfm_cap_pdev);
	if (ret) {
		vfm_cap_err("platform_device_register failed: %d\n", ret);
		return ret;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err_unreg_pdev;
	}

	dev->pdev = &vfm_cap_pdev;

	/* Initialize VFM names */
	strscpy(dev->recv_name, VFM_CAP_MODULE_NAME,
		sizeof(dev->recv_name));
	strscpy(dev->prov_name, VFM_CAP_MODULE_NAME,
		sizeof(dev->prov_name));

	/* Initialize locks and lists */
	spin_lock_init(&dev->ready_lock);
	INIT_LIST_HEAD(&dev->ready_list);
	INIT_LIST_HEAD(&dev->pending_list);
	INIT_LIST_HEAD(&dev->consumers);
	mutex_init(&dev->consumer_lock);
	spin_lock_init(&dev->consumer_spin);
	mutex_init(&dev->fmt_lock);
	spin_lock_init(&dev->fmt_spin);
	mutex_init(&dev->vdev_lock);
	init_waitqueue_head(&dev->wq);
	INIT_WORK(&dev->deliver_work, vfm_cap_deliver_work);
	INIT_DELAYED_WORK(&dev->sm_poll_work, vfm_cap_sm_poll_work);

	/* Initialize Phase 2 signal state */
	dev->sm_state = VFM_SM_NULL;
	dev->sm_polling = false;
	dev->draining = false;
	dev->standalone = false;
	dev->last_signal_type = 0;
	dev->prev_v4l2_frame = NULL;
	memset(&dev->sig_info, 0, sizeof(dev->sig_info));

	/* Initialize frame pool */
	frame_pool_init(dev);

	/* Initialize statistics */
	atomic64_set(&dev->stat_frames, 0);
	atomic64_set(&dev->stat_drops, 0);
	atomic64_set(&dev->stat_delivered, 0);

	/* Set up DMA mask on platform device */
	vfm_cap_pdev.dev.dma_mask = &vfm_cap_pdev.dev.coherent_dma_mask;
	dma_set_coherent_mask(&vfm_cap_pdev.dev, DMA_BIT_MASK(32));
	dma_set_mask(&vfm_cap_pdev.dev, DMA_BIT_MASK(32));

	/*
	 * Pre-fill v4l2_dev name before calling v4l2_device_register().
	 * Our platform device has no platform_driver bound, so dev->driver
	 * is NULL. v4l2_device_register() would dereference dev->driver->name
	 * to auto-generate the name, causing a NULL pointer crash.
	 * Setting the name here skips that code path.
	 */
	strscpy(dev->v4l2_dev.name, VFM_CAP_MODULE_NAME,
		sizeof(dev->v4l2_dev.name));

	/* Register V4L2 device */
	ret = v4l2_device_register(&vfm_cap_pdev.dev, &dev->v4l2_dev);
	if (ret) {
		vfm_cap_err("v4l2_device_register failed: %d\n", ret);
		goto err_free_dev;
	}

	/* Configure video device */
	vdev = &dev->vdev;
	strscpy(vdev->name, VFM_CAP_MODULE_NAME, sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &vfm_cap_fops;
	vdev->ioctl_ops = &vfm_cap_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &dev->vdev_lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE |
			    V4L2_CAP_STREAMING;
	vdev->vfl_type = VFL_TYPE_VIDEO;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->dev.groups = vfm_cap_attr_groups;
	video_set_drvdata(vdev, dev);

	/* Register video device */
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, vfm_cap_video_nr);
	if (ret) {
		vfm_cap_err("video_register_device failed: %d\n", ret);
		goto err_unreg_v4l2;
	}

	vfm_cap_info("registered as /dev/video%d\n", vdev->num);

	/*
	 * Register VFM receiver. The provider is registered lazily
	 * when VFRAME_EVENT_PROVIDER_START arrives from upstream.
	 */
	vf_receiver_init(&dev->vf_recv, dev->recv_name,
			 &vfm_cap_vf_receiver_ops, dev);
	ret = vf_reg_receiver(&dev->vf_recv);
	if (ret) {
		vfm_cap_err("vf_reg_receiver failed: %d\n", ret);
		goto err_unreg_video;
	}

	vfm_cap_info("VFM receiver '%s' registered\n", dev->recv_name);

	/* Start signal state monitoring (Phase 2a) */
	dev->sm_polling = true;
	schedule_delayed_work(&dev->sm_poll_work,
			      msecs_to_jiffies(VFM_CAP_SM_POLL_MS));
	vfm_cap_info("SM polling started (%ums interval)\n", VFM_CAP_SM_POLL_MS);

	g_vfm_cap_dev = dev;
	vfm_cap_info("init complete\n");
	return 0;

err_unreg_video:
	video_unregister_device(vdev);
err_unreg_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_free_dev:
	kfree(dev);
err_unreg_pdev:
	platform_device_unregister(&vfm_cap_pdev);
	return ret;
}

static void __exit vfm_cap_exit(void)
{
	struct vfm_cap_dev *dev = g_vfm_cap_dev;

	if (!dev)
		return;

	vfm_cap_info("exiting\n");

	/* Stop signal monitoring */
	dev->sm_polling = false;
	cancel_delayed_work_sync(&dev->sm_poll_work);

	/* Cancel pending V4L2 delivery work */
	cancel_work_sync(&dev->deliver_work);

	/* Unregister VFM provider if active (not in standalone mode) */
	if (dev->vfm_started && !dev->standalone) {
		vf_unreg_provider(&dev->vf_prov);
		dev->vfm_started = false;
	}

	/*
	 * Flush frame pool BEFORE unregistering VFM receiver, so that
	 * vf_put() calls inside frame_pool_recycle_all() go through
	 * the still-registered receiver path.
	 */
	{
		unsigned long flags;

		spin_lock_irqsave(&dev->ready_lock, flags);
		frame_pool_recycle_all(dev);
		spin_unlock_irqrestore(&dev->ready_lock, flags);
	}

	/* Unregister VFM receiver */
	vf_unreg_receiver(&dev->vf_recv);

	/* Unregister V4L2 */
	video_unregister_device(&dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);

	/* Unregister platform device */
	platform_device_unregister(&vfm_cap_pdev);

	kfree(dev);
	g_vfm_cap_dev = NULL;

	vfm_cap_info("exit complete\n");
}

module_init(vfm_cap_init);
module_exit(vfm_cap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("StreamBox");
MODULE_DESCRIPTION("VFM Capture Tee - Zero-copy V4L2 access to VFM frames");
