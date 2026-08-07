/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SND_SOC_MT8163_AFE_H
#define __SND_SOC_MT8163_AFE_H

#include <linux/types.h>

struct device;

int mt8163_afe_select_i2s(struct device *dev, bool enable);
int mt8163_afe_set_low_jitter(struct device *dev, bool enable);
int mt8163_afe_select_pmic(struct device *dev, bool enable);
int mt8163_afe_select_amp(struct device *dev, bool enable);
int mt8163_afe_select_dac(struct device *dev, bool enable);
int mt8163_afe_select_mclk(struct device *dev);

#endif
