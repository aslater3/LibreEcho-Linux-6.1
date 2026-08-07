// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amazon Radar-Puffin MT8163 sound card.
 *
 * Keep the three board links that are present in the release product:
 * external DAC playback, FPGA SPI microphone capture, and the raw I2S1
 * playback endpoint.  Link IDs intentionally retain the stock PCM device
 * numbers without registering the unused voice/FM/HDMI links.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "mt8163-afe.h"
#include "mt8163-mclk.h"

#define RADAR_CARD_NAME			"mt-snd-card"
#define RADAR_AFE_COMPATIBLE		"mediatek,mt8163-soc-pcm-dl1"
#define RADAR_SPI_COMPATIBLE		"amzn-mtk,spi-audio-pltfm"
#define RADAR_DAC_COMPATIBLE		"ti,tlv320aic32x4"
#define RADAR_ADC_COMPATIBLE		"ti,tlv320aic3101"

#define RADAR_DL1_DAI			"mt-soc-dl1dai-driver"
#define RADAR_SPI_DAI			"amzn-mt-spi-pcm"
#define RADAR_DAC_DAI			"tlv320aic32x4-hifi"
#define RADAR_ADC_DAI			"tlv320aic3101-codec"

#define RADAR_MCLK_RATE			9600000U

#define RADAR_ADC_PLL_BCLK		1
#define RADAR_ADC_PLL_MCLK		2

/* TLV320AIC32x4 page-zero registers used by stock board controls. */
#define RADAR_DAC_DOUTCTL		53
#define RADAR_DAC_PROCESSING_BLOCK	60
#define RADAR_DAC_SETUP			63
#define RADAR_DAC_MUTE			RADAR_AIC32X4_REG(0, 64)
#define RADAR_DAC_OUTPWR		RADAR_AIC32X4_REG(1, 9)
#define RADAR_DRC_CTRL1			68
#define RADAR_PUFFIN_DAC_PROCESSING_BLOCK 2
#define RADAR_DRC_DISABLED		0x0f
#define RADAR_DAC_HEADSTART		RADAR_AIC32X4_REG(1, 20)
#define RADAR_HP_AMP_SOFT_STARTUP	0x1d
#define RADAR_DAC_GAIN_APPLIED		RADAR_AIC32X4_REG(1, 63)
#define RADAR_HP_GAIN_APPLIED_STEREO	(BIT(7) | BIT(6))
#define RADAR_GAIN_APPLIED_POLL_MS	50
#define RADAR_GAIN_APPLIED_RETRIES	80
/*
 * HP analog output driver gain/mute registers.  Bit 6 is the analog mute:
 * the codec resets with HPLGAIN/HPRGAIN=0x40 (muted).  The 6.1 machine
 * applies the production stereo-container release policy below.
 */
#define RADAR_HPLGAIN			RADAR_AIC32X4_REG(1, 16)
#define RADAR_HPRGAIN			RADAR_AIC32X4_REG(1, 17)
#define RADAR_HP_DRIVER_MUTE		BIT(6)
/* DAC digital volume registers: 0x00 is 0 dB; DACMUTE is the mute control. */
#define RADAR_LDACVOL			RADAR_AIC32X4_REG(0, 65)
#define RADAR_RDACVOL			RADAR_AIC32X4_REG(0, 66)
/* DACSETUP soft-stepping bit retained from the 3.18 profile. */
#define RADAR_DAC_SOFT_STEP		0x02
#define RADAR_DAC_MFP2_GPIO		BIT(2)
#define RADAR_DAC_MFP2_MUTE		(RADAR_DAC_MFP2_GPIO | BIT(0))
#define RADAR_AIC32X4_REG(_page, _reg)	(((_page) * 128) + (_reg))
#define RADAR_PUFFIN_PROFILE(_page, _reg, _value)	\
	{ .reg = RADAR_AIC32X4_REG(_page, _reg), .def = (_value) }

static unsigned int radar_codec_debug_read(struct snd_soc_component *component,
						unsigned int reg)
{
	int ret;

	ret = snd_soc_component_read(component, reg);
	return ret < 0 ? 0xff : ret;
}

static void radar_log_codec_state(struct snd_soc_component *component,
					  const char *stage)
{
	dev_info(component->dev,
		 "audio-stage=%s p0[r4=%02x r5=%02x r6=%02x r11=%02x r12=%02x "
		 "r18=%02x r19=%02x r30=%02x r36=%02x r37=%02x r38=%02x "
		 "r60=%02x r61=%02x r63=%02x r64=%02x] "
		 "p1[r9=%02x r12=%02x r13=%02x r16=%02x r17=%02x r20=%02x r63=%02x]\n",
		 stage,
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 4)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 5)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 6)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 11)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 12)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 18)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 19)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 30)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 36)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 37)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 38)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 60)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 61)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 63)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(0, 64)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 9)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 12)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 13)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 16)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 17)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 20)),
		 radar_codec_debug_read(component, RADAR_AIC32X4_REG(1, 63)));
}

/*
 * Stock radar_puffin ext_speaker_output profile recovered from the final
 * Linux 3.18 audio_device.xml path.  The engine duplicates its mono speaker
 * bus into both I2S channels; these coefficients then high-pass HPL for the
 * tweeter and low-pass HPR for the larger cone, with the stock DRC settings.
 */
