/* Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/mfd/pm8xxx/misc.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/soc-dsp.h>
#include <sound/pcm.h>
#include <sound/q6asm.h>
#include <sound/jack.h>
#include <asm/mach-types.h>
#include <mach/socinfo.h>
#include "msm-pcm-routing.h"
#include "../codecs/wcd9304.h"
// HTC_AUD ++
#include "../../../arch/arm/mach-msm/htc_acoustic.h"
// HTC_AUD --
/* 8930 machine driver */

//htc audio ++
#undef pr_info
#undef pr_err
#define pr_info(fmt, ...) pr_aud_info(fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) pr_aud_err(fmt, ##__VA_ARGS__)
//htc audio --

#define MSM8930_SPK_ON 1
#define MSM8930_SPK_OFF 0

#define BTSCO_RATE_8KHZ 8000
#define BTSCO_RATE_16KHZ 16000

#define SITAR_EXT_CLK_RATE 12288000

#define SITAR_MBHC_DEF_BUTTONS 8
#define SITAR_MBHC_DEF_RLOADS 5
//HTC_AUD ++

#define GPIO_MI2S_WS     47
#define GPIO_MI2S_SCLK   48
#define GPIO_MI2S_DIN  49
#define GPIO_MI2S_DOUT  52


#define GPIO_I2S_TX_PCLK  55
#define GPIO_I2S_TX_WS    56
#define GPIO_I2S_TX_DIN   57
#define GPIO_SPK_AMP      75
#define GPIO_HAC_AMP      3

#define GPIO_AUX_PCM_DOUT 63
#define GPIO_AUX_PCM_DIN 64
#define GPIO_AUX_PCM_SYNC 65
#define GPIO_AUX_PCM_CLK 66

//HTC_AUD --
static int msm8930_spk_control;
static int msm8930_slim_0_rx_ch = 1;
static int msm8930_slim_0_tx_ch = 1;

static int msm8930_btsco_rate = BTSCO_RATE_8KHZ;
static int msm8930_btsco_ch = 1;

static struct clk *codec_clk;
static int clk_users;

static int msm8930_headset_gpios_configured;

static struct snd_soc_jack hs_jack;
static struct snd_soc_jack button_jack;

static atomic_t q6_effect_mode = ATOMIC_INIT(-1);

static int msm8930_enable_codec_ext_clk(
		struct snd_soc_codec *codec, int enable,
		bool dapm);

//HTC_AUD ++
static struct clk *pri_i2s_rx_osr_clk;
static struct clk *pri_i2s_rx_bit_clk;

