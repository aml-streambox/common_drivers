/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Narrow compatibility helpers for building the selected Amlogic vendor display
 * stack against the local Linux v7.1 tree.
 */

#ifndef __AML_KERNEL_COMPAT_H__
#define __AML_KERNEL_COMPAT_H__

#include <linux/version.h>
#include <linux/cma.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-heap.h>
#include <linux/dma-map-ops.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/iosys-map.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/amlogic/pm.h>

#define __aml_class_create_1(name) class_create(name)
#define __aml_class_create_2(owner, name) class_create(name)
#define __aml_class_create_pick(_1, _2, name, ...) name

#define class_create(...) \
	__aml_class_create_pick(__VA_ARGS__, __aml_class_create_2, \
				  __aml_class_create_1)(__VA_ARGS__)

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif

#ifndef del_timer_sync
#define del_timer_sync(timer) timer_delete_sync(timer)
#endif

#ifndef strlcpy
#define strlcpy(dest, src, size) strscpy(dest, src, size)
#endif

#ifndef ida_simple_get
#define ida_simple_get(ida, start, end, gfp) ida_alloc_range(ida, start, (end) ? (end) - 1 : ~0U, gfp)
#endif

#ifndef ida_simple_remove
#define ida_simple_remove(ida, id) ida_free(ida, id)
#endif

#ifndef prandom_bytes
#define prandom_bytes(buf, bytes) get_random_bytes(buf, bytes)
#endif

#define dma_buf_map iosys_map
#define dma_buf_map_set_vaddr iosys_map_set_vaddr

static inline int aml_sched_setscheduler(struct task_struct *p, int policy,
						 struct sched_param *param)
{
	if (policy == SCHED_FIFO) {
		sched_set_fifo(p);
		return 0;
	}

	return -EINVAL;
}

#define sched_setscheduler(p, policy, param) \
	aml_sched_setscheduler(p, policy, param)

static inline struct page *aml_dma_alloc_from_contiguous(struct device *dev,
							 size_t count,
							 unsigned int align,
							 bool no_warn)
{
	if (align > CONFIG_CMA_ALIGNMENT)
		align = CONFIG_CMA_ALIGNMENT;

	return cma_alloc(dev_get_cma_area(dev), count, align, no_warn);
}

static inline bool aml_dma_release_from_contiguous(struct device *dev,
						       struct page *pages,
						       int count)
{
	return cma_release(dev_get_cma_area(dev), pages, count);
}

#endif /* __AML_KERNEL_COMPAT_H__ */