static const struct reg_sequence radar_puffin_ext_speaker_profile[] = {
	RADAR_PUFFIN_PROFILE(44, 12, 122),
	RADAR_PUFFIN_PROFILE(44, 13, 248),
	RADAR_PUFFIN_PROFILE(44, 14, 206),
	RADAR_PUFFIN_PROFILE(44, 16, 133),
	RADAR_PUFFIN_PROFILE(44, 17, 7),
	RADAR_PUFFIN_PROFILE(44, 18, 50),
	RADAR_PUFFIN_PROFILE(44, 20, 122),
	RADAR_PUFFIN_PROFILE(44, 21, 248),
	RADAR_PUFFIN_PROFILE(44, 22, 206),
	RADAR_PUFFIN_PROFILE(44, 24, 83),
	RADAR_PUFFIN_PROFILE(44, 25, 31),
	RADAR_PUFFIN_PROFILE(44, 26, 201),
	RADAR_PUFFIN_PROFILE(44, 28, 202),
	RADAR_PUFFIN_PROFILE(44, 29, 4),
	RADAR_PUFFIN_PROFILE(44, 30, 191),
	RADAR_PUFFIN_PROFILE(44, 32, 87),
	RADAR_PUFFIN_PROFILE(44, 33, 14),
	RADAR_PUFFIN_PROFILE(44, 34, 180),
	RADAR_PUFFIN_PROFILE(44, 36, 168),
	RADAR_PUFFIN_PROFILE(44, 37, 241),
	RADAR_PUFFIN_PROFILE(44, 38, 76),
	RADAR_PUFFIN_PROFILE(44, 40, 87),
	RADAR_PUFFIN_PROFILE(44, 41, 14),
	RADAR_PUFFIN_PROFILE(44, 42, 180),
	RADAR_PUFFIN_PROFILE(44, 44, 83),
	RADAR_PUFFIN_PROFILE(44, 45, 31),
	RADAR_PUFFIN_PROFILE(44, 46, 201),
	RADAR_PUFFIN_PROFILE(44, 48, 202),
	RADAR_PUFFIN_PROFILE(44, 49, 4),
	RADAR_PUFFIN_PROFILE(44, 50, 191),
	RADAR_PUFFIN_PROFILE(44, 52, 127),
	RADAR_PUFFIN_PROFILE(44, 53, 255),
	RADAR_PUFFIN_PROFILE(44, 54, 255),
	RADAR_PUFFIN_PROFILE(44, 56, 128),
	RADAR_PUFFIN_PROFILE(44, 57, 49),
	RADAR_PUFFIN_PROFILE(44, 58, 110),
	RADAR_PUFFIN_PROFILE(44, 60, 127),
	RADAR_PUFFIN_PROFILE(44, 61, 162),
	RADAR_PUFFIN_PROFILE(44, 62, 193),
	RADAR_PUFFIN_PROFILE(44, 64, 127),
	RADAR_PUFFIN_PROFILE(44, 65, 206),
	RADAR_PUFFIN_PROFILE(44, 66, 146),
	RADAR_PUFFIN_PROFILE(44, 68, 128),
	RADAR_PUFFIN_PROFILE(44, 69, 93),
	RADAR_PUFFIN_PROFILE(44, 70, 63),
	RADAR_PUFFIN_PROFILE(45, 20, 127),
	RADAR_PUFFIN_PROFILE(45, 21, 116),
	RADAR_PUFFIN_PROFILE(45, 22, 152),
	RADAR_PUFFIN_PROFILE(45, 24, 128),
	RADAR_PUFFIN_PROFILE(45, 25, 139),
	RADAR_PUFFIN_PROFILE(45, 26, 104),
	RADAR_PUFFIN_PROFILE(45, 28, 127),
	RADAR_PUFFIN_PROFILE(45, 29, 116),
	RADAR_PUFFIN_PROFILE(45, 30, 152),
	RADAR_PUFFIN_PROFILE(45, 32, 127),
	RADAR_PUFFIN_PROFILE(45, 33, 116),
	RADAR_PUFFIN_PROFILE(45, 34, 1),
	RADAR_PUFFIN_PROFILE(45, 36, 129),
	RADAR_PUFFIN_PROFILE(45, 37, 21),
	RADAR_PUFFIN_PROFILE(45, 38, 161),
	RADAR_PUFFIN_PROFILE(45, 40, 3),
	RADAR_PUFFIN_PROFILE(45, 41, 238),
	RADAR_PUFFIN_PROFILE(45, 42, 235),
	RADAR_PUFFIN_PROFILE(45, 44, 3),
	RADAR_PUFFIN_PROFILE(45, 45, 238),
	RADAR_PUFFIN_PROFILE(45, 46, 235),
	RADAR_PUFFIN_PROFILE(45, 48, 3),
	RADAR_PUFFIN_PROFILE(45, 49, 238),
	RADAR_PUFFIN_PROFILE(45, 50, 235),
	RADAR_PUFFIN_PROFILE(45, 52, 83),
	RADAR_PUFFIN_PROFILE(45, 53, 31),
	RADAR_PUFFIN_PROFILE(45, 54, 201),
	RADAR_PUFFIN_PROFILE(45, 56, 202),
	RADAR_PUFFIN_PROFILE(45, 57, 4),
	RADAR_PUFFIN_PROFILE(45, 58, 191),
	RADAR_PUFFIN_PROFILE(45, 60, 3),
	RADAR_PUFFIN_PROFILE(45, 61, 238),
	RADAR_PUFFIN_PROFILE(45, 62, 235),
	RADAR_PUFFIN_PROFILE(45, 64, 3),
	RADAR_PUFFIN_PROFILE(45, 65, 238),
	RADAR_PUFFIN_PROFILE(45, 66, 235),
	RADAR_PUFFIN_PROFILE(45, 68, 3),
	RADAR_PUFFIN_PROFILE(45, 69, 238),
	RADAR_PUFFIN_PROFILE(45, 70, 235),
	RADAR_PUFFIN_PROFILE(45, 72, 83),
	RADAR_PUFFIN_PROFILE(45, 73, 31),
	RADAR_PUFFIN_PROFILE(45, 74, 201),
	RADAR_PUFFIN_PROFILE(45, 76, 202),
	RADAR_PUFFIN_PROFILE(45, 77, 4),
	RADAR_PUFFIN_PROFILE(45, 78, 191),
	RADAR_PUFFIN_PROFILE(46, 52, 127),
	RADAR_PUFFIN_PROFILE(46, 53, 255),
	RADAR_PUFFIN_PROFILE(46, 54, 255),
	RADAR_PUFFIN_PROFILE(46, 55, 0),
	RADAR_PUFFIN_PROFILE(46, 56, 0),
	RADAR_PUFFIN_PROFILE(46, 57, 0),
	RADAR_PUFFIN_PROFILE(46, 58, 0),
	RADAR_PUFFIN_PROFILE(46, 59, 0),
	RADAR_PUFFIN_PROFILE(46, 60, 0),
	RADAR_PUFFIN_PROFILE(46, 61, 0),
	RADAR_PUFFIN_PROFILE(46, 62, 0),
	RADAR_PUFFIN_PROFILE(46, 63, 0),
	RADAR_PUFFIN_PROFILE(46, 64, 127),
	RADAR_PUFFIN_PROFILE(46, 65, 255),
	RADAR_PUFFIN_PROFILE(46, 66, 255),
	RADAR_PUFFIN_PROFILE(46, 67, 0),
	RADAR_PUFFIN_PROFILE(46, 68, 0),
	RADAR_PUFFIN_PROFILE(46, 69, 0),
	RADAR_PUFFIN_PROFILE(46, 70, 0),
	RADAR_PUFFIN_PROFILE(46, 71, 0),
	RADAR_PUFFIN_PROFILE(46, 72, 0),
	RADAR_PUFFIN_PROFILE(46, 73, 0),
	RADAR_PUFFIN_PROFILE(46, 74, 0),
	RADAR_PUFFIN_PROFILE(46, 75, 0),
	RADAR_PUFFIN_PROFILE(0, 68, 7),
	RADAR_PUFFIN_PROFILE(0, 69, 0),
	RADAR_PUFFIN_PROFILE(0, 70, 198),
};

