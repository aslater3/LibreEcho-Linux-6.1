// SPDX-License-Identifier: GPL-2.0-only
/*
 * Narrow MT8163 AFE support for the Radar-Puffin DL1 playback path.
 *
 * The register programming follows the MT8163 vendor DL1/I2S1 path.  This
 * deliberately does not register the unrelated modem, Bluetooth, FM or HDMI
 * front ends present in the vendor tree.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt8163-afe.h"

#define MT8163_DL1_DAI_NAME		"mt-soc-dl1dai-driver"
#define MT8163_AFE_COMPONENT_NAME	"mt-soc-i2s0dl1-pcm"

#define AUDIO_TOP_CON0			0x0000
#define AFE_DAC_CON0			0x0010
#define AFE_DAC_CON1			0x0014
#define AFE_I2S_CON1			0x0034
#define AFE_CONN0			0x0020
#define AFE_CONN1			0x0024
#define AFE_CONN2			0x0028
#define AFE_DL1_BASE			0x0040
#define AFE_DL1_CUR			0x0044
#define AFE_DL1_END			0x0048
#define AFE_I2S_CON3			0x004c
#define AFE_CONN_24BIT			0x006c
#define AFE_ADDA_DL_SRC2_CON0		0x0108
#define AFE_ADDA_DL_SRC2_CON1		0x010c
#define AFE_ADDA_UL_DL_CON0		0x0124
#define AFE_ADDA_PREDIS_CON0		0x0260
#define AFE_ADDA_PREDIS_CON1		0x0264
#define AFE_SGEN_CON0			0x01f0
#define FPGA_CFG1			0x04c4
#define AFE_IRQ_MCU_CON			0x03a0
#define AFE_IRQ_STATUS			0x03a4
#define AFE_IRQ_CLR			0x03a8
#define AFE_IRQ_CNT1			0x03ac
#define AFE_MEMIF_PBUF_SIZE		0x03d8

#define MT8163_POWER_TOP_OFFSET		0x029c
#define MT8163_POWER_TOP_AUDIO		0x0000000d
#define AFE_RATE_48K			10
#define AFE_BUS_INIT			BIT(14)
/* AFE gates used by the fixed-function DL1 speaker path.  The 3.18
 * AudDrv_I2S_Clk_On path explicitly ungates AUDIO_TOP_CON0 bit 6; keep
 * that separate I2S engine gate in the 6.1 sequence as well. */
#define AFE_CLOCK_GATE_MASK		(BIT(2) | BIT(6) | BIT(25) | BIT(26))
#define AFE_24M_ENABLE			BIT(9)
#define AFE_GLOBAL_ENABLE		BIT(0)
#define AFE_DL1_ENABLE			BIT(1)
#define AFE_I2S_DAC_ENABLE		BIT(13)
#define AFE_I2S_OUT2_ENABLE		BIT(17)
#define AFE_OTHER_MEMIF_ENABLE_MASK	(GENMASK(10, 2) | \
					 AFE_I2S_DAC_ENABLE | AFE_I2S_OUT2_ENABLE)
#define AFE_DL1_RATE_MASK		GENMASK(3, 0)
#define AFE_I2S_RATE_MASK		GENMASK(11, 8)
#define AFE_DL1_MONO			BIT(21)
#define AFE_DL1_FORMAT_MASK		GENMASK(17, 16)
#define AFE_IRQ1_ENABLE			BIT(0)
#define AFE_I2S_ENABLE			BIT(0)
#define AFE_I2S_FORMAT_I2S		BIT(3)
#define AFE_I2S_WORD_32			BIT(1)
#define AFE_I2S_LOW_JITTER		BIT(12)
#define AFE_I05_TO_O00			BIT(5)
#define AFE_I06_TO_O01			BIT(22)
#define AFE_I05_TO_O03			BIT(21)
#define AFE_I06_TO_O04			BIT(6)
#define AFE_OUTPUT_FORMAT_24BIT_MASK	(BIT(0) | BIT(1) | BIT(3) | BIT(4))
/*
 * Match the working 3.18 playback oracle.  The board has two related but
 * distinct I2S engines: I2S_CON1 feeds the internal DAC/ADDA bridge and
 * I2S_CON3 drives the external codec pins.  With the working 3.18 config
 * (CONFIG_MTK_16BIT_IN_24BIT_OUT unset), SetI2SDacOut() programs CON1 for
 * 16-bit words (0x0a08), while the external output remains 32-bit slots
 * (0x0a0a).  Do not apply the external slot width to the internal DAC.
 * CONFIG_SND_I2S_MCLK is disabled in the legacy normal path.  The exposed
 * low-jitter control selects the optional bit; its default is off to match
 * the working 3.18 transaction.
 */
#define AFE_I2S_DAC_CONFIG		((AFE_RATE_48K << 8) | \
					 AFE_I2S_FORMAT_I2S)
#define AFE_ADDA_DL_SRC2_48K		0x83001802
#define AFE_ADDA_DL_SRC2_COEFF		0xf74f0000
#define AFE_FPGA_DAC_MUX_MASK		BIT(4)
#define MT6323_CLKSQ_EN_AUD		BIT(0)
#define AUDIO_CLK_CFG_6			0x00a0
#define AUDIO_CLK_AUDDIV_0		0x05a0
#define AUDIO_CLK_AUDDIV_1		0x05a4
#define AFE_APLL2_DIV0_PDN		BIT(1)
#define AFE_APLL2_DIV0_SEL_MASK	GENMASK(30, 28)
#define AFE_APLL2_DIV0_SEL_4		(3 << 28)
#define AFE_APLL2_CFG6_MASK		(BIT(24) | BIT(31))
#define AFE_APLL2_CFG6_VALUE		BIT(24)
#define AFE_I2S_SRC_MASK			(BIT(9) | BIT(11))
#define AFE_I2S_DIV_POWER_MASK		(BIT(1) | BIT(3) | BIT(5))
#define AFE_I2S_DIV1_I2S1_MASK	GENMASK(14, 8)
#define AFE_I2S_DIV1_I2S3_MASK	GENMASK(30, 24)
#define AFE_I2S_DIV1_I2S1_VALUE	(15 << 8)
#define AFE_I2S_DIV1_I2S3_VALUE	(15 << 24)
#define AFE_I2S_DIV1_ACTIVE_MASK	(AFE_I2S_DIV1_I2S1_MASK | \
					 AFE_I2S_DIV1_I2S3_MASK)
