// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/debugfs.h>
#include <soc/soundwire.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <asoc/msm-cdc-pinctrl.h>
#include <dt-bindings/sound/audio-codec-port-types.h>
#include "swr-dmic.h"

#define itoa(x)               ('0' + x)
#define DAI_NAME_NUM_INDEX    11
#define AIF_NAME_NUM_INDEX    12
#define DEFAULT_CODEC_NAME    "swr_dmic_tx0"
#define DEFAULT_AIF_NAME      "SWR_DMIC_AIF0 Playback"

/*
 * Private data Structure for swr-dmic. All parameters related to
 * external mic codec needs to be defined here.
 */
struct swr_dmic_priv {
	struct device *dev;
	struct swr_device *swr_slave;
	struct snd_soc_component *component;
	struct snd_soc_component_driver *driver;
	struct snd_soc_dai_driver *dai_driver;
	struct device_node *swr_dmic_vdd_np;
	int tx_mode;
	u8 tx_master_port_map[SWR_DMIC_MAX_PORTS];
};

const char *codec_name_list[] = {
	"swr-dmic-01",
	"swr-dmic-02",
	"swr-dmic-03",
	"swr-dmic-04",
	"swr-dmic-05",
};

static int get_master_port(int val)
{
	int master_port = 0;

	switch(val) {
	case 0:
		master_port = SWRM_TX1_CH1;
		break;
	case 1:
		master_port = SWRM_TX1_CH2;
		break;
	case 2:
		master_port = SWRM_TX1_CH3;
		break;
	case 3:
		master_port = SWRM_TX1_CH4;
		break;
	case 4:
		master_port = SWRM_TX2_CH1;
		break;
	case 5:
		master_port = SWRM_TX2_CH2;
		break;
	case 6:
		master_port = SWRM_TX2_CH3;
		break;
	case 7:
		master_port = SWRM_TX2_CH4;
		break;
	case 8:
		master_port = SWRM_TX3_CH1;
		break;
	case 9:
		master_port = SWRM_TX3_CH2;
		break;
	case 10:
		master_port = SWRM_TX3_CH3;
		break;
	case 11:
		master_port = SWRM_TX3_CH4;
		break;
	case 12:
		master_port = SWRM_PCM_IN;
		break;
	default:
		master_port = SWRM_TX1_CH1;
		pr_debug("%s: undefined value, fall back to default master_port: %d\n",
			 __func__, master_port);
		break;
	}

	pr_debug("%s: master_port: %d\n", __func__, master_port);
	return master_port;
}

static int get_master_port_val(int master_port)
{
	int val = 0;

	switch (master_port) {
	case SWRM_TX1_CH1:
		val = 0;
		break;
	case SWRM_TX1_CH2:
		val = 1;
		break;
	case SWRM_TX1_CH3:
		val = 2;
		break;
	case SWRM_TX1_CH4:
		val = 3;
		break;
	case SWRM_TX2_CH1:
		val = 4;
		break;
	case SWRM_TX2_CH2:
		val = 5;
		break;
	case SWRM_TX2_CH3:
		val = 6;
		break;
	case SWRM_TX2_CH4:
		val = 7;
		break;
	case SWRM_TX3_CH1:
		val = 8;
		break;
	case SWRM_TX3_CH2:
		val = 9;
		break;
	case SWRM_TX3_CH3:
		val = 10;
		break;
	case SWRM_TX3_CH4:
		val = 11;
		break;
	case SWRM_PCM_IN:
		val = 12;
		break;
	default:
		val = 0;
		pr_debug("%s: undefined master_port:%d, fallback to default val: %d\n",
			 __func__, master_port, val);
		break;
	}

	pr_debug("%s: master_port:%d val: %d\n", __func__, master_port, val);
	return val;
}

