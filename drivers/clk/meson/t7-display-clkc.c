// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Amlogic T7 display clock controller.
 *
 * This exposes only the vendor T7 display clock IDs needed by the local
 * display forward-port. It deliberately avoids the full vendor t7-clkc probe,
 * which also registers PLL and CPU clocks.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/clock/amlogic,t7-display-clkc.h>

#define T7_DCLK_HDMI_CLK_CTRL	0x00
#define T7_DCLK_VPU_CLK_CTRL	0x08
#define T7_DCLK_VPU_CLKB_CTRL	0x0c
#define T7_DCLK_VPU_CLKC_CTRL	0x10
#define T7_DCLK_VID_LOCK_CLK_CTRL	0x14
#define T7_DCLK_VAPBCLK_CTRL	0x1c
#define T7_DCLK_HTX_CLK_CTRL0	0x3c

struct t7_display_clkc {
	void __iomem *base;
	spinlock_t lock;
	struct clk_hw_onecell_data *data;
};

static const struct clk_parent_data t7_vpu_parent_data[] = {
	{ .fw_name = "fdiv3" },
	{ .fw_name = "fdiv4" },
	{ .fw_name = "fdiv5" },
	{ .fw_name = "fdiv7" },
};

static const struct clk_parent_data t7_vpu_clkc_parent_data[] = {
	{ .fw_name = "fdiv4" },
	{ .fw_name = "fdiv3" },
	{ .fw_name = "fdiv5" },
	{ .fw_name = "fdiv7" },
};

static const u32 t7_vapb_parent_table[] = { 0, 1, 2, 3, 7 };

static const struct clk_parent_data t7_vapb_parent_data[] = {
	{ .fw_name = "fdiv4" },
	{ .fw_name = "fdiv3" },
	{ .fw_name = "fdiv5" },
	{ .fw_name = "fdiv7" },
	{ .fw_name = "fdiv2p5" },
};

static const struct clk_parent_data t7_hdmitx_parent_data[] = {
	{ .fw_name = "xtal" },
	{ .fw_name = "fdiv4" },
	{ .fw_name = "fdiv3" },
	{ .fw_name = "fdiv5" },
};

static const struct clk_parent_data t7_vid_lock_parent_data[] = {
	{ .fw_name = "xtal" },
};

static const char * const t7_vpu_mux_parent_names[] = {
	"t7_display_vpu_0",
	"t7_display_vpu_1",
};

static const char * const t7_vpu_clkb_tmp_parent_names[] = {
	"t7_display_vpu",
	"fdiv4",
	"fdiv5",
	"fdiv7",
};

static const char * const t7_vpu_clkc_mux_parent_names[] = {
	"t7_display_vpu_clkc_p0",
	"t7_display_vpu_clkc_p1",
};

static const char * const t7_vapb_mux_parent_names[] = {
	"t7_display_vapb_0",
	"t7_display_vapb_1",
};

static int t7_display_add_mux_parent_data(struct device *dev,
					  struct t7_display_clkc *clkc,
					  unsigned int id, const char *name,
					  const struct clk_parent_data *parents,
					  u8 num_parents, unsigned int reg,
					  u8 shift, u8 width,
					  const u32 *table,
					  unsigned long flags)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_mux_parent_data_table(dev, name, parents,
			num_parents, flags, clkc->base + reg, shift, width, 0,
			table, &clkc->lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "failed to register %s\n",
				     name);

	clkc->data->hws[id] = hw;
	return 0;
}

static int t7_display_add_mux_parent_names(struct device *dev,
					   struct t7_display_clkc *clkc,
					   unsigned int id, const char *name,
					   const char * const *parents,
					   u8 num_parents, unsigned int reg,
					   u8 shift, u8 width,
					   unsigned long flags)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_mux(dev, name, parents, num_parents, flags,
					    clkc->base + reg, shift, width, 0,
					    &clkc->lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "failed to register %s\n",
				     name);

	clkc->data->hws[id] = hw;
	return 0;
}

static int t7_display_add_divider(struct device *dev,
				  struct t7_display_clkc *clkc,
				  unsigned int id, const char *name,
				  struct clk_hw *parent, unsigned int reg,
				  u8 shift, u8 width, unsigned long flags)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_divider_parent_hw(dev, name, parent, flags,
			clkc->base + reg, shift, width, 0, &clkc->lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "failed to register %s\n",
				     name);

	clkc->data->hws[id] = hw;
	return 0;
}