#define AFE_I2S_DIV1_ACTIVE_VALUE	(AFE_I2S_DIV1_I2S1_VALUE | \
					 AFE_I2S_DIV1_I2S3_VALUE)

#define MT8163_PCM_RATE			48000
#define MT8163_PCM_BUFFER_MAX		0x6000
#define MT8163_PCM_ALIGN			64

enum mt8163_pin {
	MT8163_PIN_PMIC_IDLE,
	MT8163_PIN_PMIC_ACTIVE,
	MT8163_PIN_I2S_IDLE,
	MT8163_PIN_I2S_ACTIVE,
	MT8163_PIN_AMP_ON,
	MT8163_PIN_AMP_OFF,
	MT8163_PIN_MCLK,
	MT8163_PIN_DAC_ON,
	MT8163_PIN_DAC_OFF,
	MT8163_PIN_NUM,
};

static const char * const mt8163_pin_names[MT8163_PIN_NUM] = {
	[MT8163_PIN_PMIC_IDLE] = "audpmicclk-speaker-mode0",
	[MT8163_PIN_PMIC_ACTIVE] = "audpmicclk-speaker-mode1",
	[MT8163_PIN_I2S_IDLE] = "audi2s1-speaker-mode0",
	[MT8163_PIN_I2S_ACTIVE] = "audi2s1-speaker-mode1",
	[MT8163_PIN_AMP_ON] = "extamp-pullhigh",
	[MT8163_PIN_AMP_OFF] = "extamp-pulllow",
	[MT8163_PIN_MCLK] = "cmmclk-mclk",
	[MT8163_PIN_DAC_ON] = "extamp-dacmux-pullhigh",
	[MT8163_PIN_DAC_OFF] = "extamp-dacmux-pulllow",
};

struct mt8163_afe {
	struct device *dev;
	struct regmap *regmap;
	struct regmap *topckgen;
	void __iomem *topckgen_base;
	void __iomem *sram;
	dma_addr_t sram_phys;
	size_t sram_size;
	struct clk *infra;
	struct clk *audio_mux;
	struct clk *audio_intbus_mux;
	struct clk *aud_mux2;
	struct clk *apll2;
	struct clk *audio_24m;
	struct regmap *pmic_regmap;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins[MT8163_PIN_NUM];
	struct gpio_desc *amp_gpiod;
	struct gpio_desc *dac_gpiod;
	struct snd_pcm_substream *substream;
	struct mutex lock;
	spinlock_t irq_lock;
	int irq;
	bool clocks_enabled;
	bool pmic_clock_enabled;
	bool running;
	bool low_jitter;
	bool pcm_data_logged;
	bool pcm_start_logged;
	struct dentry *debugfs_dir;
};

static const struct snd_pcm_hardware mt8163_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = MT8163_PCM_RATE,
	.rate_max = MT8163_PCM_RATE,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = MT8163_PCM_BUFFER_MAX,
	.period_bytes_min = MT8163_PCM_ALIGN,
	.period_bytes_max = MT8163_PCM_BUFFER_MAX / 2,
	.periods_min = 2,
	.periods_max = MT8163_PCM_BUFFER_MAX / MT8163_PCM_ALIGN,
};

static int mt8163_afe_pin(struct mt8163_afe *afe, enum mt8163_pin pin)
{
	if (!afe || !afe->pinctrl || IS_ERR_OR_NULL(afe->pins[pin]))
		return -ENODEV;

	return pinctrl_select_state(afe->pinctrl, afe->pins[pin]);
}

static struct mt8163_afe *mt8163_afe_from_dev(struct device *dev)
{
	if (!dev)
		return NULL;
	return dev_get_drvdata(dev);
}

int mt8163_afe_select_i2s(struct device *dev, bool enable)
{
	struct mt8163_afe *afe = mt8163_afe_from_dev(dev);

	/* Match the 3.18 AudDrv_GPIO_I2S_Select() state exactly. */
	return mt8163_afe_pin(afe, enable ? MT8163_PIN_I2S_ACTIVE :
				      MT8163_PIN_I2S_IDLE);
}
EXPORT_SYMBOL_GPL(mt8163_afe_select_i2s);

