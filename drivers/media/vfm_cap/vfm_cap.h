/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * vfm_cap.h - VFM Capture Tee Module (Phase 2c - Zero-Copy)
 *
 * Sits in the VFM chain (vdin0 -> vfm_cap -> amlvideo2.0 -> deinterlace -> amvideo)
 * and exposes a V4L2 capture device with:
 *   - Zero-copy DMA-buf export of vdin0's CMA frame buffers
 *   - V4L2_EVENT_SOURCE_CHANGE for signal detection
 *   - 10-bit format awareness (reports Amlogic-native pixel formats)
 *   - Transparent passthrough to display path
 *
 * Zero-copy flow:
 *   1. DQBUF returns buffer index + timestamp/sequence metadata
 *   2. ioctl(VFM_CAP_IOC_GET_DMABUF, &index) returns DMA-buf fd for that frame
 *   3. Consumer imports DMA-buf fd (Vulkan VkImportMemoryFdInfoKHR, etc.)
 *   4. QBUF releases the frame back to vdin0 (consumer must close the fd first)
 *
 * Copyright (C) 2026 StreamBox
 */

#ifndef VFM_CAP_H
#define VFM_CAP_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>

#define VFM_CAP_MODULE_NAME	"vfm_cap"
#define VFM_CAP_MAX_CONSUMERS	4
#define VFM_CAP_POOL_SIZE	16	/* max vframes in flight */
#define VFM_CAP_NAME_SIZE	32
#define VFM_CAP_DMABUF_CACHE	16	/* max cached DMA-buf exports */

/* ---------- Custom ioctl: Zero-copy DMA-buf export ---------- */

/*
 * VFM_CAP_IOC_GET_DMABUF - Get a DMA-buf fd for a DQBUF'd frame
 *
 * After VIDIOC_DQBUF, call this ioctl with the buffer index to get a
 * DMA-buf file descriptor pointing directly to vdin0's CMA frame buffer.
 * The fd can be imported into Vulkan, OpenGL, or any DMA-buf consumer.
 *
 * The DMA-buf fd MUST be closed before VIDIOC_QBUF of the same buffer.
 * The fd holds a reference on the vdin0 frame; closing it allows the
 * frame to be recycled back to vdin0.
 *
 * Input/output: struct vfm_cap_dmabuf_req (index in, fd + size out)
 */
struct vfm_cap_dmabuf_req {
	__u32 index;		/* in: vb2 buffer index from DQBUF */
	__s32 fd;		/* out: DMA-buf file descriptor */
	__u32 size;		/* out: buffer size in bytes */
	__u32 reserved;		/* must be 0 */
};

#define VFM_CAP_IOC_GET_DMABUF	_IOWR('V', 192, struct vfm_cap_dmabuf_req)

/* ---------- Signal monitoring ---------- */

/*
 * SM poll interval: 100ms matches vdin's 10ms timer but with lower
 * overhead since we only need to detect state transitions, not sub-frame
 * timing. Fast enough to detect NOSIG within ~200ms.
 */
#define VFM_CAP_SM_POLL_MS	100

/*
 * vdin signal state machine states.
 * Defined here to avoid depending on private vdin_sm.h header.
 * These must match enum tvin_sm_status_e in vdin_sm.h.
 */
enum vfm_cap_sm_status {
	VFM_SM_NULL = 0,
	VFM_SM_NOSIG,
	VFM_SM_UNSTABLE,
	VFM_SM_NOTSUP,
	VFM_SM_PRESTABLE,
	VFM_SM_STABLE,
};

/* Extern declaration for the exported vdin function */
extern int tvin_get_sm_status(int index);

/* ---------- V4L2 Private Events ---------- */

/*
 * Private event types for in-STABLE signal changes.
 * These don't require pipeline restart but are informational.
 */
#define VFM_CAP_EVENT_VRR	(V4L2_EVENT_PRIVATE_START + 1)
#define VFM_CAP_EVENT_ALLM	(V4L2_EVENT_PRIVATE_START + 2)
#define VFM_CAP_EVENT_FPS	(V4L2_EVENT_PRIVATE_START + 3)

/* ---------- Signal Info (V4L2 event payload) ---------- */

/*
 * Packed into v4l2_event.u.data[64] via SOURCE_CHANGE event.
 * Consumers get all new parameters from a single VIDIOC_DQEVENT.
 * Total size: 36 bytes (fits in 64-byte event data union).
 */