static void gpio_init(void)
{
	gpio_tlmm_config(GPIO_CFG(GPIO_SPK_AMP, 0, GPIO_CFG_OUTPUT,
		 GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_set_value(GPIO_SPK_AMP, 0);
	gpio_tlmm_config(GPIO_CFG(GPIO_HAC_AMP, 0, GPIO_CFG_OUTPUT,
		 GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_set_value(GPIO_HAC_AMP, 0);
}

struct request_gpio {
	unsigned gpio_no;
	char *gpio_name;
};
static struct request_gpio mi2s_gpio[] = {
	{
		.gpio_no = GPIO_MI2S_WS,
		.gpio_name = "MI2S_WS",
	},
	{
		.gpio_no = GPIO_MI2S_SCLK,
		.gpio_name = "MI2S_SCLK",
	},
	{
		.gpio_no = GPIO_MI2S_DIN,
		.gpio_name = "MI2S_DIN",
	},
	{
		.gpio_no = GPIO_MI2S_DOUT,
		.gpio_name = "MI2S_DOUT0",
	},
};

static struct clk *mi2s_osr_clk;
static struct clk *mi2s_bit_clk;
static atomic_t mi2s_rsc_ref;

static int msm8930_mi2s_free_gpios(void)
{
	int	i;
	for (i = 0; i < ARRAY_SIZE(mi2s_gpio); i++)
		gpio_free(mi2s_gpio[i].gpio_no);
	return 0;
}


static int msm8930_mi2s_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	int rate = params_rate(params);
	int bit_clk_set = 0;

	bit_clk_set = 12288000/(rate * 2 * 16);
	clk_set_rate(mi2s_bit_clk, bit_clk_set);
	return 1;
}


static void msm8930_mi2s_shutdown(struct snd_pcm_substream *substream)
{

	if (atomic_dec_return(&mi2s_rsc_ref) == 0) {
		pr_info("%s: free mi2s resources\n", __func__);
		if (mi2s_bit_clk) {
			clk_disable_unprepare(mi2s_bit_clk);
			clk_put(mi2s_bit_clk);
			mi2s_bit_clk = NULL;
		}

		if (mi2s_osr_clk) {
			clk_disable_unprepare(mi2s_osr_clk);
			clk_put(mi2s_osr_clk);
			mi2s_osr_clk = NULL;
		}

		msm8930_mi2s_free_gpios();
	}
}

static int configure_mi2s_gpio(void)
{
	int	rtn;
	int	i;
	int	j;
	pr_info("config mi2s gpio");
	for (i = 0; i < ARRAY_SIZE(mi2s_gpio); i++) {
		rtn = gpio_request(mi2s_gpio[i].gpio_no,
						   mi2s_gpio[i].gpio_name);
		pr_debug("%s: gpio = %d, gpio name = %s, rtn = %d\n",
				 __func__,
				 mi2s_gpio[i].gpio_no,
				 mi2s_gpio[i].gpio_name,
				 rtn);
		if (rtn) {
			pr_err("%s: Failed to request gpio %d\n",
				   __func__,
				   mi2s_gpio[i].gpio_no);
			for (j = i; j >= 0; j--)
				gpio_free(mi2s_gpio[j].gpio_no);
			goto err;
		}
	}
err:
	return rtn;
}
static int msm8930_mi2s_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	pr_err("%s: dai name %s %p\n", __func__, cpu_dai->name, cpu_dai->dev);

	if (atomic_inc_return(&mi2s_rsc_ref) == 1) {
		pr_info("%s: acquire mi2s resources\n", __func__);
		configure_mi2s_gpio();
		mi2s_osr_clk = clk_get(cpu_dai->dev, "osr_clk");
		if (!IS_ERR(mi2s_osr_clk)) {
			clk_set_rate(mi2s_osr_clk, 12288000);
			clk_prepare_enable(mi2s_osr_clk);
		} else
			pr_err("Failed to get mi2s_osr_clk\n");
		mi2s_bit_clk = clk_get(cpu_dai->dev, "bit_clk");
		if (IS_ERR(mi2s_bit_clk)) {
			pr_err("Failed to get mi2s_bit_clk\n");
			clk_disable_unprepare(mi2s_osr_clk);
			clk_put(mi2s_osr_clk);
			return PTR_ERR(mi2s_bit_clk);
		}
		clk_set_rate(mi2s_bit_clk, 8);
		ret = clk_prepare_enable(mi2s_bit_clk);
		if (ret != 0) {
		pr_err("Unable to enable mi2s_rx_bit_clk\n");
			clk_put(mi2s_bit_clk);
			clk_disable_unprepare(mi2s_osr_clk);
			clk_put(mi2s_osr_clk);
			return ret;
		}
		ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_CBS_CFS);
		if (ret < 0)
			ret = snd_soc_dai_set_fmt(codec_dai,
						  SND_SOC_DAIFMT_CBS_CFS);
		if (ret < 0)
			pr_err("set format for codec dai failed\n");
	}

	return ret;
}
static struct snd_soc_ops msm8930_mi2s_be_ops = {
	.startup = msm8930_mi2s_startup,
	.shutdown = msm8930_mi2s_shutdown,
	.hw_params = msm8930_mi2s_hw_params,
};


static int msm8930_i2s_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	int rate = params_rate(params);
	int bit_clk_set = 0;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		bit_clk_set = 12288000/(rate * 2 * 16);
		pr_info("%s, bit clock is %d\n", __func__, bit_clk_set);
		clk_set_rate(pri_i2s_rx_bit_clk, bit_clk_set);
	}
	return 1;
}

static int msm8930_i2s_tx_free_gpios(void)
{
	gpio_free(GPIO_I2S_TX_DIN);
	gpio_free(GPIO_I2S_TX_WS);
	gpio_free(GPIO_I2S_TX_PCLK);
	return 0;
}

static void msm8930_i2s_shutdown(struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (pri_i2s_rx_bit_clk) {
			clk_disable_unprepare(pri_i2s_rx_bit_clk);
			clk_put(pri_i2s_rx_bit_clk);
			pri_i2s_rx_bit_clk = NULL;
		}
		if (pri_i2s_rx_osr_clk) {
			clk_disable_unprepare(pri_i2s_rx_osr_clk);
			clk_put(pri_i2s_rx_osr_clk);
			pri_i2s_rx_osr_clk = NULL;
		}
		msm8930_i2s_tx_free_gpios();
	}
}

static int configure_i2s_tx_gpio(void)
{
	int rtn;

	rtn	= gpio_request(GPIO_I2S_TX_PCLK, "I2S_TX_PCLK");
	if (rtn) {
		pr_err("%s: Failed to request gpio %d\n", __func__,
			   GPIO_I2S_TX_PCLK);
		goto err;
	}
	rtn	= gpio_request(GPIO_I2S_TX_WS, "I2S_TX_WS");
	if (rtn) {
		pr_err("%s: Failed to request gpio %d\n", __func__,
			   GPIO_I2S_TX_WS);
		goto err;
	}
	rtn	= gpio_request(GPIO_I2S_TX_DIN, "I2S_TX_DIN");
	if (rtn) {
		pr_err("%s: Failed to request gpio %d\n", __func__,
			   GPIO_I2S_TX_DIN);
		goto err;
	}
err:
	return rtn;
}