int mt8163_afe_set_low_jitter(struct device *dev, bool enable)
{
	struct mt8163_afe *afe = mt8163_afe_from_dev(dev);

	if (!afe)
		return -ENODEV;
	mutex_lock(&afe->lock);
	afe->low_jitter = enable;
	mutex_unlock(&afe->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt8163_afe_set_low_jitter);

int mt8163_afe_select_pmic(struct device *dev, bool enable)
{
	struct mt8163_afe *afe = mt8163_afe_from_dev(dev);

	/* Match the 3.18 AudDrv_GPIO_PMIC_Select() state exactly. */
	return mt8163_afe_pin(afe, enable ? MT8163_PIN_PMIC_ACTIVE :
				      MT8163_PIN_PMIC_IDLE);
}
EXPORT_SYMBOL_GPL(mt8163_afe_select_pmic);

int mt8163_afe_select_amp(struct device *dev, bool enable)
{
	struct mt8163_afe *afe = mt8163_afe_from_dev(dev);

	if (afe && afe->amp_gpiod) {
		gpiod_set_value_cansleep(afe->amp_gpiod, enable);
		return 0;
	}

	return mt8163_afe_pin(afe, enable ? MT8163_PIN_AMP_ON :
			     MT8163_PIN_AMP_OFF);
}
EXPORT_SYMBOL_GPL(mt8163_afe_select_amp);

int mt8163_afe_select_dac(struct device *dev, bool enable)
{
	struct mt8163_afe *afe = mt8163_afe_from_dev(dev);

	if (afe && afe->dac_gpiod) {
		gpiod_set_value_cansleep(afe->dac_gpiod, enable);
		return 0;
	}
	/* The working 3.18 EVT DTB has no DAC-mux pinctrl states.  Its board
	 * callbacks call DacMux_Select(), ignore the failed lookup, and continue
	 * with the speaker route.  Keep the descriptor-free 6.1 DT variant
	 * compatible with that behavior. */
	if (!afe || IS_ERR_OR_NULL(afe->pins[MT8163_PIN_DAC_ON]) ||
	    IS_ERR_OR_NULL(afe->pins[MT8163_PIN_DAC_OFF]))
		return 0;

	return mt8163_afe_pin(afe, enable ? MT8163_PIN_DAC_ON :
			     MT8163_PIN_DAC_OFF);
}
EXPORT_SYMBOL_GPL(mt8163_afe_select_dac);

int mt8163_afe_select_mclk(struct device *dev)
{
	/* Match the 3.18 AudDrv_GPIO_MCLK_Select() state exactly. */
	return mt8163_afe_pin(mt8163_afe_from_dev(dev), MT8163_PIN_MCLK);
}
EXPORT_SYMBOL_GPL(mt8163_afe_select_mclk);

static int mt8163_afe_safe(struct mt8163_afe *afe)
{
	int ret, first = 0;

	ret = mt8163_afe_select_pmic(afe->dev, false);
	if (ret)
		first = ret;
	ret = mt8163_afe_select_amp(afe->dev, false);
	if (ret && !first)
		first = ret;
	ret = mt8163_afe_select_dac(afe->dev, false);
	if (ret && !first)
		first = ret;
	ret = mt8163_afe_select_i2s(afe->dev, false);
	if (ret && !first)
		first = ret;
	return first;
}

static int mt8163_afe_analog_clock(struct mt8163_afe *afe, bool enable)
{
	int ret;

	if (!afe->pmic_regmap)
		return -ENODEV;
	if (afe->pmic_clock_enabled == enable)
		return 0;

	/* 3.18 AudDrv_ANA_Clk_On/Off: upmu_set_rg_clksq_en_aud(). */
	ret = regmap_update_bits(afe->pmic_regmap, MT6323_TOP_CKPDN0,
				 MT6323_CLKSQ_EN_AUD,
				 enable ? MT6323_CLKSQ_EN_AUD : 0);
	if (!ret)
		afe->pmic_clock_enabled = enable;
	return ret;
}

static int mt8163_afe_topckgen_snapshot(struct mt8163_afe *afe,
						const char *stage, bool active);

static int mt8163_afe_topckgen_update(struct mt8163_afe *afe,
					      unsigned int reg, unsigned int mask,
					      unsigned int value, bool verify)
{
	u32 old, next, readback;
	int ret = 0;

	old = readl(afe->topckgen_base + reg);
	next = (old & ~mask) | (value & mask);
	dev_info(afe->dev,
		 "topckgen update reg=%04x old=%08x mask=%08x value=%08x next=%08x verify=%u\n",
		 reg, old, mask, value, next, verify);
	writel(next, afe->topckgen_base + reg);
	mb();
	readback = readl(afe->topckgen_base + reg);
	if (verify && (readback & mask) != (value & mask))
		ret = -EIO;
	dev_info(afe->dev,
		 "topckgen result reg=%04x readback=%08x ret=%d\n",
		 reg, readback, ret);
	return ret;
}

static int mt8163_afe_clocks_enable(struct mt8163_afe *afe)
{
	int ret;

	if (afe->clocks_enabled)
		return 0;

	ret = pm_runtime_resume_and_get(afe->dev);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(afe->infra);
	if (ret)
		goto put_pm;
	ret = clk_prepare_enable(afe->audio_intbus_mux);
	if (ret)
		goto disable_infra;
	ret = clk_prepare_enable(afe->audio_mux);
	if (ret)
		goto disable_intbus;
	ret = regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				 AFE_BUS_INIT, AFE_BUS_INIT);
	if (ret)
		goto disable_audio;
	ret = regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				 AFE_CLOCK_GATE_MASK, 0);
	if (ret)
		goto disable_audio;
	/*
	 * The codec is clocked from the AFE I2S branch, not from the generic
	 * top_mux_audio parent.  Select the 48-kHz-family APLL before enabling
	 * aud_2_sel; otherwise the APLL remains at zero consumers while the
	 * I2S source stays on clk26m.
	 */
	ret = clk_set_parent(afe->aud_mux2, afe->apll2);
	if (ret) {
		dev_err(afe->dev, "TOPCKGEN step=clk_set_parent ret=%d\n", ret);
		goto disable_audio;
	}
	ret = clk_prepare_enable(afe->aud_mux2);
	if (ret) {
		dev_err(afe->dev, "TOPCKGEN step=enable_aud_mux2 ret=%d\n", ret);
		goto disable_audio;
	}
	ret = mt8163_afe_topckgen_snapshot(afe, "before-program", false);
	if (ret)
		goto disable_aud_mux2;
	/* Match the vendor EnableApll2() sequence: program auddiv0 to
	 * APLL2/4 = 24.576 MHz while powered down, then enable the audio
	 * 24-MHz gate before releasing the divider. */
	ret = mt8163_afe_topckgen_update(afe, AUDIO_CLK_CFG_6,
				 AFE_APLL2_CFG6_MASK, AFE_APLL2_CFG6_VALUE, true);
	if (ret)
		goto disable_aud_mux2;
	ret = mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_0,
				 AFE_APLL2_DIV0_SEL_MASK | AFE_I2S_SRC_MASK |
				 AFE_I2S_DIV_POWER_MASK,
				 AFE_APLL2_DIV0_SEL_4 | AFE_I2S_DIV_POWER_MASK,
				 false);
	if (ret)
		goto disable_aud_mux2;
	ret = mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_1,
				 AFE_I2S_DIV1_ACTIVE_MASK,
				 AFE_I2S_DIV1_ACTIVE_VALUE, false);
	if (ret)
		goto disable_aud_mux2;
	ret = clk_prepare_enable(afe->audio_24m);
	if (ret) {
		dev_err(afe->dev, "TOPCKGEN step=enable_audio_24m ret=%d\n", ret);
		goto disable_aud_mux2;
	}
	ret = regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				 AFE_24M_ENABLE, 0);
	if (ret)
		goto disable_audio_24m;
	ret = mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_0,
				 AFE_I2S_DIV_POWER_MASK, 0, false);
	if (ret)
		goto disable_audio_24m;
	ret = mt8163_afe_topckgen_snapshot(afe, "after-program", true);
	if (ret)
		goto disable_apll2;
	afe->clocks_enabled = true;
	return 0;

disable_apll2:
	mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_0,
			   AFE_I2S_DIV_POWER_MASK, AFE_I2S_DIV_POWER_MASK, false);
	mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_1,
			   AFE_I2S_DIV1_ACTIVE_MASK, 0, false);
