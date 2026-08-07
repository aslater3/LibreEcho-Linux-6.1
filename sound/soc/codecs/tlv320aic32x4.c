// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * linux/sound/soc/codecs/tlv320aic32x4.c
 *
 * Copyright 2011 Vista Silicon S.L.
 *
 * Author: Javier Martin <javier.martin@vista-silicon.com>
 *
 * Based on sound/soc/codecs/wm8974 and TI driver for kernel 2.6.27.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/of_clk.h>
#include <linux/regulator/consumer.h>

#include <sound/tlv320aic32x4.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "tlv320aic32x4.h"

struct aic32x4_priv {
	struct regmap *regmap;
	struct snd_soc_component *component;
	u32 power_cfg;
	u32 micpga_routing;
	bool swapdacs;
	int rstn_gpio;
	const char *mclk_name;

	struct regulator *supply_ldo;
	struct regulator *supply_iov;
	struct regulator *supply_dv;
	struct regulator *supply_av;

	struct aic32x4_setup_data *setup;
	struct device *dev;
	enum aic32x4_type type;

	unsigned int fmt;
	bool defer_playback_unmute;
	bool playback_mono;
	bool playback_active;
	struct mutex diag_lock;
};

#define AIC32X4_GAIN_APPLIED		AIC32X4_REG(1, 63)
#define AIC32X4_GAIN_APPLIED_POLL_MS	100
#define AIC32X4_GAIN_APPLIED_RETRIES	22
#define AIC32X4_GAIN_APPLIED_MONO	BIT(6)
#define AIC32X4_GAIN_APPLIED_STEREO	(BIT(7) | BIT(6))

/* Read-only sequencing markers: final register values do not prove that
 * the analogue gain ramp saw the same clock/power sequence as 3.18. */
#define AIC32X4_ADC_FLAG		AIC32X4_REG(0, 36)
#define AIC32X4_DAC_FLAG1		AIC32X4_REG(0, 37)
#define AIC32X4_DAC_FLAG2		AIC32X4_REG(0, 38)

#if IS_ENABLED(CONFIG_SND_SOC_TLV320AIC32X4_PRB25_DIAG)
#define AIC32X4_DIAG_DACSPB		AIC32X4_REG(0, 60)
#define AIC32X4_DIAG_DAC_FLAG2	AIC32X4_REG(0, 38)
#define AIC32X4_DIAG_STICKY1		AIC32X4_REG(0, 42)
#define AIC32X4_DIAG_STICKY2		AIC32X4_REG(0, 44)
#define AIC32X4_DIAG_DOUTCTL		AIC32X4_REG(0, 53)
#define AIC32X4_DIAG_DRC_CTRL1	AIC32X4_REG(0, 68)
#define AIC32X4_DIAG_BEEP_CONTROL	AIC32X4_REG(0, 71)
#define AIC32X4_DIAG_BEEP_VOLUME_R	AIC32X4_REG(0, 72)
#define AIC32X4_DIAG_BEEP_LENGTH_MSB	AIC32X4_REG(0, 73)
#define AIC32X4_DIAG_BEEP_LENGTH_MID	AIC32X4_REG(0, 74)
#define AIC32X4_DIAG_BEEP_LENGTH_LSB	AIC32X4_REG(0, 75)
#define AIC32X4_DIAG_BEEP_SIN_MSB	AIC32X4_REG(0, 76)
#define AIC32X4_DIAG_BEEP_SIN_LSB	AIC32X4_REG(0, 77)
#define AIC32X4_DIAG_BEEP_COS_MSB	AIC32X4_REG(0, 78)
#define AIC32X4_DIAG_BEEP_COS_LSB	AIC32X4_REG(0, 79)
#define AIC32X4_DIAG_DAC_POWER_MASK	(BIT(7) | BIT(3))
#define AIC32X4_DIAG_DAC_SETUP_MASK	(BIT(7) | BIT(6))
#define AIC32X4_DIAG_OUTPWR_MASK		(BIT(5) | BIT(4))
#define AIC32X4_DIAG_FULL_OUTPUT_MASK	0xaa
#define AIC32X4_DIAG_DAC_MUTE_MASK	0x0c
#define AIC32X4_DIAG_HP_ROUTE_MASK	BIT(3)
#define AIC32X4_DIAG_DAC_GAIN_MASK	0x11
#define AIC32X4_DIAG_MFP2_FUNC_MASK	0x0e
#define AIC32X4_DIAG_MFP2_GPIO_FUNC	0x04
#define AIC32X4_DIAG_DAC_STICKY_MASK	0xc0
#define AIC32X4_DIAG_HP_STICKY_MASK	0xc0
#define AIC32X4_DIAG_BEEP_ENABLE		BIT(7)
#define AIC32X4_DIAG_BEEP_DURATION_MS	2000
#define AIC32X4_DIAG_BEEP_LENGTH_MSB_VALUE	0x01
#define AIC32X4_DIAG_BEEP_LENGTH_MID_VALUE	0x77
#define AIC32X4_DIAG_BEEP_LENGTH_LSB_VALUE	0x00
#define AIC32X4_DIAG_POLL_MS		20
#define AIC32X4_DIAG_DOWN_TIMEOUT_MS	6000
#define AIC32X4_DIAG_UP_TIMEOUT_MS	8000
#endif

static unsigned int aic32x4_debug_read(struct snd_soc_component *component,
					       unsigned int reg)
{
	int ret;

	ret = snd_soc_component_read(component, reg);
	return ret < 0 ? 0xff : ret;
}

static void aic32x4_log_playback_state(struct snd_soc_component *component,
					       const char *stage)
{
	dev_info(component->dev,
		 "audio-stage=%s p0[r4=%02x r5=%02x r6=%02x r11=%02x r12=%02x "
		 "r18=%02x r19=%02x r30=%02x r36=%02x r37=%02x r38=%02x "
		 "r60=%02x r61=%02x r63=%02x r64=%02x] "
		 "p1[r9=%02x r12=%02x r13=%02x r16=%02x r17=%02x r20=%02x r63=%02x]\n",
		 stage,
		 aic32x4_debug_read(component, AIC32X4_CLKMUX),
		 aic32x4_debug_read(component, AIC32X4_PLLPR),
		 aic32x4_debug_read(component, AIC32X4_PLLJ),
		 aic32x4_debug_read(component, AIC32X4_NDAC),
		 aic32x4_debug_read(component, AIC32X4_MDAC),
		 aic32x4_debug_read(component, AIC32X4_NADC),
		 aic32x4_debug_read(component, AIC32X4_MADC),
		 aic32x4_debug_read(component, AIC32X4_BCLKN),
		 aic32x4_debug_read(component, AIC32X4_ADC_FLAG),
		 aic32x4_debug_read(component, AIC32X4_DAC_FLAG1),
		 aic32x4_debug_read(component, AIC32X4_DAC_FLAG2),
		 aic32x4_debug_read(component, AIC32X4_DACSPB),
		 aic32x4_debug_read(component, AIC32X4_ADCSPB),
		 aic32x4_debug_read(component, AIC32X4_DACSETUP),
		 aic32x4_debug_read(component, AIC32X4_DACMUTE),
		 aic32x4_debug_read(component, AIC32X4_OUTPWRCTL),
		 aic32x4_debug_read(component, AIC32X4_HPLROUTE),
		 aic32x4_debug_read(component, AIC32X4_HPRROUTE),
		 aic32x4_debug_read(component, AIC32X4_HPLGAIN),
		 aic32x4_debug_read(component, AIC32X4_HPRGAIN),
		 aic32x4_debug_read(component, AIC32X4_HEADSTART),
		 aic32x4_debug_read(component, AIC32X4_GAIN_APPLIED));
}

static int aic32x4_reset_adc(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	u32 adc_reg;

	/*
	 * Workaround: the datasheet does not mention a required programming
	 * sequence but experiments show the ADC needs to be reset after each
	 * capture to avoid audible artifacts.
	 */
	switch (event) {
	case SND_SOC_DAPM_POST_PMD:
		adc_reg = snd_soc_component_read(component, AIC32X4_ADCSETUP);
		snd_soc_component_write(component, AIC32X4_ADCSETUP, adc_reg |
					AIC32X4_LADC_EN | AIC32X4_RADC_EN);
		snd_soc_component_write(component, AIC32X4_ADCSETUP, adc_reg);
		break;
	}
	return 0;
};

static int mic_bias_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* Change Mic Bias Registor */
		snd_soc_component_update_bits(component, AIC32X4_MICBIAS,
				AIC32x4_MICBIAS_MASK,
				AIC32X4_MICBIAS_LDOIN |
				AIC32X4_MICBIAS_2075V);
		printk(KERN_DEBUG "%s: Mic Bias will be turned ON\n", __func__);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_component_update_bits(component, AIC32X4_MICBIAS,
				AIC32x4_MICBIAS_MASK, 0);
		printk(KERN_DEBUG "%s: Mic Bias will be turned OFF\n",
				__func__);
		break;
	}

	return 0;
}


static int aic32x4_get_mfp1_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;

	val = snd_soc_component_read(component, AIC32X4_DINCTL);

	ucontrol->value.integer.value[0] = (val & 0x01);

	return 0;
};

static int aic32x4_set_mfp2_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;
	u8 gpio_check;

	val = snd_soc_component_read(component, AIC32X4_DOUTCTL);
	gpio_check = (val & AIC32X4_MFP_GPIO_ENABLED);
	if (gpio_check != AIC32X4_MFP_GPIO_ENABLED) {
		printk(KERN_ERR "%s: MFP2 is not configure as a GPIO output\n",
			__func__);
		return -EINVAL;
	}

	if (ucontrol->value.integer.value[0] == (val & AIC32X4_MFP2_GPIO_OUT_HIGH))
		return 0;

	if (ucontrol->value.integer.value[0])
		val |= ucontrol->value.integer.value[0];
	else
		val &= ~AIC32X4_MFP2_GPIO_OUT_HIGH;

	snd_soc_component_write(component, AIC32X4_DOUTCTL, val);

	return 0;
};

static int aic32x4_get_mfp3_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;

	val = snd_soc_component_read(component, AIC32X4_SCLKCTL);

	ucontrol->value.integer.value[0] = (val & 0x01);

	return 0;
};

static int aic32x4_set_mfp4_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;
	u8 gpio_check;

	val = snd_soc_component_read(component, AIC32X4_MISOCTL);
	gpio_check = (val & AIC32X4_MFP_GPIO_ENABLED);
	if (gpio_check != AIC32X4_MFP_GPIO_ENABLED) {
		printk(KERN_ERR "%s: MFP4 is not configure as a GPIO output\n",
			__func__);
		return -EINVAL;
	}

	if (ucontrol->value.integer.value[0] == (val & AIC32X4_MFP5_GPIO_OUT_HIGH))
		return 0;

	if (ucontrol->value.integer.value[0])
		val |= ucontrol->value.integer.value[0];
	else
		val &= ~AIC32X4_MFP5_GPIO_OUT_HIGH;

	snd_soc_component_write(component, AIC32X4_MISOCTL, val);

	return 0;
};

static int aic32x4_get_mfp5_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;

	val = snd_soc_component_read(component, AIC32X4_GPIOCTL);
	ucontrol->value.integer.value[0] = ((val & 0x2) >> 1);

	return 0;
};

static int aic32x4_set_mfp5_gpio(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	u8 val;
	u8 gpio_check;

	val = snd_soc_component_read(component, AIC32X4_GPIOCTL);
	gpio_check = (val & AIC32X4_MFP5_GPIO_OUTPUT);
	if (gpio_check != AIC32X4_MFP5_GPIO_OUTPUT) {
		printk(KERN_ERR "%s: MFP5 is not configure as a GPIO output\n",
			__func__);
		return -EINVAL;
	}

	if (ucontrol->value.integer.value[0] == (val & 0x1))
		return 0;

	if (ucontrol->value.integer.value[0])
		val |= ucontrol->value.integer.value[0];
	else
		val &= 0xfe;

	snd_soc_component_write(component, AIC32X4_GPIOCTL, val);

	return 0;
};

static const struct snd_kcontrol_new aic32x4_mfp1[] = {
	SOC_SINGLE_BOOL_EXT("MFP1 GPIO", 0, aic32x4_get_mfp1_gpio, NULL),
};

