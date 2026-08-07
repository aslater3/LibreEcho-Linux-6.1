// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Device Tree support for Mediatek SoCs
 *
 * Copyright (c) 2014 MundoReader S.L.
 * Author: Matthias Brugger <matthias.bgg@gmail.com>
 */
#include <linux/init.h>
#include <linux/io.h>
#include <asm/mach/arch.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/clocksource.h>


#define GPT6_CON_MT65xx 0x10008060
#define GPT_ENABLE      0x31

static void __init mediatek_timer_init(void)
{
	void __iomem *gpt_base;

	if (of_machine_is_compatible("mediatek,mt6589") ||
	    of_machine_is_compatible("mediatek,mt7623") ||
	    of_machine_is_compatible("mediatek,mt8135") ||
	    of_machine_is_compatible("mediatek,mt8127")) {
		/* turn on GPT6 which ungates arch timer clocks */
		gpt_base = ioremap(GPT6_CON_MT65xx, 0x04);

		/* enable clock and set to free-run */
		writel(GPT_ENABLE, gpt_base);
		iounmap(gpt_base);
	}

	of_clk_init(NULL);
	timer_probe();
};

/*
 * arm_dt_init_cpu_maps() selects the CPU enable-method from DT before
 * setup_arch() initializes PSCI. setup_arch() normally replaces those ops
 * with generic PSCI whenever PSCI CPU_ON exists. MT8163 still needs its
 * mt-boot wrapper for the per-core MTCMOS/reset release after CPU_ON.
 */
static bool __init mediatek_smp_init(void)
{
	return of_machine_is_compatible("mediatek,mt8163");
}

static const char * const mediatek_board_dt_compat[] = {
	"mediatek,mt2701",
	"mediatek,mt8163",
	"mediatek,mt6589",
	"mediatek,mt6592",
	"mediatek,mt7623",
	"mediatek,mt7629",
	"mediatek,mt8127",
	"mediatek,mt8135",
	NULL,
};

DT_MACHINE_START(MEDIATEK_DT, "Mediatek Cortex-A7 (Device Tree)")
	.dt_compat	= mediatek_board_dt_compat,
	.init_time	= mediatek_timer_init,
	.smp_init	= mediatek_smp_init,
MACHINE_END