disable_audio_24m:
	clk_disable_unprepare(afe->audio_24m);
disable_aud_mux2:
	clk_disable_unprepare(afe->aud_mux2);
disable_audio:
	clk_disable_unprepare(afe->audio_mux);
disable_intbus:
	clk_disable_unprepare(afe->audio_intbus_mux);
disable_infra:
	clk_disable_unprepare(afe->infra);
put_pm:
	pm_runtime_put_sync(afe->dev);
	return ret;
}

static int mt8163_afe_topckgen_snapshot(struct mt8163_afe *afe,
						const char *stage, bool active)
{
	unsigned int cfg6, div0, div1, afe_div0 = 0, afe_div1 = 0;
	unsigned int dac_con0 = 0, dac_con1 = 0, i2s_con1 = 0, i2s_con3 = 0;
	unsigned int conn0 = 0, conn1 = 0, conn2 = 0;
	unsigned int adda0 = 0, adda1 = 0, fpga = 0, top0 = 0;
	int afe_ret0, afe_ret1;
	int state_ret;

	cfg6 = readl(afe->topckgen_base + AUDIO_CLK_CFG_6);
	div0 = readl(afe->topckgen_base + AUDIO_CLK_AUDDIV_0);
	div1 = readl(afe->topckgen_base + AUDIO_CLK_AUDDIV_1);
	afe_ret0 = regmap_read(afe->regmap, AUDIO_CLK_AUDDIV_0, &afe_div0);
	afe_ret1 = regmap_read(afe->regmap, AUDIO_CLK_AUDDIV_1, &afe_div1);
	state_ret = regmap_read(afe->regmap, AFE_DAC_CON0, &dac_con0);
	state_ret |= regmap_read(afe->regmap, AFE_DAC_CON1, &dac_con1);
	state_ret |= regmap_read(afe->regmap, AFE_I2S_CON1, &i2s_con1);
	state_ret |= regmap_read(afe->regmap, AFE_I2S_CON3, &i2s_con3);
	state_ret |= regmap_read(afe->regmap, AFE_CONN0, &conn0);
	state_ret |= regmap_read(afe->regmap, AFE_CONN1, &conn1);
	state_ret |= regmap_read(afe->regmap, AFE_CONN2, &conn2);
	state_ret |= regmap_read(afe->regmap, AFE_ADDA_DL_SRC2_CON0, &adda0);
	state_ret |= regmap_read(afe->regmap, AFE_ADDA_DL_SRC2_CON1, &adda1);
	state_ret |= regmap_read(afe->regmap, FPGA_CFG1, &fpga);
	state_ret |= regmap_read(afe->regmap, AUDIO_TOP_CON0, &top0);

	dev_info(afe->dev,
		 "topckgen stage=%s cfg6=%08x auddiv0=%08x auddiv1=%08x\n",
		 stage, cfg6, div0, div1);
	dev_info(afe->dev,
		 "auddiv dual stage=%s afe[05a0]=%08x ret=%d afe[05a4]=%08x ret=%d "
		 "topckgen[05a0]=%08x topckgen[05a4]=%08x\n",
		 stage, afe_div0, afe_ret0, afe_div1, afe_ret1, div0, div1);
	dev_info(afe->dev,
		 "afe state stage=%s ret=%d dac_con0=%08x dac_con1=%08x "
		 "i2s_con1=%08x i2s_con3=%08x conn0=%08x conn1=%08x conn2=%08x "
		 "adda0=%08x adda1=%08x fpga_cfg1=%08x top_con0=%08x\n",
		 stage, state_ret, dac_con0, dac_con1, i2s_con1, i2s_con3,
		 conn0, conn1, conn2, adda0, adda1, fpga, top0);
	if (!active)
		return 0;

	if ((cfg6 & AFE_APLL2_CFG6_MASK) != AFE_APLL2_CFG6_VALUE) {
		dev_err(afe->dev,
			"TOPCKGEN active readback mismatch stage=%s cfg6=%08x "
			"auddiv0=%08x auddiv1=%08x\n",
			stage, cfg6, div0, div1);
		return -EIO;
	}
	dev_warn(afe->dev,
		 "TOPCKGEN AUDDIV readback is diagnostic only stage=%s "
		 "auddiv0=%08x auddiv1=%08x\n", stage, div0, div1);
	return 0;
}

static void mt8163_afe_clocks_disable(struct mt8163_afe *afe)
{
	if (!afe->clocks_enabled)
		return;

	regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
			   AFE_CLOCK_GATE_MASK | AFE_24M_ENABLE,
			   AFE_CLOCK_GATE_MASK | AFE_24M_ENABLE);
	mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_0,
			   AFE_I2S_DIV_POWER_MASK, AFE_I2S_DIV_POWER_MASK, false);
	mt8163_afe_topckgen_update(afe, AUDIO_CLK_AUDDIV_1,
			   AFE_I2S_DIV1_ACTIVE_MASK, 0, false);
	mt8163_afe_topckgen_snapshot(afe, "after-clock-disable", false);
	clk_disable_unprepare(afe->audio_24m);
	clk_disable_unprepare(afe->aud_mux2);
	clk_disable_unprepare(afe->audio_mux);
	clk_disable_unprepare(afe->audio_intbus_mux);
	clk_disable_unprepare(afe->infra);
	pm_runtime_put_sync(afe->dev);
	afe->clocks_enabled = false;
}

static void mt8163_afe_record_error(int *first, int ret)
{
	if (ret && !*first)
		*first = ret;
}

static int mt8163_afe_preserve_error(struct mt8163_afe *afe, int primary,
				     int cleanup, const char *operation)
{
	if (cleanup && primary)
		dev_err(afe->dev,
			"%s cleanup failed: primary=%d cleanup=%d\n",
			operation, primary, cleanup);
	return primary ? primary : cleanup;
}

/*
 * The 3.18 TI_DAC_Playback path has two distinct output stages.  I2S_OUT_2
 * drives the board I2S pins, while I2S_OUT_DAC enables the internal DAC/ADDA
 * bridge that feeds that output.  The 6.1 candidate originally programmed
 * only DL1 and I2S_OUT_2's format, leaving the latter stage gated and muted.
 */
