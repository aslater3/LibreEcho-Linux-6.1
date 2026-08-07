// SPDX-License-Identifier: GPL-2.0-only
/*
 * Radar-Puffin codec MCLK.
 *
 * The board routes the MT8163 SENINF test clock to the audio MCLK pin.  A
 * 48 MHz parent divided by five produces the 9.6 MHz clock used by the stock
 * TLV320 configuration.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "mt8163-mclk.h"

#define MT8163_MCLK_RATE		9600000U

#define SENINF_TOP_CTRL			0x0000
#define SENINF1_CTRL			0x0100
#define SENINF1_MUX_CTRL		0x0120
#define SENINF_TG1_PH_CNT		0x0200
#define SENINF_TG1_SEN_CK		0x0204

#define SENINF1_ENABLE			BIT(0)
#define SENINF1_MUX_ENABLE		BIT(31)
#define SENINF12_PCLK_CLEAR_MASK	GENMASK(11, 10)
#define SENINF12_PCLK_ROUTE_MASK	GENMASK(9, 8)
#define SENINF12_PCLK_ROUTE_VALUE	GENMASK(9, 8)
#define SENINF12_PCLK_STATE_MASK	GENMASK(11, 8)
#define SENINF12_PCLK_STATE_VALUE	0x00000300
#define SENINF_CLK_FALL_MASK		GENMASK(5, 0)
#define SENINF_CLK_RISE_MASK		GENMASK(13, 8)
#define SENINF_CLK_COUNT_MASK		GENMASK(21, 16)
#define SENINF_TGCLK_SEL_MASK		GENMASK(1, 0)
#define SENINF_FALL_POLARITY		BIT(2)
#define SENINF_PADCLK_INVERT		BIT(6)
#define SENINF_CLK_POLARITY		BIT(28)
#define SENINF_PHASE_COUNTER_ENABLE	BIT(31)
#define SENINF_OUTPUT_ENABLE		BIT(29)

struct mt8163_mclk {
	struct device *dev;
	struct platform_device *isp;
	struct platform_device *camera;
	void __iomem *base;
	/* MM_SMI_COMMON belongs to the disabled mmsys node on Radar-Puffin. */
	struct clk *smi_common;
	struct clk *sen_tg;
	struct clk *sen_cam;
	struct clk *larb2_smi;
	struct clk *camtg;
	struct clk *parent_48m;
	struct clk *saved_parent;
	struct mutex lock;
	unsigned int users;
	bool isp_powered;
	bool smi_common_enabled;
	bool sen_tg_enabled;
	bool sen_cam_enabled;
	bool larb2_smi_enabled;
	bool parent_changed;
	bool camtg_enabled;
	bool isp_domain_attached;
	bool isp_pm_enabled;
};

static struct platform_device *mt8163_find_pdev(struct device *dev,
						const char *compatible)
{
	struct platform_device *pdev;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np) {
		dev_err(dev, "MCLK DT node missing for %s\n", compatible);
		return NULL;
	}
	pdev = of_find_device_by_node(np);
	if (!pdev)
		dev_err(dev, "MCLK DT node %pOF has no platform device for %s\n",
			np, compatible);
	else
		dev_info(dev, "MCLK platform device found: %s for %s\n",
			dev_name(&pdev->dev), compatible);
	of_node_put(np);
	return pdev;
}

static void mt8163_mclk_update_bits(struct mt8163_mclk *mclk,
				    u32 reg, u32 mask, u32 val)
{
	u32 tmp = readl(mclk->base + reg);

	writel((tmp & ~mask) | (val & mask), mclk->base + reg);
}