enum radar_link_id {
	RADAR_LINK_DAC,
	RADAR_LINK_CAPTURE,
	RADAR_LINK_I2S1,
	RADAR_LINK_NUM,
};

struct radar_card {
	struct snd_soc_card card;
	struct snd_soc_dai_link links[RADAR_LINK_NUM];
	struct snd_soc_dai_link_component cpus[RADAR_LINK_NUM];
	struct snd_soc_dai_link_component platforms[RADAR_LINK_NUM];
	struct snd_soc_dai_link_component codecs[RADAR_LINK_NUM];
	struct device_node *afe_np;
	struct device_node *spi_np;
	struct device_node *dac_np;
	struct device_node *adc_np;
	struct platform_device *afe_pdev;
	struct mt8163_mclk *mclk;
	struct snd_soc_pcm_runtime *speaker_rtd;
	unsigned int low_jitter;
	unsigned int channel_config;
	unsigned int linein_adc;
	unsigned int amp_fault_enable;
	unsigned int codec_mute;
	unsigned int amp_enable;
	unsigned int dac_route;
	unsigned int right_only;
	unsigned int hd_output;
	unsigned int ignore_ramp;
	bool speaker_mclk_enabled;
	bool speaker_release_pending;
	unsigned int speaker_release_retries;
	struct delayed_work speaker_release_work;
};

static const char * const radar_on_off[] = { "Off", "On" };
static SOC_ENUM_SINGLE_EXT_DECL(radar_on_off_enum, radar_on_off);

static const char * const radar_channel_config[] = {
	"Stereo", "MonoLeft", "MonoRight"
};
static SOC_ENUM_SINGLE_EXT_DECL(radar_channel_enum, radar_channel_config);

static struct radar_card *radar_kcontrol_priv(struct snd_kcontrol *kcontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);

	return snd_soc_card_get_drvdata(card);
}

static int radar_low_jitter_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->low_jitter;
	return 0;
}

static int radar_low_jitter_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];

	if (requested >= ARRAY_SIZE(radar_on_off))
		return -EINVAL;
	if (priv->low_jitter == requested)
		return 0;
	if (!priv->afe_pdev)
		return -ENODEV;
	if (mt8163_afe_set_low_jitter(&priv->afe_pdev->dev, requested))
		return -EIO;
	priv->low_jitter = requested;
	return 1;
}

static int radar_channel_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->channel_config;
	return 0;
}

static int radar_channel_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];

	if (requested >= ARRAY_SIZE(radar_channel_config))
		return -EINVAL;
	if (priv->channel_config == requested)
		return 0;
	priv->channel_config = requested;
	return 1;
}

#define RADAR_STATE_CONTROL_FUNCS(_name, _field)				\
static int radar_##_name##_get(struct snd_kcontrol *kcontrol,		\
			       struct snd_ctl_elem_value *value)		\
{									\
	value->value.enumerated.item[0] =					\
		radar_kcontrol_priv(kcontrol)->_field;			\
	return 0;								\
}									\
static int radar_##_name##_put(struct snd_kcontrol *kcontrol,		\
			       struct snd_ctl_elem_value *value)		\
{									\
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);		\
	unsigned int requested = value->value.enumerated.item[0];		\
	if (requested >= ARRAY_SIZE(radar_on_off))				\
		return -EINVAL;							\
	if (priv->_field == requested)					\
		return 0;							\
	priv->_field = requested;						\
	return 1;								\
}

RADAR_STATE_CONTROL_FUNCS(linein, linein_adc)
RADAR_STATE_CONTROL_FUNCS(amp_fault, amp_fault_enable)
RADAR_STATE_CONTROL_FUNCS(hd_output, hd_output)
RADAR_STATE_CONTROL_FUNCS(ignore_ramp, ignore_ramp)

static int radar_right_only_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->right_only;
	return 0;
}