static const struct snd_kcontrol_new aic32x4_mfp2[] = {
	SOC_SINGLE_BOOL_EXT("MFP2 GPIO", 0, NULL, aic32x4_set_mfp2_gpio),
};

static const struct snd_kcontrol_new aic32x4_mfp3[] = {
	SOC_SINGLE_BOOL_EXT("MFP3 GPIO", 0, aic32x4_get_mfp3_gpio, NULL),
};

static const struct snd_kcontrol_new aic32x4_mfp4[] = {
	SOC_SINGLE_BOOL_EXT("MFP4 GPIO", 0, NULL, aic32x4_set_mfp4_gpio),
};

static const struct snd_kcontrol_new aic32x4_mfp5[] = {
	SOC_SINGLE_BOOL_EXT("MFP5 GPIO", 0, aic32x4_get_mfp5_gpio,
		aic32x4_set_mfp5_gpio),
};

/* 0dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_step_0_5, 0, 50, 0);
/* -63.5dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_pcm, -6350, 50, 0);
/* -6dB min, 1dB steps */
static DECLARE_TLV_DB_SCALE(tlv_driver_gain, -600, 100, 0);
/* -12dB min, 0.5dB steps */
static DECLARE_TLV_DB_SCALE(tlv_adc_vol, -1200, 50, 0);
/* -6dB min, 1dB steps */
static DECLARE_TLV_DB_SCALE(tlv_tas_driver_gain, -5850, 50, 0);
static DECLARE_TLV_DB_SCALE(tlv_amp_vol, 0, 600, 1);

static const char * const lo_cm_text[] = {
	"Full Chip", "1.65V",
};

static SOC_ENUM_SINGLE_DECL(lo_cm_enum, AIC32X4_CMMODE, 3, lo_cm_text);

static const char * const ptm_text[] = {
	"P3", "P2", "P1",
};

static SOC_ENUM_SINGLE_DECL(l_ptm_enum, AIC32X4_LPLAYBACK, 2, ptm_text);
static SOC_ENUM_SINGLE_DECL(r_ptm_enum, AIC32X4_RPLAYBACK, 2, ptm_text);

#if IS_ENABLED(CONFIG_SND_SOC_TLV320AIC32X4_PRB25_DIAG)
struct aic32x4_diag_saved_reg {
	unsigned int reg;
	unsigned int value;
};

static void aic32x4_diag_record_error(int *first, int ret)
{
	if (ret && !*first)
		*first = ret;
}

static unsigned int
aic32x4_diag_saved_value(const struct aic32x4_diag_saved_reg *saved,
			 unsigned int count, unsigned int reg)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (saved[i].reg == reg)
			return saved[i].value;
	return 0;
}

static int aic32x4_diag_wait_bits(struct snd_soc_component *component,
				 unsigned int mask, unsigned int expected,
				 unsigned int timeout_ms, const char *stage)
{
	unsigned int elapsed;
	unsigned int value = 0;
	int ret;

	for (elapsed = 0; elapsed <= timeout_ms;
	     elapsed += AIC32X4_DIAG_POLL_MS) {
		ret = regmap_read(component->regmap, AIC32X4_DAC_FLAG1, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected) {
			dev_info(component->dev,
				 "prb25-diag-stage=%s elapsed_ms=%u dacflag1=%02x\n",
				 stage, elapsed, value);
			return 0;
		}
		msleep(AIC32X4_DIAG_POLL_MS);
	}

	dev_err(component->dev,
		"prb25-diag-stage=%s timeout mask=%02x expected=%02x dacflag1=%02x\n",
		stage, mask, expected, value);
	return -ETIMEDOUT;
}

static int aic32x4_diag_wait_beep(struct snd_soc_component *component,
					 unsigned int msb, unsigned int mid,
					 unsigned int lsb)
{
	unsigned int elapsed;
	unsigned int value = 0;
	bool enabled = false;
	u64 start_ns;
	u64 clear_ns = 0;
	int ret;

	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   0x18);
	if (ret)
		return ret;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_LENGTH_MSB,
			   msb);
	if (ret)
		return ret;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_LENGTH_MID,
			   mid);
	if (ret)
		return ret;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_LENGTH_LSB,
			   lsb);
	if (ret)
		return ret;

	start_ns = ktime_get_boottime_ns();
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   0x98);
	if (ret)
		return ret;
	dev_info(component->dev,
		 "prb25-diag-r71-enable count=%02x%02x%02x start_ns=%llu\n",
		 msb, mid, lsb, start_ns);

	for (elapsed = 10; elapsed <= 4000; elapsed += 10) {
		ret = regmap_read(component->regmap,
				  AIC32X4_DIAG_BEEP_CONTROL, &value);
		if (ret)
			return ret;
		if (value & AIC32X4_DIAG_BEEP_ENABLE)
			enabled = true;
		else if (enabled) {
			clear_ns = ktime_get_boottime_ns();
			dev_info(component->dev,
				 "prb25-diag-r71-clear count=%02x%02x%02x "
				 "elapsed_ms=%u elapsed_us=%llu clear_ns=%llu\n",
				 msb, mid, lsb, elapsed,
				 div_u64(clear_ns - start_ns, 1000), clear_ns);
			return 0;
		}
		if (elapsed == 10 || elapsed == 500 || elapsed == 1500 ||
		    elapsed == 2000 || elapsed == 2100)
			dev_info(component->dev,
				 "prb25-diag-r71-sample count=%02x%02x%02x "
				 "elapsed_ms=%u value=%02x\n",
				 msb, mid, lsb, elapsed, value);
		msleep(10);
	}

	dev_err(component->dev,
		"prb25-diag-r71-timeout count=%02x%02x%02x last=%02x\n",
		msb, mid, lsb, value);
	return enabled ? -ETIMEDOUT : -EIO;
}

static int aic32x4_diag_save(struct snd_soc_component *component,
		   struct aic32x4_diag_saved_reg *saved, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_read(component->regmap, saved[i].reg,
				  &saved[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int aic32x4_diag_restore(struct snd_soc_component *component,
		      const struct aic32x4_diag_saved_reg *saved,
		      unsigned int count)
{
	unsigned int beep_control = aic32x4_diag_saved_value(saved, count,
							      AIC32X4_DIAG_BEEP_CONTROL);
	unsigned int dac_setup = aic32x4_diag_saved_value(saved, count,
							   AIC32X4_DACSETUP);
	unsigned int dac_mute = aic32x4_diag_saved_value(saved, count,
							  AIC32X4_DACMUTE);
	unsigned int outpwr = aic32x4_diag_saved_value(saved, count,
							AIC32X4_OUTPWRCTL);
	unsigned int hpl_route = aic32x4_diag_saved_value(saved, count,
							 AIC32X4_HPLROUTE);
	unsigned int hpr_route = aic32x4_diag_saved_value(saved, count,
							 AIC32X4_HPRROUTE);
	unsigned int ldacvol = aic32x4_diag_saved_value(saved, count,
							 AIC32X4_LDACVOL);
	unsigned int rdacvol = aic32x4_diag_saved_value(saved, count,
							 AIC32X4_RDACVOL);
	unsigned int doutctl = aic32x4_diag_saved_value(saved, count,
							 AIC32X4_DIAG_DOUTCTL);
	unsigned int drc_ctrl1 = aic32x4_diag_saved_value(saved, count,
							   AIC32X4_DIAG_DRC_CTRL1);
	unsigned int i;
	unsigned int value = 0xff;
	int first = 0;
	int ret;

	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   beep_control & ~AIC32X4_DIAG_BEEP_ENABLE);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_DACMUTE,
			   dac_mute | AIC32X4_DIAG_DAC_MUTE_MASK);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_HPLROUTE,
			   hpl_route & ~AIC32X4_DIAG_HP_ROUTE_MASK);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_HPRROUTE,
			   hpr_route & ~AIC32X4_DIAG_HP_ROUTE_MASK);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_OUTPWRCTL,
			   outpwr & ~AIC32X4_DIAG_OUTPWR_MASK);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_DACSETUP,
			   dac_setup & ~AIC32X4_DIAG_DAC_SETUP_MASK);
	aic32x4_diag_record_error(&first, ret);
	ret = aic32x4_diag_wait_bits(component,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK, 0,
				     AIC32X4_DIAG_DOWN_TIMEOUT_MS,
				     "restore-output-off");
	aic32x4_diag_record_error(&first, ret);

	ret = regmap_write(component->regmap, AIC32X4_DIAG_DACSPB,
			   aic32x4_diag_saved_value(saved, count,
						   AIC32X4_DIAG_DACSPB));
	aic32x4_diag_record_error(&first, ret);
	for (i = 0; i < count; i++) {
		if (saved[i].reg < AIC32X4_DIAG_BEEP_CONTROL ||
		    saved[i].reg > AIC32X4_DIAG_BEEP_COS_LSB)
			continue;
		ret = regmap_write(component->regmap, saved[i].reg,
				   saved[i].value);
		aic32x4_diag_record_error(&first, ret);
	}
	ret = regmap_write(component->regmap, AIC32X4_LDACVOL, ldacvol);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_RDACVOL, rdacvol);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_DIAG_DOUTCTL, doutctl);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_DIAG_DRC_CTRL1,
			   drc_ctrl1);
	aic32x4_diag_record_error(&first, ret);

	ret = regmap_write(component->regmap, AIC32X4_DACSETUP, dac_setup);
	aic32x4_diag_record_error(&first, ret);
	ret = aic32x4_diag_wait_bits(component, AIC32X4_DIAG_DAC_POWER_MASK,
				     AIC32X4_DIAG_DAC_POWER_MASK,
				     AIC32X4_DIAG_DOWN_TIMEOUT_MS,
				     "restore-dacs-on");
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_OUTPWRCTL, outpwr);
	aic32x4_diag_record_error(&first, ret);
	ret = aic32x4_diag_wait_bits(component,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK,
				     AIC32X4_DIAG_UP_TIMEOUT_MS,
				     "restore-full-output");
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_HPLROUTE, hpl_route);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_HPRROUTE, hpr_route);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_write(component->regmap, AIC32X4_DACMUTE, dac_mute);
	aic32x4_diag_record_error(&first, ret);
	ret = regmap_read(component->regmap, AIC32X4_DAC_FLAG1, &value);
	aic32x4_diag_record_error(&first, ret);

	if (first)
		dev_err(component->dev,
			"prb25-diag-restore-unconfirmed ret=%d dacflag1=%02x\n",
			first, value);
	else
		dev_info(component->dev,
			 "prb25-diag-restored dacflag1=%02x\n", value);
	return first;
}