static int msm8930_i2s_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		configure_i2s_tx_gpio();

		pri_i2s_rx_osr_clk = clk_get(cpu_dai->dev, "osr_clk");
		if (IS_ERR(pri_i2s_rx_osr_clk)) {
			pr_err("Failed to get pri_i2s_rx_osr_clk\n");
			return PTR_ERR(pri_i2s_rx_osr_clk);
		}
		clk_set_rate(pri_i2s_rx_osr_clk, 12288000);
		clk_prepare_enable(pri_i2s_rx_osr_clk);

		pri_i2s_rx_bit_clk = clk_get(cpu_dai->dev, "bit_clk");
		if (IS_ERR(pri_i2s_rx_bit_clk)) {
			pr_err("Failed to get pri_i2s_rx_bit_clk\n");
			clk_disable_unprepare(pri_i2s_rx_osr_clk);
			clk_put(pri_i2s_rx_osr_clk);
			return PTR_ERR(pri_i2s_rx_bit_clk);
		}
		clk_set_rate(pri_i2s_rx_bit_clk, 8);
		ret = clk_prepare_enable(pri_i2s_rx_bit_clk);
		if (ret != 0) {
			pr_err("Unable to enable pri_i2s_rx_bit_clk\n");
			clk_put(pri_i2s_rx_bit_clk);
			clk_disable_unprepare(pri_i2s_rx_osr_clk);
			clk_put(pri_i2s_rx_osr_clk);
			return ret;
		}

		/* configurea as master mode */
		ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_CBS_CFS);
		if (ret < 0) {
			pr_info("%s: set format for cpu dai failed\n", __func__);
			ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_CBS_CFS);
		}
		if (ret < 0)
			pr_err("set format for codec dai failed\n");
	}
	return ret;
}

static struct snd_soc_ops msm8930_i2s_be_ops = {
	.startup = msm8930_i2s_startup,
	.shutdown = msm8930_i2s_shutdown,
	.hw_params = msm8930_i2s_hw_params,
};
//HTC_AUD --

static void msm8930_ext_control(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	pr_debug("%s: msm8930_spk_control = %d", __func__, msm8930_spk_control);
	if (msm8930_spk_control == MSM8930_SPK_ON) {
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Left Pos");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk left Neg");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Left Pos");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Left Neg");
	}

	snd_soc_dapm_sync(dapm);
}

static int msm8930_get_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8930_spk_control = %d", __func__, msm8930_spk_control);
	ucontrol->value.integer.value[0] = msm8930_spk_control;
	return 0;
}
static int msm8930_set_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	pr_debug("%s()\n", __func__);
	if (msm8930_spk_control == ucontrol->value.integer.value[0])
		return 0;

	msm8930_spk_control = ucontrol->value.integer.value[0];
	msm8930_ext_control(codec);
	return 1;
}

//HTC_AUD ++
static int msm8930_spkramp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *k, int event)
{
	pr_info("%s() %x\n", __func__, SND_SOC_DAPM_EVENT_ON(event));
	/* TODO: add external speaker power amps support */
	if (SND_SOC_DAPM_EVENT_ON(event))
		gpio_set_value(GPIO_SPK_AMP, 1);
	else
		gpio_set_value(GPIO_SPK_AMP, 0);

	return 0;
}

static int msm8930_hacramp_event(struct snd_soc_dapm_widget *w,
        struct snd_kcontrol *k, int event)
{
        pr_info("%s() %x\n", __func__, SND_SOC_DAPM_EVENT_ON(event));
        /* TODO: add external hac amps support */
        if (SND_SOC_DAPM_EVENT_ON(event)) {
            pr_err("HAC amp on");
            gpio_set_value(GPIO_HAC_AMP, 1);
        } else {
            pr_err("HAC amp off");
                gpio_set_value(GPIO_HAC_AMP, 0);
        }
        return 0;
}
//HTC_AUD --

static int msm8930_enable_codec_ext_clk(
		struct snd_soc_codec *codec, int enable,
		bool dapm)
{
	pr_debug("%s: enable = %d\n", __func__, enable);
	if (enable) {
		clk_users++;
		pr_debug("%s: clk_users = %d\n", __func__, clk_users);
		if (clk_users != 1)
			return 0;

		if (codec_clk) {
			clk_set_rate(codec_clk, SITAR_EXT_CLK_RATE);
			clk_prepare_enable(codec_clk);
			sitar_mclk_enable(codec, 1, dapm);
		} else {
			pr_err("%s: Error setting Sitar MCLK\n", __func__);
			clk_users--;
			return -EINVAL;
		}
	} else {
		pr_debug("%s: clk_users = %d\n", __func__, clk_users);
		if (clk_users == 0)
			return 0;
		clk_users--;
		if (!clk_users) {
			pr_debug("%s: disabling MCLK. clk_users = %d\n",
					 __func__, clk_users);
			sitar_mclk_enable(codec, 0, dapm);
			clk_disable_unprepare(codec_clk);
		}
	}
	return 0;
}