static int radar_right_only_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];

	if (requested >= ARRAY_SIZE(radar_on_off))
		return -EINVAL;
	if (priv->right_only == requested)
		return 0;
	/*
	 * This is a legacy mute/channel-state control.  The codec DAI owns
	 * DACSETUP serial-slot routing and derives it from the actual PCM
	 * channel count in aic32x4_hw_params(); changing this control must not
	 * overwrite that route while a stream is active.
	 */
	priv->right_only = requested;
	return 1;
}

static int radar_amp_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->amp_enable;
	return 0;
}

static int radar_amp_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];
	int ret;

	if (requested >= ARRAY_SIZE(radar_on_off))
		return -EINVAL;
	if (priv->amp_enable == requested)
		return 0;
	ret = mt8163_afe_select_amp(&priv->afe_pdev->dev, requested);
	if (ret)
		return ret;
	priv->amp_enable = requested;
	return 1;
}

static int radar_dac_route_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->dac_route;
	return 0;
}

static int radar_dac_route_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];
	int ret;

	if (requested >= ARRAY_SIZE(radar_on_off))
		return -EINVAL;
	if (priv->dac_route == requested)
		return 0;
	ret = mt8163_afe_select_dac(&priv->afe_pdev->dev, requested);
	if (ret)
		return ret;
	priv->dac_route = requested;
	return 1;
}

static int radar_codec_mute_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	value->value.enumerated.item[0] =
		radar_kcontrol_priv(kcontrol)->codec_mute;
	return 0;
}

static int radar_codec_mute_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	struct radar_card *priv = radar_kcontrol_priv(kcontrol);
	unsigned int requested = value->value.enumerated.item[0];
	struct snd_soc_dai *codec_dai;
	int ret;

	if (requested >= ARRAY_SIZE(radar_on_off))
		return -EINVAL;
	if (!priv->speaker_rtd)
		return -ENODEV;
	if (priv->codec_mute == requested)
		return 0;

	codec_dai = asoc_rtd_to_codec(priv->speaker_rtd, 0);
	ret = snd_soc_component_write(codec_dai->component,
				      RADAR_DAC_DOUTCTL,
				      requested ? RADAR_DAC_MFP2_MUTE :
				      RADAR_DAC_MFP2_GPIO);
	if (ret < 0)
		return ret;
	priv->codec_mute = requested;
	return 1;
}

static const struct snd_kcontrol_new radar_controls[] = {
	SOC_ENUM_EXT("I2S low Jitter function", radar_on_off_enum,
		     radar_low_jitter_get, radar_low_jitter_put),
	SOC_ENUM_EXT("Audio_I2S0dl1_hd_Switch", radar_on_off_enum,
		     radar_hd_output_get, radar_hd_output_put),
	SOC_ENUM_EXT("Board Channel Config", radar_channel_enum,
		     radar_channel_get, radar_channel_put),
	SOC_ENUM_EXT("LineIn ADC", radar_on_off_enum,
		     radar_linein_get, radar_linein_put),
	SOC_ENUM_EXT("Amp Fault Enable", radar_on_off_enum,
		     radar_amp_fault_get, radar_amp_fault_put),
	SOC_ENUM_EXT("MFP Gpio Mute", radar_on_off_enum,
		     radar_codec_mute_get, radar_codec_mute_put),
	SOC_ENUM_EXT("Ext_Speaker_Amp_Switch", radar_on_off_enum,
		     radar_amp_get, radar_amp_put),
	SOC_ENUM_EXT("Audio_DacMux_Setting", radar_on_off_enum,
		     radar_dac_route_get, radar_dac_route_put),
	SOC_ENUM_EXT("Right Channel Only", radar_on_off_enum,
		     radar_right_only_get, radar_right_only_put),
	SOC_ENUM_EXT("Ignore Ramp Up", radar_on_off_enum,
		     radar_ignore_ramp_get, radar_ignore_ramp_put),
};

static int radar_speaker_init(struct snd_soc_pcm_runtime *rtd)
{
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	int ret;

	priv->speaker_rtd = rtd;
	priv->codec_mute = 1;
	ret = snd_soc_dai_digital_mute(codec_dai, 1,
				       SNDRV_PCM_STREAM_PLAYBACK);
	if (ret && ret != -ENOTSUPP)
		return ret;
	ret = snd_soc_component_write(codec_dai->component,
				      RADAR_DAC_DOUTCTL, RADAR_DAC_MFP2_MUTE);
	if (ret < 0)
		return ret;
	ret = mt8163_afe_select_amp(&priv->afe_pdev->dev, false);
	if (ret)
		return ret;
	ret = mt8163_afe_select_dac(&priv->afe_pdev->dev, false);
	if (ret)
		return ret;
	ret = mt8163_afe_select_i2s(&priv->afe_pdev->dev, false);
	if (ret)
		return ret;
	/*
	 * The working 3.18 link initializer brought the physical 9.6 MHz
	 * codec MCLK up before the first AIC32X4 bias/stream transition and kept
	 * it alive for the lifetime of the card.  6.1 previously deferred this
	 * until PCM startup, leaving the codec's initial analogue bootstrap
	 * without its external clock.  Establish the same lifetime here; the
	 * normal startup path will retain the reference rather than toggle it.
	 */
	ret = mt8163_afe_select_mclk(&priv->afe_pdev->dev);
	if (ret)
		return ret;
	ret = mt8163_mclk_enable(priv->mclk);
	if (ret)
		return ret;
	priv->speaker_mclk_enabled = true;
	return 0;
}

static void radar_record_error(int *first, int ret)
{
	if (ret && !*first)
		*first = ret;
}