static inline int swr_dmic_tx_get_slave_port_type_idx(const char *wname,
				      unsigned int *port_idx)
{
	u8 port_type;

	if (strnstr(wname, "HIFI", sizeof("HIFI")))
		port_type = SWR_DMIC_HIFI_PORT;
	else if (strnstr(wname, "LP", sizeof("LP")))
		port_type = SWR_DMIC_LP_PORT;
	else
		return -EINVAL;

	*port_idx = port_type;
	return 0;
}

static int swr_dmic_tx_master_port_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
				snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic = snd_soc_component_get_drvdata(component);
	int ret = 0;
	int slave_port_idx;

	ret = swr_dmic_tx_get_slave_port_type_idx(kcontrol->id.name,
							&slave_port_idx);
	if (ret) {
		dev_dbg(component->dev, "%s: invalid port string\n", __func__);
		return ret;
	}

	if (slave_port_idx >= 0 &&
		slave_port_idx < SWR_DMIC_MAX_PORTS)
		ucontrol->value.integer.value[0] = get_master_port_val(
				swr_dmic->tx_master_port_map[slave_port_idx]);

	dev_dbg(component->dev, "%s: ucontrol->value.integer.value[0] = %ld\n",
			__func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int swr_dmic_tx_master_port_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
				snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic = snd_soc_component_get_drvdata(component);
	int ret = 0;
	int slave_port_idx;

	ret  = swr_dmic_tx_get_slave_port_type_idx(kcontrol->id.name,
							&slave_port_idx);
	if (ret) {
		dev_dbg(component->dev, "%s: invalid port string\n", __func__);
		return ret;
	}


	dev_dbg(component->dev, "%s: slave_port_idx: %d",
			__func__, slave_port_idx);
	dev_dbg(component->dev, "%s: ucontrol->value.enumerated.item[0] = %ld\n",
			__func__, ucontrol->value.enumerated.item[0]);
	if (slave_port_idx >= 0 &&
		slave_port_idx < SWR_DMIC_MAX_PORTS)
		swr_dmic->tx_master_port_map[slave_port_idx] =
				get_master_port(
					ucontrol->value.enumerated.item[0]);

	return 0;
}

static int swr_dmic_vdd_ctrl(struct swr_dmic_priv *swr_dmic, bool enable)
{
	int ret = 0;

	if (swr_dmic->swr_dmic_vdd_np) {
		if (enable)
			ret = msm_cdc_pinctrl_select_active_state(
							swr_dmic->swr_dmic_vdd_np);
		else
			ret = msm_cdc_pinctrl_select_sleep_state(
							swr_dmic->swr_dmic_vdd_np);
		if (ret != 0)
			dev_err(swr_dmic->dev, "%s: Failed to turn state %d; ret=%d\n",
				__func__, enable, ret);
	} else {
		dev_err(swr_dmic->dev, "%s: invalid pinctrl node\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}

static int dmic_swr_ctrl(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	int ret = 0;
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	u8 num_ch = 1;
	u8 ch_mask = 0x01; // only DpnChannelEN1 register is available
	u8 port_type = 0;
	u32 ch_rate = 0;
	u8 num_port = 1;
	/*
	 * Port 1 is high quality / 2.4 or 3.072 Mbps
	 * Port 2 is listen low power / 0.6 or 0.768 Mbps
	 */
	u8 port_id = swr_dmic->tx_mode;
	port_type = swr_dmic->tx_master_port_map[port_id];

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = swr_connect_port(swr_dmic->swr_slave, &port_id, num_port,
			&ch_mask, &ch_rate, &num_ch, &port_type);
		break;
	case SND_SOC_DAPM_POST_PMU:
		ret = swr_slvdev_datapath_control(swr_dmic->swr_slave,
			swr_dmic->swr_slave->dev_num, true);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = swr_slvdev_datapath_control(swr_dmic->swr_slave,
			swr_dmic->swr_slave->dev_num, false);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = swr_disconnect_port(swr_dmic->swr_slave, &port_id,
					  num_port, &ch_mask, &port_type);
		break;
	};

	return ret;
}

static const char * const tx_mode_text_swr_mic[] = {
	"MIC_HIFI", "MIC_LP",
};

static int swr_mic_tx_mode_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
			snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "%s: tx_mode  = %ld\n",
		__func__, ucontrol->value.integer.value[0]);

	swr_dmic->tx_mode =  ucontrol->value.integer.value[0];

	return 0;
}