static int mt8163_afe_i2s_dac_config(struct mt8163_afe *afe)
{
	int first = 0;
	int ret;

	/* Match SetI2SDacOut()/SetDLSrc2(48000) from the working 3.18 path. */
	ret = regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   AFE_ADDA_DL_SRC2_48K);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1,
			   AFE_ADDA_DL_SRC2_COEFF);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_write(afe->regmap, AFE_I2S_CON1,
			   AFE_I2S_DAC_CONFIG |
			   (afe->low_jitter ? AFE_I2S_LOW_JITTER : 0));
	mt8163_afe_record_error(&first, ret);

	return first;
}

static int mt8163_afe_i2s_dac_enable(struct mt8163_afe *afe, bool enable)
{
	int first = 0;
	int ret;

	ret = regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
				 BIT(0), enable ? BIT(0) : 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_I2S_CON1, BIT(0),
				 enable ? BIT(0) : 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0, BIT(0),
				 enable ? BIT(0) : 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, FPGA_CFG1,
				 AFE_FPGA_DAC_MUX_MASK,
				 enable ? 0 : AFE_FPGA_DAC_MUX_MASK);
	mt8163_afe_record_error(&first, ret);

	return first;
}

static int mt8163_afe_quiesce(struct mt8163_afe *afe)
{
	unsigned long flags;
	unsigned int val;
	int first = 0;
	int ret;

	spin_lock_irqsave(&afe->irq_lock, flags);
	afe->running = false;
	spin_unlock_irqrestore(&afe->irq_lock, flags);

	ret = regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
				 AFE_IRQ1_ENABLE, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_write(afe->regmap, AFE_IRQ_CLR, AFE_IRQ1_ENABLE);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				 AFE_DL1_ENABLE, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_CONN0,
				 AFE_I05_TO_O00 | AFE_I06_TO_O01, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_CONN1,
				 AFE_I05_TO_O03, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_CONN2,
				 AFE_I06_TO_O04, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_I2S_CON3,
				 AFE_I2S_ENABLE, 0);
	mt8163_afe_record_error(&first, ret);
	ret = mt8163_afe_i2s_dac_enable(afe, false);
	mt8163_afe_record_error(&first, ret);
	/* The legacy driver disconnects the AFE sine generator on every stop. */
	ret = regmap_write(afe->regmap, AFE_SGEN_CON0, 0xf0000000);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				 AFE_I2S_DAC_ENABLE | AFE_I2S_OUT2_ENABLE, 0);
	mt8163_afe_record_error(&first, ret);
	ret = regmap_read(afe->regmap, AFE_DAC_CON0, &val);
	mt8163_afe_record_error(&first, ret);
	if (!ret && !(val & AFE_OTHER_MEMIF_ENABLE_MASK)) {
		ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					 AFE_GLOBAL_ENABLE, 0);
		mt8163_afe_record_error(&first, ret);
	}
	return first;
}

/* One-shot bring-up diagnostic: prove that userspace data reaches the AFE
 * SRAM, and that the DL1 engine is pointed at the same buffer. */
static void mt8163_afe_log_sram(struct mt8163_afe *afe, const char *stage,
				unsigned long pos, unsigned long bytes)
{
	u32 base = 0, cur = 0, end = 0;

	regmap_read(afe->regmap, AFE_DL1_BASE, &base);
	regmap_read(afe->regmap, AFE_DL1_CUR, &cur);
	regmap_read(afe->regmap, AFE_DL1_END, &end);
	dev_info(afe->dev,
		 "PCM SRAM %s pos=%lu bytes=%lu words=%08x,%08x,%08x,%08x dl1=%08x/%08x-%08x\n",
		 stage, pos, bytes, readl(afe->sram), readl(afe->sram + 4),
		 readl(afe->sram + 8), readl(afe->sram + 12), cur, base, end);
}

static ssize_t mt8163_afe_sgen_write(struct file *file,
					 const char __user *buf, size_t count,
					 loff_t *ppos)
{
	struct mt8163_afe *afe = file->private_data;
	char input[32];
	u32 value;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	if (kstrtou32(strim(input), 0, &value))
		return -EINVAL;

	mutex_lock(&afe->lock);
	ret = regmap_write(afe->regmap, AFE_SGEN_CON0, value);
	mutex_unlock(&afe->lock);
	return ret ? ret : count;
}

static const struct file_operations mt8163_afe_sgen_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mt8163_afe_sgen_write,
	.llseek = no_llseek,
};

static int mt8163_afe_stop(struct mt8163_afe *afe)
{
	int ret;
	int safe_ret;

	ret = mt8163_afe_quiesce(afe);
	safe_ret = mt8163_afe_safe(afe);
	return mt8163_afe_preserve_error(afe, ret, safe_ret, "stop");
}

static irqreturn_t mt8163_afe_irq(int irq, void *data)
{
	struct mt8163_afe *afe = data;
	struct snd_pcm_substream *substream = NULL;
	unsigned long flags;
	unsigned int status;

	if (regmap_read(afe->regmap, AFE_IRQ_STATUS, &status))
		return IRQ_NONE;
	if (!(status & AFE_IRQ1_ENABLE))
		return IRQ_NONE;

	regmap_write(afe->regmap, AFE_IRQ_CLR, AFE_IRQ1_ENABLE);
	spin_lock_irqsave(&afe->irq_lock, flags);
	if (afe->running)
		substream = afe->substream;
	spin_unlock_irqrestore(&afe->irq_lock, flags);
	if (substream)
		snd_pcm_period_elapsed(substream);
	return IRQ_HANDLED;
}

static int mt8163_pcm_open(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	mutex_lock(&afe->lock);
	if (afe->substream) {
		ret = -EBUSY;
		goto unlock;
	}
	/* Match the legacy order: analog audio clock before APLL/divider setup. */
	ret = mt8163_afe_analog_clock(afe, true);
	if (ret)
		goto unlock;
	ret = mt8163_afe_clocks_enable(afe);
	if (ret) {
		mt8163_afe_analog_clock(afe, false);
		goto unlock;
	}

	runtime->hw = mt8163_pcm_hardware;
	runtime->dma_area = (void *)afe->sram;
	runtime->dma_addr = afe->sram_phys;
	runtime->dma_bytes = afe->sram_size;
	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 MT8163_PCM_ALIGN);
	if (!ret)
		ret = snd_pcm_hw_constraint_step(runtime, 0,
						 SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
						 MT8163_PCM_ALIGN);
	if (ret) {
		mt8163_afe_analog_clock(afe, false);
		mt8163_afe_clocks_disable(afe);
		goto unlock;
	}
	afe->substream = substream;