static int msm8930_mclk_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return msm8930_enable_codec_ext_clk(w->codec, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return msm8930_enable_codec_ext_clk(w->codec, 0, true);
	}
	return 0;
}
//HTC_AUD ++
static const struct snd_kcontrol_new earamp_switch_controls =
	SOC_DAPM_SINGLE("Switch", 0, 0, 1, 0);

static const struct snd_kcontrol_new spkamp_switch_controls =
        SOC_DAPM_SINGLE("Switch", 0, 0, 1, 0);


static const struct snd_soc_dapm_widget tc2_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER("Lineout Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SPK AMP EN", SND_SOC_NOPM, 0, 0, &spkamp_switch_controls, 1),
	SND_SOC_DAPM_MIXER("HAC AMP EN", SND_SOC_NOPM, 0, 0, &earamp_switch_controls, 1),

};
//HTC_AUD --
static const struct snd_soc_dapm_widget msm8930_dapm_widgets[] = {

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	msm8930_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SPK("Ext Spk Left Pos", msm8930_spkramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Left Neg", NULL),
//HTC_AUD ++
	SND_SOC_DAPM_SPK("Ext Spk Top Pos", msm8930_hacramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Top Neg", NULL),
//HTC_AUD --

	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),

	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),

};

static const struct snd_soc_dapm_route common_audio_map[] = {

	{"RX_BIAS", NULL, "MCLK"},
	{"LDO_H", NULL, "MCLK"},

	{"MIC BIAS1 Internal1", NULL, "MCLK"},
	{"MIC BIAS2 Internal1", NULL, "MCLK"},

//HTC_AUD ++
	/* Speaker path */
	{"Ext Spk Left Pos", NULL, "SPK AMP EN"},
	{"Ext Spk Left Neg", NULL, "SPK AMP EN"},
	{"SPK AMP EN", "Switch", "Lineout Mixer"},
	/* HAC path */
	{"Ext Spk Top Pos", NULL, "HAC AMP EN"},
	{"Ext Spk Top Neg", NULL, "HAC AMP EN"},
	{"HAC AMP EN", "Switch", "Lineout Mixer"},

	{"Lineout Mixer", NULL, "LINEOUT2"},
	{"Lineout Mixer", NULL, "LINEOUT1"},

	/* Headset Mic */
/*
	{"AMIC2", NULL, "MIC BIAS2 Internal1"},
	{"MIC BIAS2 Internal1", NULL, "Headset Mic"},
*/
	{"AMIC2", NULL, "MIC BIAS2 External"},
	{"MIC BIAS2 External", NULL, "Headset Mic"},

	/* Microphone path */
/*
	{"AMIC1", NULL, "MIC BIAS2 Internal1"},
	{"MIC BIAS2 Internal1", NULL, "ANCLeft Headset Mic"},
*/
	{"AMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "ANCLeft Headset Mic"},
//HTC_AUD --
	{"AMIC3", NULL, "MIC BIAS2 External"},
	{"MIC BIAS2 External", NULL, "ANCRight Headset Mic"},

	{"HEADPHONE", NULL, "LDO_H"},

	/**
	 * The digital Mic routes are setup considering
	 * fluid as default device.
	 */

	/**
	 * Digital Mic1. Front Bottom left Mic on Fluid and MTP.
	 * Digital Mic GM5 on CDP mainboard.
	 * Conncted to DMIC1 Input on Sitar codec.
	 */
	{"DMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic1"},

	/**
	 * Digital Mic2. Back top MIC on Fluid.
	 * Digital Mic GM6 on CDP mainboard.
	 * Conncted to DMIC2 Input on Sitar codec.
	 */
	{"DMIC2", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic2"},
	/**
	 * Digital Mic3. Back Bottom Digital Mic on Fluid.
	 * Digital Mic GM1 on CDP mainboard.
	 * Conncted to DMIC4 Input on Sitar codec.
	 */
	{"DMIC3", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic3"},

	/**
	 * Digital Mic4. Back top Digital Mic on Fluid.
	 * Digital Mic GM2 on CDP mainboard.
	 * Conncted to DMIC3 Input on Sitar codec.
	 */
	{"DMIC4", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Digital Mic4"},


};

static const char *spk_function[] = {"Off", "On"};
static const char *slim0_rx_ch_text[] = {"One", "Two"};
static const char *slim0_tx_ch_text[] = {"One", "Two", "Three", "Four"};

static const struct soc_enum msm8930_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, spk_function),
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim0_tx_ch_text),
};

static const char *btsco_rate_text[] = {"8000", "16000"};
static const struct soc_enum msm8930_btsco_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, btsco_rate_text),
};