static int mt8163_mclk_program(struct mt8163_mclk *mclk)
{
	u32 top_ctrl;
	u32 ph_cnt;
	u32 sen_ck;
	u32 mux_ctrl;
	u32 seninf_ctrl;

	/* 48 MHz / (4 + 1), with a symmetric-enough 2/3 duty cycle. */
	mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
				SENINF_PHASE_COUNTER_ENABLE,
				SENINF_PHASE_COUNTER_ENABLE);
	mt8163_mclk_update_bits(mclk, SENINF_TOP_CTRL,
				SENINF12_PCLK_CLEAR_MASK | SENINF12_PCLK_ROUTE_MASK,
				SENINF12_PCLK_ROUTE_VALUE);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_SEN_CK,
				SENINF_CLK_FALL_MASK, 2);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_SEN_CK,
				SENINF_CLK_RISE_MASK, 0);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_SEN_CK,
				SENINF_CLK_COUNT_MASK, 4 << 16);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
				SENINF_TGCLK_SEL_MASK, 1);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
				SENINF_FALL_POLARITY, SENINF_FALL_POLARITY);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
				SENINF_PADCLK_INVERT | SENINF_CLK_POLARITY, 0);
	mt8163_mclk_update_bits(mclk, SENINF1_MUX_CTRL,
				SENINF1_MUX_ENABLE, SENINF1_MUX_ENABLE);
	mt8163_mclk_update_bits(mclk, SENINF1_CTRL,
				SENINF1_ENABLE, SENINF1_ENABLE);
	mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
				SENINF_OUTPUT_ENABLE, SENINF_OUTPUT_ENABLE);

	top_ctrl = readl(mclk->base + SENINF_TOP_CTRL);
	ph_cnt = readl(mclk->base + SENINF_TG1_PH_CNT);
	sen_ck = readl(mclk->base + SENINF_TG1_SEN_CK);
	mux_ctrl = readl(mclk->base + SENINF1_MUX_CTRL);
	seninf_ctrl = readl(mclk->base + SENINF1_CTRL);
	dev_info(mclk->dev,
		 "MCLK state top=%#010x ph=%#010x sen_ck=%#010x mux=%#010x ctrl=%#010x\n",
		top_ctrl, ph_cnt, sen_ck, mux_ctrl, seninf_ctrl);

	if ((top_ctrl & SENINF12_PCLK_STATE_MASK) !=
	    SENINF12_PCLK_STATE_VALUE) {
		dev_err(mclk->dev,
			"MCLK SENINF_TOP_CTRL invalid: %#010x\n", top_ctrl);
		return -EIO;
	}
	if ((sen_ck & SENINF_CLK_COUNT_MASK) != (4U << 16) ||
	    (sen_ck & SENINF_CLK_FALL_MASK) != 2 ||
	    (sen_ck & SENINF_CLK_RISE_MASK) != 0) {
		dev_err(mclk->dev, "MCLK divider invalid: SEN_CK=%#010x\n",
			sen_ck);
		return -EIO;
	}
	if (!(ph_cnt & SENINF_PHASE_COUNTER_ENABLE) ||
	    !(ph_cnt & SENINF_OUTPUT_ENABLE) ||
	    !(mux_ctrl & SENINF1_MUX_ENABLE) ||
	    !(seninf_ctrl & SENINF1_ENABLE)) {
		dev_err(mclk->dev,
			"MCLK output incomplete: ph=%#x mux=%#x ctrl=%#x\n",
			ph_cnt, mux_ctrl, seninf_ctrl);
		return -EIO;
	}

	return 0;
}

static void mt8163_mclk_disable_locked(struct mt8163_mclk *mclk)
{
	if (mclk->base)
		mt8163_mclk_update_bits(mclk, SENINF_TG1_PH_CNT,
					SENINF_OUTPUT_ENABLE, 0);
	if (mclk->camtg_enabled) {
		clk_disable_unprepare(mclk->camtg);
		mclk->camtg_enabled = false;
	}
	if (mclk->parent_changed && mclk->saved_parent) {
		if (!clk_set_parent(mclk->camtg, mclk->saved_parent))
			mclk->parent_changed = false;
	}
	if (mclk->larb2_smi_enabled) {
		clk_disable_unprepare(mclk->larb2_smi);
		mclk->larb2_smi_enabled = false;
	}
	if (mclk->sen_cam_enabled) {
		clk_disable_unprepare(mclk->sen_cam);
		mclk->sen_cam_enabled = false;
	}
	if (mclk->sen_tg_enabled) {
		clk_disable_unprepare(mclk->sen_tg);
		mclk->sen_tg_enabled = false;
	}
	if (mclk->smi_common_enabled) {
		clk_disable_unprepare(mclk->smi_common);
		mclk->smi_common_enabled = false;
	}
	if (mclk->isp_powered) {
		pm_runtime_put_sync(&mclk->isp->dev);
		mclk->isp_powered = false;
	}
}

