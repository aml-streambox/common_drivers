/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __AML_COMPAT_OF_GPIO_H__
#define __AML_COMPAT_OF_GPIO_H__

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>

enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW = 0x1,
	OF_GPIO_SINGLE_ENDED = 0x2,
	OF_GPIO_OPEN_DRAIN = 0x4,
	OF_GPIO_TRANSITORY = 0x8,
	OF_GPIO_PULL_UP = 0x10,
	OF_GPIO_PULL_DOWN = 0x20,
	OF_GPIO_PULL_DISABLE = 0x40,
};

static inline int of_get_named_gpio_flags(const struct device_node *np,
					 const char *propname, int index,
					 enum of_gpio_flags *flags)
{
#if defined(CONFIG_OF_GPIO) && defined(CONFIG_GPIOLIB)
	struct of_phandle_args gpiospec;
	struct gpio_device *gdev;
	struct gpio_chip *gc;
	struct gpio_desc *desc;
	u32 of_flags = 0;
	int ret;

	ret = of_parse_phandle_with_args_map(np, propname, "gpio", index,
					  &gpiospec);
	if (ret)
		return ret;

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(gpiospec.np));
	if (!gdev) {
		ret = -EPROBE_DEFER;
		goto out_node;
	}

	gc = gpio_device_get_chip(gdev);
	if (!gc || !gc->of_xlate || gc->of_gpio_n_cells != gpiospec.args_count) {
		ret = -EINVAL;
		goto out_gdev;
	}

	ret = gc->of_xlate(gc, &gpiospec, &of_flags);
	if (ret < 0)
		goto out_gdev;

	desc = gpio_device_get_desc(gdev, ret);
	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		goto out_gdev;
	}

	if (flags)
		*flags = of_flags;
	ret = desc_to_gpio(desc);

out_gdev:
	gpio_device_put(gdev);
out_node:
	of_node_put(gpiospec.np);
	return ret;
#else
	return -ENOENT;
#endif
}

#endif /* __AML_COMPAT_OF_GPIO_H__ */