static int msm8930_slim_0_rx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8930_slim_0_rx_ch  = %d\n", __func__,
		msm8930_slim_0_rx_ch);
	ucontrol->value.integer.value[0] = msm8930_slim_0_rx_ch - 1;
	return 0;
}

static int msm8930_slim_0_rx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm8930_slim_0_rx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm8930_slim_0_rx_ch = %d\n", __func__,
		msm8930_slim_0_rx_ch);
	return 1;
}

static int msm8930_slim_0_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8930_slim_0_tx_ch  = %d\n", __func__,
		 msm8930_slim_0_tx_ch);
	ucontrol->value.integer.value[0] = msm8930_slim_0_tx_ch - 1;
	return 0;
}

static int msm8930_slim_0_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm8930_slim_0_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm8930_slim_0_tx_ch = %d\n", __func__,
		 msm8930_slim_0_tx_ch);
	return 1;
}

static int msm8930_btsco_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8930_btsco_rate  = %d", __func__, msm8930_btsco_rate);
	ucontrol->value.integer.value[0] = msm8930_btsco_rate;
	return 0;
}

static int msm8930_btsco_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm8930_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	case 1:
		msm8930_btsco_rate = BTSCO_RATE_16KHZ;
		break;
	default:
		msm8930_btsco_rate = BTSCO_RATE_8KHZ;
		break;
	}
	pr_debug("%s: msm8930_btsco_rate = %d\n", __func__, msm8930_btsco_rate);
	return 0;
}

static const struct snd_kcontrol_new sitar_msm8930_controls[] = {
	SOC_ENUM_EXT("Speaker Function", msm8930_enum[0], msm8930_get_spk,
		msm8930_set_spk),
	SOC_ENUM_EXT("SLIM_0_RX Channels", msm8930_enum[1],
		msm8930_slim_0_rx_ch_get, msm8930_slim_0_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_TX Channels", msm8930_enum[2],
		msm8930_slim_0_tx_ch_get, msm8930_slim_0_tx_ch_put),
};

static const struct snd_kcontrol_new int_btsco_rate_mixer_controls[] = {
	SOC_ENUM_EXT("Internal BTSCO SampleRate", msm8930_btsco_enum[0],
		msm8930_btsco_rate_get, msm8930_btsco_rate_put),
};

static int msm8930_btsco_init(struct snd_soc_pcm_runtime *rtd)
{
	int err = 0;
	struct snd_soc_platform *platform = rtd->platform;

	err = snd_soc_add_platform_controls(platform,
			int_btsco_rate_mixer_controls,
		ARRAY_SIZE(int_btsco_rate_mixer_controls));
	if (err < 0)
		return err;
	return 0;
}

static int msm_aux_pcm_get_gpios(void)
{
	int ret = 0;

	pr_debug("%s\n", __func__);

	ret = gpio_request(GPIO_AUX_PCM_DOUT, "AUX PCM DOUT");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DOUT",
				__func__, GPIO_AUX_PCM_DOUT);
		goto fail_dout;
	}

	ret = gpio_request(GPIO_AUX_PCM_DIN, "AUX PCM DIN");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DIN",
				__func__, GPIO_AUX_PCM_DIN);
		goto fail_din;
	}

	ret = gpio_request(GPIO_AUX_PCM_SYNC, "AUX PCM SYNC");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM SYNC",
				__func__, GPIO_AUX_PCM_SYNC);
		goto fail_sync;
	}
	ret = gpio_request(GPIO_AUX_PCM_CLK, "AUX PCM CLK");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM CLK",
				__func__, GPIO_AUX_PCM_CLK);
		goto fail_clk;
	}

	return 0;

fail_clk:
	gpio_free(GPIO_AUX_PCM_SYNC);
fail_sync:
	gpio_free(GPIO_AUX_PCM_DIN);
fail_din:
	gpio_free(GPIO_AUX_PCM_DOUT);
fail_dout:

	return ret;
}

static int msm_aux_pcm_free_gpios(void)
{
	gpio_free(GPIO_AUX_PCM_DIN);
	gpio_free(GPIO_AUX_PCM_DOUT);
	gpio_free(GPIO_AUX_PCM_SYNC);
	gpio_free(GPIO_AUX_PCM_CLK);

	return 0;
}

static int msm8930_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	unsigned int rx_ch_cnt = 0, tx_ch_cnt = 0;

	pr_debug("%s: ch=%d\n", __func__,
					msm8930_slim_0_rx_ch);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				msm8930_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, 0,
				msm8930_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
							       __func__);
			goto end;
		}
	} else {
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(cpu_dai,
				msm8930_slim_0_tx_ch, tx_ch, 0 , 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai,
				msm8930_slim_0_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
							       __func__);
			goto end;
		}

	}
end:
	return ret;
}

