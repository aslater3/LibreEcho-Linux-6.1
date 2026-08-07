/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SND_SOC_MT8163_MCLK_H
#define __SND_SOC_MT8163_MCLK_H

struct device;
struct mt8163_mclk;

struct mt8163_mclk *mt8163_mclk_get(struct device *dev);
int mt8163_mclk_enable(struct mt8163_mclk *mclk);
void mt8163_mclk_disable(struct mt8163_mclk *mclk);

#endif
