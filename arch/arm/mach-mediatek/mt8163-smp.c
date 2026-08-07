// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT8163 secondary-CPU bring-up.
 *
 * The firmware PSCI CPU_ON call releases the secure-world boot path, but the
 * per-core MTCMOS island is still owned by the application processor.  Mirror
 * the vendor 3.18 ordering with bounded polls so a broken rail cannot hang the
 * boot CPU forever.
 */
#ifdef CONFIG_SMP

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/psci.h>
#include <linux/regmap.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/memory.h>
#include <asm/smp_plat.h>

#define SPM_POWERON_CONFIG_SET	0x0000
#define SPM_CA7_CPU1_PWR_CON	0x0218
#define SPM_CA7_CPU2_PWR_CON	0x021c
#define SPM_CA7_CPU3_PWR_CON	0x0220
#define SPM_CA7_CPU1_L1_PDN	0x0264
#define SPM_CA7_CPU2_L1_PDN	0x026c
#define SPM_CA7_CPU3_L1_PDN	0x0274
#define SPM_PWR_STATUS		0x060c
#define SPM_PWR_STATUS_2ND	0x0610

#define SPM_PROJECT_CODE	0x0b16
#define SPM_REGWR_EN		BIT(0)
#define SRAM_ISOINT_B		BIT(6)
#define SRAM_CKISO		BIT(5)
#define PWR_CLK_DIS		BIT(4)
#define PWR_ON_2ND		BIT(3)
#define PWR_ON			BIT(2)
#define PWR_ISO			BIT(1)
#define PWR_RST_B		BIT(0)
#define L1_PDN_ACK		BIT(8)
#define L1_PDN			BIT(0)
#define MT8163_MTCMOS_TIMEOUT_US	100000

static const unsigned int mt8163_pwr_con[] = {
	[1] = SPM_CA7_CPU1_PWR_CON,
	[2] = SPM_CA7_CPU2_PWR_CON,
	[3] = SPM_CA7_CPU3_PWR_CON,
};

static const unsigned int mt8163_l1_pdn[] = {
	[1] = SPM_CA7_CPU1_L1_PDN,
	[2] = SPM_CA7_CPU2_L1_PDN,
	[3] = SPM_CA7_CPU3_L1_PDN,
};

static const unsigned int mt8163_pwr_status[] = {
	[1] = BIT(10),
	[2] = BIT(11),
	[3] = BIT(12),
};

static struct regmap *mt8163_spm;
static DEFINE_SPINLOCK(mt8163_mtcmos_lock);

static int mt8163_mtcmos_power_on(unsigned int cpu)
{
	unsigned long flags;
	unsigned int status;
	unsigned int mask;
	int ret;

	if (!mt8163_spm)
		return -ENODEV;
	if (cpu < 1 || cpu > 3)
		return -EINVAL;

	mask = mt8163_pwr_status[cpu];
	spin_lock_irqsave(&mt8163_mtcmos_lock, flags);

	ret = regmap_write(mt8163_spm, SPM_POWERON_CONFIG_SET,
			   (SPM_PROJECT_CODE << 16) | SPM_REGWR_EN);
	if (ret)
		goto out;

	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 PWR_ON, PWR_ON);
	if (ret)
		goto out;
	udelay(1);
	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 PWR_ON_2ND, PWR_ON_2ND);
	if (ret)
		goto out;

	ret = regmap_read_poll_timeout_atomic(mt8163_spm, SPM_PWR_STATUS,
					      status, status & mask, 1,
					      MT8163_MTCMOS_TIMEOUT_US);
	if (ret)
		goto out;
	ret = regmap_read_poll_timeout_atomic(mt8163_spm, SPM_PWR_STATUS_2ND,
					      status, status & mask, 1,
					      MT8163_MTCMOS_TIMEOUT_US);
	if (ret)
		goto out;

	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 PWR_ISO, 0);
	if (ret)
		goto out;
	ret = regmap_update_bits(mt8163_spm, mt8163_l1_pdn[cpu], L1_PDN, 0);
	if (ret)
		goto out;
	ret = regmap_read_poll_timeout_atomic(mt8163_spm, mt8163_l1_pdn[cpu],
					      status, !(status & L1_PDN_ACK),
					      1, MT8163_MTCMOS_TIMEOUT_US);
	if (ret)
		goto out;

	udelay(1);
	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 SRAM_ISOINT_B, SRAM_ISOINT_B);
	if (ret)
		goto out;
	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 SRAM_CKISO, 0);
	if (ret)
		goto out;
	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 PWR_CLK_DIS, 0);
	if (ret)
		goto out;
	ret = regmap_update_bits(mt8163_spm, mt8163_pwr_con[cpu],
				 PWR_RST_B, PWR_RST_B);

out:
	spin_unlock_irqrestore(&mt8163_mtcmos_lock, flags);
	if (ret)
		pr_err("MT8163: CPU%u MTCMOS power-on failed: %d\n", cpu, ret);
	return ret;
}

static void __init mt8163_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-scpsys");
	if (!node) {
		pr_err("MT8163: SPM syscon node is missing\n");
		return;
	}

	mt8163_spm = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(mt8163_spm)) {
		pr_err("MT8163: cannot map SPM syscon: %ld\n",
		       PTR_ERR(mt8163_spm));
		mt8163_spm = NULL;
		return;
	}

	pr_info("MT8163: mt-boot PSCI/MTCMOS method active\n");
}

extern void secondary_startup(void);

static int mt8163_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	int ret;

	if (!mt8163_spm)
		return -ENODEV;
	if (cpu < 1 || cpu > 3)
		return -EINVAL;
	if (!psci_ops.cpu_on)
		return -ENODEV;

	ret = psci_ops.cpu_on(cpu_logical_map(cpu),
			      virt_to_idmap(&secondary_startup));
	if (ret) {
		pr_err("MT8163: PSCI failed to boot CPU%u: %d\n", cpu, ret);
		return ret;
	}

	ret = mt8163_mtcmos_power_on(cpu);
	if (ret)
		return ret;

	pr_info("MT8163: CPU%u PSCI/MTCMOS release complete\n", cpu);
	return 0;
}

static const struct smp_operations mt8163_smp_ops __initconst = {
	.smp_prepare_cpus = mt8163_smp_prepare_cpus,
	.smp_boot_secondary = mt8163_boot_secondary,
};
CPU_METHOD_OF_DECLARE(mt8163_smp, "mt-boot", &mt8163_smp_ops);

#endif /* CONFIG_SMP */