static int t7_display_add_gate(struct device *dev,
			       struct t7_display_clkc *clkc,
			       unsigned int id, const char *name,
			       struct clk_hw *parent, unsigned int reg,
			       u8 bit_idx, unsigned long flags)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_gate_parent_hw(dev, name, parent, flags,
			clkc->base + reg, bit_idx, 0, &clkc->lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "failed to register %s\n",
				     name);

	clkc->data->hws[id] = hw;
	return 0;
}

#define T7_HW(_id) clkc->data->hws[CLKID_T7_DISPLAY_##_id]

static int t7_display_register_vpu(struct device *dev,
				   struct t7_display_clkc *clkc)
{
	int ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VPU_0_MUX, "t7_display_vpu_0_sel",
		t7_vpu_parent_data, ARRAY_SIZE(t7_vpu_parent_data),
		T7_DCLK_VPU_CLK_CTRL, 9, 2, NULL, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc, CLKID_T7_DISPLAY_VPU_0_DIV,
		"t7_display_vpu_0_div", T7_HW(VPU_0_MUX),
		T7_DCLK_VPU_CLK_CTRL, 0, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_0,
		"t7_display_vpu_0", T7_HW(VPU_0_DIV), T7_DCLK_VPU_CLK_CTRL,
		8, CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED);
	if (ret)
		return ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VPU_1_MUX, "t7_display_vpu_1_sel",
		t7_vpu_parent_data, ARRAY_SIZE(t7_vpu_parent_data),
		T7_DCLK_VPU_CLK_CTRL, 25, 2, NULL, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc, CLKID_T7_DISPLAY_VPU_1_DIV,
		"t7_display_vpu_1_div", T7_HW(VPU_1_MUX),
		T7_DCLK_VPU_CLK_CTRL, 16, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_1,
		"t7_display_vpu_1", T7_HW(VPU_1_DIV), T7_DCLK_VPU_CLK_CTRL,
		24, CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED);
	if (ret)
		return ret;

	return t7_display_add_mux_parent_names(dev, clkc,
		CLKID_T7_DISPLAY_VPU, "t7_display_vpu",
		t7_vpu_mux_parent_names, ARRAY_SIZE(t7_vpu_mux_parent_names),
		T7_DCLK_VPU_CLK_CTRL, 31, 1, CLK_SET_RATE_NO_REPARENT);
}

static int t7_display_register_vpu_clkb(struct device *dev,
					struct t7_display_clkc *clkc)
{
	int ret;

	ret = t7_display_add_mux_parent_names(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKB_TMP_MUX,
		"t7_display_vpu_clkb_tmp_mux", t7_vpu_clkb_tmp_parent_names,
		ARRAY_SIZE(t7_vpu_clkb_tmp_parent_names), T7_DCLK_VPU_CLKB_CTRL,
		20, 2, CLK_GET_RATE_NOCACHE);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKB_TMP_DIV,
		"t7_display_vpu_clkb_tmp_div", T7_HW(VPU_CLKB_TMP_MUX),
		T7_DCLK_VPU_CLKB_CTRL, 16, 4,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_CLKB_TMP,
		"t7_display_vpu_clkb_tmp", T7_HW(VPU_CLKB_TMP_DIV),
		T7_DCLK_VPU_CLKB_CTRL, 24,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc, CLKID_T7_DISPLAY_VPU_CLKB_DIV,
		"t7_display_vpu_clkb_div", T7_HW(VPU_CLKB_TMP),
		T7_DCLK_VPU_CLKB_CTRL, 0, 8,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	return t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_CLKB,
		"t7_display_vpu_clkb", T7_HW(VPU_CLKB_DIV),
		T7_DCLK_VPU_CLKB_CTRL, 8,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
}