static int aic32x4_diag_run(struct snd_soc_component *component)
{
	struct aic32x4_diag_saved_reg saved[] = {
		{ .reg = AIC32X4_DIAG_DACSPB },
		{ .reg = AIC32X4_DACSETUP },
		{ .reg = AIC32X4_DACMUTE },
		{ .reg = AIC32X4_LDACVOL },
		{ .reg = AIC32X4_RDACVOL },
		{ .reg = AIC32X4_DIAG_DOUTCTL },
		{ .reg = AIC32X4_DIAG_DRC_CTRL1 },
		{ .reg = AIC32X4_DIAG_BEEP_CONTROL },
		{ .reg = AIC32X4_DIAG_BEEP_VOLUME_R },
		{ .reg = AIC32X4_DIAG_BEEP_LENGTH_MSB },
		{ .reg = AIC32X4_DIAG_BEEP_LENGTH_MID },
		{ .reg = AIC32X4_DIAG_BEEP_LENGTH_LSB },
		{ .reg = AIC32X4_DIAG_BEEP_SIN_MSB },
		{ .reg = AIC32X4_DIAG_BEEP_SIN_LSB },
		{ .reg = AIC32X4_DIAG_BEEP_COS_MSB },
		{ .reg = AIC32X4_DIAG_BEEP_COS_LSB },
		{ .reg = AIC32X4_OUTPWRCTL },
		{ .reg = AIC32X4_HPLROUTE },
		{ .reg = AIC32X4_HPRROUTE },
		{ .reg = AIC32X4_HPLGAIN },
		{ .reg = AIC32X4_HPRGAIN },
	};
	unsigned int dac_setup;
	unsigned int dac_mute;
	unsigned int outpwr;
	unsigned int beep_control;
	unsigned int hpl_route;
	unsigned int hpr_route;
	unsigned int ldacvol;
	unsigned int rdacvol;
	unsigned int doutctl;
	unsigned int drc_ctrl1;
	unsigned int status;
	unsigned int dac_flag2;
	unsigned int sticky1;
	unsigned int sticky2;
	unsigned int snapshot_mute;
	unsigned int snapshot_ldacvol;
	unsigned int snapshot_rdacvol;
	unsigned int snapshot_doutctl;
	unsigned int snapshot_drc;
	int restore_ret;
	int ret;

	ret = aic32x4_diag_save(component, saved, ARRAY_SIZE(saved));
	if (ret)
		return ret;
	dac_setup = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					     AIC32X4_DACSETUP);
	dac_mute = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					    AIC32X4_DACMUTE);
	outpwr = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					  AIC32X4_OUTPWRCTL);
	hpl_route = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					     AIC32X4_HPLROUTE);
	hpr_route = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					     AIC32X4_HPRROUTE);
	ldacvol = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					  AIC32X4_LDACVOL);
	rdacvol = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					  AIC32X4_RDACVOL);
	doutctl = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					  AIC32X4_DIAG_DOUTCTL);
	drc_ctrl1 = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
					     AIC32X4_DIAG_DRC_CTRL1);
	beep_control = aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
						AIC32X4_DIAG_BEEP_CONTROL);
	ret = regmap_read(component->regmap, AIC32X4_DAC_FLAG1, &status);
	if (ret)
		return ret;
	if ((dac_setup & AIC32X4_DIAG_DAC_SETUP_MASK) !=
			AIC32X4_DIAG_DAC_SETUP_MASK ||
	    (outpwr & AIC32X4_DIAG_OUTPWR_MASK) != AIC32X4_DIAG_OUTPWR_MASK ||
	    (status & AIC32X4_DIAG_FULL_OUTPUT_MASK) !=
			AIC32X4_DIAG_FULL_OUTPUT_MASK ||
	    (beep_control & AIC32X4_DIAG_BEEP_ENABLE) ||
	    ldacvol == 0x80 || rdacvol == 0x80 ||
	    (doutctl & AIC32X4_DIAG_MFP2_FUNC_MASK) !=
			AIC32X4_DIAG_MFP2_GPIO_FUNC)
		return -EBUSY;

	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   beep_control & ~AIC32X4_DIAG_BEEP_ENABLE);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DACMUTE,
			   dac_mute | AIC32X4_DIAG_DAC_MUTE_MASK);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_HPLROUTE,
			   hpl_route & ~AIC32X4_DIAG_HP_ROUTE_MASK);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_HPRROUTE,
			   hpr_route & ~AIC32X4_DIAG_HP_ROUTE_MASK);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_OUTPWRCTL,
			   outpwr & ~AIC32X4_DIAG_OUTPWR_MASK);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DACSETUP,
			   dac_setup & ~AIC32X4_DIAG_DAC_SETUP_MASK);
	if (ret)
		goto restore;
	ret = aic32x4_diag_wait_bits(component,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK, 0,
				     AIC32X4_DIAG_DOWN_TIMEOUT_MS,
				     "prepare-output-off");
	if (ret)
		goto restore;

	ret = regmap_write(component->regmap, AIC32X4_DIAG_DACSPB, 25);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   0x18);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_VOLUME_R,
			   0x80);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap,
			   AIC32X4_DIAG_BEEP_LENGTH_MSB,
			   AIC32X4_DIAG_BEEP_LENGTH_MSB_VALUE);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap,
			   AIC32X4_DIAG_BEEP_LENGTH_MID,
			   AIC32X4_DIAG_BEEP_LENGTH_MID_VALUE);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap,
			   AIC32X4_DIAG_BEEP_LENGTH_LSB,
			   AIC32X4_DIAG_BEEP_LENGTH_LSB_VALUE);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_SIN_MSB,
			   0x10);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_SIN_LSB,
			   0xd8);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_COS_MSB,
			   0x7e);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_COS_LSB,
			   0xe3);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_DRC_CTRL1, 0x0f);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_LDACVOL, 0x00);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_RDACVOL, 0x00);
	if (ret)
		goto restore;

	ret = regmap_write(component->regmap, AIC32X4_DACSETUP, dac_setup);
	if (ret)
		goto restore;
	ret = aic32x4_diag_wait_bits(component, AIC32X4_DIAG_DAC_POWER_MASK,
				     AIC32X4_DIAG_DAC_POWER_MASK,
				     AIC32X4_DIAG_DOWN_TIMEOUT_MS,
				     "beep-dacs-on");
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_OUTPWRCTL, outpwr);
	if (ret)
		goto restore;
	ret = aic32x4_diag_wait_bits(component,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK,
				     AIC32X4_DIAG_FULL_OUTPUT_MASK,
				     AIC32X4_DIAG_UP_TIMEOUT_MS,
				     "beep-full-output");
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DACMUTE, dac_mute);
	if (ret)
		goto restore;
	ret = regmap_write(component->regmap, AIC32X4_DIAG_BEEP_CONTROL,
			   0x18);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DIAG_DAC_FLAG2,
			  &dac_flag2);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DACMUTE,
			  &snapshot_mute);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_LDACVOL,
			  &snapshot_ldacvol);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_RDACVOL,
			  &snapshot_rdacvol);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DIAG_DOUTCTL,
			  &snapshot_doutctl);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DIAG_DRC_CTRL1,
			  &snapshot_drc);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DIAG_STICKY1, &sticky1);
	if (ret)
		goto restore;
	ret = regmap_read(component->regmap, AIC32X4_DIAG_STICKY2, &sticky2);
	if (ret)
		goto restore;
	dev_info(component->dev,
		 "prb25-diag-snapshot r60=19 r37=%02x r38=%02x r42=%02x "
		 "r44=%02x r53=%02x r63=%02x r64=%02x r65=%02x r66=%02x "
		 "r68=%02x r71=18 r72=80 r73=%02x r74=%02x r75=%02x "
		 "r76=10 r77=d8 r78=7e r79=e3 r9=%02x r12=%02x r13=%02x "
		 "r16=%02x r17=%02x\n",
		 status, dac_flag2, sticky1, sticky2, snapshot_doutctl,
		 dac_setup, snapshot_mute, snapshot_ldacvol, snapshot_rdacvol,
		 snapshot_drc, AIC32X4_DIAG_BEEP_LENGTH_MSB_VALUE,
		 AIC32X4_DIAG_BEEP_LENGTH_MID_VALUE,
		 AIC32X4_DIAG_BEEP_LENGTH_LSB_VALUE, outpwr, hpl_route,
		 hpr_route, aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved),
							 AIC32X4_HPLGAIN),
		 aic32x4_diag_saved_value(saved, ARRAY_SIZE(saved), AIC32X4_HPRGAIN));
	if ((dac_flag2 & AIC32X4_DIAG_DAC_GAIN_MASK) !=
			AIC32X4_DIAG_DAC_GAIN_MASK ||
	    (snapshot_mute & AIC32X4_DIAG_DAC_MUTE_MASK) ||
	    snapshot_ldacvol == 0x80 || snapshot_rdacvol == 0x80 ||
	    (snapshot_doutctl & AIC32X4_DIAG_MFP2_FUNC_MASK) !=
			AIC32X4_DIAG_MFP2_GPIO_FUNC || snapshot_drc != 0x0f)
		goto restore;
	if (sticky1 & AIC32X4_DIAG_DAC_STICKY_MASK)
		dev_warn(component->dev,
			 "prb25-diag-dac-overflow sticky=%02x\n", sticky1);
	if (sticky2 & AIC32X4_DIAG_HP_STICKY_MASK)
		dev_warn(component->dev,
			 "prb25-diag-hp-overcurrent sticky=%02x\n", sticky2);
	dev_info(component->dev,
		 "prb25-diag-prepared dacflag1=%02x dacflag2=%02x\n",
		 status, dac_flag2);
	ret = aic32x4_diag_wait_beep(component, 0x00, 0x77, 0x00);
	if (ret)
		goto restore;
	ret = aic32x4_diag_wait_beep(component, 0x01, 0x77, 0x00);
	if (ret)
		goto restore;
	ret = aic32x4_diag_wait_beep(component, 0x02, 0x77, 0x00);
	if (ret)
		goto restore;
	ret = 0;

restore:
	restore_ret = aic32x4_diag_restore(component, saved,
					   ARRAY_SIZE(saved));
	if (!ret)
		ret = restore_ret;
	return ret;
}

static int aic32x4_diag_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *value)
{
	value->value.integer.value[0] = 0;
	return 0;
}

static int aic32x4_diag_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aic32x4_priv *aic32x4 =
		snd_soc_component_get_drvdata(component);
	int ret;

	if (!value->value.integer.value[0])
		return 0;
	if (mutex_lock_interruptible(&aic32x4->diag_lock))
		return -ERESTARTSYS;
	if (!aic32x4->playback_active) {
		mutex_unlock(&aic32x4->diag_lock);
		dev_warn(component->dev,
			 "prb25-diag rejected: playback stream is not active\n");
		return -EBUSY;
	}
	mutex_lock(&component->io_mutex);
	ret = aic32x4_diag_run(component);
	mutex_unlock(&component->io_mutex);
	mutex_unlock(&aic32x4->diag_lock);

	return ret ? ret : 1;
}

static const struct snd_kcontrol_new aic32x4_diag_controls[] = {
	SOC_SINGLE_BOOL_EXT("PRB_P25 Internal Beep Trigger", 0,
			    aic32x4_diag_get, aic32x4_diag_put),
};
#endif

static const struct snd_kcontrol_new aic32x4_snd_controls[] = {
	SOC_DOUBLE_R_S_TLV("PCM Playback Volume", AIC32X4_LDACVOL,
			AIC32X4_RDACVOL, 0, -0x7f, 0x30, 7, 0, tlv_pcm),
	SOC_ENUM("DAC Left Playback PowerTune Switch", l_ptm_enum),
	SOC_ENUM("DAC Right Playback PowerTune Switch", r_ptm_enum),
	SOC_DOUBLE_R_S_TLV("HP Driver Gain Volume", AIC32X4_HPLGAIN,
			AIC32X4_HPRGAIN, 0, -0x6, 0x1d, 5, 0,
			tlv_driver_gain),
	SOC_DOUBLE_R_S_TLV("LO Driver Gain Volume", AIC32X4_LOLGAIN,
			AIC32X4_LORGAIN, 0, -0x6, 0x1d, 5, 0,
			tlv_driver_gain),
	SOC_DOUBLE_R("HP DAC Playback Switch", AIC32X4_HPLGAIN,
			AIC32X4_HPRGAIN, 6, 0x01, 1),
	SOC_DOUBLE_R("LO DAC Playback Switch", AIC32X4_LOLGAIN,
			AIC32X4_LORGAIN, 6, 0x01, 1),
	SOC_ENUM("LO Playback Common Mode Switch", lo_cm_enum),
	SOC_DOUBLE_R("Mic PGA Switch", AIC32X4_LMICPGAVOL,
			AIC32X4_RMICPGAVOL, 7, 0x01, 1),

	SOC_SINGLE("ADCFGA Left Mute Switch", AIC32X4_ADCFGA, 7, 1, 0),
	SOC_SINGLE("ADCFGA Right Mute Switch", AIC32X4_ADCFGA, 3, 1, 0),

	SOC_DOUBLE_R_S_TLV("ADC Level Volume", AIC32X4_LADCVOL,
			AIC32X4_RADCVOL, 0, -0x18, 0x28, 6, 0, tlv_adc_vol),
	SOC_DOUBLE_R_TLV("PGA Level Volume", AIC32X4_LMICPGAVOL,
			AIC32X4_RMICPGAVOL, 0, 0x5f, 0, tlv_step_0_5),

	SOC_SINGLE("Auto-mute Switch", AIC32X4_DACMUTE, 4, 7, 0),

	SOC_SINGLE("AGC Left Switch", AIC32X4_LAGC1, 7, 1, 0),
	SOC_SINGLE("AGC Right Switch", AIC32X4_RAGC1, 7, 1, 0),
	SOC_DOUBLE_R("AGC Target Level", AIC32X4_LAGC1, AIC32X4_RAGC1,
			4, 0x07, 0),
	SOC_DOUBLE_R("AGC Gain Hysteresis", AIC32X4_LAGC1, AIC32X4_RAGC1,
			0, 0x03, 0),
	SOC_DOUBLE_R("AGC Hysteresis", AIC32X4_LAGC2, AIC32X4_RAGC2,
			6, 0x03, 0),
	SOC_DOUBLE_R("AGC Noise Threshold", AIC32X4_LAGC2, AIC32X4_RAGC2,
			1, 0x1F, 0),
	SOC_DOUBLE_R("AGC Max PGA", AIC32X4_LAGC3, AIC32X4_RAGC3,
			0, 0x7F, 0),
	SOC_DOUBLE_R("AGC Attack Time", AIC32X4_LAGC4, AIC32X4_RAGC4,
			3, 0x1F, 0),
	SOC_DOUBLE_R("AGC Decay Time", AIC32X4_LAGC5, AIC32X4_RAGC5,
			3, 0x1F, 0),
	SOC_DOUBLE_R("AGC Noise Debounce", AIC32X4_LAGC6, AIC32X4_RAGC6,
			0, 0x1F, 0),
	SOC_DOUBLE_R("AGC Signal Debounce", AIC32X4_LAGC7, AIC32X4_RAGC7,
			0, 0x0F, 0),
};