unlock:
	mutex_unlock(&afe->lock);
	return ret;
}

static int mt8163_pcm_close(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	unsigned long flags;

	mutex_lock(&afe->lock);
	mt8163_afe_stop(afe);
	synchronize_irq(afe->irq);
	spin_lock_irqsave(&afe->irq_lock, flags);
	afe->substream = NULL;
	spin_unlock_irqrestore(&afe->irq_lock, flags);
	mt8163_afe_safe(afe);
	mt8163_afe_analog_clock(afe, false);
	mt8163_afe_clocks_disable(afe);
	mutex_unlock(&afe->lock);
	return 0;
}

static int mt8163_pcm_hw_params(struct snd_soc_component *component,
				struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	size_t bytes = params_buffer_bytes(params);

	if (params_rate(params) != MT8163_PCM_RATE ||
	    params_channels(params) < 1 || params_channels(params) > 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE ||
	    !bytes || bytes > afe->sram_size ||
	    bytes > MT8163_PCM_BUFFER_MAX ||
	    !IS_ALIGNED(bytes, MT8163_PCM_ALIGN))
		return -EINVAL;

	substream->runtime->dma_area = (void *)afe->sram;
	substream->runtime->dma_addr = afe->sram_phys;
	substream->runtime->dma_bytes = bytes;
	regmap_write(afe->regmap, AFE_DL1_BASE, (u32)afe->sram_phys);
	return regmap_write(afe->regmap, AFE_DL1_END,
			    (u32)(afe->sram_phys + bytes - 1));
}

static int mt8163_pcm_prepare(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	unsigned int i2s_ext = (AFE_RATE_48K << 8) |
			       AFE_I2S_FORMAT_I2S | AFE_I2S_WORD_32 |
			       (afe->low_jitter ? AFE_I2S_LOW_JITTER : 0);
	bool mono = substream->runtime->channels == 1;
	int safe_ret;
	int ret;

	WRITE_ONCE(afe->pcm_data_logged, false);
	WRITE_ONCE(afe->pcm_start_logged, false);
	memset_io(afe->sram, 0, substream->runtime->dma_bytes);
	/*
	 * Clear any state left by an interrupted/previous stream before selecting
	 * active pins or programming the next serializer transaction.  The old
	 * 3.18 path has no reset between its prepare-time output configuration and
	 * EnableAfe(true); keep that sequence uninterrupted here.
	 */
	ret = mt8163_afe_quiesce(afe);
	if (ret)
		goto safe;
	ret = mt8163_afe_select_i2s(afe->dev, true);
	if (ret)
		goto safe;
	ret = mt8163_afe_select_mclk(afe->dev);
	if (ret)
		goto safe;
	ret = mt8163_afe_select_dac(afe->dev, false);
	if (ret)
		goto safe;

	ret = regmap_update_bits(afe->regmap, AFE_MEMIF_PBUF_SIZE,
				 AFE_DL1_FORMAT_MASK, 0);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1,
				 AFE_DL1_RATE_MASK, AFE_RATE_48K);
	if (ret)
		goto safe;
	/* SetSampleRate(MEM_I2S, 48000) from the working 3.18 path. */
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1,
				 AFE_I2S_RATE_MASK, AFE_RATE_48K << 8);
	if (ret)
		goto safe;
	/* Match 3.18 SetChannels(): bit 21 is set only for a mono stream. */
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1, AFE_DL1_MONO,
				 mono ? AFE_DL1_MONO : 0);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_CONN_24BIT,
				 AFE_OUTPUT_FORMAT_24BIT_MASK, 0);
	if (ret)
		goto safe;
	ret = mt8163_afe_i2s_dac_config(afe);
	if (ret)
		goto safe;
	ret = regmap_write(afe->regmap, AFE_I2S_CON3, i2s_ext);
	if (ret)
		goto safe;
	ret = regmap_write(afe->regmap, AFE_IRQ_CNT1,
			   (u32)substream->runtime->period_size);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
					 GENMASK(7, 4), AFE_RATE_48K << 4);
	if (ret)
		goto safe;
	/*
	 * The working 3.18 path brings the ADDA/I2S bridge, external I2S
	 * serializer, routes, and AFE global gate up in prepare(), before the
	 * codec unmute and trigger callbacks.  Leave DL1 and IRQ1 quiescent; the
	 * trigger still enables the DL1 engine immediately before streaming.
	 */
	ret = mt8163_afe_i2s_dac_enable(afe, true);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_I2S_CON3,
				 AFE_I2S_ENABLE, AFE_I2S_ENABLE);
	if (ret)
		goto safe;
	/*
	 * Match the working 3.18 mtk_pcm_I2S0dl1_prepare() ordering: route
	 * DL1 to both the external I2S outputs and the internal DAC bridge
	 * before the AFE global gate is enabled.  The 6.1 candidate used to
	 * create these routes from trigger(), after the codec had been
	 * unmuted; that ordering is not equivalent at the serializer start
	 * boundary even when the final register values match.
	 */
	ret = regmap_update_bits(afe->regmap, AFE_CONN0,
				 AFE_I05_TO_O00 | AFE_I06_TO_O01,
				 AFE_I05_TO_O00 | AFE_I06_TO_O01);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_CONN1,
				 AFE_I05_TO_O03, AFE_I05_TO_O03);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_CONN2,
				 AFE_I06_TO_O04, AFE_I06_TO_O04);
	if (ret)
		goto safe;
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				 AFE_GLOBAL_ENABLE, AFE_GLOBAL_ENABLE);
	if (ret)
		goto safe;
	if (!ret)
		return 0;
safe:
	safe_ret = mt8163_afe_safe(afe);
	return mt8163_afe_preserve_error(afe, ret, safe_ret, "prepare");
}