static int radar_speaker_enable_clocks(struct radar_card *priv)
{
	int idle_ret;
	int ret;

	ret = mt8163_afe_select_i2s(&priv->afe_pdev->dev, true);
	if (ret)
		return ret;
	ret = mt8163_afe_select_mclk(&priv->afe_pdev->dev);
	if (ret)
		goto idle_i2s;
	if (!priv->speaker_mclk_enabled) {
		ret = mt8163_mclk_enable(priv->mclk);
		if (ret)
			goto idle_i2s;
		priv->speaker_mclk_enabled = true;
	}
	return 0;

idle_i2s:
	idle_ret = mt8163_afe_select_i2s(&priv->afe_pdev->dev, false);
	if (idle_ret)
		dev_err(priv->card.dev,
			"Speaker clock rollback failed: primary=%d cleanup=%d\n",
			ret, idle_ret);
	return ret ? ret : idle_ret;
}

static int radar_speaker_safe(struct radar_card *priv)
{
	struct snd_soc_dai *codec_dai =
		asoc_rtd_to_codec(priv->speaker_rtd, 0);
	int first = 0;
	int ret;

	priv->speaker_release_pending = false;
	cancel_delayed_work_sync(&priv->speaker_release_work);

	ret = snd_soc_dai_digital_mute(codec_dai, 1,
				       SNDRV_PCM_STREAM_PLAYBACK);
	if (ret == -ENOTSUPP)
		ret = 0;
	radar_record_error(&first, ret);
	/* Re-mute the HP analog output drivers to prevent pops and restore
	 * the codec's reset state (bit 6 set on HPLGAIN/HPRGAIN). */
	ret = snd_soc_component_update_bits(codec_dai->component,
					    RADAR_HPLGAIN,
					    RADAR_HP_DRIVER_MUTE,
					    RADAR_HP_DRIVER_MUTE);
	radar_record_error(&first, ret);
	ret = snd_soc_component_update_bits(codec_dai->component,
					    RADAR_HPRGAIN,
					    RADAR_HP_DRIVER_MUTE,
					    RADAR_HP_DRIVER_MUTE);
	radar_record_error(&first, ret);
	/* Restore 0 dB attenuation; DACMUTE above provides the shutdown mute. */
	ret = snd_soc_component_write(codec_dai->component,
				      RADAR_LDACVOL, 0);
	radar_record_error(&first, ret);
	ret = snd_soc_component_write(codec_dai->component,
				      RADAR_RDACVOL, 0);
	radar_record_error(&first, ret);
	ret = snd_soc_component_write(codec_dai->component,
				      RADAR_DAC_DOUTCTL, RADAR_DAC_MFP2_MUTE);
	radar_record_error(&first, ret);
	if (!ret)
		priv->codec_mute = 1;
	ret = mt8163_afe_select_amp(&priv->afe_pdev->dev, false);
	radar_record_error(&first, ret);
	if (!ret)
		priv->amp_enable = 0;
	ret = mt8163_afe_select_dac(&priv->afe_pdev->dev, false);
	radar_record_error(&first, ret);
	if (!ret)
		priv->dac_route = 0;
	/*
	 * The codec MCLK is acquired for the lifetime of the sound card during
	 * probe.  Do not release that card-lifetime reference from stream safe
	 * handling: doing so powers down SENINF/ISP and the next capture reopen
	 * can see an unpowered register window.  mt8163_mclk_release() owns the
	 * final teardown when the card device is removed.
	 */
	ret = mt8163_afe_select_i2s(&priv->afe_pdev->dev, false);
	radar_record_error(&first, ret);
	return first;
}

static int radar_speaker_fail_safe(struct radar_card *priv, int primary,
				   const char *operation)
{
	int safe_ret;

	safe_ret = radar_speaker_safe(priv);
	if (safe_ret)
		dev_err(priv->card.dev,
			"Speaker %s rollback failed: primary=%d cleanup=%d\n",
			operation, primary, safe_ret);
	return primary ? primary : safe_ret;
}

static void radar_speaker_release_work(struct work_struct *work)
{
	struct radar_card *priv = container_of(to_delayed_work(work),
						      struct radar_card,
						      speaker_release_work);
	struct snd_soc_component *component;
	int status;
	int ret;

	if (!priv->speaker_release_pending || !priv->speaker_rtd)
		return;

	component = asoc_rtd_to_codec(priv->speaker_rtd, 0)->component;
	status = snd_soc_component_read(component, RADAR_DAC_GAIN_APPLIED);
	if (status < 0) {
		dev_err(component->dev,
			"cannot read DAC gain-applied status: %d\n", status);
		priv->speaker_release_pending = false;
		return;
	}
	/* The production transport is duplicated mono in a stereo container, so
	 * both headphone-driver gains must be applied before releasing the amp. */
	if (!(status & RADAR_HP_GAIN_APPLIED_STEREO)) {
		if (priv->speaker_release_retries++ <
		    RADAR_GAIN_APPLIED_RETRIES) {
			schedule_delayed_work(&priv->speaker_release_work,
					      msecs_to_jiffies(
						RADAR_GAIN_APPLIED_POLL_MS));
			return;
		}
		dev_err(component->dev,
			"DAC gain-applied status timed out: 0x%02x\n", status);
		priv->speaker_release_pending = false;
		return;
	}
	/* Keep MFP2 and the external amplifier muted until the codec confirms
	 * that the analog HP gain ramp has completed. */
	ret = snd_soc_component_write(component, RADAR_DAC_DOUTCTL,
				      RADAR_DAC_MFP2_GPIO);
	if (ret < 0) {
		dev_err(component->dev, "cannot release codec MFP2 mute: %d\n", ret);
		priv->speaker_release_pending = false;
		return;
	}
	priv->codec_mute = 0;
	ret = mt8163_afe_select_amp(&priv->afe_pdev->dev, true);
	if (ret) {
		dev_err(component->dev, "cannot enable speaker amplifier: %d\n", ret);
		/* Leave the codec MFP2 gate muted if the external amp did not start. */
		snd_soc_component_write(component, RADAR_DAC_DOUTCTL,
					RADAR_DAC_MFP2_MUTE);
		priv->codec_mute = 1;
		priv->speaker_release_pending = false;
		return;
	}
	priv->amp_enable = 1;
	priv->speaker_release_pending = false;
	dev_info(component->dev,
		 "speaker gain applied after %u ms; MFP2/amp released\n",
		 priv->speaker_release_retries * RADAR_GAIN_APPLIED_POLL_MS);
}