int mt8163_mclk_enable(struct mt8163_mclk *mclk)
{
	int ret;

	if (!mclk)
		return -ENODEV;

	mutex_lock(&mclk->lock);
	if (mclk->users++) {
		mutex_unlock(&mclk->lock);
		return 0;
	}

	ret = pm_runtime_resume_and_get(&mclk->isp->dev);
	if (ret < 0)
		goto fail_users;
	mclk->isp_powered = true;
	if (mclk->smi_common) {
		ret = clk_prepare_enable(mclk->smi_common);
		if (ret)
			goto fail;
		mclk->smi_common_enabled = true;
	}
	ret = clk_prepare_enable(mclk->sen_tg);
	if (ret)
		goto fail;
	mclk->sen_tg_enabled = true;
	ret = clk_prepare_enable(mclk->sen_cam);
	if (ret)
		goto fail;
	mclk->sen_cam_enabled = true;
	ret = clk_prepare_enable(mclk->larb2_smi);
	if (ret)
		goto fail;
	mclk->larb2_smi_enabled = true;

	mclk->saved_parent = clk_get_parent(mclk->camtg);
	if (!mclk->saved_parent) {
		ret = -ENODEV;
		goto fail;
	}
	if (mclk->saved_parent != mclk->parent_48m) {
		ret = clk_set_parent(mclk->camtg, mclk->parent_48m);
		if (ret)
			goto fail;
		mclk->parent_changed = true;
	}
	ret = clk_prepare_enable(mclk->camtg);
	if (ret)
		goto fail;
	mclk->camtg_enabled = true;

	ret = mt8163_mclk_program(mclk);
	if (ret)
		goto fail;
	mutex_unlock(&mclk->lock);
	return 0;

fail:
	mt8163_mclk_disable_locked(mclk);
fail_users:
	mclk->users = 0;
	mutex_unlock(&mclk->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt8163_mclk_enable);

void mt8163_mclk_disable(struct mt8163_mclk *mclk)
{
	if (!mclk)
		return;

	mutex_lock(&mclk->lock);
	if (mclk->users && !--mclk->users)
		mt8163_mclk_disable_locked(mclk);
	mutex_unlock(&mclk->lock);
}
EXPORT_SYMBOL_GPL(mt8163_mclk_disable);

static void mt8163_mclk_release(void *data)
{
	struct mt8163_mclk *mclk = data;

	mutex_lock(&mclk->lock);
	mclk->users = 0;
	mt8163_mclk_disable_locked(mclk);
	mutex_unlock(&mclk->lock);
	if (mclk->isp_pm_enabled) {
		pm_runtime_disable(&mclk->isp->dev);
		mclk->isp_pm_enabled = false;
	}
	if (mclk->isp_domain_attached) {
		dev_pm_domain_detach(&mclk->isp->dev, true);
		mclk->isp_domain_attached = false;
	}
	if (mclk->base)
		iounmap(mclk->base);
	if (mclk->parent_48m)
		clk_put(mclk->parent_48m);
	if (mclk->camtg)
		clk_put(mclk->camtg);
	if (mclk->sen_cam)
		clk_put(mclk->sen_cam);
	if (mclk->sen_tg)
		clk_put(mclk->sen_tg);
	if (mclk->larb2_smi)
		clk_put(mclk->larb2_smi);
	if (mclk->smi_common)
		clk_put(mclk->smi_common);
	if (mclk->camera)
		put_device(&mclk->camera->dev);
	if (mclk->isp)
		put_device(&mclk->isp->dev);
}

struct mt8163_mclk *mt8163_mclk_get(struct device *dev)
{
	struct mt8163_mclk *mclk;
	int ret;

	mclk = devm_kzalloc(dev, sizeof(*mclk), GFP_KERNEL);
	if (!mclk)
		return ERR_PTR(-ENOMEM);
	mclk->dev = dev;
	mutex_init(&mclk->lock);

	mclk->isp = mt8163_find_pdev(dev, "mediatek,mt8163-ispsys");
	if (!mclk->isp) {
		dev_err(dev,
			"MCLK ISP provider DT node exists but platform device is not ready; deferring\n");
		return ERR_PTR(-EPROBE_DEFER);
	}
	mclk->camera = mt8163_find_pdev(dev, "mediatek,mt8163-camera_hw");
	if (!mclk->camera) {
		dev_err(dev,
			"MCLK camera provider DT node exists but platform device is not ready; deferring\n");
		put_device(&mclk->isp->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	dev_info(dev, "MCLK providers ready: isp=%s camera=%s\n",
		 dev_name(&mclk->isp->dev), dev_name(&mclk->camera->dev));
	ret = dev_pm_domain_attach(&mclk->isp->dev, true);
	dev_info(dev, "MCLK ISP PM-domain attach returned %d\n", ret);
	if (ret == -ENOENT) {
		/*
		 * MT8163's ISP domain is powered by the SoC bring-up path on
		 * this board.  Some 6.1 DT variants do not expose a matching
		 * genpd provider to this late-created helper, although the ISP
		 * platform device and its clocks are usable (the 3.18 path has
		 * the same assumption).  Do not make codec registration depend
		 * on an optional PM-domain attachment in that case.
		 */
		dev_warn(dev,
			 "ISP PM-domain unavailable (%d); continuing without attachment\n",
			 ret);
		ret = 0;
	} else if (ret) {
		dev_err(dev, "ISP PM-domain attach failed: %d\n", ret);
		goto fail;
	} else {
		mclk->isp_domain_attached = true;
	}
	pm_runtime_enable(&mclk->isp->dev);
	mclk->isp_pm_enabled = true;

	mclk->base = of_iomap(mclk->camera->dev.of_node, 0);
	if (!mclk->base) {
		dev_err(dev, "MCLK camera register mapping failed\n");
		ret = -ENOMEM;
		goto fail;
	}
	dev_info(dev, "MCLK camera register mapping ready\n");
	/*
	 * The stock Radar-Puffin DT keeps mmsys disabled, so its
	 * MM_SMI_COMMON entry is absent even though the ISP clock provider is
	 * present.  3.18 does not require this unrelated display/MM gate for
	 * the SENINF MCLK path.  Keep using it when a DT variant exposes it,
	 * but never make codec registration depend on it.
	 */
	mclk->smi_common = clk_get_optional(&mclk->isp->dev, "MM_SMI_COMMON");
	if (IS_ERR(mclk->smi_common)) {
		ret = PTR_ERR(mclk->smi_common);
		dev_err(dev, "optional MM_SMI_COMMON clock lookup failed: %d\n", ret);
		mclk->smi_common = NULL;
		goto fail;
	}
	mclk->sen_tg = clk_get(&mclk->isp->dev, "IMG_SEN_TG");
	if (IS_ERR(mclk->sen_tg)) {
		ret = PTR_ERR(mclk->sen_tg);
		dev_err(dev, "IMG_SEN_TG clock lookup failed: %d\n", ret);
		mclk->sen_tg = NULL;
		goto fail;
	}
	mclk->sen_cam = clk_get(&mclk->isp->dev, "IMG_SEN_CAM");
	if (IS_ERR(mclk->sen_cam)) {
		ret = PTR_ERR(mclk->sen_cam);
		dev_err(dev, "IMG_SEN_CAM clock lookup failed: %d\n", ret);
		mclk->sen_cam = NULL;
		goto fail;
	}
	mclk->larb2_smi = clk_get(&mclk->isp->dev, "IMG_LARB2_SMI");
	if (IS_ERR(mclk->larb2_smi)) {
		ret = PTR_ERR(mclk->larb2_smi);
		dev_err(dev, "IMG_LARB2_SMI clock lookup failed: %d\n", ret);
		mclk->larb2_smi = NULL;
		goto fail;
	}
	mclk->camtg = clk_get(&mclk->camera->dev, "TOP_CAMTG_SEL");
	if (IS_ERR(mclk->camtg)) {
		ret = PTR_ERR(mclk->camtg);
		dev_err(dev, "TOP_CAMTG_SEL clock lookup failed: %d\n", ret);
		mclk->camtg = NULL;
		goto fail;
	}
	mclk->parent_48m = clk_get(&mclk->camera->dev, "TOP_UNIVPLL_D26");
	if (IS_ERR(mclk->parent_48m)) {
		ret = PTR_ERR(mclk->parent_48m);
		dev_err(dev, "TOP_UNIVPLL_D26 clock lookup failed: %d\n", ret);
		mclk->parent_48m = NULL;
		goto fail;
	}

	ret = devm_add_action_or_reset(dev, mt8163_mclk_release, mclk);
	if (ret)
		return ERR_PTR(ret);
	dev_dbg(dev, "Radar-Puffin codec MCLK configured for %u Hz\n",
		MT8163_MCLK_RATE);
	return mclk;

fail:
	mt8163_mclk_release(mclk);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(mt8163_mclk_get);

MODULE_DESCRIPTION("MediaTek MT8163 Radar-Puffin codec MCLK");
MODULE_LICENSE("GPL");