static const struct snd_kcontrol_new hpl_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("L_DAC Switch", AIC32X4_HPLROUTE, 3, 1, 0),
	SOC_DAPM_SINGLE("IN1_L Switch", AIC32X4_HPLROUTE, 2, 1, 0),
};

static const struct snd_kcontrol_new hpr_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("R_DAC Switch", AIC32X4_HPRROUTE, 3, 1, 0),
	SOC_DAPM_SINGLE("IN1_R Switch", AIC32X4_HPRROUTE, 2, 1, 0),
};

static const struct snd_kcontrol_new lol_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("L_DAC Switch", AIC32X4_LOLROUTE, 3, 1, 0),
};

static const struct snd_kcontrol_new lor_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("R_DAC Switch", AIC32X4_LORROUTE, 3, 1, 0),
};

static const char * const resistor_text[] = {
	"Off", "10 kOhm", "20 kOhm", "40 kOhm",
};

/* Left mixer pins */
static SOC_ENUM_SINGLE_DECL(in1l_lpga_p_enum, AIC32X4_LMICPGAPIN, 6, resistor_text);
static SOC_ENUM_SINGLE_DECL(in2l_lpga_p_enum, AIC32X4_LMICPGAPIN, 4, resistor_text);
static SOC_ENUM_SINGLE_DECL(in3l_lpga_p_enum, AIC32X4_LMICPGAPIN, 2, resistor_text);
static SOC_ENUM_SINGLE_DECL(in1r_lpga_p_enum, AIC32X4_LMICPGAPIN, 0, resistor_text);

static SOC_ENUM_SINGLE_DECL(cml_lpga_n_enum, AIC32X4_LMICPGANIN, 6, resistor_text);
static SOC_ENUM_SINGLE_DECL(in2r_lpga_n_enum, AIC32X4_LMICPGANIN, 4, resistor_text);
static SOC_ENUM_SINGLE_DECL(in3r_lpga_n_enum, AIC32X4_LMICPGANIN, 2, resistor_text);

static const struct snd_kcontrol_new in1l_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN1_L L+ Switch", in1l_lpga_p_enum),
};
static const struct snd_kcontrol_new in2l_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN2_L L+ Switch", in2l_lpga_p_enum),
};
static const struct snd_kcontrol_new in3l_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN3_L L+ Switch", in3l_lpga_p_enum),
};
static const struct snd_kcontrol_new in1r_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN1_R L+ Switch", in1r_lpga_p_enum),
};
static const struct snd_kcontrol_new cml_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("CM_L L- Switch", cml_lpga_n_enum),
};
static const struct snd_kcontrol_new in2r_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN2_R L- Switch", in2r_lpga_n_enum),
};
static const struct snd_kcontrol_new in3r_to_lmixer_controls[] = {
	SOC_DAPM_ENUM("IN3_R L- Switch", in3r_lpga_n_enum),
};

/*	Right mixer pins */
static SOC_ENUM_SINGLE_DECL(in1r_rpga_p_enum, AIC32X4_RMICPGAPIN, 6, resistor_text);
static SOC_ENUM_SINGLE_DECL(in2r_rpga_p_enum, AIC32X4_RMICPGAPIN, 4, resistor_text);
static SOC_ENUM_SINGLE_DECL(in3r_rpga_p_enum, AIC32X4_RMICPGAPIN, 2, resistor_text);
static SOC_ENUM_SINGLE_DECL(in2l_rpga_p_enum, AIC32X4_RMICPGAPIN, 0, resistor_text);
static SOC_ENUM_SINGLE_DECL(cmr_rpga_n_enum, AIC32X4_RMICPGANIN, 6, resistor_text);
static SOC_ENUM_SINGLE_DECL(in1l_rpga_n_enum, AIC32X4_RMICPGANIN, 4, resistor_text);
static SOC_ENUM_SINGLE_DECL(in3l_rpga_n_enum, AIC32X4_RMICPGANIN, 2, resistor_text);

static const struct snd_kcontrol_new in1r_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN1_R R+ Switch", in1r_rpga_p_enum),
};
static const struct snd_kcontrol_new in2r_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN2_R R+ Switch", in2r_rpga_p_enum),
};
static const struct snd_kcontrol_new in3r_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN3_R R+ Switch", in3r_rpga_p_enum),
};
static const struct snd_kcontrol_new in2l_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN2_L R+ Switch", in2l_rpga_p_enum),
};
static const struct snd_kcontrol_new cmr_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("CM_R R- Switch", cmr_rpga_n_enum),
};
static const struct snd_kcontrol_new in1l_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN1_L R- Switch", in1l_rpga_n_enum),
};
static const struct snd_kcontrol_new in3l_to_rmixer_controls[] = {
	SOC_DAPM_ENUM("IN3_L R- Switch", in3l_rpga_n_enum),
};