static int mt8163_pcm_trigger(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream, int cmd)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int safe_ret;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		/* Keep the legacy digital output blocks enabled through prepare(). */
		ret = mt8163_afe_i2s_dac_enable(afe, true);
		if (ret)
			goto fail;
		ret = regmap_write(afe->regmap, AFE_IRQ_CLR,
					AFE_IRQ1_ENABLE);
		if (ret)
			goto fail;
		ret = regmap_update_bits(afe->regmap, AFE_I2S_CON3,
					 AFE_I2S_ENABLE, AFE_I2S_ENABLE);
		if (ret)
			goto fail;
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					 AFE_DL1_ENABLE, AFE_DL1_ENABLE);
		if (ret)
			goto fail;
		/* 3.18 asserts the external amp after DL1 is live and before AFE on. */
		ret = mt8163_afe_select_amp(afe->dev, true);
		if (ret)
			goto fail;
		ret = regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
					 AFE_IRQ1_ENABLE, AFE_IRQ1_ENABLE);
		if (!ret)
			ret = mt8163_afe_select_pmic(afe->dev, true);
		if (!ret)
			ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
						 AFE_GLOBAL_ENABLE,
						 AFE_GLOBAL_ENABLE);
		if (ret)
			goto fail;
		ret = mt8163_afe_topckgen_snapshot(afe, "after-start", true);
		if (ret)
			goto fail;
		spin_lock_irqsave(&afe->irq_lock, flags);
		afe->running = true;
		spin_unlock_irqrestore(&afe->irq_lock, flags);
		if (!READ_ONCE(afe->pcm_start_logged)) {
			WRITE_ONCE(afe->pcm_start_logged, true);
			mt8163_afe_log_sram(afe, "after-start",
					     0, substream->runtime->dma_bytes);
		}
		return 0;
fail:
		safe_ret = mt8163_afe_stop(afe);
		return mt8163_afe_preserve_error(afe, ret, safe_ret,
						 "start");
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = mt8163_afe_stop(afe);
		safe_ret = mt8163_afe_topckgen_snapshot(afe, "after-stop", true);
		return ret ? ret : safe_ret;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t
mt8163_pcm_pointer(struct snd_soc_component *component,
		   struct snd_pcm_substream *substream)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	unsigned int ptr;
	u32 base = (u32)afe->sram_phys;
	u32 bytes = (u32)substream->runtime->dma_bytes;

	if (regmap_read(afe->regmap, AFE_DL1_CUR, &ptr) ||
	    ptr < base || ptr >= base + bytes)
		return 0;
	return bytes_to_frames(substream->runtime, ptr - base);
}

static int mt8163_pcm_copy_user(struct snd_soc_component *component,
				struct snd_pcm_substream *substream,
				int channel, unsigned long pos,
				void __user *buf, unsigned long bytes)
{
	struct mt8163_afe *afe = snd_soc_component_get_drvdata(component);
	size_t first;
	void *bounce;

	if (!bytes || pos >= substream->runtime->dma_bytes ||
	    bytes > substream->runtime->dma_bytes)
		return -EINVAL;
	bounce = memdup_user(buf, bytes);
	if (IS_ERR(bounce))
		return PTR_ERR(bounce);
	first = min_t(size_t, bytes, substream->runtime->dma_bytes - pos);
	memcpy_toio(afe->sram + pos, bounce, first);
	if (first < bytes)
		memcpy_toio(afe->sram, bounce + first, bytes - first);
	if (!READ_ONCE(afe->pcm_data_logged)) {
		WRITE_ONCE(afe->pcm_data_logged, true);
		mt8163_afe_log_sram(afe, "after-copy", pos, bytes);
	}
	kfree(bounce);
	return 0;
}

static const struct snd_soc_component_driver mt8163_afe_component = {
	.name = MT8163_AFE_COMPONENT_NAME,
	.open = mt8163_pcm_open,
	.close = mt8163_pcm_close,
	.hw_params = mt8163_pcm_hw_params,
	.prepare = mt8163_pcm_prepare,
	.trigger = mt8163_pcm_trigger,
	.pointer = mt8163_pcm_pointer,
	.copy_user = mt8163_pcm_copy_user,
	.use_dai_pcm_id = true,
	.legacy_dai_naming = true,
};

static struct snd_soc_dai_driver mt8163_afe_dai = {
	.name = MT8163_DL1_DAI_NAME,
	.playback = {
		.stream_name = "DL1 Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static const struct regmap_config mt8163_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x9000 - 4,
	.cache_type = REGCACHE_NONE,
};

static int mt8163_afe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *sram_res;
	struct regmap *scpsys;
	struct device_node *pmic_np;
	struct device_node *topckgen_np;
	struct platform_device *pmic_pdev;
	struct mt8163_afe *afe;
	void __iomem *base;
	int i, ret;

	dev_info(dev, "MT8163 AFE probe entered\n");
	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->dev = dev;
	mutex_init(&afe->lock);
	spin_lock_init(&afe->irq_lock);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	afe->regmap = devm_regmap_init_mmio(dev, base,
					    &mt8163_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	afe->topckgen = syscon_regmap_lookup_by_compatible(
		"mediatek,mt8163-topckgen");
	if (IS_ERR(afe->topckgen))
		return dev_err_probe(dev, PTR_ERR(afe->topckgen),
				     "cannot find TOPCKGEN regmap\n");
	topckgen_np = of_find_compatible_node(NULL, NULL,
						"mediatek,mt8163-topckgen");
	if (!topckgen_np)
		return dev_err_probe(dev, -ENODEV,
				     "cannot find TOPCKGEN node\n");
	afe->topckgen_base = devm_of_iomap(dev, topckgen_np, 0, NULL);
	of_node_put(topckgen_np);
	if (IS_ERR(afe->topckgen_base))
		return dev_err_probe(dev, PTR_ERR(afe->topckgen_base),
				     "cannot map TOPCKGEN\n");

	scpsys = syscon_regmap_lookup_by_compatible("mediatek,mt8163-scpsys");
	if (IS_ERR(scpsys))
		return dev_err_probe(dev, PTR_ERR(scpsys),
				     "cannot find SCPSYS regmap\n");
	ret = regmap_write(scpsys, MT8163_POWER_TOP_OFFSET,
			   MT8163_POWER_TOP_AUDIO);
	if (ret)
		return ret;

	sram_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!sram_res)
		return -ENODEV;
	afe->sram_size = min_t(size_t, resource_size(sram_res),
			       MT8163_PCM_BUFFER_MAX);
	afe->sram = devm_ioremap_resource(dev, sram_res);
	if (IS_ERR(afe->sram))
		return PTR_ERR(afe->sram);
	afe->sram_phys = sram_res->start;