static int radar_speaker_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = mt8163_afe_select_amp(&priv->afe_pdev->dev, false);
	if (ret)
		return ret;
	priv->amp_enable = 0;
	/* Stock Puffin ext_speaker_output uses the board DacMux Off path. */
	ret = mt8163_afe_select_dac(&priv->afe_pdev->dev, false);
	if (ret)
		return ret;
	priv->dac_route = 0;
	ret = radar_speaker_enable_clocks(priv);
	if (ret)
		return ret;
	return 0;
}

static void radar_speaker_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = radar_speaker_safe(priv);
	if (ret)
		dev_err(priv->card.dev, "Speaker shutdown cleanup failed: %d\n",
			ret);
}

static int radar_speaker_apply_profile(struct snd_soc_component *component)
{
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(radar_puffin_ext_speaker_profile); i++) {
		ret = snd_soc_component_write(
			component, radar_puffin_ext_speaker_profile[i].reg,
			radar_puffin_ext_speaker_profile[i].def);
		if (ret < 0) {
			dev_err(component->dev,
				"Puffin speaker profile write %zu failed: %d\n",
				i, ret);
			return ret;
		}
	}

	/* The working 3.18 sequence applies this after the coefficient table. */
	ret = snd_soc_component_write(component, RADAR_DRC_CTRL1,
				      RADAR_DRC_DISABLED);
	if (ret < 0)
		dev_err(component->dev,
			"Puffin speaker DRC disable failed: %d\n", ret);
	return ret;
}

static int radar_speaker_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	int ret;

	ret = radar_speaker_enable_clocks(priv);
	if (ret)
		return radar_speaker_fail_safe(priv, ret, "hw_params");
	radar_log_codec_state(codec_dai->component, "machine-hw_params-entry");
	if (params_rate(params) != 48000 ||
	    params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE) {
		dev_err(codec_dai->dev,
			"Radar-Puffin speaker requires duplicated-stereo PCM: "
			"rate=%u channels=%u format=%u\n",
			params_rate(params), params_channels(params),
			params_format(params));
		ret = -EINVAL;
		goto fail;
	}

	/* The codec is the clock/frame consumer; the MT8163 AFE provides the
	 * speaker-link BCLK/LRCLK.  Use the explicit 6.1 provider/consumer name
	 * so this cannot be confused with the deprecated CBS_CFS alias. */
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_BC_FC);
	if (ret)
		goto fail;
	ret = snd_soc_dai_set_sysclk(codec_dai, 0, RADAR_MCLK_RATE,
				     SND_SOC_CLOCK_IN);
	if (!ret) {
		radar_log_codec_state(codec_dai->component,
						 "machine-hw_params-end");
		return 0;
	}
fail:
	return radar_speaker_fail_safe(priv, ret, "hw_params");
}

static int radar_speaker_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);

	return radar_speaker_safe(priv);
}

static int radar_speaker_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component =
		asoc_rtd_to_codec(rtd, 0)->component;
	int ret;

	ret = radar_speaker_enable_clocks(priv);
	if (ret)
		return radar_speaker_fail_safe(priv, ret, "prepare");
	radar_log_codec_state(component, "machine-prepare-entry");
	/*
	 * Keep PLL/divider programming exclusively in the AIC32x4 CCF
	 * provider.  The machine driver must not write rate fields behind
	 * CCF's back: doing so leaves cached rates describing one tuple while
	 * the codec hardware contains another, breaking the next open.
	 *
	 * Step 1: Select PRB_P2 and load the board's calibrated biquad
	 * coefficients.  The non-zero profile is required: zeroing it
	 * (an earlier experiment) silenced the output entirely.
	 */
	ret = snd_soc_component_write(component, RADAR_DAC_PROCESSING_BLOCK,
				      RADAR_PUFFIN_DAC_PROCESSING_BLOCK);
	if (ret < 0)
		goto fail;
	ret = radar_speaker_apply_profile(component);
	if (ret)
		goto fail;
	/*
	 * Step 3: HP amp soft-route startup delay (0x1d), matching 3.18.
	 */
	ret = snd_soc_component_write(component, RADAR_DAC_HEADSTART,
				      RADAR_HP_AMP_SOFT_STARTUP);
	if (ret < 0)
		goto fail;
	/*
	 * Step 4: Enable DAC soft-stepping after PRB/profile/HEADSTART, as
	 * in the working 3.18 codec hw_params sequence.  The ordering matters
	 * to the codec's live gain-application state even when the final value
	 * of DACSETUP is identical.
	 */
	ret = snd_soc_component_update_bits(component, RADAR_DAC_SETUP,
					    RADAR_DAC_SOFT_STEP,
					    RADAR_DAC_SOFT_STEP);
	if (ret < 0)
		goto fail;
	/*
	 * Step 5: Unmute both HP analog output drivers (bit 6).  The
	 * Radar-Puffin PRB_P2 profile sends the left DAC through HPL to the
	 * tweeter and the right DAC through HPR to the woofer.  The production
	 * engine therefore duplicates its mono programme into a two-channel
	 * container with normal left/right DAC routing; both physical HP
	 * outputs must remain active.
	 */
	ret = snd_soc_component_update_bits(component, RADAR_HPLGAIN,
					    RADAR_HP_DRIVER_MUTE, 0);
	if (ret < 0)
		goto fail;
	ret = snd_soc_component_update_bits(component, RADAR_HPRGAIN,
					    RADAR_HP_DRIVER_MUTE, 0);
	if (ret < 0)
		goto fail;
	/*
	 * Step 6: Set DAC digital volume to 0 dB (full volume).  The
	 * "PCM Playback Volume" mixer control maps to -60.5 dB attenuation;
	 * 3.18 leaves DACVOL=0x00 (0 dB, full volume).
	 */
	ret = snd_soc_component_write(component, RADAR_LDACVOL, 0);
	if (ret < 0)
		goto fail;
	ret = snd_soc_component_write(component, RADAR_RDACVOL, 0);
	if (ret < 0)
		goto fail;
	radar_log_codec_state(component, "machine-prepare-end");
	return 0;