static const struct snd_soc_dapm_widget aic32x4_dapm_widgets[] = {
	/* Both physical speaker bands belong to the single Playback stream. */
	SND_SOC_DAPM_DAC("Left DAC", "Playback", AIC32X4_DACSETUP, 7, 0),
	SND_SOC_DAPM_MIXER("HPL Output Mixer", SND_SOC_NOPM, 0, 0,
			   &hpl_output_mixer_controls[0],
			   ARRAY_SIZE(hpl_output_mixer_controls)),
	SND_SOC_DAPM_PGA("HPL Power", AIC32X4_OUTPWRCTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("LOL Output Mixer", SND_SOC_NOPM, 0, 0,
			   &lol_output_mixer_controls[0],
			   ARRAY_SIZE(lol_output_mixer_controls)),
	SND_SOC_DAPM_PGA("LOL Power", AIC32X4_OUTPWRCTL, 3, 0, NULL, 0),

	SND_SOC_DAPM_DAC("Right DAC", "Playback", AIC32X4_DACSETUP, 6, 0),
	SND_SOC_DAPM_MIXER("HPR Output Mixer", SND_SOC_NOPM, 0, 0,
			   &hpr_output_mixer_controls[0],
			   ARRAY_SIZE(hpr_output_mixer_controls)),
	SND_SOC_DAPM_PGA("HPR Power", AIC32X4_OUTPWRCTL, 4, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("LOR Output Mixer", SND_SOC_NOPM, 0, 0,
			   &lor_output_mixer_controls[0],
			   ARRAY_SIZE(lor_output_mixer_controls)),
	SND_SOC_DAPM_PGA("LOR Power", AIC32X4_OUTPWRCTL, 2, 0, NULL, 0),

	SND_SOC_DAPM_ADC("Right ADC", "Right Capture", AIC32X4_ADCSETUP, 6, 0),
	SND_SOC_DAPM_MUX("IN1_R to Right Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in1r_to_rmixer_controls),
	SND_SOC_DAPM_MUX("IN2_R to Right Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in2r_to_rmixer_controls),
	SND_SOC_DAPM_MUX("IN3_R to Right Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in3r_to_rmixer_controls),
	SND_SOC_DAPM_MUX("IN2_L to Right Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in2l_to_rmixer_controls),
	SND_SOC_DAPM_MUX("CM_R to Right Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			cmr_to_rmixer_controls),
	SND_SOC_DAPM_MUX("IN1_L to Right Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			in1l_to_rmixer_controls),
	SND_SOC_DAPM_MUX("IN3_L to Right Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			in3l_to_rmixer_controls),

	SND_SOC_DAPM_ADC("Left ADC", "Left Capture", AIC32X4_ADCSETUP, 7, 0),
	SND_SOC_DAPM_MUX("IN1_L to Left Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in1l_to_lmixer_controls),
	SND_SOC_DAPM_MUX("IN2_L to Left Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in2l_to_lmixer_controls),
	SND_SOC_DAPM_MUX("IN3_L to Left Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in3l_to_lmixer_controls),
	SND_SOC_DAPM_MUX("IN1_R to Left Mixer Positive Resistor", SND_SOC_NOPM, 0, 0,
			in1r_to_lmixer_controls),
	SND_SOC_DAPM_MUX("CM_L to Left Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			cml_to_lmixer_controls),
	SND_SOC_DAPM_MUX("IN2_R to Left Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			in2r_to_lmixer_controls),
	SND_SOC_DAPM_MUX("IN3_R to Left Mixer Negative Resistor", SND_SOC_NOPM, 0, 0,
			in3r_to_lmixer_controls),

	SND_SOC_DAPM_SUPPLY("Mic Bias", AIC32X4_MICBIAS, 6, 0, mic_bias_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_POST("ADC Reset", aic32x4_reset_adc),

	SND_SOC_DAPM_OUTPUT("HPL"),
	SND_SOC_DAPM_OUTPUT("HPR"),
	SND_SOC_DAPM_OUTPUT("LOL"),
	SND_SOC_DAPM_OUTPUT("LOR"),
	SND_SOC_DAPM_INPUT("IN1_L"),
	SND_SOC_DAPM_INPUT("IN1_R"),
	SND_SOC_DAPM_INPUT("IN2_L"),
	SND_SOC_DAPM_INPUT("IN2_R"),
	SND_SOC_DAPM_INPUT("IN3_L"),
	SND_SOC_DAPM_INPUT("IN3_R"),
	SND_SOC_DAPM_INPUT("CM_L"),
	SND_SOC_DAPM_INPUT("CM_R"),
};

static const struct snd_soc_dapm_route aic32x4_dapm_routes[] = {
	/* Left Output */
	{"HPL Output Mixer", "L_DAC Switch", "Left DAC"},
	{"HPL Output Mixer", "IN1_L Switch", "IN1_L"},

	{"HPL Power", NULL, "HPL Output Mixer"},
	{"HPL", NULL, "HPL Power"},

	{"LOL Output Mixer", "L_DAC Switch", "Left DAC"},

	{"LOL Power", NULL, "LOL Output Mixer"},
	{"LOL", NULL, "LOL Power"},

	/* Right Output */
	{"HPR Output Mixer", "R_DAC Switch", "Right DAC"},
	{"HPR Output Mixer", "IN1_R Switch", "IN1_R"},

	{"HPR Power", NULL, "HPR Output Mixer"},
	{"HPR", NULL, "HPR Power"},

	{"LOR Output Mixer", "R_DAC Switch", "Right DAC"},

	{"LOR Power", NULL, "LOR Output Mixer"},
	{"LOR", NULL, "LOR Power"},

	/* Right Input */
	{"Right ADC", NULL, "IN1_R to Right Mixer Positive Resistor"},
	{"IN1_R to Right Mixer Positive Resistor", "10 kOhm", "IN1_R"},
	{"IN1_R to Right Mixer Positive Resistor", "20 kOhm", "IN1_R"},
	{"IN1_R to Right Mixer Positive Resistor", "40 kOhm", "IN1_R"},

	{"Right ADC", NULL, "IN2_R to Right Mixer Positive Resistor"},
	{"IN2_R to Right Mixer Positive Resistor", "10 kOhm", "IN2_R"},
	{"IN2_R to Right Mixer Positive Resistor", "20 kOhm", "IN2_R"},
	{"IN2_R to Right Mixer Positive Resistor", "40 kOhm", "IN2_R"},

	{"Right ADC", NULL, "IN3_R to Right Mixer Positive Resistor"},
	{"IN3_R to Right Mixer Positive Resistor", "10 kOhm", "IN3_R"},
	{"IN3_R to Right Mixer Positive Resistor", "20 kOhm", "IN3_R"},
	{"IN3_R to Right Mixer Positive Resistor", "40 kOhm", "IN3_R"},

	{"Right ADC", NULL, "IN2_L to Right Mixer Positive Resistor"},
	{"IN2_L to Right Mixer Positive Resistor", "10 kOhm", "IN2_L"},
	{"IN2_L to Right Mixer Positive Resistor", "20 kOhm", "IN2_L"},
	{"IN2_L to Right Mixer Positive Resistor", "40 kOhm", "IN2_L"},

	{"Right ADC", NULL, "CM_R to Right Mixer Negative Resistor"},
	{"CM_R to Right Mixer Negative Resistor", "10 kOhm", "CM_R"},
	{"CM_R to Right Mixer Negative Resistor", "20 kOhm", "CM_R"},
	{"CM_R to Right Mixer Negative Resistor", "40 kOhm", "CM_R"},

	{"Right ADC", NULL, "IN1_L to Right Mixer Negative Resistor"},
	{"IN1_L to Right Mixer Negative Resistor", "10 kOhm", "IN1_L"},
	{"IN1_L to Right Mixer Negative Resistor", "20 kOhm", "IN1_L"},
	{"IN1_L to Right Mixer Negative Resistor", "40 kOhm", "IN1_L"},

	{"Right ADC", NULL, "IN3_L to Right Mixer Negative Resistor"},
	{"IN3_L to Right Mixer Negative Resistor", "10 kOhm", "IN3_L"},
	{"IN3_L to Right Mixer Negative Resistor", "20 kOhm", "IN3_L"},
	{"IN3_L to Right Mixer Negative Resistor", "40 kOhm", "IN3_L"},

	/* Left Input */
	{"Left ADC", NULL, "IN1_L to Left Mixer Positive Resistor"},
	{"IN1_L to Left Mixer Positive Resistor", "10 kOhm", "IN1_L"},
	{"IN1_L to Left Mixer Positive Resistor", "20 kOhm", "IN1_L"},
	{"IN1_L to Left Mixer Positive Resistor", "40 kOhm", "IN1_L"},

	{"Left ADC", NULL, "IN2_L to Left Mixer Positive Resistor"},
	{"IN2_L to Left Mixer Positive Resistor", "10 kOhm", "IN2_L"},
	{"IN2_L to Left Mixer Positive Resistor", "20 kOhm", "IN2_L"},
	{"IN2_L to Left Mixer Positive Resistor", "40 kOhm", "IN2_L"},

	{"Left ADC", NULL, "IN3_L to Left Mixer Positive Resistor"},
	{"IN3_L to Left Mixer Positive Resistor", "10 kOhm", "IN3_L"},
	{"IN3_L to Left Mixer Positive Resistor", "20 kOhm", "IN3_L"},
	{"IN3_L to Left Mixer Positive Resistor", "40 kOhm", "IN3_L"},

	{"Left ADC", NULL, "IN1_R to Left Mixer Positive Resistor"},
	{"IN1_R to Left Mixer Positive Resistor", "10 kOhm", "IN1_R"},
	{"IN1_R to Left Mixer Positive Resistor", "20 kOhm", "IN1_R"},
	{"IN1_R to Left Mixer Positive Resistor", "40 kOhm", "IN1_R"},

	{"Left ADC", NULL, "CM_L to Left Mixer Negative Resistor"},
	{"CM_L to Left Mixer Negative Resistor", "10 kOhm", "CM_L"},
	{"CM_L to Left Mixer Negative Resistor", "20 kOhm", "CM_L"},
	{"CM_L to Left Mixer Negative Resistor", "40 kOhm", "CM_L"},

	{"Left ADC", NULL, "IN2_R to Left Mixer Negative Resistor"},
	{"IN2_R to Left Mixer Negative Resistor", "10 kOhm", "IN2_R"},
	{"IN2_R to Left Mixer Negative Resistor", "20 kOhm", "IN2_R"},
	{"IN2_R to Left Mixer Negative Resistor", "40 kOhm", "IN2_R"},

	{"Left ADC", NULL, "IN3_R to Left Mixer Negative Resistor"},
	{"IN3_R to Left Mixer Negative Resistor", "10 kOhm", "IN3_R"},
	{"IN3_R to Left Mixer Negative Resistor", "20 kOhm", "IN3_R"},
	{"IN3_R to Left Mixer Negative Resistor", "40 kOhm", "IN3_R"},
};

static const struct regmap_range_cfg aic32x4_regmap_pages[] = {
	{
		.selector_reg = 0,
		.selector_mask	= 0xff,
		.window_start = 0,
		.window_len = 128,
		.range_min = 0,
		.range_max = AIC32X4_MAX_DSP_REG,
	},
};

const struct regmap_config aic32x4_regmap_config = {
	.max_register = AIC32X4_MAX_DSP_REG,
	.ranges = aic32x4_regmap_pages,
	.num_ranges = ARRAY_SIZE(aic32x4_regmap_pages),
};
EXPORT_SYMBOL(aic32x4_regmap_config);

static int aic32x4_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *component = codec_dai->component;
	struct clk *mclk;
	struct clk *pll;

	pll = devm_clk_get(component->dev, "pll");
	if (IS_ERR(pll))
		return PTR_ERR(pll);

	mclk = clk_get_parent(pll);

	return clk_set_rate(mclk, freq);
}

static int aic32x4_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);
	u8 iface_reg_1 = 0;
	u8 iface_reg_2 = 0;
	u8 iface_reg_3 = 0;
	u8 iface_reg_3_mask = AIC32X4_BCLKINV_MASK;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		iface_reg_1 |= AIC32X4_BCLKMASTER | AIC32X4_WCLKMASTER;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		/* Match the working 3.18 Puffin slave-mode clock source. */
		if (aic32x4->type == AIC32X4_TYPE_AIC32X4) {
			iface_reg_3 |= AIC32X4_DACMOD2BCLK;
			iface_reg_3_mask |= AIC32X4_BDIVCLK_MASK;
		}
		break;
	default:
		printk(KERN_ERR "aic32x4: invalid clock provider\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface_reg_1 |= (AIC32X4_DSP_MODE <<
				AIC32X4_IFACE1_DATATYPE_SHIFT);
		iface_reg_3 |= AIC32X4_BCLKINV_MASK; /* invert bit clock */
		iface_reg_2 = 0x01; /* add offset 1 */
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface_reg_1 |= (AIC32X4_DSP_MODE <<
				AIC32X4_IFACE1_DATATYPE_SHIFT);
		iface_reg_3 |= AIC32X4_BCLKINV_MASK; /* invert bit clock */
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		iface_reg_1 |= (AIC32X4_RIGHT_JUSTIFIED_MODE <<
				AIC32X4_IFACE1_DATATYPE_SHIFT);
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface_reg_1 |= (AIC32X4_LEFT_JUSTIFIED_MODE <<
				AIC32X4_IFACE1_DATATYPE_SHIFT);
		break;
	default:
		printk(KERN_ERR "aic32x4: invalid DAI interface format\n");
		return -EINVAL;
	}

	aic32x4->fmt = fmt;

	snd_soc_component_update_bits(component, AIC32X4_IFACE1,
				AIC32X4_IFACE1_DATATYPE_MASK |
				AIC32X4_IFACE1_MASTER_MASK, iface_reg_1);
	snd_soc_component_update_bits(component, AIC32X4_IFACE2,
				AIC32X4_DATA_OFFSET_MASK, iface_reg_2);
	snd_soc_component_update_bits(component, AIC32X4_IFACE3,
				iface_reg_3_mask, iface_reg_3);

	return 0;
}

static int aic32x4_set_aosr(struct snd_soc_component *component, u8 aosr)
{
	return snd_soc_component_write(component, AIC32X4_AOSR, aosr);
}

static int aic32x4_set_dosr(struct snd_soc_component *component, u16 dosr)
{
	int ret;

	ret = snd_soc_component_write(component, AIC32X4_DOSRMSB, dosr >> 8);
	if (ret)
		return ret;
	return snd_soc_component_write(component, AIC32X4_DOSRLSB,
				       dosr & 0xff);
}

static int aic32x4_set_processing_blocks(struct snd_soc_component *component,
						u8 r_block, u8 p_block)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);

	if (aic32x4->type == AIC32X4_TYPE_TAS2505) {
		if (r_block || p_block > 3)
			return -EINVAL;

		snd_soc_component_write(component, AIC32X4_DACSPB, p_block);
	} else { /* AIC32x4 */
		if (r_block > 18 || p_block > 25)
			return -EINVAL;

		snd_soc_component_write(component, AIC32X4_ADCSPB, r_block);
		snd_soc_component_write(component, AIC32X4_DACSPB, p_block);
	}

	return 0;
}

#define AIC32X4_PUFFIN_MCLK_RATE	9600000
#define AIC32X4_PUFFIN_SAMPLE_RATE	48000

static int aic32x4_setup_puffin_48k_clocks(
		struct snd_soc_component *component,
		struct clk_bulk_data *clocks)
{
	int ret;

	/*
	 * Reproduce the codec-owned 3.18 Radar-Puffin clock transaction:
	 *
	 *   MCLK  = 9.6 MHz
	 *   PLL   = 9.6 MHz * 48 / 5 = 92.16 MHz
	 *   DACFS = 92.16 MHz / 5 / 3 / 128 = 48 kHz
	 *   ADCFS = 92.16 MHz / 5 / 3 / 128 = 48 kHz
	 *
	 * Keep CCF as the sole owner.  The matching clock-provider special case
	 * below makes the PLL rate request use the stock P=5/J=48/D=0 encoding.
	 */
	ret = clk_set_rate(clocks[0].clk, 92160000);
	if (ret)
		return ret;
	ret = clk_set_rate(clocks[1].clk, 18432000);
	if (ret)
		return ret;
	ret = clk_set_rate(clocks[2].clk, 6144000);
	if (ret)
		return ret;
	ret = aic32x4_set_aosr(component, 128);
	if (ret)
		return ret;
	ret = clk_set_rate(clocks[3].clk, 18432000);
	if (ret)
		return ret;
	ret = clk_set_rate(clocks[4].clk, 6144000);
	if (ret)
		return ret;
	ret = aic32x4_set_dosr(component, 128);
	if (ret)
		return ret;
	ret = clk_set_rate(clocks[5].clk, 6144000);
	if (ret)
		return ret;

	return aic32x4_set_processing_blocks(component, 1, 2);
}

static int aic32x4_setup_clocks(struct snd_soc_component *component,
				unsigned int sample_rate, unsigned int channels,
				unsigned int bit_depth)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);
	u8 aosr;
	u16 dosr;
	u8 adc_resource_class, dac_resource_class;
	u8 madc, nadc, mdac, ndac, max_nadc, min_mdac, max_ndac;
	u8 dosr_increment;
	u16 max_dosr, min_dosr;
	unsigned long adc_clock_rate, dac_clock_rate;
	int ret;

	static struct clk_bulk_data clocks[] = {
		{ .id = "pll" },
		{ .id = "nadc" },
		{ .id = "madc" },
		{ .id = "ndac" },
		{ .id = "mdac" },
		{ .id = "bdiv" },
	};
	ret = devm_clk_bulk_get(component->dev, ARRAY_SIZE(clocks), clocks);
	if (ret)
		return ret;
	if (aic32x4->type == AIC32X4_TYPE_AIC32X4 &&
	    sample_rate == AIC32X4_PUFFIN_SAMPLE_RATE &&
	    clk_get_rate(clk_get_parent(clocks[0].clk)) ==
						AIC32X4_PUFFIN_MCLK_RATE)
		return aic32x4_setup_puffin_48k_clocks(component, clocks);

	if (sample_rate <= 48000) {
		aosr = 128;
		adc_resource_class = 6;
		dac_resource_class = 8;
		dosr_increment = 8;
		if (aic32x4->type == AIC32X4_TYPE_TAS2505) {
			aic32x4_set_processing_blocks(component, 0, 1);
		} else if (aic32x4->type == AIC32X4_TYPE_AIC32X4) {
			/*
			 * Radar-Puffin uses PRB_P2 and its calibrated six-biquad
			 * speaker profile.  PRB_P2 is resource class 12, so select
			 * it before solving the divider tree.  Solving as PRB_P1/RC8
			 * permits MDAC=2 with DOSR=128, below PRB_P2's requirement;
			 * RC12 requires MDAC >= 3 for that DOSR, matching 3.18.
			 */
			dac_resource_class = 12;
			aic32x4_set_processing_blocks(component, 1, 2);
		} else {
			aic32x4_set_processing_blocks(component, 1, 1);
		}
	} else if (sample_rate <= 96000) {
		aosr = 64;
		adc_resource_class = 6;
		dac_resource_class = 8;
		dosr_increment = 4;
		if (aic32x4->type == AIC32X4_TYPE_TAS2505)
			aic32x4_set_processing_blocks(component, 0, 1);
		else
			aic32x4_set_processing_blocks(component, 1, 9);
	} else if (sample_rate == 192000) {
		aosr = 32;
		adc_resource_class = 3;
		dac_resource_class = 4;
		dosr_increment = 2;
		if (aic32x4->type == AIC32X4_TYPE_TAS2505)
			aic32x4_set_processing_blocks(component, 0, 1);
		else
			aic32x4_set_processing_blocks(component, 13, 19);
	} else {
		dev_err(component->dev, "Sampling rate not supported\n");
		return -EINVAL;
	}

	/* PCM over I2S is always 2-channel */
	if ((aic32x4->fmt & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_I2S)
		channels = 2;

	madc = DIV_ROUND_UP((32 * adc_resource_class), aosr);
	max_dosr = (AIC32X4_MAX_DOSR_FREQ / sample_rate / dosr_increment) *
			dosr_increment;
	min_dosr = (AIC32X4_MIN_DOSR_FREQ / sample_rate / dosr_increment) *
			dosr_increment;
	max_nadc = AIC32X4_MAX_CODEC_CLKIN_FREQ / (madc * aosr * sample_rate);

	for (nadc = max_nadc; nadc > 0; --nadc) {
		adc_clock_rate = nadc * madc * aosr * sample_rate;
		for (dosr = max_dosr; dosr >= min_dosr;
				dosr -= dosr_increment) {
			min_mdac = DIV_ROUND_UP((32 * dac_resource_class), dosr);
			max_ndac = AIC32X4_MAX_CODEC_CLKIN_FREQ /
					(min_mdac * dosr * sample_rate);
			for (mdac = min_mdac; mdac <= 128; ++mdac) {
				for (ndac = max_ndac; ndac > 0; --ndac) {
					dac_clock_rate = ndac * mdac * dosr *
							sample_rate;
					if (dac_clock_rate == adc_clock_rate) {
						if (clk_round_rate(clocks[0].clk, dac_clock_rate) == 0)
							continue;

						ret = clk_set_rate(clocks[0].clk,
							   dac_clock_rate);
						if (ret)
							return ret;

						ret = clk_set_rate(clocks[1].clk,
							   sample_rate * aosr * madc);
						if (ret)
							return ret;
						ret = clk_set_rate(clocks[2].clk,
							   sample_rate * aosr);
						if (ret)
							return ret;
						ret = aic32x4_set_aosr(component, aosr);
						if (ret)
							return ret;

						ret = clk_set_rate(clocks[3].clk,
							   sample_rate * dosr * mdac);
						if (ret)
							return ret;
						ret = clk_set_rate(clocks[4].clk,
							   sample_rate * dosr);
						if (ret)
							return ret;
						ret = aic32x4_set_dosr(component, dosr);
						if (ret)
							return ret;

						ret = clk_set_rate(clocks[5].clk,
							   sample_rate * channels * bit_depth);
						if (ret)
							return ret;

						return 0;
					}
				}
			}
		}
	}

	dev_err(component->dev,
		"Could not set clocks to support sample rate.\n");
	return -EINVAL;
}

static int aic32x4_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);
	u8 iface1_reg = 0;
	u8 dacsetup_reg = 0;
	int ret;

	/*
	 * The working 3.18 speaker codec asserts the physical MFP2 gate and
	 * both headphone-driver mutes before touching the PLL/divider tree.
	 * Keep that ordering across the modern CCF clock setup: the AIC32X4
	 * DAC PGA gain engine must be allowed to ramp with the analog outputs
	 * isolated, otherwise P0_R38 remains 0x00 even after the HP gain flag
	 * (P1_R63) reports applied.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_component_write(component, AIC32X4_DOUTCTL,
					      AIC32X4_MFP_GPIO_ENABLED |
					      AIC32X4_MFP2_GPIO_OUT_HIGH);
		if (ret < 0)
			return ret;
		ret = snd_soc_component_update_bits(component, AIC32X4_HPLGAIN,
						    BIT(6), BIT(6));
		if (ret < 0)
			return ret;
		ret = snd_soc_component_update_bits(component, AIC32X4_HPRGAIN,
						    BIT(6), BIT(6));
		if (ret < 0)
			return ret;
	}

	ret = aic32x4_setup_clocks(component, params_rate(params),
				params_channels(params),
				params_physical_width(params));
	if (ret)
		return ret;
	aic32x4_log_playback_state(component, "codec-hw_params-after-clocks");

	switch (params_physical_width(params)) {
	case 16:
		iface1_reg |= (AIC32X4_WORD_LEN_16BITS <<
				   AIC32X4_IFACE1_DATALEN_SHIFT);
		break;
	case 20:
		iface1_reg |= (AIC32X4_WORD_LEN_20BITS <<
				   AIC32X4_IFACE1_DATALEN_SHIFT);
		break;
	case 24:
		iface1_reg |= (AIC32X4_WORD_LEN_24BITS <<
				   AIC32X4_IFACE1_DATALEN_SHIFT);
		break;
	case 32:
		iface1_reg |= (AIC32X4_WORD_LEN_32BITS <<
				   AIC32X4_IFACE1_DATALEN_SHIFT);
		break;
	}
	snd_soc_component_update_bits(component, AIC32X4_IFACE1,
				AIC32X4_IFACE1_DATALEN_MASK, iface1_reg);

	if (params_channels(params) == 1) {
		aic32x4->playback_mono = true;
		/*
		 * Radar-Puffin is the stock MonoRight speaker path.  The
		 * working 3.18 speaker codec feeds the right serial slot to
		 * both DACs (0x24); feeding the left slot here leaves the
		 * physical speaker path silent even though the PCM runs.
		 */
		dacsetup_reg = AIC32X4_RDAC2RCHN | AIC32X4_LDAC2RCHN;
	} else {
		aic32x4->playback_mono = false;
		if (aic32x4->swapdacs)
			dacsetup_reg = AIC32X4_RDAC2LCHN | AIC32X4_LDAC2RCHN;
		else
			dacsetup_reg = AIC32X4_LDAC2LCHN | AIC32X4_RDAC2RCHN;
	}
	snd_soc_component_update_bits(component, AIC32X4_DACSETUP,
				AIC32X4_DAC_CHAN_MASK, dacsetup_reg);
	dev_info(component->dev,
		 "hw_params playback channels=%u dacsetup_route=0x%02x\n",
		 params_channels(params), dacsetup_reg);
	aic32x4_log_playback_state(component, "codec-hw_params-end");

	return 0;
}

static int aic32x4_playback_fail_closed(struct snd_soc_component *component,
						int original_error)
{
	int ret;

	/* Keep the codec DAC muted on any error.  MFP2 is already asserted by
	 * the caller before entering this cleanup path. */
	ret = snd_soc_component_update_bits(component, AIC32X4_DACMUTE,
						AIC32X4_MUTEON, AIC32X4_MUTEON);
	if (ret < 0)
		dev_err(component->dev,
			"failed to reassert DAC playback mute: %d\n", ret);
	return original_error;
}

static int aic32x4_apply_playback_mute(struct snd_soc_component *component,
						struct aic32x4_priv *aic32x4,
						int mute)
{
	unsigned int gain_ready_mask;
	unsigned int unmute_state;
	unsigned int retry = 0;
	int status;
	int ret;

	/* Match the working 3.18 sequence: assert the physical gate before
	 * every fallible codec read or write. */
	ret = snd_soc_component_write(component, AIC32X4_DOUTCTL,
				      AIC32X4_MFP_GPIO_ENABLED |
				      AIC32X4_MFP2_GPIO_OUT_HIGH);
	if (ret < 0)
		return ret;
	aic32x4_log_playback_state(component,
					   mute ? "mute-asserted" : "unmute-before-wait");

	status = snd_soc_component_read(component, AIC32X4_DACMUTE);
	if (status < 0)
		return aic32x4_playback_fail_closed(component, status);
	unmute_state = status & ~AIC32X4_MUTEON;

	if (mute) {
		ret = snd_soc_component_write(component, AIC32X4_DACMUTE,
					      unmute_state | AIC32X4_MUTEON);
		if (ret < 0)
			return aic32x4_playback_fail_closed(component, ret);
		return 0;
	}

	if (aic32x4->playback_mono) {
		unmute_state |= AIC32X4_UNMUTE_MONO;
		gain_ready_mask = AIC32X4_GAIN_APPLIED_MONO;
	} else {
		gain_ready_mask = AIC32X4_GAIN_APPLIED_STEREO;
	}

	status = snd_soc_component_read(component, AIC32X4_GAIN_APPLIED);
	if (status < 0)
		return aic32x4_playback_fail_closed(component, status);
	while ((status & gain_ready_mask) != gain_ready_mask &&
	       retry < AIC32X4_GAIN_APPLIED_RETRIES) {
		msleep(AIC32X4_GAIN_APPLIED_POLL_MS);
		status = snd_soc_component_read(component, AIC32X4_GAIN_APPLIED);
		if (status < 0)
			return aic32x4_playback_fail_closed(component, status);
		retry++;
	}
	if ((status & gain_ready_mask) != gain_ready_mask)
		dev_warn(component->dev,
			 "DAC gain-applied timeout (state=%x mask=%x); continuing unmute\n",
			 status, gain_ready_mask);

	/* The mono codec state is 0x08, as in the working 3.18 driver. */
	ret = snd_soc_component_write(component, AIC32X4_DACMUTE,
				      unmute_state);
	if (ret < 0)
		return aic32x4_playback_fail_closed(component, ret);

	/* Release MFP2 only after the DAC is successfully unmuted. */
	ret = snd_soc_component_write(component, AIC32X4_DOUTCTL,
				      AIC32X4_MFP_GPIO_ENABLED);
	if (ret < 0)
		return aic32x4_playback_fail_closed(component, ret);
	aic32x4_log_playback_state(component, "unmute-complete");

	dev_info(component->dev,
		 "playback unmute synchronous gain=%x retries=%u mono=%u\n",
		 status, retry, aic32x4->playback_mono);
	return 0;
}

static int aic32x4_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;
	struct aic32x4_priv *aic32x4 =
		snd_soc_component_get_drvdata(component);

	if (direction == SNDRV_PCM_STREAM_PLAYBACK &&
	    aic32x4->defer_playback_unmute) {
		/* The 3.18 AIC32x4 path performs this synchronously from the
		 * link trigger, before the MT8163 AFE start callback. */
		return aic32x4_apply_playback_mute(component, aic32x4, mute);
	}

	return snd_soc_component_update_bits(component, AIC32X4_DACMUTE,
					     AIC32X4_MUTEON,
					     mute ? AIC32X4_MUTEON : 0);
}

static int aic32x4_set_bias_level(struct snd_soc_component *component,
				  enum snd_soc_bias_level level)
{
	int ret;

	static struct clk_bulk_data clocks[] = {
		{ .id = "madc" },
		{ .id = "mdac" },
		{ .id = "bdiv" },
	};

	ret = devm_clk_bulk_get(component->dev, ARRAY_SIZE(clocks), clocks);
	if (ret)
		return ret;
	aic32x4_log_playback_state(component, "bias-enter");

	switch (level) {
	case SND_SOC_BIAS_ON:
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(clocks), clocks);
		if (ret) {
			dev_err(component->dev, "Failed to enable clocks\n");
			return ret;
		}
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		/* CCF owns both divider rate fields and enable bits. */
		if (snd_soc_component_get_bias_level(component) == SND_SOC_BIAS_OFF)
			break;
		clk_bulk_disable_unprepare(ARRAY_SIZE(clocks), clocks);
		break;
	case SND_SOC_BIAS_OFF:
		break;
	}
	aic32x4_log_playback_state(component, "bias-exit");
	return 0;
}

#define AIC32X4_RATES	SNDRV_PCM_RATE_8000_192000
#define AIC32X4_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE \
			 | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE \
			 | SNDRV_PCM_FMTBIT_S32_LE)

static int aic32x4_playback_startup(struct snd_pcm_substream *substream,
					    struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aic32x4_priv *aic32x4 =
		snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mutex_lock(&aic32x4->diag_lock);
		aic32x4->playback_active = true;
		mutex_unlock(&aic32x4->diag_lock);
	}
	return 0;
}

static void aic32x4_playback_shutdown(struct snd_pcm_substream *substream,
					      struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aic32x4_priv *aic32x4 =
		snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mutex_lock(&aic32x4->diag_lock);
		aic32x4->playback_active = false;
		mutex_unlock(&aic32x4->diag_lock);
	}
}