static int swr_mic_tx_mode_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
			snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = swr_dmic->tx_mode;

	dev_dbg(component->dev, "%s: tx_mode = 0x%x\n", __func__,
			swr_dmic->tx_mode);

	return 0;
}

static const struct soc_enum tx_mode_enum_swr_mic =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tx_mode_text_swr_mic),
			    tx_mode_text_swr_mic);

static const char * const tx_master_port_text[] = {
	"SWRM_TX1_CH1", "SWRM_TX1_CH2", "SWRM_TX1_CH3", "SWRM_TX1_CH4",
	"SWRM_TX2_CH1", "SWRM_TX2_CH2", "SWRM_TX2_CH3", "SWRM_TX2_CH4",
	"SWRM_TX3_CH1", "SWRM_TX3_CH2", "SWRM_TX3_CH3", "SWRM_TX3_CH4",
	"SWRM_PCM_IN",
};

static const struct soc_enum tx_master_port_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tx_master_port_text),
				tx_master_port_text);

static const struct snd_kcontrol_new swr_dmic_snd_controls[] = {
	SOC_ENUM_EXT("TX MODE", tx_mode_enum_swr_mic,
			swr_mic_tx_mode_get, swr_mic_tx_mode_put),

	SOC_ENUM_EXT("HIFI PortMap", tx_master_port_enum,
		swr_dmic_tx_master_port_get, swr_dmic_tx_master_port_put),
	SOC_ENUM_EXT("LP PortMap", tx_master_port_enum,
		swr_dmic_tx_master_port_get, swr_dmic_tx_master_port_put),
};