fail:
	return radar_speaker_fail_safe(priv, ret, "prepare");
}

static int radar_speaker_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component =
		asoc_rtd_to_codec(rtd, 0)->component;

	if (cmd != SNDRV_PCM_TRIGGER_START &&
	    cmd != SNDRV_PCM_TRIGGER_RESUME) {
		priv->speaker_release_pending = false;
		cancel_delayed_work_sync(&priv->speaker_release_work);
		return 0;
	}
	radar_log_codec_state(component, "machine-trigger-entry");

	/*
	 * The codec PLL/divider/profile/volume programming is complete in
	 * prepare().  The working 3.18 machine driver has no link trigger
	 * callback, so do not rewrite the codec clock tree after ASoC has
	 * unmuted the DAC.  A second PLL/divider transaction at this boundary
	 * can perturb the live DAC clock/gain engine even when the final register
	 * dump is identical.
	 */
	/* ASoC already performs the codec unmute from soc_pcm_prepare(), matching
	 * the 3.18 core.  Do not repeat that transaction here: the trigger runs
	 * after prepare and the duplicate unmute perturbs the AIC32X4 DAC gain
	 * engine on 6.1.  The physical amp GPIO is asserted by the MT8163 AFE
	 * trigger after DL1/routes are live, matching the old I2S-start boundary.
	 * Keep the machine-visible control state in sync here. */
	priv->amp_enable = 1;
	priv->speaker_release_pending = false;
	radar_log_codec_state(component, "machine-trigger-end");

	return 0;
}

static const struct snd_soc_ops radar_speaker_ops = {
	.startup = radar_speaker_startup,
	.shutdown = radar_speaker_shutdown,
	.hw_params = radar_speaker_hw_params,
	.hw_free = radar_speaker_hw_free,
	.prepare = radar_speaker_prepare,
	.trigger = radar_speaker_trigger,
};

static int radar_i2s1_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = mt8163_afe_select_i2s(&priv->afe_pdev->dev, true);
	if (ret)
		return ret;
	ret = mt8163_afe_select_mclk(&priv->afe_pdev->dev);
	if (ret)
		goto idle_i2s;
	ret = mt8163_mclk_enable(priv->mclk);
	if (ret)
		goto idle_i2s;
	return 0;

idle_i2s:
	mt8163_afe_select_i2s(&priv->afe_pdev->dev, false);
	return ret;
}

static void radar_i2s1_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);

	mt8163_afe_select_amp(&priv->afe_pdev->dev, false);
	mt8163_mclk_disable(priv->mclk);
	mt8163_afe_select_i2s(&priv->afe_pdev->dev, false);
}

static const struct snd_soc_ops radar_i2s1_ops = {
	.startup = radar_i2s1_startup,
	.shutdown = radar_i2s1_shutdown,
};

static int radar_capture_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	unsigned int rx_mask;
	int slot_width;
	int ret;

	if (params_rate(params) != 16000 ||
	    params_channels(params) != 9 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S24_3LE)
		return -EINVAL;

	ret = snd_soc_dai_set_pll(codec_dai, RADAR_ADC_PLL_BCLK,
				  RADAR_ADC_PLL_MCLK, RADAR_MCLK_RATE,
				  params_rate(params));
	if (ret)
		return ret;
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_B |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBM_CFM);
	if (ret)
		return ret;

	rx_mask = priv->linein_adc ? 0x40 : 0x7f;
	slot_width = priv->linein_adc ? 0 : snd_pcm_format_width(params_format(params));
	return snd_soc_dai_set_tdm_slot(codec_dai, 0, rx_mask,
					params_channels(params), slot_width);
}

static int radar_capture_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	ret = mt8163_afe_select_mclk(&priv->afe_pdev->dev);
	if (ret)
		return ret;
	return mt8163_mclk_enable(priv->mclk);
}

static void radar_capture_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct radar_card *priv = snd_soc_card_get_drvdata(rtd->card);

	mt8163_mclk_disable(priv->mclk);
}

static const struct snd_soc_ops radar_capture_ops = {
	.startup = radar_capture_startup,
	.shutdown = radar_capture_shutdown,
	.hw_params = radar_capture_hw_params,
};

static void radar_put_nodes(void *data)
{
	struct radar_card *priv = data;

	if (priv->afe_pdev)
		put_device(&priv->afe_pdev->dev);
	of_node_put(priv->afe_np);
	of_node_put(priv->spi_np);
	of_node_put(priv->dac_np);
	of_node_put(priv->adc_np);
}