static const struct snd_soc_dai_ops aic32x4_ops = {
	.startup = aic32x4_playback_startup,
	.shutdown = aic32x4_playback_shutdown,
	.hw_params = aic32x4_hw_params,
	.mute_stream = aic32x4_mute,
	.set_fmt = aic32x4_set_dai_fmt,
	.set_sysclk = aic32x4_set_dai_sysclk,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver aic32x4_dai = {
	.name = "tlv320aic32x4-hifi",
	.playback = {
			 .stream_name = "Playback",
			 .channels_min = 1,
			 .channels_max = 2,
			 .rates = AIC32X4_RATES,
			 .formats = AIC32X4_FORMATS,},
	.capture = {
			.stream_name = "Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = AIC32X4_RATES,
			.formats = AIC32X4_FORMATS,},
	.ops = &aic32x4_ops,
	.symmetric_rate = 1,
};

static void aic32x4_setup_gpios(struct snd_soc_component *component)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);

	/* setup GPIO functions */
	/* MFP1 */
	if (aic32x4->setup->gpio_func[0] != AIC32X4_MFPX_DEFAULT_VALUE) {
		snd_soc_component_write(component, AIC32X4_DINCTL,
			  aic32x4->setup->gpio_func[0]);
		snd_soc_add_component_controls(component, aic32x4_mfp1,
			ARRAY_SIZE(aic32x4_mfp1));
	}

	/* MFP2 */
	if (aic32x4->setup->gpio_func[1] != AIC32X4_MFPX_DEFAULT_VALUE) {
		snd_soc_component_write(component, AIC32X4_DOUTCTL,
			  aic32x4->setup->gpio_func[1]);
		snd_soc_add_component_controls(component, aic32x4_mfp2,
			ARRAY_SIZE(aic32x4_mfp2));
	}

	/* MFP3 */
	if (aic32x4->setup->gpio_func[2] != AIC32X4_MFPX_DEFAULT_VALUE) {
		snd_soc_component_write(component, AIC32X4_SCLKCTL,
			  aic32x4->setup->gpio_func[2]);
		snd_soc_add_component_controls(component, aic32x4_mfp3,
			ARRAY_SIZE(aic32x4_mfp3));
	}

	/* MFP4 */
	if (aic32x4->setup->gpio_func[3] != AIC32X4_MFPX_DEFAULT_VALUE) {
		snd_soc_component_write(component, AIC32X4_MISOCTL,
			  aic32x4->setup->gpio_func[3]);
		snd_soc_add_component_controls(component, aic32x4_mfp4,
			ARRAY_SIZE(aic32x4_mfp4));
	}

	/* MFP5 */
	if (aic32x4->setup->gpio_func[4] != AIC32X4_MFPX_DEFAULT_VALUE) {
		snd_soc_component_write(component, AIC32X4_GPIOCTL,
			  aic32x4->setup->gpio_func[4]);
		snd_soc_add_component_controls(component, aic32x4_mfp5,
			ARRAY_SIZE(aic32x4_mfp5));
	}
}