static const struct snd_kcontrol_new dmic_switch[] = {
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_soc_dapm_widget swr_dmic_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER_E("DMIC_SWR_MIXER", SND_SOC_NOPM, 0, 0,
			dmic_switch, ARRAY_SIZE(dmic_switch), dmic_swr_ctrl,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("SWR_DMIC"),

	SND_SOC_DAPM_OUTPUT("SWR_DMIC_OUTPUT"),
};

static const struct snd_soc_dapm_route swr_dmic_audio_map[] = {
	{"SWR_DMIC_MIXER", "Switch", "SWR_DMIC"},
	{"SWR_DMIC_OUTPUT", NULL, "SWR_DMIC_MIXER"},
};

static int swr_dmic_codec_probe(struct snd_soc_component *component)
{
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	if (!swr_dmic)
		return -EINVAL;

	swr_dmic->component = component;
	return 0;
}

static void swr_dmic_codec_remove(struct snd_soc_component *component)
{
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	swr_dmic->component = NULL;
	return;
}

static const struct snd_soc_component_driver soc_codec_dev_swr_dmic = {
	.name = NULL,
	.probe = swr_dmic_codec_probe,
	.remove = swr_dmic_codec_remove,
	.controls = swr_dmic_snd_controls,
	.num_controls = ARRAY_SIZE(swr_dmic_snd_controls),
	.dapm_widgets = swr_dmic_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(swr_dmic_dapm_widgets),
	.dapm_routes = swr_dmic_audio_map,
	.num_dapm_routes = ARRAY_SIZE(swr_dmic_audio_map),
};

static struct snd_soc_dai_ops wsa_dai_ops = {
};

static struct snd_soc_dai_driver swr_dmic_dai[] = {
	{
		.name = "",
		.id = 0,
		.playback = {
			.stream_name = "",
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE),
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &wsa_dai_ops,
	},
};

static int swr_dmic_probe(struct swr_device *pdev)
{
	int ret = 0;
	struct swr_dmic_priv *swr_dmic;
	int i = 0;
	u8 devnum = 0;
	const char *swr_dmic_name_prefix_of = NULL;
	const char *swr_dmic_codec_name_of = NULL;
	struct snd_soc_component *component;
	char *dai_name;
	char *aif_name;

	swr_dmic = devm_kzalloc(&pdev->dev, sizeof(struct swr_dmic_priv),
			    GFP_KERNEL);
	if (!swr_dmic)
		return -ENOMEM;
	swr_dmic->swr_dmic_vdd_np = of_parse_phandle(pdev->dev.of_node,
					     "qcom,swr-dmic-vdd-node", 0);
	if (!swr_dmic->swr_dmic_vdd_np) {
		ret = -EINVAL;
		dev_dbg(&pdev->dev, "%s: Not using pinctrl\n", __func__);
	}
	swr_set_dev_data(pdev, swr_dmic);

	swr_dmic->swr_slave = pdev;

	ret = of_property_read_string(pdev->dev.of_node, "qcom,swr-dmic-prefix",
				&swr_dmic_name_prefix_of);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: Looking up %s property in node %s failed\n",
		__func__, "qcom,swr-dmic-prefix",
		pdev->dev.of_node->full_name);
		goto err;
	}

	ret = of_property_read_string(pdev->dev.of_node, "qcom,codec-name",
				&swr_dmic_codec_name_of);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: Looking up %s property in node %s failed\n",
		__func__, "qcom,codec-name",
		pdev->dev.of_node->full_name);
		goto err;
	}

	swr_dmic_vdd_ctrl(swr_dmic, true);
	/*
	 * Add 5msec delay to provide sufficient time for
	 * soundwire auto enumeration of slave devices as
	 * as per HW requirement.
	 */
	usleep_range(5000, 5010);
	ret = swr_get_logical_dev_num(pdev, pdev->addr, &devnum);
	if (ret) {
		dev_dbg(&pdev->dev,
			"%s get devnum %d for dev addr %lx failed\n",
			__func__, devnum, pdev->addr);
		goto dev_err;
	}
	pdev->dev_num = devnum;

	swr_dmic->driver = devm_kzalloc(&pdev->dev,
			sizeof(struct snd_soc_component_driver), GFP_KERNEL);
	if (!swr_dmic->driver) {
		ret = -ENOMEM;
		goto dev_err;
	}

	memcpy(swr_dmic->driver, &soc_codec_dev_swr_dmic,
			sizeof(struct snd_soc_component_driver));
	swr_dmic->driver->name = devm_kzalloc(&pdev->dev,
			strlen(swr_dmic_codec_name_of), GFP_KERNEL);
	if (!swr_dmic->driver->name) {
		ret = -ENOMEM;
		goto dev_err;
	}

	for (i = 0; i < ARRAY_SIZE(codec_name_list); i++) {
		if (!strcmp(swr_dmic_codec_name_of, codec_name_list[i]))
			break;
	}
	if (i == ARRAY_SIZE(codec_name_list))
		goto dev_err;

	swr_dmic->dai_driver = devm_kzalloc(&pdev->dev,
			sizeof(struct snd_soc_dai_driver), GFP_KERNEL);
	if (!swr_dmic->dai_driver) {
		ret = -ENOMEM;
		goto dev_err;
	}

	memcpy(swr_dmic->dai_driver, swr_dmic_dai,
			sizeof(struct snd_soc_dai_driver));
	dai_name = devm_kzalloc(&pdev->dev, strlen(DEFAULT_CODEC_NAME),
			GFP_KERNEL);
	if (!dai_name) {
		ret = -ENOMEM;
		goto dev_err;
	}

	memcpy(dai_name, DEFAULT_CODEC_NAME, strlen(DEFAULT_CODEC_NAME));
	dai_name[DAI_NAME_NUM_INDEX] = itoa(i);
	swr_dmic->dai_driver->name = dai_name;
	swr_dmic->dai_driver->id = i;
	aif_name = devm_kzalloc(&pdev->dev, strlen(DEFAULT_AIF_NAME),
			GFP_KERNEL);
	if (!aif_name) {
		ret = -ENOMEM;
		goto dev_err;
	}

	memcpy(aif_name, DEFAULT_AIF_NAME, strlen(DEFAULT_AIF_NAME));
	aif_name[AIF_NAME_NUM_INDEX] = itoa(i);
	swr_dmic->dai_driver->playback.stream_name = aif_name;

	ret = snd_soc_register_component(&pdev->dev, swr_dmic->driver,
				swr_dmic->dai_driver, 1);
	if (ret) {
		dev_err(&pdev->dev, "%s: Codec registration failed\n",
			__func__);
		goto dev_err;
	}

	component = snd_soc_lookup_component(&pdev->dev,
						swr_dmic->driver->name);
	swr_dmic->component = component;
	component->name_prefix = devm_kzalloc(&pdev->dev,
					strlen(swr_dmic_name_prefix_of),
	   GFP_KERNEL);
	if (!component->name_prefix) {
		ret = -ENOMEM;
		goto dev_err;
	}

	return 0;