static int msm8930_audrx_init(struct snd_soc_pcm_runtime *rtd)
{
	int err;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s()\n", __func__);


	rtd->pmdown_time = 0;

	err = snd_soc_add_controls(codec, sitar_msm8930_controls,
				ARRAY_SIZE(sitar_msm8930_controls));
	if (err < 0)
		return err;

	snd_soc_dapm_new_controls(dapm, msm8930_dapm_widgets,
				ARRAY_SIZE(msm8930_dapm_widgets));
//HTC_AUD ++
	 snd_soc_dapm_new_controls(dapm, tc2_dapm_widgets,
								 ARRAY_SIZE(tc2_dapm_widgets));
//HTC_AUD --
	snd_soc_dapm_add_routes(dapm, common_audio_map,
		ARRAY_SIZE(common_audio_map));

	snd_soc_dapm_enable_pin(dapm, "Ext Spk Left Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Left Neg");
//HTC_AUD ++
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Neg");
//HTC_AUD --

	snd_soc_dapm_sync(dapm);

	err = snd_soc_jack_new(codec, "Headset Jack",
		(SND_JACK_HEADSET | SND_JACK_OC_HPHL | SND_JACK_OC_HPHR),
		&hs_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}

	err = snd_soc_jack_new(codec, "Button Jack",
				SND_JACK_BTN_0, &button_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}
	codec_clk = clk_get(cpu_dai->dev, "osr_clk");

	return 0;
}

static struct snd_soc_dsp_link lpa_fe_media = {
	.playback = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link fe_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link slimbus0_hl_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link int_fm_hl_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

/* bi-directional media definition for hostless PCM device */
static struct snd_soc_dsp_link bidir_hl_media = {
	.playback = true,
	.capture = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static struct snd_soc_dsp_link hdmi_rx_hl = {
	.playback = true,
	.trigger = {
		SND_SOC_DSP_TRIGGER_POST,
		SND_SOC_DSP_TRIGGER_POST
	},
};

static int msm8930_slim_0_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm8930_slim_0_rx_ch;

	return 0;
}

static int msm8930_slim_0_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm8930_slim_0_tx_ch;

	return 0;
}

static int msm8930_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;

	return 0;
}

static int msm8930_hdmi_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	return 0;
}

static int msm8930_btsco_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm8930_btsco_rate;
	channels->min = channels->max = msm8930_btsco_ch;

	return 0;
}

static int msm_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	/* PCM only supports mono output with 8khz sample rate */
	rate->min = rate->max = 8000;
	channels->min = channels->max = 1;

	return 0;
}

static int msm_proxy_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	printk("%s()\n", __func__);
	rate->min = rate->max = 48000;

	return 0;
}