static int aic32x4_component_probe(struct snd_soc_component *component)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);
	u32 tmp_reg;
	int ret;

	aic32x4->component = component;

	static struct clk_bulk_data clocks[] = {
		{ .id = "codec_clkin" },
		{ .id = "pll" },
		{ .id = "bdiv" },
		{ .id = "mdac" },
	};

	ret = devm_clk_bulk_get(component->dev, ARRAY_SIZE(clocks), clocks);
	if (ret)
		return ret;

	if (aic32x4->setup)
		aic32x4_setup_gpios(component);

	clk_set_parent(clocks[0].clk, clocks[1].clk);
	clk_set_parent(clocks[2].clk, clocks[3].clk);

	/*
	 * Match the working Puffin 3.18 bootstrap: feed the speaker
	 * outputs from the left/right DACs and leave the analogue input
	 * route disconnected.  The DAPM controls remain available for
	 * userspace, but a fresh codec must already have the physical
	 * speaker route established before the first PCM stream.
	 */
	ret = snd_soc_component_update_bits(component, AIC32X4_HPLROUTE,
					    BIT(3), BIT(3));
	if (ret < 0)
		return ret;
	ret = snd_soc_component_update_bits(component, AIC32X4_HPRROUTE,
					    BIT(3) | BIT(2), BIT(3));
	if (ret < 0)
		return ret;

	/* Power platform configuration */
	if (aic32x4->power_cfg & AIC32X4_PWR_MICBIAS_2075_LDOIN) {
		snd_soc_component_write(component, AIC32X4_MICBIAS,
				AIC32X4_MICBIAS_LDOIN | AIC32X4_MICBIAS_2075V);
	}
	if (aic32x4->power_cfg & AIC32X4_PWR_AVDD_DVDD_WEAK_DISABLE)
		snd_soc_component_write(component, AIC32X4_PWRCFG, AIC32X4_AVDDWEAKDISABLE);

	tmp_reg = (aic32x4->power_cfg & AIC32X4_PWR_AIC32X4_LDO_ENABLE) ?
			AIC32X4_LDOCTLEN : 0;
	snd_soc_component_write(component, AIC32X4_LDOCTL, tmp_reg);

	tmp_reg = AIC32X4_HP_CMMODE;
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_LDOIN_RANGE_18_36)
		tmp_reg |= AIC32X4_LDOIN_18_36;
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_HP_LDOIN_POWERED)
		tmp_reg |= AIC32X4_LDOIN2HP;
	snd_soc_component_write(component, AIC32X4_CMMODE, tmp_reg);

	/* Mic PGA routing */
	if (aic32x4->micpga_routing & AIC32X4_MICPGA_ROUTE_LMIC_IN2R_10K)
		snd_soc_component_write(component, AIC32X4_LMICPGANIN,
				AIC32X4_LMICPGANIN_IN2R_10K);
	else
		snd_soc_component_write(component, AIC32X4_LMICPGANIN,
				AIC32X4_LMICPGANIN_CM1L_10K);
	if (aic32x4->micpga_routing & AIC32X4_MICPGA_ROUTE_RMIC_IN1L_10K)
		snd_soc_component_write(component, AIC32X4_RMICPGANIN,
				AIC32X4_RMICPGANIN_IN1L_10K);
	else
		snd_soc_component_write(component, AIC32X4_RMICPGANIN,
				AIC32X4_RMICPGANIN_CM1R_10K);

	/* The old codec driver forced the reset-to-standby transition here. */
	ret = aic32x4_set_bias_level(component, SND_SOC_BIAS_STANDBY);
	if (ret < 0)
		return ret;

	/* Match the working 3.18 probe: keep the external amplifier muted while
	 * the ADC workaround and analogue reference are brought up. */
	/*
	 * Workaround: for an unknown reason, the ADC needs to be powered up
	 * and down for the first capture to work properly. It seems related to
	 * a HW BUG or some kind of behavior not documented in the datasheet.
	 */
	tmp_reg = snd_soc_component_read(component, AIC32X4_ADCSETUP);
	snd_soc_component_write(component, AIC32X4_ADCSETUP, tmp_reg |
				AIC32X4_LADC_EN | AIC32X4_RADC_EN);
	snd_soc_component_write(component, AIC32X4_ADCSETUP, tmp_reg);
	ret = snd_soc_component_write(component, AIC32X4_DOUTCTL,
				      AIC32X4_MFP_GPIO_ENABLED |
				      AIC32X4_MFP2_GPIO_OUT_HIGH);
	if (ret < 0)
		return ret;

	/* Match the working 3.18 board bootstrap: MFP3/SCLK is unused and
	 * must not retain the codec reset default (0x02). */
	snd_soc_component_write(component, AIC32X4_SCLKCTL, 0);

	/*
	 * Enable the fast charging feature and ensure the needed 40ms ellapsed
	 * before using the analog circuits.
	 */
	/* The working 3.18 AIC32x4 path uses normal 40 ms reference startup
	 * (P1_R123=0x01).  0x05 is the force-power-up variant and leaves the
	 * codec's DAC PGA gain-applied status at 0x00 on 6.1. */
	snd_soc_component_write(component, AIC32X4_REFPOWERUP,
				AIC32X4_REFPOWERUP_NORMAL_40MS);
	msleep(40);

	/*
	 * Set the DAC digital volume to 0 dB (full volume).  The DACVOL
	 * register is an attenuation control: 0x00 = 0 dB, higher values
	 * = quieter.  Without this, the codec starts at a high attenuation
	 * and the "PCM Playback Volume" mixer control maps to -60 dB.
	 * The 3.18 driver leaves DACVOL=0x00 (0 dB, full volume).
	 */
	snd_soc_component_write(component, AIC32X4_LDACVOL, 0);
	snd_soc_component_write(component, AIC32X4_RDACVOL, 0);

#if IS_ENABLED(CONFIG_SND_SOC_TLV320AIC32X4_PRB25_DIAG)
	if (aic32x4->type == AIC32X4_TYPE_AIC32X4) {
		ret = snd_soc_add_component_controls(component,
					     aic32x4_diag_controls,
					     ARRAY_SIZE(aic32x4_diag_controls));
		if (ret)
			return ret;
	}
#endif

	return 0;
}

static void aic32x4_component_remove(struct snd_soc_component *component)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);

	aic32x4->component = NULL;
}

