/*
 * test_dmabuf.c - Test VFM_CAP_IOC_GET_DMABUF zero-copy ioctl
 *
 * Opens /dev/video_cap, starts streaming, DQBUFs frames,
 * retrieves DMA-buf fds via custom ioctl, mmaps them to verify
 * frame data is accessible, then QBUFs back.
 *
 * Usage: ./test_dmabuf [num_frames]
 *
 * Build (cross-compile):
 *   aarch64-poky-linux-gcc -o test_dmabuf test_dmabuf.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>

/* Custom ioctl - must match kernel header */
struct vfm_cap_dmabuf_req {
	__u32 index;
	__s32 fd;
	__u32 size;
	__u32 reserved;
};

#define VFM_CAP_IOC_GET_DMABUF	_IOWR('V', 192, struct vfm_cap_dmabuf_req)

#define DEVICE	"/dev/video_cap"
#define NUM_BUFS 4

struct buffer_info {
	void *start;
	size_t length;
};

static struct buffer_info buffers[NUM_BUFS];

int main(int argc, char **argv)
{
	int fd, ret, i;
	int num_frames = 5;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];
	enum v4l2_buf_type type;

	if (argc > 1)
		num_frames = atoi(argv[1]);

	printf("Opening %s...\n", DEVICE);
	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Query current format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		perror("VIDIOC_G_FMT");
		close(fd);
		return 1;
	}
	printf("Format: %ux%u pixfmt=0x%08x sizeimage=%u bytesperline=%u\n",
	       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	       fmt.fmt.pix_mp.pixelformat,
	       fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	       fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

	/* Request buffers */
	memset(&req, 0, sizeof(req));
	req.count = NUM_BUFS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		perror("VIDIOC_REQBUFS");
		close(fd);
		return 1;
	}
	printf("Got %u buffers (flow-control tokens)\n", req.count);

	/* MMAP the flow-control token buffers (needed for QBUF/DQBUF) */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = 1;
		buf.m.planes = planes;
		ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QUERYBUF");
			close(fd);
			return 1;
		}

		buffers[i].length = planes[0].length;
		buffers[i].start = mmap(NULL, planes[0].length,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					fd, planes[0].m.mem_offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return 1;
		}
	}

	/* Queue all buffers */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = 1;
		buf.m.planes = planes;
		ret = ioctl(fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QBUF");
			close(fd);
			return 1;
		}
	}

	/* Start streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		perror("VIDIOC_STREAMON");
		close(fd);
		return 1;
	}
	printf("Streaming started. Capturing %d frames...\n", num_frames);

	/* Capture loop */
	for (i = 0; i < num_frames; i++) {
		struct vfm_cap_dmabuf_req dmabuf_req;
		void *frame_data;
		struct dma_buf_sync sync;

		/* DQBUF */
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.length = 1;
		buf.m.planes = planes;
		ret = ioctl(fd, VIDIOC_DQBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}

		printf("Frame %d: buf.index=%u seq=%u bytesused=%u\n",
		       i, buf.index, buf.sequence, planes[0].bytesused);

		/* Get DMA-buf fd via custom ioctl */
		memset(&dmabuf_req, 0, sizeof(dmabuf_req));
		dmabuf_req.index = buf.index;
		ret = ioctl(fd, VFM_CAP_IOC_GET_DMABUF, &dmabuf_req);
		if (ret < 0) {
			perror("VFM_CAP_IOC_GET_DMABUF");
			/* Still QBUF back */
			goto qbuf;
		}

		printf("  DMA-buf fd=%d size=%u\n", dmabuf_req.fd, dmabuf_req.size);

		/* Verify: mmap the DMA-buf fd and read first bytes */
		if (dmabuf_req.fd >= 0 && dmabuf_req.size > 0) {
			/* DMA_BUF_SYNC_START before read */
			memset(&sync, 0, sizeof(sync));
			sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
			ioctl(dmabuf_req.fd, DMA_BUF_IOCTL_SYNC, &sync);

			frame_data = mmap(NULL, dmabuf_req.size,
					  PROT_READ, MAP_SHARED,
					  dmabuf_req.fd, 0);
			if (frame_data != MAP_FAILED) {
				unsigned char *p = (unsigned char *)frame_data;
				printf("  First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
				       "%02x %02x %02x %02x %02x %02x %02x %02x\n",
				       p[0], p[1], p[2], p[3],
				       p[4], p[5], p[6], p[7],
				       p[8], p[9], p[10], p[11],
				       p[12], p[13], p[14], p[15]);

				/* Check for non-zero data (real frame data vs zeros) */
				int nonzero = 0;
				for (int j = 0; j < 256 && j < (int)dmabuf_req.size; j++)
					if (p[j] != 0) nonzero++;
				printf("  Non-zero bytes in first 256: %d\n", nonzero);

				munmap(frame_data, dmabuf_req.size);
			} else {
				perror("  mmap DMA-buf");
			}

			/* DMA_BUF_SYNC_END after read */
			sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
			ioctl(dmabuf_req.fd, DMA_BUF_IOCTL_SYNC, &sync);

			/* Close the DMA-buf fd */
			close(dmabuf_req.fd);
		}

qbuf:
		/* QBUF */
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = dmabuf_req.index;
		buf.length = 1;
		buf.m.planes = planes;
		ret = ioctl(fd, VIDIOC_QBUF, &buf);
		if (ret < 0) {
			perror("VIDIOC_QBUF (re-queue)");
			break;
		}
	}

	/* Stop streaming */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);

	/* Unmap token buffers */
	for (i = 0; i < (int)req.count; i++) {
		if (buffers[i].start && buffers[i].start != MAP_FAILED)
			munmap(buffers[i].start, buffers[i].length);
	}

	close(fd);
	printf("Done.\n");
	return 0;
}