dev_err:
	swr_dmic_vdd_ctrl(swr_dmic, false);
	swr_remove_device(pdev);
err:
	return ret;
}

static int swr_dmic_remove(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	snd_soc_unregister_component(&pdev->dev);
	swr_set_dev_data(pdev, NULL);
	return 0;
}

static int swr_dmic_up(struct swr_device *pdev)
{
	int ret = 0;
	struct swr_dmic_priv *swr_dmic;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}
	swr_dmic_vdd_ctrl(swr_dmic, true);

	return ret;
}

static int swr_dmic_down(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;
	int ret = 0;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}
	swr_dmic_vdd_ctrl(swr_dmic, false);

	return ret;
}

static int swr_dmic_reset(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;
	u8 retry = 5;
	u8 devnum = 0;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	while (swr_get_logical_dev_num(pdev, pdev->addr, &devnum) && retry--) {
		/* Retry after 1 msec delay */
		usleep_range(1000, 1100);
	}
	pdev->dev_num = devnum;

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int swr_dmic_suspend(struct device *dev)
{
	dev_dbg(dev, "%s: system suspend\n", __func__);
	return 0;
}

static int swr_dmic_resume(struct device *dev)
{
	struct swr_dmic_priv *swr_dmic = swr_get_dev_data(to_swr_device(dev));

	if (!swr_dmic) {
		dev_err(dev, "%s: swr_dmic private data is NULL\n", __func__);
		return -EINVAL;
	}
	dev_dbg(dev, "%s: system resume\n", __func__);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops swr_dmic_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(swr_dmic_suspend, swr_dmic_resume)
};

static const struct swr_device_id swr_dmic_id[] = {
	{"swr-dmic", 0},
	{}
};

static const struct of_device_id swr_dmic_dt_match[] = {
	{
		.compatible = "qcom,swr-dmic",
	},
	{}
};

static struct swr_driver swr_dmic_driver = {
	.driver = {
		.name = "swr-dmic",
		.owner = THIS_MODULE,
		.pm = &swr_dmic_pm_ops,
		.of_match_table = swr_dmic_dt_match,
	},
	.probe = swr_dmic_probe,
	.remove = swr_dmic_remove,
	.id_table = swr_dmic_id,
	.device_up = swr_dmic_up,
	.device_down = swr_dmic_down,
	.reset_device = swr_dmic_reset,
};

static int __init swr_dmic_init(void)
{
	return swr_driver_register(&swr_dmic_driver);
}

static void __exit swr_dmic_exit(void)
{
	swr_driver_unregister(&swr_dmic_driver);
}

module_init(swr_dmic_init);
module_exit(swr_dmic_exit);

MODULE_DESCRIPTION("SWR DMIC driver");
MODULE_LICENSE("GPL v2");