struct vfm_cap_signal_info {
	__u32 width;
	__u32 height;
	__u32 fps;		/* frames/sec * 1000 (e.g., 59940 = 59.94fps) */
	__u32 color_format;	/* V4L2_PIX_FMT_* */
	__u32 signal_type;	/* HDR/DV/colorimetry from vframe.signal_type */
	__u32 hdr_status;	/* 0=SDR, 1=HDR10, 2=HLG, 3=HDR10+, 4=DV */
	__u32 is_interlaced;
	__u32 status;		/* 0=STABLE, 1=NOSIG, 2=NOTSUP */
	__u32 bitdepth;		/* 8, 10, or 12 */
};

#define VFM_CAP_SIG_STATUS_STABLE	0
#define VFM_CAP_SIG_STATUS_NOSIG	1
#define VFM_CAP_SIG_STATUS_NOTSUP	2

/* ---------- Amlogic Private V4L2 Pixel Formats ---------- */

/*
 * Amlogic 40-bit packed YUV422 10-bit format.
 * Layout: [U0(10)][Y0(10)][V0(10)][Y1(10)] = 40 bits per pixel pair.
 * Bytesperline = width * 5 / 2
 * Matches V4L2_PIX_FMT_AMLOGIC_YUV422_10BIT_PACKED ('AMLY') from vdin.
 */
#ifndef V4L2_PIX_FMT_AML_YUV422_10BIT
#define V4L2_PIX_FMT_AML_YUV422_10BIT	v4l2_fourcc('A', 'M', 'L', 'Y')
#endif

/* ---------- DMA-buf export from vdin0 CMA buffers ---------- */

/**
 * struct vfm_cap_dmabuf - Private data for an exported DMA-buf
 * @paddr:     Physical address of the CMA buffer (from vf->canvas0_config)
 * @size:      Buffer size in bytes (PAGE_ALIGN'd)
 * @frame:     Back-pointer to cap_frame (holds vframe reference)
 * @dev:       Back-pointer to vfm_cap_dev
 *
 * Allocated per DMA-buf export, freed in dma_buf_ops.release.
 * The cap_frame refcount is incremented when this is created and
 * decremented when the DMA-buf is released (consumer closes fd).
 */
struct vfm_cap_dmabuf {
	phys_addr_t		paddr;
	size_t			size;
	struct cap_frame	*frame;
	struct vfm_cap_dev	*dev;
};

/**
 * struct vfm_cap_dmabuf_attach - Per-attachment state for DMA-buf
 */
struct vfm_cap_dmabuf_attach {
	struct sg_table		sgt;
	enum dma_data_direction	dma_dir;
};

/* ---------- Per-frame wrapper with refcounting ---------- */

/**
 * struct cap_frame - Wraps a vframe_s with reference counting
 * @vf:           Original vframe pointer from upstream (vdin0)
 * @refcount:     1 (display path) + N (one per outstanding DMA-buf export)
 * @list:         Linkage in ready_list (for downstream display path)
 * @pending_node: Linkage in pending_list (for V4L2 delivery workqueue)
 * @acquired_at:  Timestamp when frame was obtained from upstream
 * @index:        Slot index in the frame pool (0..VFM_CAP_POOL_SIZE-1)
 * @in_use:       True if this slot currently holds a vframe
 * @phy_addr:     Cached physical address of the CMA buffer
 * @buf_size:     Cached buffer size in bytes
 */
struct cap_frame {
	struct vframe_s		*vf;
	atomic_t		refcount;
	struct list_head	list;
	struct list_head	pending_node;
	ktime_t			acquired_at;
	unsigned int		index;
	bool			in_use;
	phys_addr_t		phy_addr;
	size_t			buf_size;
};

/* ---------- V4L2 buffer wrapper ---------- */

/**
 * struct vfm_cap_buffer - Wraps vb2_v4l2_buffer for our queue
 * @vb:      V4L2 vb2 buffer (must be first for container_of)
 * @list:    Linkage in queued_list within a consumer
 * @frame:   Pointer to the cap_frame this buffer is delivering
 * @dbuf:    DMA-buf exported for this frame (NULL if not yet exported)
 * @dbuf_fd: File descriptor for the DMA-buf (-1 if not exported)
 *
 * In zero-copy mode, the vb2 MMAP buffer is a flow-control token only.
 * The actual frame data is accessed via the DMA-buf fd obtained through
 * VFM_CAP_IOC_GET_DMABUF after DQBUF.
 */