static const struct snd_soc_component_driver soc_component_dev_aic32x4 = {
	.probe			= aic32x4_component_probe,
	.remove			= aic32x4_component_remove,
	.set_bias_level		= aic32x4_set_bias_level,
	.controls		= aic32x4_snd_controls,
	.num_controls		= ARRAY_SIZE(aic32x4_snd_controls),
	.dapm_widgets		= aic32x4_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(aic32x4_dapm_widgets),
	.dapm_routes		= aic32x4_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(aic32x4_dapm_routes),
	.suspend_bias_off	= 1,
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_kcontrol_new aic32x4_tas2505_snd_controls[] = {
	SOC_SINGLE_S8_TLV("PCM Playback Volume",
			  AIC32X4_LDACVOL, -0x7f, 0x30, tlv_pcm),
	SOC_ENUM("DAC Playback PowerTune Switch", l_ptm_enum),

	SOC_SINGLE_TLV("HP Driver Gain Volume",
			AIC32X4_HPLGAIN, 0, 0x74, 1, tlv_tas_driver_gain),
	SOC_SINGLE("HP DAC Playback Switch", AIC32X4_HPLGAIN, 6, 1, 1),

	SOC_SINGLE_TLV("Speaker Driver Playback Volume",
			TAS2505_SPKVOL1, 0, 0x74, 1, tlv_tas_driver_gain),
	SOC_SINGLE_TLV("Speaker Amplifier Playback Volume",
			TAS2505_SPKVOL2, 4, 5, 0, tlv_amp_vol),

	SOC_SINGLE("Auto-mute Switch", AIC32X4_DACMUTE, 4, 7, 0),
};

static const struct snd_kcontrol_new hp_output_mixer_controls[] = {
	SOC_DAPM_SINGLE("DAC Switch", AIC32X4_HPLROUTE, 3, 1, 0),
};

static const struct snd_soc_dapm_widget aic32x4_tas2505_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", "Playback", AIC32X4_DACSETUP, 7, 0),
	SND_SOC_DAPM_MIXER("HP Output Mixer", SND_SOC_NOPM, 0, 0,
			   &hp_output_mixer_controls[0],
			   ARRAY_SIZE(hp_output_mixer_controls)),
	SND_SOC_DAPM_PGA("HP Power", AIC32X4_OUTPWRCTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_PGA("Speaker Driver", TAS2505_SPK, 1, 0, NULL, 0),

	SND_SOC_DAPM_OUTPUT("HP"),
	SND_SOC_DAPM_OUTPUT("Speaker"),
};

static const struct snd_soc_dapm_route aic32x4_tas2505_dapm_routes[] = {
	/* Left Output */
	{"HP Output Mixer", "DAC Switch", "DAC"},

	{"HP Power", NULL, "HP Output Mixer"},
	{"HP", NULL, "HP Power"},

	{"Speaker Driver", NULL, "DAC"},
	{"Speaker", NULL, "Speaker Driver"},
};

static struct snd_soc_dai_driver aic32x4_tas2505_dai = {
	.name = "tas2505-hifi",
	.playback = {
			 .stream_name = "Playback",
			 .channels_min = 1,
			 .channels_max = 2,
			 .rates = SNDRV_PCM_RATE_8000_96000,
			 .formats = AIC32X4_FORMATS,},
	.ops = &aic32x4_ops,
	.symmetric_rate = 1,
};

static int aic32x4_tas2505_component_probe(struct snd_soc_component *component)
{
	struct aic32x4_priv *aic32x4 = snd_soc_component_get_drvdata(component);
	u32 tmp_reg;
	int ret;

	static struct clk_bulk_data clocks[] = {
		{ .id = "codec_clkin" },
		{ .id = "pll" },
		{ .id = "bdiv" },
		{ .id = "mdac" },
	};

	ret = devm_clk_bulk_get(component->dev, ARRAY_SIZE(clocks), clocks);
	if (ret)
		return ret;

	if (aic32x4->setup)
		aic32x4_setup_gpios(component);

	clk_set_parent(clocks[0].clk, clocks[1].clk);
	clk_set_parent(clocks[2].clk, clocks[3].clk);

	/* Power platform configuration */
	if (aic32x4->power_cfg & AIC32X4_PWR_AVDD_DVDD_WEAK_DISABLE)
		snd_soc_component_write(component, AIC32X4_PWRCFG, AIC32X4_AVDDWEAKDISABLE);

	tmp_reg = (aic32x4->power_cfg & AIC32X4_PWR_AIC32X4_LDO_ENABLE) ?
			AIC32X4_LDOCTLEN : 0;
	snd_soc_component_write(component, AIC32X4_LDOCTL, tmp_reg);

	tmp_reg = snd_soc_component_read(component, AIC32X4_CMMODE);
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_LDOIN_RANGE_18_36)
		tmp_reg |= AIC32X4_LDOIN_18_36;
	if (aic32x4->power_cfg & AIC32X4_PWR_CMMODE_HP_LDOIN_POWERED)
		tmp_reg |= AIC32X4_LDOIN2HP;
	snd_soc_component_write(component, AIC32X4_CMMODE, tmp_reg);

	/*
	 * Enable the fast charging feature and ensure the needed 40ms ellapsed
	 * before using the analog circuits.
	 */
	snd_soc_component_write(component, TAS2505_REFPOWERUP,
				AIC32X4_REFPOWERUP_40MS);
	msleep(40);

	return 0;
}

static const struct snd_soc_component_driver soc_component_dev_aic32x4_tas2505 = {
	.probe			= aic32x4_tas2505_component_probe,
	.remove			= aic32x4_component_remove,
	.set_bias_level		= aic32x4_set_bias_level,
	.controls		= aic32x4_tas2505_snd_controls,
	.num_controls		= ARRAY_SIZE(aic32x4_tas2505_snd_controls),
	.dapm_widgets		= aic32x4_tas2505_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(aic32x4_tas2505_dapm_widgets),
	.dapm_routes		= aic32x4_tas2505_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(aic32x4_tas2505_dapm_routes),
	.suspend_bias_off	= 1,
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static int aic32x4_parse_dt(struct aic32x4_priv *aic32x4,
		struct device_node *np)
{
	struct aic32x4_setup_data *aic32x4_setup;
	int ret;

	aic32x4_setup = devm_kzalloc(aic32x4->dev, sizeof(*aic32x4_setup),
							GFP_KERNEL);
	if (!aic32x4_setup)
		return -ENOMEM;

	ret = of_property_match_string(np, "clock-names", "mclk");
	if (ret < 0)
		return -EINVAL;
	aic32x4->mclk_name = of_clk_get_parent_name(np, ret);

	aic32x4->swapdacs = false;
	aic32x4->micpga_routing = 0;
	aic32x4->rstn_gpio = of_get_named_gpio(np, "reset-gpios", 0);
	/*
	 * Radar-Puffin hardcodes the analog power configuration exactly as
	 * the working Linux 3.18 driver does in its parse_dt.  Without these
	 * flags the probe skips PWRCFG, LDOCTL, and CMMODE writes, leaving
	 * the HP common-mode feedback disabled (CMMODE=0x00 vs 0x33) and
	 * the output stage without DC bias — the root cause of silent
	 * playback despite a running DAC datapath.
	 */
	aic32x4->power_cfg = AIC32X4_PWR_AVDD_DVDD_WEAK_DISABLE |
			     AIC32X4_PWR_AIC32X4_LDO_ENABLE |
			     AIC32X4_PWR_CMMODE_LDOIN_RANGE_18_36 |
			     AIC32X4_PWR_CMMODE_HP_LDOIN_POWERED;

	if (of_property_read_u32_array(np, "aic32x4-gpio-func",
				aic32x4_setup->gpio_func, 5) >= 0)
		aic32x4->setup = aic32x4_setup;
	return 0;
}

static void aic32x4_disable_regulators(struct aic32x4_priv *aic32x4)
{
	regulator_disable(aic32x4->supply_iov);

	if (!IS_ERR(aic32x4->supply_ldo))
		regulator_disable(aic32x4->supply_ldo);

	if (!IS_ERR(aic32x4->supply_dv))
		regulator_disable(aic32x4->supply_dv);

	if (!IS_ERR(aic32x4->supply_av))
		regulator_disable(aic32x4->supply_av);
}

static int aic32x4_setup_regulators(struct device *dev,
		struct aic32x4_priv *aic32x4)
{
	int ret = 0;

	aic32x4->supply_ldo = devm_regulator_get_optional(dev, "ldoin");
	aic32x4->supply_iov = devm_regulator_get(dev, "iov");
	aic32x4->supply_dv = devm_regulator_get_optional(dev, "dv");
	aic32x4->supply_av = devm_regulator_get_optional(dev, "av");

	/* Check if the regulator requirements are fulfilled */

	if (IS_ERR(aic32x4->supply_iov)) {
		dev_err(dev, "Missing supply 'iov'\n");
		return PTR_ERR(aic32x4->supply_iov);
	}

	if (IS_ERR(aic32x4->supply_ldo)) {
		if (PTR_ERR(aic32x4->supply_ldo) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		if (IS_ERR(aic32x4->supply_dv)) {
			dev_err(dev, "Missing supply 'dv' or 'ldoin'\n");
			return PTR_ERR(aic32x4->supply_dv);
		}
		if (IS_ERR(aic32x4->supply_av)) {
			dev_err(dev, "Missing supply 'av' or 'ldoin'\n");
			return PTR_ERR(aic32x4->supply_av);
		}
	} else {
		if (PTR_ERR(aic32x4->supply_dv) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (PTR_ERR(aic32x4->supply_av) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	ret = regulator_enable(aic32x4->supply_iov);
	if (ret) {
		dev_err(dev, "Failed to enable regulator iov\n");
		return ret;
	}

	if (!IS_ERR(aic32x4->supply_ldo)) {
		ret = regulator_enable(aic32x4->supply_ldo);
		if (ret) {
			dev_err(dev, "Failed to enable regulator ldo\n");
			goto error_ldo;
		}
	}

	if (!IS_ERR(aic32x4->supply_dv)) {
		ret = regulator_enable(aic32x4->supply_dv);
		if (ret) {
			dev_err(dev, "Failed to enable regulator dv\n");
			goto error_dv;
		}
	}

	if (!IS_ERR(aic32x4->supply_av)) {
		ret = regulator_enable(aic32x4->supply_av);
		if (ret) {
			dev_err(dev, "Failed to enable regulator av\n");
			goto error_av;
		}
	}

	if (!IS_ERR(aic32x4->supply_ldo) && IS_ERR(aic32x4->supply_av))
		aic32x4->power_cfg |= AIC32X4_PWR_AIC32X4_LDO_ENABLE;

	return 0;

error_av:
	if (!IS_ERR(aic32x4->supply_dv))
		regulator_disable(aic32x4->supply_dv);

error_dv:
	if (!IS_ERR(aic32x4->supply_ldo))
		regulator_disable(aic32x4->supply_ldo);

error_ldo:
	regulator_disable(aic32x4->supply_iov);
	return ret;
}

int aic32x4_probe(struct device *dev, struct regmap *regmap)
{
	struct aic32x4_priv *aic32x4;
	struct aic32x4_pdata *pdata = dev->platform_data;
	struct device_node *np = dev->of_node;
	int ret;

	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	aic32x4 = devm_kzalloc(dev, sizeof(struct aic32x4_priv),
				   GFP_KERNEL);
	if (aic32x4 == NULL)
		return -ENOMEM;

	aic32x4->dev = dev;
	aic32x4->type = (enum aic32x4_type)dev_get_drvdata(dev);
	aic32x4->defer_playback_unmute =
		aic32x4->type == AIC32X4_TYPE_AIC32X4;
	mutex_init(&aic32x4->diag_lock);

	dev_set_drvdata(dev, aic32x4);

	if (pdata) {
		aic32x4->power_cfg = pdata->power_cfg;
		aic32x4->swapdacs = pdata->swapdacs;
		aic32x4->micpga_routing = pdata->micpga_routing;
		aic32x4->rstn_gpio = pdata->rstn_gpio;
		aic32x4->mclk_name = "mclk";
	} else if (np) {
		ret = aic32x4_parse_dt(aic32x4, np);
		if (ret) {
			dev_err(dev, "Failed to parse DT node\n");
			return ret;
		}
	} else {
		aic32x4->power_cfg = 0;
		aic32x4->swapdacs = false;
		aic32x4->micpga_routing = 0;
		aic32x4->rstn_gpio = -1;
		aic32x4->mclk_name = "mclk";
	}

	if (gpio_is_valid(aic32x4->rstn_gpio)) {
		ret = devm_gpio_request_one(dev, aic32x4->rstn_gpio,
				GPIOF_OUT_INIT_LOW, "tlv320aic32x4 rstn");
		if (ret != 0)
			return ret;
	}

	ret = aic32x4_setup_regulators(dev, aic32x4);
	if (ret) {
		dev_err(dev, "Failed to setup regulators\n");
		return ret;
	}

	if (gpio_is_valid(aic32x4->rstn_gpio)) {
		ndelay(10);
		gpio_set_value_cansleep(aic32x4->rstn_gpio, 1);
	}

	ret = regmap_write(regmap, AIC32X4_RESET, 0x01);
	if (ret)
		goto err_disable_regulators;
	/* The 3.18 driver waits after the codec software reset completes. */
	mdelay(1);

	ret = aic32x4_register_clocks(dev, aic32x4->mclk_name);
	if (ret)
		goto err_disable_regulators;

	switch (aic32x4->type) {
	case AIC32X4_TYPE_TAS2505:
		ret = devm_snd_soc_register_component(dev,
			&soc_component_dev_aic32x4_tas2505, &aic32x4_tas2505_dai, 1);
		break;
	default:
		ret = devm_snd_soc_register_component(dev,
			&soc_component_dev_aic32x4, &aic32x4_dai, 1);
	}

	if (ret) {
		dev_err(dev, "Failed to register component\n");
		goto err_disable_regulators;
	}

	return 0;

err_disable_regulators:
	aic32x4_disable_regulators(aic32x4);

	return ret;
}
EXPORT_SYMBOL(aic32x4_probe);

void aic32x4_remove(struct device *dev)
{
	struct aic32x4_priv *aic32x4 = dev_get_drvdata(dev);

	aic32x4_disable_regulators(aic32x4);
}
EXPORT_SYMBOL(aic32x4_remove);

MODULE_DESCRIPTION("ASoC tlv320aic32x4 codec driver");
MODULE_AUTHOR("Javier Martin <javier.martin@vista-silicon.com>");
MODULE_LICENSE("GPL");
