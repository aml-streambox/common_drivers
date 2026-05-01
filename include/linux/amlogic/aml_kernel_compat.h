/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Narrow compatibility helpers for building the selected Amlogic vendor display
 * stack against the local Linux v7.1 tree.
 */

#ifndef __AML_KERNEL_COMPAT_H__
#define __AML_KERNEL_COMPAT_H__

#include <linux/version.h>
#include <linux/vmalloc.h>

#define __aml_class_create_1(name) class_create(name)
#define __aml_class_create_2(owner, name) class_create(name)
#define __aml_class_create_pick(_1, _2, name, ...) name

#define class_create(...) \
	__aml_class_create_pick(__VA_ARGS__, __aml_class_create_2, \
				  __aml_class_create_1)(__VA_ARGS__)

#endif /* __AML_KERNEL_COMPAT_H__ */