struct vfm_cap_buffer {
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
	struct cap_frame	*frame;
	struct dma_buf		*dbuf;
	int			dbuf_fd;
};

static inline struct vfm_cap_buffer *to_vfm_cap_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct vfm_cap_buffer, vb);
}

/* ---------- Per-consumer (per-open) context ---------- */

/**
 * struct vfm_cap_consumer - One per open() on /dev/video_cap
 * @fh:          V4L2 file handle (for events, priority)
 * @dev:         Back-pointer to parent device
 * @vb2q:        Independent vb2 queue for this consumer
 * @queued_list: Buffers queued by userspace (QBUF'd), waiting for frames
 * @list:        Linkage in dev->consumers list
 * @streaming:   True after STREAMON
 * @id:          Consumer ID for debug
 * @lock:        Serializes ioctl access to this consumer's queue
 * @queue_lock:  Protects queued_list
 * @frame_count: Number of frames delivered to this consumer
 */
struct vfm_cap_consumer {
	struct v4l2_fh		fh;
	struct vfm_cap_dev	*dev;
	struct vb2_queue	vb2q;
	struct list_head	queued_list;
	struct list_head	list;
	bool			streaming;
	unsigned int		id;
	struct mutex		lock;
	spinlock_t		queue_lock;
	u64			frame_count;
};

/* ---------- Main device context ---------- */

/**
 * struct vfm_cap_dev - The singleton device context
 */
struct vfm_cap_dev {
	/* VFM */
	struct vframe_receiver_s	vf_recv;
	struct vframe_provider_s	vf_prov;
	char				recv_name[VFM_CAP_NAME_SIZE];
	char				prov_name[VFM_CAP_NAME_SIZE];
	bool				vfm_started;

	/* Frame pool */
	struct cap_frame		frame_pool[VFM_CAP_POOL_SIZE];
	struct list_head		ready_list;
	struct list_head		pending_list;
	spinlock_t			ready_lock;
	struct vframe_s			*pool_backing[VFM_CAP_POOL_SIZE + 1];

	/* V4L2 */
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct mutex			vdev_lock; /* serializes V4L2 ioctls */
	struct list_head		consumers;
	struct mutex			consumer_lock; /* serializes open/close */
	spinlock_t			consumer_spin; /* IRQ-safe consumer iteration */
	unsigned int			num_consumers;
	unsigned int			consumer_id_gen;

	/* Current format (from upstream vframes) */
	u32				width;
	u32				height;
	u32				pixelformat;
	u32				bytesperline[VIDEO_MAX_PLANES];
	u32				sizeimage[VIDEO_MAX_PLANES];
	u32				num_planes;
	u32				colorspace;
	u32				field;
	u32				bitdepth;    /* raw vf->bitdepth value */
	u32				bitdepth_y;  /* 8, 10, or 12 */
	struct mutex			fmt_lock;
	spinlock_t			fmt_spin; /* IRQ-safe format updates */
	bool				fmt_valid;

	/* Signal state monitoring (Phase 2) */
	struct delayed_work		sm_poll_work;
	enum vfm_cap_sm_status		sm_state;      /* last known SM state */
	bool				sm_polling;    /* true if poll work is active */
	bool				draining;      /* true during signal change drain */
	u32				last_signal_type; /* vf->signal_type for change detection */
	struct vfm_cap_signal_info	sig_info;      /* current signal params for events */

	/* Statistics */
	atomic64_t			stat_frames;
	atomic64_t			stat_drops;
	atomic64_t			stat_delivered;

	/* Wait queue */
	wait_queue_head_t		wq;

	/* Work for deferred frame delivery (not safe in ISR context) */
	struct work_struct		deliver_work;

	/* Platform device (for DMA alloc) */
	struct platform_device		*pdev;
};

/* ---------- Module parameters (declared in vfm_cap.c) ---------- */

extern unsigned int vfm_cap_max_consumers;
extern int vfm_cap_video_nr;
extern unsigned int vfm_cap_debug;

/* Debug print macro */
#define vfm_cap_dbg(level, fmt, ...) \
	do { \
		if (vfm_cap_debug >= (level)) \
			pr_info("[vfm_cap] " fmt, ##__VA_ARGS__); \
	} while (0)

#define vfm_cap_err(fmt, ...) \
	pr_err("[vfm_cap] ERROR: " fmt, ##__VA_ARGS__)

#define vfm_cap_info(fmt, ...) \
	pr_info("[vfm_cap] " fmt, ##__VA_ARGS__)

#endif /* VFM_CAP_H */