static int msm_auxpcm_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	ret = msm_aux_pcm_get_gpios();
	if (ret < 0) {
		pr_err("%s: Aux PCM GPIO request failed\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static void msm_auxpcm_shutdown(struct snd_pcm_substream *substream)
{

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	msm_aux_pcm_free_gpios();
}

static int msm8930_startup(struct snd_pcm_substream *substream)
{
	pr_debug("%s(): substream = %s  stream = %d\n", __func__,
		 substream->name, substream->stream);
	return 0;
}

static void msm8930_shutdown(struct snd_pcm_substream *substream)
{
	pr_debug("%s(): substream = %s  stream = %d\n", __func__,
		 substream->name, substream->stream);
}

static struct snd_soc_ops msm8930_be_ops = {
	.startup = msm8930_startup,
	.hw_params = msm8930_hw_params,
	.shutdown = msm8930_shutdown,
};

static struct snd_soc_ops msm_auxpcm_be_ops = {
	.startup = msm_auxpcm_startup,
	.shutdown = msm_auxpcm_shutdown,
};


 /*Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link msm8930_XB_dai[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MSM8930 Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name	= "MultiMedia1",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM8930 Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name	= "MultiMedia2",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name	= "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "MSM8930 LPA",
		.stream_name = "LPA",
		.cpu_dai_name	= "MultiMedia3",
		.platform_name  = "msm-pcm-lpa",
		.dynamic = 1,
		.dsp_link = &lpa_fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA3,
		.ignore_suspend = 1,
	},
	/* Hostless PMC purpose */
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpu_dai_name	= "SLIMBUS0_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &slimbus0_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
//HTC_AUD ++ : replace QCT RIVA by BoardCom4334
#if 0
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpu_dai_name	= "INT_FM_HOSTLESS",
#endif
		.name = "PRI_I2S_TX Hostless",
		.stream_name = "PRI_I2S Hostless",
		.cpu_dai_name = "MI2S_TX_HOSTLESS",
//HTC_AUD --
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &int_fm_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM8930 Compr",
		.stream_name = "COMPR",
		.cpu_dai_name	= "MultiMedia4",
		.platform_name  = "msm-compr-dsp",
		.dynamic = 1,
		.dsp_link = &lpa_fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA4,
		.ignore_suspend = 1,
	},
	 {
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.cpu_dai_name   = "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &bidir_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	/* HDMI Hostless */
	{
		.name = "HDMI_RX_HOSTLESS",
		.stream_name = "HDMI_RX_HOSTLESS",
		.cpu_dai_name = "HDMI_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &hdmi_rx_hl,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.no_codec = 1,
		.ignore_suspend = 1,
	},
	/* Backend DAI Links */
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.cpu_dai_name = "msm-dai-q6.16384",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "sitar_codec",
		.codec_dai_name	= "sitar_rx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm8930_audrx_init,
		.be_hw_params_fixup = msm8930_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8930_be_ops,
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.cpu_dai_name = "msm-dai-q6.16385",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "sitar_codec",
		.codec_dai_name	= "sitar_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm8930_slim_0_tx_be_hw_params_fixup,
		.ops = &msm8930_be_ops,
	},
	/* Backend BT/FM DAI Links */
	{
		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.cpu_dai_name = "msm-dai-q6.12288",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-rx",
		.init = &msm8930_btsco_init,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm8930_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.cpu_dai_name = "msm-dai-q6.12289",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm8930_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.cpu_dai_name = "msm-dai-q6.12292",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.cpu_dai_name = "msm-dai-q6.12293",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* HDMI BACK END DAI Link */
	{
		.name = LPASS_BE_HDMI,
		.stream_name = "HDMI Playback",
		.cpu_dai_name = "msm-dai-q6-hdmi.8",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_HDMI_RX,
		.be_hw_params_fixup = msm8930_hdmi_be_hw_params_fixup,
	},
//HTC_AUD ++
	{
		.name = LPASS_BE_MI2S_RX,
		.stream_name = "MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s",
		.platform_name = "msm-pcm-routing",
		.codec_name 	= "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_MI2S_RX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
		.ops = &msm8930_mi2s_be_ops,
	},
	{
		.name = LPASS_BE_MI2S_TX,
		.stream_name = "MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s",
		.platform_name = "msm-pcm-routing",
		.codec_name 	= "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_MI2S_TX,
		.ops = &msm8930_mi2s_be_ops,
	},
//HTC_AUD --
	/* Backend AFE DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.be_hw_params_fixup = msm_proxy_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.be_hw_params_fixup = msm_proxy_be_hw_params_fixup,
	},
	/* AUX PCM Backend DAI Links */
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_auxpcm_be_ops,
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6.3",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
	},
	/* Incall Music BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.cpu_dai_name = "msm-dai-q6.32773",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.cpu_dai_name = "msm-dai-q6.32772",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.cpu_dai_name = "msm-dai-q6.32771",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
};

/* Digital audio interface glue - connects codec <---> CPU */
static struct snd_soc_dai_link msm8930_dai[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MSM8930 Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name	= "MultiMedia1",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM8930 Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name	= "MultiMedia2",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name	= "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.dsp_link = &fe_media,
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "MSM8930 LPA",
		.stream_name = "LPA",
		.cpu_dai_name	= "MultiMedia3",
		.platform_name  = "msm-pcm-lpa",
		.dynamic = 1,
		.dsp_link = &lpa_fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA3,
		.ignore_suspend = 1,
	},
	/* Hostless PMC purpose */
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpu_dai_name	= "SLIMBUS0_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &slimbus0_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
//HTC_AUD ++ : replace QCT RIVA by BoardCom4334
#if 0
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpu_dai_name	= "INT_FM_HOSTLESS",
#endif
		.name = "PRI_I2S_TX Hostless",
		.stream_name = "PRI_I2S Hostless",
		.cpu_dai_name = "PRI_I2S_HOSTLESS",
//HTC_AUD --
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &int_fm_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* .be_id = do not care */
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM8930 Compr",
		.stream_name = "COMPR",
		.cpu_dai_name	= "MultiMedia4",
		.platform_name  = "msm-compr-dsp",
		.dynamic = 1,
		.dsp_link = &lpa_fe_media,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA4,
		.ignore_suspend = 1,
	},
	 {
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.cpu_dai_name   = "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &bidir_hl_media,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	/* HDMI Hostless */
	{
		.name = "HDMI_RX_HOSTLESS",
		.stream_name = "HDMI_RX_HOSTLESS",
		.cpu_dai_name = "HDMI_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dsp_link = &hdmi_rx_hl,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.no_codec = 1,
		.ignore_suspend = 1,
	},
	/* Backend DAI Links */
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.cpu_dai_name = "msm-dai-q6.16384",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "sitar_codec",
		.codec_dai_name	= "sitar_rx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm8930_audrx_init,
		.be_hw_params_fixup = msm8930_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8930_be_ops,
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.cpu_dai_name = "msm-dai-q6.16385",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "sitar_codec",
		.codec_dai_name	= "sitar_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm8930_slim_0_tx_be_hw_params_fixup,
		.ops = &msm8930_be_ops,
	},
	/* Backend BT/FM DAI Links */
	{
		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.cpu_dai_name = "msm-dai-q6.12288",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-rx",
		.init = &msm8930_btsco_init,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm8930_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.cpu_dai_name = "msm-dai-q6.12289",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm8930_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.cpu_dai_name = "msm-dai-q6.12292",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.cpu_dai_name = "msm-dai-q6.12293",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* HDMI BACK END DAI Link */
	{
		.name = LPASS_BE_HDMI,
		.stream_name = "HDMI Playback",
		.cpu_dai_name = "msm-dai-q6-hdmi.8",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_HDMI_RX,
		.be_hw_params_fixup = msm8930_hdmi_be_hw_params_fixup,
	},
//HTC_AUD ++
	{
		.name = LPASS_BE_PRI_I2S_TX,
		.stream_name = "Primary I2S Capture",
		.cpu_dai_name = "msm-dai-q6.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_PRI_I2S_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
		.ops = &msm8930_i2s_be_ops,
	},
//HTC_AUD --
	/* Backend AFE DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.be_hw_params_fixup = msm_proxy_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_codec = 1,
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.be_hw_params_fixup = msm_proxy_be_hw_params_fixup,
	},
	/* AUX PCM Backend DAI Links */
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_auxpcm_be_ops,
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6.3",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
	},
	/* Incall Music BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.cpu_dai_name = "msm-dai-q6.32773",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.cpu_dai_name = "msm-dai-q6.32772",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.cpu_dai_name = "msm-dai-q6.32771",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.no_codec = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm8930_be_hw_params_fixup,
	},
};

struct snd_soc_card snd_soc_card_msm8930 = {
	.name		= "msm8930-sitar-snd-card",
	.dai_link	= msm8930_dai,
	.num_links	= ARRAY_SIZE(msm8930_dai),
};

struct snd_soc_card snd_soc_card_msm8930_XB = {
	.name		= "msm8930-sitar-snd-card",
	.dai_link	= msm8930_XB_dai,
	.num_links	= ARRAY_SIZE(msm8930_XB_dai),
};

static struct platform_device *msm8930_snd_device;

void tc2_set_q6_effect_mode(int mode)
{
	pr_info("%s: mode %d\n", __func__, mode);
	atomic_set(&q6_effect_mode, mode);
}

int tc2_get_q6_effect_mode(void)
{
	int mode = atomic_read(&q6_effect_mode);
	pr_info("%s: mode %d\n", __func__, mode);
	return mode;
}

int tc2_get_hw_revision(void)
{
	int audio_hw_rev;

	audio_hw_rev = system_rev;

	pr_info("%s: audio hw rev is %d\n", __func__, audio_hw_rev);
	return audio_hw_rev;
}
int tc2_get_component_info(void)
{
	return 0;
}

static struct acoustic_ops acoustic = {
	.set_q6_effect = tc2_set_q6_effect_mode,
	.get_htc_revision = tc2_get_hw_revision,
	.get_hw_component = tc2_get_component_info,
};

static struct q6asm_ops qops = {
	.get_q6_effect = tc2_get_q6_effect_mode,
};

static struct msm_pcm_routing_ops rops = {
	.get_q6_effect = tc2_get_q6_effect_mode,
};

//HTC_AUD --


static int __init msm8930_audio_init(void)
{
	int ret;

	if (!cpu_is_msm8930() && !cpu_is_msm8930aa()) {
		pr_err("%s: Not the right machine type\n", __func__);
		return -ENODEV ;
	}

	msm8930_snd_device = platform_device_alloc("soc-audio", 0);
	if (!msm8930_snd_device) {
		pr_err("Platform device allocation failed\n");
		return -ENOMEM;
	}
//HTC_AUD ++
	if (system_rev == 0) {
		pr_info("register XA sound card");
		platform_set_drvdata(msm8930_snd_device, &snd_soc_card_msm8930);
	} else {
		pr_info("register XB sound card");

	atomic_set(&mi2s_rsc_ref, 0);

		platform_set_drvdata(msm8930_snd_device, &snd_soc_card_msm8930_XB);

	}
// HTC_AUD --
	ret = platform_device_add(msm8930_snd_device);
	if (ret) {
		platform_device_put(msm8930_snd_device);
		return ret;
	}

	msm8930_headset_gpios_configured = 0;


// HTC_AUD ++
	gpio_init();
	htc_register_q6asm_ops(&qops);
	htc_register_pcm_routing_ops(&rops);
	acoustic_register_ops(&acoustic);
// HTC_AUD --
	return ret;

}
module_init(msm8930_audio_init);

static void __exit msm8930_audio_exit(void)
{
	if (!cpu_is_msm8930() && !cpu_is_msm8930aa()) {
		pr_err("%s: Not the right machine type\n", __func__);
		return ;
	}
	platform_device_unregister(msm8930_snd_device);


}
module_exit(msm8930_audio_exit);

MODULE_DESCRIPTION("ALSA SoC MSM8930");
MODULE_LICENSE("GPL v2");