static int t7_display_register_vpu_clkc(struct device *dev,
					struct t7_display_clkc *clkc)
{
	int ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKC_P0_MUX,
		"t7_display_vpu_clkc_p0_mux", t7_vpu_clkc_parent_data,
		ARRAY_SIZE(t7_vpu_clkc_parent_data), T7_DCLK_VPU_CLKC_CTRL,
		9, 2, NULL, CLK_GET_RATE_NOCACHE);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKC_P0_DIV,
		"t7_display_vpu_clkc_p0_div", T7_HW(VPU_CLKC_P0_MUX),
		T7_DCLK_VPU_CLKC_CTRL, 0, 7,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_CLKC_P0,
		"t7_display_vpu_clkc_p0", T7_HW(VPU_CLKC_P0_DIV),
		T7_DCLK_VPU_CLKC_CTRL, 8,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKC_P1_MUX,
		"t7_display_vpu_clkc_p1_mux", t7_vpu_clkc_parent_data,
		ARRAY_SIZE(t7_vpu_clkc_parent_data), T7_DCLK_VPU_CLKC_CTRL,
		25, 2, NULL, CLK_GET_RATE_NOCACHE);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKC_P1_DIV,
		"t7_display_vpu_clkc_p1_div", T7_HW(VPU_CLKC_P1_MUX),
		T7_DCLK_VPU_CLKC_CTRL, 16, 7,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VPU_CLKC_P1,
		"t7_display_vpu_clkc_p1", T7_HW(VPU_CLKC_P1_DIV),
		T7_DCLK_VPU_CLKC_CTRL, 24,
		CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	return t7_display_add_mux_parent_names(dev, clkc,
		CLKID_T7_DISPLAY_VPU_CLKC_MUX, "t7_display_vpu_clkc_mux",
		t7_vpu_clkc_mux_parent_names,
		ARRAY_SIZE(t7_vpu_clkc_mux_parent_names), T7_DCLK_VPU_CLKC_CTRL,
		31, 1, CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT);
}

static int t7_display_register_vapb(struct device *dev,
				    struct t7_display_clkc *clkc)
{
	int ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VAPB_0_MUX, "t7_display_vapb_0_sel",
		t7_vapb_parent_data, ARRAY_SIZE(t7_vapb_parent_data),
		T7_DCLK_VAPBCLK_CTRL, 9, 3, t7_vapb_parent_table, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc, CLKID_T7_DISPLAY_VAPB_0_DIV,
		"t7_display_vapb_0_div", T7_HW(VAPB_0_MUX),
		T7_DCLK_VAPBCLK_CTRL, 0, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VAPB_0,
		"t7_display_vapb_0", T7_HW(VAPB_0_DIV), T7_DCLK_VAPBCLK_CTRL,
		8, CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED);
	if (ret)
		return ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_VAPB_1_MUX, "t7_display_vapb_1_sel",
		t7_vapb_parent_data, ARRAY_SIZE(t7_vapb_parent_data),
		T7_DCLK_VAPBCLK_CTRL, 25, 3, NULL, CLK_SET_RATE_NO_REPARENT);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc, CLKID_T7_DISPLAY_VAPB_1_DIV,
		"t7_display_vapb_1_div", T7_HW(VAPB_1_MUX),
		T7_DCLK_VAPBCLK_CTRL, 16, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VAPB_1,
		"t7_display_vapb_1", T7_HW(VAPB_1_DIV), T7_DCLK_VAPBCLK_CTRL,
		24, CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED);
	if (ret)
		return ret;

	return t7_display_add_mux_parent_names(dev, clkc,
		CLKID_T7_DISPLAY_VAPB, "t7_display_vapb",
		t7_vapb_mux_parent_names, ARRAY_SIZE(t7_vapb_mux_parent_names),
		T7_DCLK_VAPBCLK_CTRL, 31, 1,
		CLK_SET_RATE_NO_REPARENT | CLK_SET_RATE_PARENT);
}