	afe->infra = devm_clk_get(dev, "aud_infra_clk");
	if (IS_ERR(afe->infra))
		return dev_err_probe(dev, PTR_ERR(afe->infra),
				     "missing aud_infra_clk\n");
	afe->audio_mux = devm_clk_get(dev, "top_mux_audio");
	if (IS_ERR(afe->audio_mux))
		return dev_err_probe(dev, PTR_ERR(afe->audio_mux),
				     "missing top_mux_audio\n");
	afe->audio_intbus_mux = devm_clk_get(dev, "top_mux_audio_intbus");
	if (IS_ERR(afe->audio_intbus_mux))
		return dev_err_probe(dev, PTR_ERR(afe->audio_intbus_mux),
				     "missing top_mux_audio_intbus\n");
	afe->aud_mux2 = devm_clk_get(dev, "aud_mux2_clk");
	if (IS_ERR(afe->aud_mux2))
		return dev_err_probe(dev, PTR_ERR(afe->aud_mux2),
				     "missing aud_mux2_clk\n");
	afe->apll2 = devm_clk_get(dev, "apmixed_apll2_clk");
	if (IS_ERR(afe->apll2))
		return dev_err_probe(dev, PTR_ERR(afe->apll2),
				     "missing apmixed_apll2_clk\n");
	afe->audio_24m = devm_clk_get(dev, "aud_24m_clk");
	if (IS_ERR(afe->audio_24m))
		return dev_err_probe(dev, PTR_ERR(afe->audio_24m),
				     "missing aud_24m_clk\n");
	dev_info(dev, "MT8163 AFE clocks acquired\n");

	/* The 3.18 driver reaches the MT6323 through the pwrap regmap. */
	pmic_np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!pmic_np)
		return dev_err_probe(dev, -ENODEV,
				     "missing mediatek,pmic phandle\n");
	pmic_pdev = of_find_device_by_node(pmic_np);
	if (!pmic_pdev) {
		of_node_put(pmic_np);
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "MT6323 PMIC device is not ready\n");
	}
	afe->pmic_regmap = dev_get_regmap(pmic_pdev->dev.parent, NULL);
	put_device(&pmic_pdev->dev);
	of_node_put(pmic_np);
	if (!afe->pmic_regmap)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "MT6323 PMIC regmap is not ready\n");

	afe->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(afe->pinctrl))
		return dev_err_probe(dev, PTR_ERR(afe->pinctrl),
				     "missing audio pinctrl\n");
	for (i = 0; i < MT8163_PIN_NUM; i++) {
		afe->pins[i] = pinctrl_lookup_state(afe->pinctrl,
						    mt8163_pin_names[i]);
		if (IS_ERR(afe->pins[i])) {
			if (i == MT8163_PIN_DAC_ON || i == MT8163_PIN_DAC_OFF) {
				dev_dbg(dev, "optional pinctrl state %s is absent\n",
					mt8163_pin_names[i]);
				continue;
			}
			return dev_err_probe(dev, PTR_ERR(afe->pins[i]),
					     "missing pinctrl state %s\n",
					     mt8163_pin_names[i]);
		}
	}

	/* Keep amp/DAC descriptors optional.  The old 3.18 external-amp path
	 * selects extamp-pullhigh/low, which this DT variant restores by omitting
	 * extamp-gpios.  Its DAC-mux state lookup was absent and ignored, so the
	 * corresponding 6.1 state/descriptor is optional as well. */
	afe->amp_gpiod = devm_gpiod_get_optional(dev, "extamp", GPIOD_OUT_LOW);
	if (IS_ERR(afe->amp_gpiod))
		return dev_err_probe(dev, PTR_ERR(afe->amp_gpiod),
				     "missing external amp GPIO\n");
	afe->dac_gpiod = devm_gpiod_get_optional(dev, "dacmux", GPIOD_OUT_LOW);
	if (IS_ERR(afe->dac_gpiod))
		return dev_err_probe(dev, PTR_ERR(afe->dac_gpiod),
				     "missing DAC mux GPIO\n");
	dev_info(dev, "MT8163 AFE pinctrl states acquired\n");

	afe->irq = platform_get_irq(pdev, 0);
	if (afe->irq < 0)
		return afe->irq;
	ret = devm_request_irq(dev, afe->irq, mt8163_afe_irq, 0,
			       "mt8163-afe", afe);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, afe);
	pm_runtime_enable(dev);
	ret = mt8163_afe_safe(afe);
	if (ret)
		goto disable_pm;

	ret = devm_snd_soc_register_component(dev, &mt8163_afe_component,
					      &mt8163_afe_dai, 1);
	if (ret) {
		dev_err(dev, "MT8163 AFE component registration failed: %d\n", ret);
		goto disable_pm;
	}
	/* Temporary bring-up hook: the vendor AFE has a hardware sine generator
	 * that isolates the codec/I2S output chain from the DL1 PCM buffer. */
	afe->debugfs_dir = debugfs_create_dir(dev_name(dev), NULL);
	if (afe->debugfs_dir)
		debugfs_create_file("sgen", 0600, afe->debugfs_dir, afe,
				    &mt8163_afe_sgen_fops);
	dev_info(dev, "MT8163 AFE component registered\n");
	return 0;

disable_pm:
	pm_runtime_disable(dev);
	return ret;
}

static int mt8163_afe_remove(struct platform_device *pdev)
{
	struct mt8163_afe *afe = platform_get_drvdata(pdev);

	debugfs_remove_recursive(afe->debugfs_dir);
	mt8163_afe_safe(afe);
	mt8163_afe_analog_clock(afe, false);
	mt8163_afe_clocks_disable(afe);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id mt8163_afe_of_match[] = {
	{ .compatible = "mediatek,mt8163-soc-pcm-dl1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt8163_afe_of_match);

static struct platform_driver mt8163_afe_driver = {
	.probe = mt8163_afe_probe,
	.remove = mt8163_afe_remove,
	.driver = {
		.name = "mt8163-afe",
		.of_match_table = mt8163_afe_of_match,
	},
};
module_platform_driver(mt8163_afe_driver);

MODULE_DESCRIPTION("MediaTek MT8163 narrow DL1/I2S AFE driver");
MODULE_LICENSE("GPL");