static int radar_find_components(struct device *dev, struct radar_card *priv)
{
	int ret;

	priv->afe_np = of_find_compatible_node(NULL, NULL,
					       RADAR_AFE_COMPATIBLE);
	priv->spi_np = of_find_compatible_node(NULL, NULL,
					       RADAR_SPI_COMPATIBLE);
	priv->dac_np = of_find_compatible_node(NULL, NULL,
					       RADAR_DAC_COMPATIBLE);
	priv->adc_np = of_find_compatible_node(NULL, NULL,
					       RADAR_ADC_COMPATIBLE);
	dev_info(dev, "component lookup: afe=%pOF spi=%pOF dac=%pOF adc=%pOF\n",
		 priv->afe_np, priv->spi_np, priv->dac_np, priv->adc_np);
	if (!priv->afe_np || !priv->spi_np || !priv->dac_np || !priv->adc_np) {
		dev_err(dev, "component lookup incomplete; deferring\n");
		ret = -EPROBE_DEFER;
		goto put_nodes;
	}

	priv->afe_pdev = of_find_device_by_node(priv->afe_np);
	if (!priv->afe_pdev) {
		dev_err(dev, "AFE node exists but has no platform device; deferring\n");
		ret = -EPROBE_DEFER;
		goto put_nodes;
	}
	dev_info(dev, "AFE platform device is present: %s\n",
		 dev_name(&priv->afe_pdev->dev));
	return devm_add_action_or_reset(dev, radar_put_nodes, priv);

put_nodes:
	radar_put_nodes(priv);
	return ret;
}

static void radar_setup_link(struct radar_card *priv, int index,
			     const char *name, const char *stream, int pcm_id,
			     struct device_node *cpu_np, const char *cpu_dai,
			     struct device_node *codec_np, const char *codec_dai)
{
	struct snd_soc_dai_link *link = &priv->links[index];

	priv->cpus[index].of_node = cpu_np;
	priv->cpus[index].dai_name = cpu_dai;
	priv->platforms[index].of_node = cpu_np;
	priv->codecs[index].of_node = codec_np;
	priv->codecs[index].dai_name = codec_dai;
	link->name = name;
	link->stream_name = stream;
	link->id = pcm_id;
	link->cpus = &priv->cpus[index];
	link->num_cpus = 1;
	link->platforms = &priv->platforms[index];
	link->num_platforms = 1;
	link->codecs = &priv->codecs[index];
	link->num_codecs = 1;
}

static int radar_card_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct radar_card *priv;
	int ret;

	dev_info(dev, "Radar-Puffin machine probe entered\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	/* Puffin's DT audiosys node declares channel-type=MonoRight. */
	priv->channel_config = 2;
	priv->right_only = 1;
	INIT_DELAYED_WORK(&priv->speaker_release_work,
			  radar_speaker_release_work);
	ret = radar_find_components(dev, priv);
	if (ret)
		return ret;
	dev_info(dev, "component lookup complete; acquiring MCLK\n");
	priv->mclk = mt8163_mclk_get(dev);
	if (IS_ERR(priv->mclk)) {
		ret = PTR_ERR(priv->mclk);
		dev_err(dev, "cannot acquire codec MCLK: %d (%pe)\n", ret,
			priv->mclk);
		return ret;
	}

	radar_setup_link(priv, RADAR_LINK_DAC, "TI_DAC_Playback",
			 "TLV320AIC3204 Playback", 23, priv->afe_np,
			 RADAR_DL1_DAI, priv->dac_np, RADAR_DAC_DAI);
	priv->links[RADAR_LINK_DAC].playback_only = 1;
	priv->links[RADAR_LINK_DAC].nonatomic = 1;
	priv->links[RADAR_LINK_DAC].ignore_pmdown_time = 1;
	priv->links[RADAR_LINK_DAC].init = radar_speaker_init;
	priv->links[RADAR_LINK_DAC].ops = &radar_speaker_ops;

	radar_setup_link(priv, RADAR_LINK_CAPTURE, "AMZN_SPI_Capture",
			 "TLV320AIC3101 Capture", 24, priv->spi_np,
			 RADAR_SPI_DAI, priv->adc_np, RADAR_ADC_DAI);
	priv->links[RADAR_LINK_CAPTURE].capture_only = 1;
	priv->links[RADAR_LINK_CAPTURE].ignore_pmdown_time = 1;
	priv->links[RADAR_LINK_CAPTURE].ops = &radar_capture_ops;

	radar_setup_link(priv, RADAR_LINK_I2S1, "I2S1_Playback",
			 "I2S1_Playback", 25, priv->afe_np, RADAR_DL1_DAI,
			 NULL, "snd-soc-dummy-dai");
	priv->codecs[RADAR_LINK_I2S1].name = "snd-soc-dummy";
	priv->links[RADAR_LINK_I2S1].playback_only = 1;
	priv->links[RADAR_LINK_I2S1].nonatomic = 1;
	priv->links[RADAR_LINK_I2S1].ignore_pmdown_time = 1;
	priv->links[RADAR_LINK_I2S1].ops = &radar_i2s1_ops;

	priv->card.name = RADAR_CARD_NAME;
	priv->card.owner = THIS_MODULE;
	priv->card.dev = dev;
	priv->card.dai_link = priv->links;
	priv->card.num_links = ARRAY_SIZE(priv->links);
	priv->card.controls = radar_controls;
	priv->card.num_controls = ARRAY_SIZE(radar_controls);
	snd_soc_card_set_drvdata(&priv->card, priv);
	platform_set_drvdata(pdev, priv);

	ret = devm_snd_soc_register_card(dev, &priv->card);
	if (ret) {
		dev_err(dev, "cannot register Radar-Puffin card: %d\n", ret);
		return ret;
	}
	dev_info(dev, "Radar-Puffin machine card registered\n");
	return 0;
}

static const struct of_device_id radar_card_of_match[] = {
	{ .compatible = "mediatek,mt8163-soc-codec-63xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, radar_card_of_match);

static struct platform_driver radar_card_driver = {
	.probe = radar_card_probe,
	.driver = {
		.name = "mt8163-radar-puffin-audio",
		.of_match_table = radar_card_of_match,
	},
};
module_platform_driver(radar_card_driver);

MODULE_DESCRIPTION("Amazon Radar-Puffin MT8163 ASoC machine driver");
MODULE_LICENSE("GPL");