static int t7_display_register_hdmitx(struct device *dev,
				      struct t7_display_clkc *clkc)
{
	int ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_SYS_MUX,
		"t7_display_hdmitx_sys_sel", t7_hdmitx_parent_data,
		ARRAY_SIZE(t7_hdmitx_parent_data), T7_DCLK_HDMI_CLK_CTRL,
		9, 2, NULL, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_SYS_DIV,
		"t7_display_hdmitx_sys_div", T7_HW(HDMITX_SYS_MUX),
		T7_DCLK_HDMI_CLK_CTRL, 0, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_HDMITX_SYS,
		"t7_display_hdmitx_sys", T7_HW(HDMITX_SYS_DIV),
		T7_DCLK_HDMI_CLK_CTRL, 8, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_PRIF_MUX,
		"t7_display_hdmitx_prif_sel", t7_hdmitx_parent_data,
		ARRAY_SIZE(t7_hdmitx_parent_data), T7_DCLK_HTX_CLK_CTRL0,
		9, 2, NULL, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_PRIF_DIV,
		"t7_display_hdmitx_prif_div", T7_HW(HDMITX_PRIF_MUX),
		T7_DCLK_HTX_CLK_CTRL0, 0, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_HDMITX_PRIF,
		"t7_display_hdmitx_prif", T7_HW(HDMITX_PRIF_DIV),
		T7_DCLK_HTX_CLK_CTRL0, 8, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	ret = t7_display_add_mux_parent_data(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_200M_MUX,
		"t7_display_hdmitx_200m_sel", t7_hdmitx_parent_data,
		ARRAY_SIZE(t7_hdmitx_parent_data), T7_DCLK_HTX_CLK_CTRL0,
		25, 2, NULL, 0);
	if (ret)
		return ret;

	ret = t7_display_add_divider(dev, clkc,
		CLKID_T7_DISPLAY_HDMITX_200M_DIV,
		"t7_display_hdmitx_200m_div", T7_HW(HDMITX_200M_MUX),
		T7_DCLK_HTX_CLK_CTRL0, 16, 7, CLK_SET_RATE_PARENT);
	if (ret)
		return ret;

	return t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_HDMITX_200M,
		"t7_display_hdmitx_200m", T7_HW(HDMITX_200M_DIV),
		T7_DCLK_HTX_CLK_CTRL0, 24, CLK_SET_RATE_PARENT);
}

static int t7_display_register_vid_lock(struct device *dev,
					struct t7_display_clkc *clkc)
{
	struct clk_hw *hw;

	hw = devm_clk_hw_register_divider_parent_data(dev,
		"t7_display_vid_lock_div", &t7_vid_lock_parent_data[0], 0,
		clkc->base + T7_DCLK_VID_LOCK_CLK_CTRL, 0, 7, 0,
		&clkc->lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw),
				     "failed to register t7_display_vid_lock_div\n");

	clkc->data->hws[CLKID_T7_DISPLAY_VID_LOCK_DIV] = hw;

	return t7_display_add_gate(dev, clkc, CLKID_T7_DISPLAY_VID_LOCK,
		"t7_display_vid_lock", T7_HW(VID_LOCK_DIV),
		T7_DCLK_VID_LOCK_CLK_CTRL, 7, CLK_SET_RATE_PARENT);
}

static int t7_display_clkc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct t7_display_clkc *clkc;
	struct clk_hw_onecell_data *data;
	int ret;

	clkc = devm_kzalloc(dev, sizeof(*clkc), GFP_KERNEL);
	if (!clkc)
		return -ENOMEM;

	data = devm_kzalloc(dev, struct_size(data, hws, NR_T7_DISPLAY_CLKS),
			   GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num = NR_T7_DISPLAY_CLKS;
	clkc->data = data;
	spin_lock_init(&clkc->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	/* The upstream peripherals clock provider already owns this register page. */
	clkc->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!clkc->base)
		return -ENOMEM;

	ret = t7_display_register_vpu(dev, clkc);
	if (ret)
		return ret;

	ret = t7_display_register_vpu_clkb(dev, clkc);
	if (ret)
		return ret;

	ret = t7_display_register_vpu_clkc(dev, clkc);
	if (ret)
		return ret;

	ret = t7_display_register_vapb(dev, clkc);
	if (ret)
		return ret;

	ret = t7_display_register_hdmitx(dev, clkc);
	if (ret)
		return ret;

	ret = t7_display_register_vid_lock(dev, clkc);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static const struct of_device_id t7_display_clkc_of_match[] = {
	{ .compatible = "amlogic,t7-display-clkc" },
	{ }
};
MODULE_DEVICE_TABLE(of, t7_display_clkc_of_match);

static struct platform_driver t7_display_clkc_driver = {
	.probe = t7_display_clkc_probe,
	.driver = {
		.name = "t7-display-clkc",
		.of_match_table = t7_display_clkc_of_match,
	},
};
module_platform_driver(t7_display_clkc_driver);

MODULE_DESCRIPTION("Amlogic T7 display clock controller");
MODULE_LICENSE("GPL");
