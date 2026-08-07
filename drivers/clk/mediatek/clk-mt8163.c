/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/mfd/syscon.h>

#include "clk-mtk.h"
#include "clk-gate.h"
#include "clk-mux.h"
#include "clk-pll.h"

#include <dt-bindings/clock/mt8163-clk.h>

static DEFINE_SPINLOCK(mt8163_clk_lock);

static const struct mtk_fixed_factor root_clks[] __initconst = {
	FACTOR(CLK_TOP_HDMITX_DIG_CTS, "hdmitx_dig_cts", "clk_null", 1, 1),
};

static const struct mtk_fixed_factor top_divs[] __initconst = {
	FACTOR(CLK_TOP_MAIN_H546M, "main_h546m", "mainpll", 1, 2),
	FACTOR(CLK_TOP_MAIN_H364M, "main_h364m", "mainpll", 1, 3),
	FACTOR(CLK_TOP_MAIN_H218P4M, "main_h218p4m", "mainpll", 1, 5),
	FACTOR(CLK_TOP_MAIN_H156M, "main_h156m", "mainpll", 1, 7),

	FACTOR(CLK_TOP_UNIV_624M, "univ_624m", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIV_416M, "univ_416m", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIV_249P6M, "univ_249p6m", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIV_178P3M, "univ_178p3m", "univpll", 1, 7),
	FACTOR(CLK_TOP_UNIV_48M, "univ_48m", "univpll", 1, 26),

	FACTOR(CLK_TOP_APLL1, "apll1_ck", "aud1pll", 1, 1),
	FACTOR(CLK_TOP_APLL2, "apll2_ck", "aud2pll", 1, 1),

	FACTOR(CLK_TOP_CLKRTC_EXT, "clkrtc_ext", "clk32k", 1, 1),
	FACTOR(CLK_TOP_CLKRTC_INT, "clkrtc_int", "clk26m", 1, 793),

	FACTOR(CLK_TOP_DMPLL, "dmpll_ck", "clk_null", 1, 1),
	FACTOR(CLK_TOP_F26M_MEM_CKGEN_OCC, "f26m_mem_ckgen", "clk_null", 1, 1),

	FACTOR(CLK_TOP_HDMI_CTS, "hdmi_cts_ck", "hdmitx_dig_cts", 1, 2),
	FACTOR(CLK_TOP_HDMI_CTS_D2, "hdmi_cts_d2", "hdmitx_dig_cts", 1, 3),
	FACTOR(CLK_TOP_HDMI_CTS_D3, "hdmi_cts_d3", "hdmitx_dig_cts", 1, 2),

	FACTOR(CLK_TOP_LVDSPLL, "lvdspll_ck", "lvdspll", 1, 1),
	FACTOR(CLK_TOP_LVDSPLL_D2, "lvdspll_d2", "lvdspll", 1, 2),
	FACTOR(CLK_TOP_LVDSPLL_D4, "lvdspll_d4", "lvdspll", 1, 4),
	FACTOR(CLK_TOP_LVDSPLL_D8, "lvdspll_d8", "lvdspll", 1, 8),
	FACTOR(CLK_TOP_LVDSPLL_ETH, "lvdspll_eth_ck", "lvdspll", 1, 4),

	FACTOR(CLK_TOP_MMPLL, "mmpll_ck", "mmpll", 1, 1),

	FACTOR(CLK_TOP_MPLL_208M, "mpll_208m_ck", "mpll", 1, 1),

	FACTOR(CLK_TOP_MSDCPLL, "msdcpll_ck", "msdcpll", 1, 1),
	FACTOR(CLK_TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1, 2),
	FACTOR(CLK_TOP_MSDCPLL_D4, "msdcpll_d4", "msdcpll", 1, 4),

	FACTOR(CLK_TOP_SYSPLL_D2, "syspll_d2", "main_h546m", 1, 1),
	FACTOR(CLK_TOP_SYSPLL_D2P5, "syspll_d2p5", "main_h218p4m", 2, 1),
	FACTOR(CLK_TOP_SYSPLL_D3, "syspll_d3", "main_h364m", 1, 1),
	FACTOR(CLK_TOP_SYSPLL_D5, "syspll_d5", "main_h218p4m", 1, 1),
	FACTOR(CLK_TOP_SYSPLL_D7, "syspll_d7", "main_h156m", 1, 1),
	FACTOR(CLK_TOP_SYSPLL1_D2, "syspll1_d2", "main_h546m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D4, "syspll1_d4", "main_h546m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL1_D8, "syspll1_d8", "main_h546m", 1, 8),
	FACTOR(CLK_TOP_SYSPLL1_D16, "syspll1_d16", "main_h546m", 1, 16),
	FACTOR(CLK_TOP_SYSPLL2_D2, "syspll2_d2", "main_h364m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL2_D4, "syspll2_d4", "main_h364m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL2_D8, "syspll2_d8", "main_h364m", 1, 8),
	FACTOR(CLK_TOP_SYSPLL3_D2, "syspll3_d2", "main_h218p4m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL3_D4, "syspll3_d4", "main_h218p4m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL4_D2, "syspll4_d2", "main_h156m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL4_D4, "syspll4_d4", "main_h156m", 1, 4),

	FACTOR(CLK_TOP_TVDPLL_D2, "tvdpll_d2", "tvdpll", 1, 2),
	FACTOR(CLK_TOP_TVDPLL_D4, "tvdpll_d4", "tvdpll", 1, 4),
	FACTOR(CLK_TOP_TVDPLL_D8, "tvdpll_d8", "tvdpll", 1, 8),
	FACTOR(CLK_TOP_TVDPLL_D16, "tvdpll_d16", "tvdpll", 1, 16),

	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univ_624m", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D2P5, "univpll_d2p5", "univ_249p6m", 2, 1),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univ_416m", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univ_249p6m", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D7, "univpll_d7", "univ_178p3m", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D26, "univpll_d26", "univ_48m", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univ_624m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univ_624m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univ_416m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univ_416m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univ_416m", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL3_D2, "univpll3_d2", "univ_249p6m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL3_D4, "univpll3_d4", "univ_249p6m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL3_D8, "univpll3_d8", "univ_249p6m", 1, 8),

	FACTOR(CLK_TOP_VENCPLL, "vencpll_ck", "vencpll", 1, 1),

	FACTOR(CLK_TOP_APLL2_DIV0, "apll2_div0", "aud_2_sel", 1, 1),
	FACTOR(CLK_TOP_APLL2_DIV1, "apll2_div1", "aud_2_sel", 1, 1),
};

static const char * const axi_parents[] __initconst = {
	"clk26m",
	"syspll1_d4"
};

static const char * const mem_parents[] __initconst = {
	"clk26m",
	"dmpll_ck",
	"f26m_mem_ckgen"
};

static const char * const ddrphycfg_parents[] __initconst = {
	"clk26m",
	"syspll1_d8"
};

static const char * const mm_parents[] __initconst = {
	"clk26m",
	"syspll_d3",
	"univpll_d3",
	"vencpll_ck"
};

static const char * const pwm_parents[] __initconst = {
	"clk26m",
	"univpll2_d2",
	"univpll2_d4"
};

static const char * const vdec_parents[] __initconst = {
	"clk26m",
	"syspll_d3",
	"syspll1_d2",
	"univpll1_d2",
	"syspll1_d4"
};

static const char * const mfg_parents[] __initconst = {
	"clk26m",
	"mmpll_ck",
	"univpll_d3",
	"syspll_d3"
};

static const char * const camtg_parents[] __initconst = {
	"clk26m",
	"univpll_d26",
	"univpll2_d2"
};

static const char * const uart_parents[] __initconst = {
	"clk26m",
	"univpll2_d8"
};

static const char * const spi_parents[] __initconst = {
	"clk26m",
	"syspll3_d2"
};

static const char * const msdc30_0_parents[] __initconst = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"univpll_d7"
};

static const char * const msdc30_1_parents[] __initconst = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"univpll_d7"
};

static const char * const msdc30_2_parents[] __initconst = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"univpll_d7"
};

static const char * const msdc50_3_h_parents[] __initconst = {
	"clk26m",
	"syspll1_d2",
	"syspll2_d2",
	"syspll4_d2"
};

static const char * const msdc50_3_parents[] __initconst = {
	"clk26m",
	"msdcpll_ck",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"syspll_d7",
	"msdcpll_d4",
	"univpll_d2",
	"univpll1_d2"
};

static const char * const audio_parents[] __initconst = {
	"clk26m",
	"syspll3_d4",
	"syspll4_d4",
	"syspll1_d16"
};

static const char * const aud_intbus_parents[] __initconst = {
	"clk26m",
	"syspll1_d4",
	"syspll4_d2"
};

static const char * const pmicspi_parents[] __initconst = {
	"clk26m",
	"univpll_d26"
};

static const char * const scp_parents[] __initconst = {
	"clk26m",
	"mpll_208m_ck",
	"syspll1_d4"
};

static const char * const atb_parents[] __initconst = {
	"clk26m",
	"syspll1_d2"
};

static const char * const mjc_parents[] __initconst = {
	"clk26m",
	"syspll_d5",
	"univpll_d5"
};

static const char * const dpi0_parents[] __initconst = {
	"clk26m",
	"lvdspll_ck",
	"lvdspll_d2",
	"lvdspll_d4",
	"lvdspll_d8"
};

static const char * const scam_parents[] __initconst = {
	"clk26m",
	"syspll3_d2"
};

static const char * const aud_1_parents[] __initconst = {
	"clk26m",
	"apll1_ck"
};

static const char * const aud_2_parents[] __initconst = {
	"clk26m",
	"apll2_ck"
};

static const char * const dpi1_parents[] __initconst = {
	"clk26m",
	"tvdpll_d2",
	"tvdpll_d4",
	"tvdpll_d8",
	"tvdpll_d16"
};

static const char * const ufoenc_parents[] __initconst = {
	"clk26m",
	"univpll_d2",
	"syspll_d2",
	"univpll_d2p5",
	"syspll_d2p5",
	"univpll_d3",
	"syspll_d3",
	"syspll1_d2"
};

static const char * const ufodec_parents[] __initconst = {
	"clk26m",
	"syspll_d2",
	"univpll_d2p5",
	"syspll_d2p5",
	"univpll_d3",
	"syspll_d3",
	"syspll1_d2"
};

static const char * const eth_parents[] __initconst = {
	"clk26m",
	"syspll3_d4",
	"univpll2_d8",
	"lvdspll_eth_ck",
	"univpll_d26",
	"syspll2_d8",
	"syspll4_d4",
	"univpll3_d8",
	"clk26m"
};

static const char * const onfi_parents[] __initconst = {
	"clk26m",
	"syspll2_d2",
	"syspll_d7",
	"univpll3_d2",
	"syspll2_d4",
	"univpll3_d4",
	"syspll4_d4"
};

static const char * const snfi_parents[] __initconst = {
	"clk26m",
	"univpll2_d8",
	"univpll3_d4",
	"syspll4_d2",
	"univpll2_d4",
	"univpll3_d2",
	"syspll1_d4",
	"univpll1_d4",
	"clk26m"
};

static const char * const hdmi_parents[] __initconst = {
	"clk26m",
	"hdmi_cts_ck",
	"hdmi_cts_d2",
	"hdmi_cts_d3"
};

static const char * const rtc_parents[] __initconst = {
	"clkrtc_int",
	"clkrtc_ext",
	"clk26m",
	"univpll3_d8"
};

static const struct mtk_mux_upd top_muxes[] __initconst = {
	/* CLK_CFG_0 */
	MUX_UPD(CLK_TOP_AXI_SEL, "axi_sel", axi_parents,
		0x0040, 0, 1, 7, 0x0004, 0),
	MUX_UPD(CLK_TOP_MEM_SEL, "mem_sel", mem_parents,
		0x0040, 8, 2, 15, 0x0004, 1),
	MUX_UPD(CLK_TOP_DDRPHYCFG_SEL, "ddrphycfg_sel", ddrphycfg_parents,
		0x0040, 16, 1, 23, 0x0004, 2),
	MUX_UPD(CLK_TOP_MM_SEL, "mm_sel", mm_parents,
		0x0040, 24, 2, 31, 0x0004, 3),
	/* CLK_CFG_1 */
	MUX_UPD(CLK_TOP_PWM_SEL, "pwm_sel", pwm_parents,
		0x0050, 0, 2, 7, 0x0004, 4),
	MUX_UPD(CLK_TOP_VDEC_SEL, "vdec_sel", vdec_parents,
		0x0050, 8, 3, 15, 0x0004, 5),
	MUX_UPD(CLK_TOP_MFG_SEL, "mfg_sel", mfg_parents,
		0x0050, 24, 2, 31, 0x0004, 7),
	/* CLK_CFG_2 */
	MUX_UPD(CLK_TOP_CAMTG_SEL, "camtg_sel", camtg_parents,
		0x0060, 0, 2, 7, 0x0004, 8),
	MUX_UPD(CLK_TOP_UART_SEL, "uart_sel", uart_parents,
		0x0060, 8, 1, 15, 0x0004, 9),
	MUX_UPD(CLK_TOP_SPI_SEL, "spi_sel", spi_parents,
		0x0060, 16, 1, 23, 0x0004, 10),
	/* CLK_CFG_3 */
	MUX_UPD(CLK_TOP_MSDC30_0_SEL, "msdc30_0_sel", msdc30_0_parents,
		0x0070, 0, 3, 7, 0x0004, 12),
	MUX_UPD(CLK_TOP_MSDC30_1_SEL, "msdc30_1_sel", msdc30_1_parents,
		0x0070, 8, 3, 15, 0x0004, 13),
	MUX_UPD(CLK_TOP_MSDC30_2_SEL, "msdc30_2_sel", msdc30_2_parents,
		0x0070, 16, 3, 23, 0x0004, 14),
	/* CLK_CFG_4 */
	MUX_UPD(CLK_TOP_MSDC50_3_HSEL, "msdc50_3_hsel", msdc50_3_h_parents,
		0x0080, 0, 2, 7, 0x0004, 15),
	MUX_UPD(CLK_TOP_MSDC50_3_SEL, "msdc50_3_sel", msdc50_3_parents,
		0x0080, 8, 4, 15, 0x0004, 16),
	MUX_UPD(CLK_TOP_AUDIO_SEL, "audio_sel", audio_parents,
		0x0080, 16, 2, 23, 0x0004, 17),
	MUX_UPD(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel", aud_intbus_parents,
		0x0080, 24, 2, 31, 0x0004, 18),
	/* CLK_CFG_5 */
	MUX_UPD(CLK_TOP_PMICSPI_SEL, "pmicspi_sel", pmicspi_parents,
		0x0090, 0, 1, 7, 0x0004, 19),
	MUX_UPD(CLK_TOP_SCP_SEL, "scp_sel", scp_parents,
		0x0090, 8, 2, 15, 0x0004, 20),
	MUX_UPD(CLK_TOP_ATB_SEL, "atb_sel", atb_parents,
		0x0090, 16, 1, 23, 0x0004, 21),
	MUX_UPD(CLK_TOP_MJC_SEL, "mjc_sel", mjc_parents,
		0x0090, 24, 2, 31, 0x0004, 22),
	/* CLK_CFG_6 */
	MUX_UPD(CLK_TOP_DPI0_SEL, "dpi0_sel", dpi0_parents,
		0x00a0, 0, 3, 7, 0x0004, 23),
	MUX_UPD(CLK_TOP_SCAM_SEL, "scam_sel", scam_parents,
		0x00a0, 8, 1, 15, 0x0004, 24),
	MUX_UPD(CLK_TOP_AUD_1_SEL, "aud_1_sel", aud_1_parents,
		0x00a0, 16, 1, 23, 0x0004, 25),
	MUX_UPD(CLK_TOP_AUD_2_SEL, "aud_2_sel", aud_2_parents,
		0x00a0, 24, 1, 31, 0x0004, 26),
	/* CLK_CFG_7 */
	MUX_UPD(CLK_TOP_DPI1_SEL, "dpi1_sel", dpi1_parents,
		0x00b0, 0, 3, 7, 0x0004, 6),
	MUX_UPD(CLK_TOP_UFOENC_SEL, "ufoenc_sel", ufoenc_parents,
		0x00b0, 8, 3, 15, 0x0004, 27),
	MUX_UPD(CLK_TOP_UFODEC_SEL, "ufodec_sel", ufodec_parents,
		0x00b0, 16, 3, 23, 0x0004, 28),
	MUX_UPD(CLK_TOP_ETH_SEL, "eth_sel", eth_parents,
		0x00b0, 24, 4, 31, 0x0004, 29),
	/* CLK_CFG_8 */
	MUX_UPD(CLK_TOP_ONFI_SEL, "onfi_sel", onfi_parents,
		0x00c0, 0, 3, 7, 0x0004, 30),
	MUX_UPD(CLK_TOP_SNFI_SEL, "snfi_sel", snfi_parents,
		0x00c0, 8, 2, 15, 0x0004, 31),
	MUX_UPD(CLK_TOP_HDMI_SEL, "hdmi_sel", hdmi_parents,
		0x00c0, 16, 2, 23, 0x0004, 11),
	MUX_UPD(CLK_TOP_RTC_SEL, "rtc_sel", rtc_parents,
		0x00c0, 24, 2, 31, 0x0008, 0),
};

static const struct mtk_gate_regs infra0_cg_regs = {
	.set_ofs = 0x0080,
	.clr_ofs = 0x0084,
	.sta_ofs = 0x0090,
};

static const struct mtk_gate_regs infra1_cg_regs = {
	.set_ofs = 0x0088,
	.clr_ofs = 0x008c,
	.sta_ofs = 0x0094,
};

#define GATE_ICG0(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &infra0_cg_regs,			\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

#define GATE_ICG1(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &infra1_cg_regs,			\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

static const struct mtk_gate infra_clks[] __initconst = {
	/* INFRA0 */
	GATE_ICG0(CLK_INFRA_PMIC_TMR, "infra_pmic_tmr", "clk26m", 0),
	GATE_ICG0(CLK_INFRA_PMIC_AP, "infra_pmic_ap", "clk26m", 1),
	GATE_ICG0(CLK_INFRA_PMIC_MD, "infra_pmic_md", "clk26m", 2),
	GATE_ICG0(CLK_INFRA_PMIC_CONN, "infra_pmic_conn", "clk26m", 3),
	GATE_ICG0(CLK_INFRA_SCPSYS, "infra_scpsys", "scp_sel", 4),
	GATE_ICG0(CLK_INFRA_SEJ, "infra_sej", "axi_sel", 5),
	GATE_ICG0(CLK_INFRA_APXGPT, "infra_apxgpt", "axi_sel", 6),
	GATE_ICG0(CLK_INFRA_USB, "infra_usb", "axi_sel", 7),
	GATE_ICG0(CLK_INFRA_ICUSB, "infra_icusb", "axi_sel", 8),
	GATE_ICG0(CLK_INFRA_GCE, "infra_gce", "axi_sel", 9),
	GATE_ICG0(CLK_INFRA_THERM, "infra_therm", "axi_sel", 10),
	GATE_ICG0(CLK_INFRA_I2C0, "infra_i2c0", "axi_sel", 11),
	GATE_ICG0(CLK_INFRA_I2C1, "infra_i2c1", "axi_sel", 12),
	GATE_ICG0(CLK_INFRA_I2C2, "infra_i2c2", "axi_sel", 13),
	GATE_ICG0(CLK_INFRA_PWM_HCLK, "infra_pwm_hclk", "axi_sel", 15),
	GATE_ICG0(CLK_INFRA_PWM1, "infra_pwm1", "axi_sel", 16),
	GATE_ICG0(CLK_INFRA_PWM2, "infra_pwm2", "axi_sel", 17),
	GATE_ICG0(CLK_INFRA_PWM3, "infra_pwm3", "axi_sel", 18),
	GATE_ICG0(CLK_INFRA_PWM, "infra_pwm", "axi_sel", 21),
	GATE_ICG0(CLK_INFRA_UART0, "infra_uart0", "uart_sel", 22),
	GATE_ICG0(CLK_INFRA_UART1, "infra_uart1", "uart_sel", 23),
	GATE_ICG0(CLK_INFRA_UART2, "infra_uart2", "uart_sel", 24),
	GATE_ICG0(CLK_INFRA_UART3, "infra_uart3", "uart_sel", 25),
	GATE_ICG0(CLK_INFRA_USB_MCU, "infra_usb_mcu", "axi_sel", 26),
	GATE_ICG0(CLK_INFRA_NFI_ECC_66M, "infra_nfi_ecc66", "axi_sel", 27),
	GATE_ICG0(CLK_INFRA_NFI_66M, "infra_nfi_66m", "axi_sel", 28),
	GATE_ICG0(CLK_INFRA_BTIF, "infra_btif", "axi_sel", 31),
	/* INFRA1 */
	GATE_ICG1(CLK_INFRA_SPI, "infra_spi", "spi_sel", 1),
	GATE_ICG1(CLK_INFRA_MSDC3, "infra_msdc3", "msdc50_3_sel", 2),
	GATE_ICG1(CLK_INFRA_MSDC1, "infra_msdc1", "msdc30_1_sel", 4),
	GATE_ICG1(CLK_INFRA_MSDC2, "infra_msdc2", "msdc30_2_sel", 5),
	GATE_ICG1(CLK_INFRA_MSDC0, "infra_msdc0", "msdc30_0_sel", 6),
	GATE_ICG1(CLK_INFRA_GCPU, "infra_gcpu", "axi_sel", 8),
	GATE_ICG1(CLK_INFRA_AUXADC, "infra_auxadc", "clk26m", 10),
	GATE_ICG1(CLK_INFRA_CPUM, "infra_cpum", "axi_sel", 11),
	GATE_ICG1(CLK_INFRA_IRRX, "infra_irrx", "rtc_sel", 12),
	GATE_ICG1(CLK_INFRA_UFO, "infra_ufo", "axi_sel", 13),
	GATE_ICG1(CLK_INFRA_CEC, "infra_cec", "rtc_sel", 14),
	GATE_ICG1(CLK_INFRA_CEC_26M, "infra_cec_26m", "clk26m", 15),
	GATE_ICG1(CLK_INFRA_NFI_BCLK, "infra_nfi_bclk", "spi_sel", 16),
	GATE_ICG1(CLK_INFRA_NFI_ECC, "infra_nfi_ecc", "spi_sel", 17),
	GATE_ICG1(CLK_INFRA_AP_DMA, "infra_ap_dma", "axi_sel", 18),
	GATE_ICG1(CLK_INFRA_XIU, "infra_xiu", "axi_sel", 19),
	GATE_ICG1(CLK_INFRA_DEVICE_APC, "infra_devapc", "axi_sel", 20),
	GATE_ICG1(CLK_INFRA_XIU2AHB, "infra_xiu2ahb", "axi_sel", 21),
	GATE_ICG1(CLK_INFRA_L2C_SRAM, "infra_l2c_sram", "axi_sel", 22),
	GATE_ICG1(CLK_INFRA_ETH_50M, "infra_eth_50m", "eth_sel", 23),
	GATE_ICG1(CLK_INFRA_DEBUGSYS, "infra_debugsys", "axi_sel", 24),
	GATE_ICG1(CLK_INFRA_AUDIO, "infra_audio", "axi_sel", 25),
	GATE_ICG1(CLK_INFRA_ETH_25M, "infra_eth_25m", "eth_sel", 26),
	GATE_ICG1(CLK_INFRA_NFI, "infra_nfi", "axi_sel", 27),
	GATE_ICG1(CLK_INFRA_ONFI, "infra_onfi", "onfi_sel", 28),
	GATE_ICG1(CLK_INFRA_SNFI, "infra_snfi", "snfi_sel", 29),
	GATE_ICG1(CLK_INFRA_ETH, "infra_eth", "axi_sel", 30),
	GATE_ICG1(CLK_INFRA_DRAMC_F26M, "infra_dramc_26m", "clk26m", 31),
};

static const struct mtk_fixed_factor infra_divs[] __initconst = {
	FACTOR(CLK_INFRA_OSC, "osc_ck", "clk26m", 8, 1),
	FACTOR(CLK_INFRA_OSC_D8, "osc_d8", "osc_ck", 1, 8),
	FACTOR(CLK_INFRA_OSC_D16, "osc_d16", "osc_ck", 1, 16),
	FACTOR(CLK_INFRA_CLK26M_D8, "clk26m_d8", "clk26m", 1, 8),
	FACTOR(CLK_INFRA_ETH_D2, "eth_d2", "eth_sel", 1, 2),
	FACTOR(CLK_INFRA_ONFI_D2, "onfi_d2", "onfi_sel", 1, 2),
	FACTOR(CLK_INFRA_CLK_13M, "clk13m", "clk26m", 1, 2),
};

static const char * const infra_uart_parents[] __initconst = {
	"clk26m",
	"uart_sel"
};

static const char * const infra_spi_parents[] __initconst = {
	"axi_sel",
	"spi_sel"
};

static const char * const infra_dramc_parents[] __initconst = {
	"clk26m",
	"clk26m_d8"
};

static const char * const infra_ulp_parents[] __initconst = {
	"osc_d8",
	"osc_d16"
};

static const char * const infra_eth_parents[] __initconst = {
	"eth_d2",
	"eth_sel"
};

static const char * const infra_nfi_parents[] __initconst = {
	"axi_sel",
	"onfi_d2"
};

static const struct mtk_composite infra_muxes[] __initconst = {
	MUX(CLK_INFRA_UART0_SEL, "infra_uart0_sel", infra_uart_parents, 0x098, 0, 1),
	MUX(CLK_INFRA_UART1_SEL, "infra_uart1_sel", infra_uart_parents, 0x098, 1, 1),
	MUX(CLK_INFRA_UART2_SEL, "infra_uart2_sel", infra_uart_parents, 0x098, 2, 1),
	MUX(CLK_INFRA_UART3_SEL, "infra_uart3_sel", infra_uart_parents, 0x098, 3, 1),
	MUX(CLK_INFRA_SPI_SEL, "infra_spi_sel", infra_spi_parents, 0x098, 4, 1),
	MUX(CLK_INFRA_DRAMC_SEL, "infra_dramc_sel", infra_dramc_parents, 0x098, 7, 1),
	MUX(CLK_INFRA_ULPOSC_SEL, "infra_ulp_sel", infra_ulp_parents, 0x098, 8, 1),
	MUX(CLK_INFRA_ETH_25M_SEL, "infra_eth_sel", infra_eth_parents, 0x098, 9, 1),
	MUX(CLK_INFRA_NFI_SEL, "infra_nfi_sel", infra_nfi_parents, 0x098, 10, 1),
};

static struct mtk_gate_regs mfg_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_MFG(_id, _name, _parent, _shift) {			\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &mfg_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

static struct mtk_gate mfg_clks[] __initdata = {
	GATE_MFG(CLK_MFG_FF, "mfg_ff", "mfg_sel", 0),
	GATE_MFG(CLK_MFG_DW, "mfg_dw", "mfg_sel", 1),
	GATE_MFG(CLK_MFG_TX, "mfg_tx", "mfg_sel", 2),
	GATE_MFG(CLK_MFG_MX, "mfg_mx", "mfg_sel", 3),
};

static struct mtk_gate_regs img_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_IMG(_id, _name, _parent, _shift) {			\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &img_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

static struct mtk_gate img_clks[] __initdata = {
	GATE_IMG(CLK_IMG_LARB2_SMI, "img_larb2_smi", "mm_sel", 0),
	GATE_IMG(CLK_IMG_JPGENC, "img_jpgenc", "mm_sel", 4),
	GATE_IMG(CLK_IMG_CAM_SMI, "img_cam_smi", "mm_sel", 5),
	GATE_IMG(CLK_IMG_CAM_CAM, "img_cam_cam", "mm_sel", 6),
	GATE_IMG(CLK_IMG_SEN_TG, "img_sen_tg", "camtg_sel", 7),
	GATE_IMG(CLK_IMG_SEN_CAM, "img_sen_cam", "mm_sel", 8),
	GATE_IMG(CLK_IMG_CAM_SV, "img_cam_sv", "mm_sel", 9),
};

static struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x0104,
	.clr_ofs = 0x0108,
	.sta_ofs = 0x0100,
};

static struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x0114,
	.clr_ofs = 0x0118,
	.sta_ofs = 0x0110,
};

#define GATE_MM0(_id, _name, _parent, _shift) {			\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &mm0_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

#define GATE_MM1(_id, _name, _parent, _shift) {			\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &mm1_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr,		\
	}

static struct mtk_gate mm_clks[] __initdata = {
	/* MM0 */
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_comoon", "mm_sel", 0),
	GATE_MM0(CLK_MM_SMI_LARB0, "mm_smi_larb0", "mm_sel", 1),
	GATE_MM0(CLK_MM_CAM_MDP, "mm_cam_mdp", "mm_sel", 2),
	GATE_MM0(CLK_MM_MDP_RDMA, "mm_mdp_rdma", "mm_sel", 3),
	GATE_MM0(CLK_MM_MDP_RSZ0, "mm_mdp_rsz0", "mm_sel", 4),
	GATE_MM0(CLK_MM_MDP_RSZ1, "mm_mdp_rsz1", "mm_sel", 5),
	GATE_MM0(CLK_MM_MDP_TDSHP, "mm_mdp_tdshp", "mm_sel", 6),
	GATE_MM0(CLK_MM_MDP_WDMA, "mm_mdp_wdma", "mm_sel", 7),
	GATE_MM0(CLK_MM_MDP_WROT, "mm_mdp_wrot", "mm_sel", 8),
	GATE_MM0(CLK_MM_FAKE_ENG, "mm_fake_eng", "mm_sel", 9),
	GATE_MM0(CLK_MM_DISP_OVL0, "mm_disp_ovl0", "mm_sel", 10),
	GATE_MM0(CLK_MM_DISP_OVL1, "mm_disp_ovl1", "mm_sel", 11),
	GATE_MM0(CLK_MM_DISP_RDMA0, "mm_disp_rdma0", "mm_sel", 12),
	GATE_MM0(CLK_MM_DISP_RDMA1, "mm_disp_rdma1", "mm_sel", 13),
	GATE_MM0(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", "rtc_sel", 14),
	GATE_MM0(CLK_MM_DISP_COLOR, "mm_disp_color", "mm_sel", 15),
	GATE_MM0(CLK_MM_DISP_CCORR, "mm_disp_ccorr", "mm_sel", 16),
	GATE_MM0(CLK_MM_DISP_AAL, "mm_disp_aal", "mm_sel", 17),
	GATE_MM0(CLK_MM_DISP_GAMMA, "mm_disp_gamma", "mm_sel", 18),
	GATE_MM0(CLK_MM_DISP_DITHER, "mm_disp_dither", "mm_sel", 19),
	GATE_MM0(CLK_MM_DISP_UFOE, "mm_disp_ufoe", "mm_sel", 20),
	GATE_MM0(CLK_MM_LARB4_AXI_ASIF_MM, "mm_larb4_mm", "mm_sel", 21),
	GATE_MM0(CLK_MM_LARB4_AXI_ASIF_MJC, "mm_larb4_mjc", "mm_sel", 22),
	GATE_MM0(CLK_MM_DISP_WDMA1, "mm_disp_wdma1", "mm_sel", 23),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L0, "mm_ufodrdma0_l0", "mm_sel", 24),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L1, "mm_ufodrdma0_l1", "mm_sel", 25),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L2, "mm_ufodrdma0_l2", "mm_sel", 26),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L3, "mm_ufodrdma0_l3", "mm_sel", 27),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L0, "mm_ufodrdma1_l0", "mm_sel", 28),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L1, "mm_ufodrdma1_l1", "mm_sel", 29),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L2, "mm_ufodrdma1_l2", "mm_sel", 30),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L3, "mm_ufodrdma1_l3", "mm_sel", 31),
	/* MM1 */
	GATE_MM1(CLK_MM_DISP_PWM_MM, "mm_disp_pwm0mm", "mm_sel", 0),
	GATE_MM1(CLK_MM_DISP_PWM_26M, "mm_disp_pwm026m", "clk26m", 1),
	GATE_MM1(CLK_MM_DSI_ENGINE, "mm_dsi0_engine", "mm_sel", 2),
	GATE_MM1(CLK_MM_DSI_DIGITAL, "mm_dsi0_digital", "clk_null", 3),
	GATE_MM1(CLK_MM_DPI0_PIXEL, "mm_dpi_pixel", "mm_sel", 4),
	GATE_MM1(CLK_MM_DPI0_ENGINE, "mm_dpi_engine", "dpi0_sel", 5),
	GATE_MM1(CLK_MM_LVDS_PIXEL, "mm_lvds_pixel", "dpi0_sel", 6),
	GATE_MM1(CLK_MM_LVDS_CTS, "mm_lvds_cts", "clk_null", 7),
	GATE_MM1(CLK_MM_DPI1_PIXEL, "mm_dpi1_pixel", "mm_sel", 8),
	GATE_MM1(CLK_MM_DPI1_ENGINE, "mm_dpi1_engine", "dpi1_sel", 9),
	GATE_MM1(CLK_MM_HDMI_PIXEL, "mm_hdmi_pixel", "dpi1_sel", 10),
	GATE_MM1(CLK_MM_HDMI_SPDIF, "mm_hdmi_spdif", "apll2_div1", 11),
	GATE_MM1(CLK_MM_HDMI_ADSP, "mm_hdmi_audio", "apll2_div0", 12),
	GATE_MM1(CLK_MM_HDMI_PLLCK, "mm_hdmi_pllck", "hdmi_sel", 13),
	GATE_MM1(CLK_MM_DISP_DSC_ENGINE, "mm_disp_dsc_eng", "mm_sel", 14),
	GATE_MM1(CLK_MM_DISP_DSC_MEM, "mm_disp_dsc_mem", "mm_sel", 15),
};

static struct mtk_gate_regs vdec0_cg_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0004,
	.sta_ofs = 0x0000,
};

static struct mtk_gate_regs vdec1_cg_regs = {
	.set_ofs = 0x0008,
	.clr_ofs = 0x000c,
	.sta_ofs = 0x0008,
};

#define GATE_VDEC0(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &vdec0_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr_inv,		\
	}

#define GATE_VDEC1(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &vdec1_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr_inv,		\
	}

static struct mtk_gate vdec_clks[] __initdata = {
	GATE_VDEC0(CLK_VDEC_CKEN, "vdec_cken", "vdec_sel", 0),
	GATE_VDEC1(CLK_VDEC_LARB_CKEN, "vdec_larb_cken", "mm_sel", 0),
};

static struct mtk_gate_regs venc_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_VENC(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &venc_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr_inv,		\
	}

static struct mtk_gate venc_clks[] __initdata = {
	GATE_VENC(CLK_VENC_CKE0, "venc_cke0", "mm_sel", 0),
	GATE_VENC(CLK_VENC_CKE1, "venc_cke1", "mm_sel", 4),
	GATE_VENC(CLK_VENC_CKE2, "venc_cke2", "mm_sel", 8),
	GATE_VENC(CLK_VENC_CKE3, "venc_cke3", "mm_sel", 12),
};

/* Audio gates use the common 6.1 direct-register gate operations. */
static struct mtk_gate_regs audio_cg_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0000,
	.sta_ofs = 0x0000,
};

#define GATE_AUD(_id, _name, _parent, _shift) {			\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &audio_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_no_setclr,			\
	}

#define GATE_AUD_I(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &audio_cg_regs,				\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,			\
	}

static struct mtk_gate audio_clks[] __initdata = {
	GATE_AUD(CLK_AUDIO_AFE, "aud_afe", "audio_sel", 2),
	GATE_AUD(CLK_AUDIO_I2S, "aud_i2s", "clk_null", 6),
	GATE_AUD(CLK_AUDIO_22M, "aud_22m", "aud_1_sel", 8),
	GATE_AUD(CLK_AUDIO_24M, "aud_24m", "aud_2_sel", 9),
	GATE_AUD(CLK_AUDIO_SPDF2, "aud_spdf2", "clk_null", 11),
	GATE_AUD(CLK_AUDIO_APLL2_TUNER, "aud_apll2_tnr", "aud_2_sel", 18),
	GATE_AUD(CLK_AUDIO_APLL_TUNER, "aud_apll_tnr", "aud_1_sel", 19),
	GATE_AUD(CLK_AUDIO_HDMI, "aud_hdmi", "clk_null", 20),
	GATE_AUD(CLK_AUDIO_SPDF, "aud_spdf", "clk_null", 21),
	GATE_AUD(CLK_AUDIO_ADC, "aud_adc", "audio_sel", 24),
	GATE_AUD(CLK_AUDIO_DAC, "aud_dac", "audio_sel", 25),
	GATE_AUD(CLK_AUDIO_DAC_PREDIS, "aud_dac_predis", "audio_sel", 26),
	GATE_AUD(CLK_AUDIO_TML, "aud_tml", "audio_sel", 27),
	GATE_AUD_I(CLK_AUDIO_IDLE_EN_EXT, "aud_ahb_idl_ex", "axi_sel", 29),
	GATE_AUD_I(CLK_AUDIO_IDLE_EN_INT, "aud_ahb_idl_in", "axi_sel", 30),
};

static bool timer_ready;
static struct clk_hw_onecell_data *top_data;
static struct clk_hw_onecell_data *pll_data;

static void mtk_clk_enable_critical(void)
{
	/* Critical clock registration is handled by the 6.1 clock framework. */
}

static void __init mtk_topckgen_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	void __iomem *base;
	int r;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%s(): ioremap failed\n", __func__);
		return;
	}

	top_data = clk_data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);

	mtk_clk_register_factors(root_clks, ARRAY_SIZE(root_clks), clk_data);
	mtk_clk_register_factors(top_divs, ARRAY_SIZE(top_divs), clk_data);
	mtk_clk_register_mux_upds(top_muxes, ARRAY_SIZE(top_muxes), base,
			&mt8163_clk_lock, clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);

	mtk_clk_enable_critical();
}
CLK_OF_DECLARE(mtk_topckgen, "mediatek,mt8163-topckgen", mtk_topckgen_init);

static u16 mt8163_rst_bank_ofs[] = { 0x30, 0x34 };
static const struct mtk_clk_rst_desc mt8163_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = mt8163_rst_bank_ofs,
	.rst_bank_nr = ARRAY_SIZE(mt8163_rst_bank_ofs),
};

static void __init mtk_infrasys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	void __iomem *base;
	int r;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%s(): ioremap failed\n", __func__);
		return;
	}

	clk_data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);

	mtk_clk_register_gates(NULL, node, infra_clks, ARRAY_SIZE(infra_clks),
						clk_data);
	mtk_clk_register_factors(infra_divs, ARRAY_SIZE(infra_divs), clk_data);
	mtk_clk_register_composites(NULL, infra_muxes, ARRAY_SIZE(infra_muxes), base,
			&mt8163_clk_lock, clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);

	mtk_register_reset_controller(node, &mt8163_rst_desc);
}
CLK_OF_DECLARE(mtk_infrasys, "mediatek,mt8163-infracfg", mtk_infrasys_init);

struct mtk_clk_usb {
	int id;
	const char *name;
	const char *parent;
	u32 reg_ofs;
};

#define APMIXED_USB(_id, _name, _parent, _reg_ofs) {			\
		.id = _id,						\
		.name = _name,						\
		.parent = _parent,					\
		.reg_ofs = _reg_ofs,					\
	}

static const struct mtk_clk_usb apmixed_usb[] __initconst = {
	APMIXED_USB(CLK_APMIXED_REF2USB_TX, "ref2usb_tx", "clk26m", 0x8),
};

#define MT8163_PLL_FMAX		(2500UL * MHZ)

#define CON0_MT8163_RST_BAR	BIT(24)

#define PLL_B(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
			_pd_reg, _pd_shift, _tuner_reg, _pcw_reg,	\
			_pcw_shift, _div_table) {			\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT8163_RST_BAR,			\
		.fmax = MT8163_PLL_FMAX,				\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.tuner_reg = _tuner_reg,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.div_table = _div_table,				\
	}

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
			_pd_reg, _pd_shift, _tuner_reg, _pcw_reg,	\
			_pcw_shift)					\
		PLL_B(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits, \
			_pd_reg, _pd_shift, _tuner_reg, _pcw_reg, _pcw_shift, \
			NULL)

static const struct mtk_pll_div_table mmpll_div_table[] = {
	{ .div = 0, .freq = MT8163_PLL_FMAX },
	{ .div = 1, .freq = 1000000000 },
	{ .div = 2, .freq = 625000000 },
	{ .div = 3, .freq = 253500000 },
	{ .div = 4, .freq = 126750000 },
	{ } /* sentinel */
};

static int mt8163_armpll_safe_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate);

static const struct mtk_pll_data plls[] = {
	{
		.id = CLK_APMIXED_ARMPLL,
		.name = "armpll",
		.reg = 0x210,
		.pwr_reg = 0x21c,
		.en_mask = 0x1,
		.flags = 0,
		.rst_bar_mask = CON0_MT8163_RST_BAR,
		.fmax = MT8163_PLL_FMAX,
		.pcwbits = 21,
		.pd_reg = 0x214,
		.pd_shift = 24,
		.tuner_reg = 0x0,
		.pcw_reg = 0x214,
		.pcw_shift = 0,
		.div_table = NULL,
		.set_rate_override = mt8163_armpll_safe_set_rate,
	},
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x220, 0x22c, 0x1, HAVE_RST_BAR, 21, 0x220, 4, 0x0, 0x224, 0),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x230, 0x23c, 0x1, HAVE_RST_BAR, 21, 0x230, 4, 0x0, 0x234, 0),
	PLL_B(CLK_APMIXED_MMPLL, "mmpll", 0x240, 0x24c, 0x1, 0, 21, 0x244, 24, 0x0, 0x244, 0, mmpll_div_table),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x250, 0x25c, 0x1, 0, 21, 0x250, 4, 0x0, 0x254, 0),
	PLL(CLK_APMIXED_VENCPLL, "vencpll", 0x260, 0x26c, 0x1, 0, 21, 0x260, 4, 0x0, 0x264, 0),
	PLL(CLK_APMIXED_TVDPLL, "tvdpll", 0x270, 0x27c, 0x1, 0, 21, 0x270, 4, 0x0, 0x274, 0),
	PLL(CLK_APMIXED_MPLL, "mpll", 0x280, 0x28c, 0x1, 0, 21, 0x280, 4, 0x0, 0x284, 0),
	PLL(CLK_APMIXED_AUD1PLL, "aud1pll", 0x2a0, 0x2ac, 0x1, 0, 31, 0x2a0, 4, 0x300, 0x2a4, 0),
	PLL(CLK_APMIXED_AUD2PLL, "aud2pll", 0x2b0, 0x2bc, 0x1, 0, 31, 0x2b0, 4, 0x304, 0x2b4, 0),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", 0x2c0, 0x2cc, 0x1, 0, 21, 0x2c0, 4, 0x0, 0x2c4, 0),
};

/* Cached register maps for the safe ARMPLL set_rate path. Mapped once at
 * apmixedsys init; the CPU clock mux handoff runs on every DVFS transition,
 * so we must not ioremap/iounmap per call. */
static void __iomem *mt8163_cpufreq_infra;
static void __iomem *mt8163_cpufreq_topck;

/* CPU clock mux select values (TOP_CKMUXSEL[1:0]). */
#define MT8163_CKMUX_CLKSQ	0
#define MT8163_CKMUX_ARMPLL	1
#define MT8163_CKMUX_MAINPLL	2
#define MT8163_CKMUX_UNIVPLL	3

/* Register offsets. */
#define MT8163_TOP_CKMUXSEL	0x00	/* infracfg */
#define MT8163_CLK_MISC_CFG_0	0x104	/* topckgen */
#define MT8163_GATE_SHIFT	4
#define MT8163_GATE_MASK	(0x3 << MT8163_GATE_SHIFT)

static void __init mt8163_cpufreq_clock_probe(void)
{
	struct device_node *np;
	void __iomem *infra, *topck, *apmix;
	u32 v;

	/* Read-only snapshot of the CPU clock path. No writes. Used to
	 * confirm the register map before enabling safe DVFS transitions. */
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-infracfg");
	infra = np ? of_iomap(np, 0) : NULL;
	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-topckgen");
	topck = np ? of_iomap(np, 0) : NULL;
	of_node_put(np);

	/* Keep these mapped for the safe ARMPLL set_rate path. */
	mt8163_cpufreq_infra = infra;
	mt8163_cpufreq_topck = topck;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-apmixedsys");
	apmix = np ? of_iomap(np, 0) : NULL;
	of_node_put(np);

	if (infra) {
		v = readl(infra + 0x00);
		pr_info("mt8163-cpufreq-probe: TOP_CKMUXSEL=0x%08x cpu_mux=%u\n",
			v, v & 0x3);
		v = readl(infra + 0x08);
		pr_info("mt8163-cpufreq-probe: TOP_CKDIV1=0x%08x div=%u\n",
			v, v & 0x1f);
	}
	if (topck) {
		v = readl(topck + 0x104);
		pr_info("mt8163-cpufreq-probe: CLK_MISC_CFG_0=0x%08x gate=%u\n",
			v, (v >> 4) & 0x3);
	}
	if (apmix) {
		v = readl(apmix + 0x210);
		pr_info("mt8163-cpufreq-probe: ARMPLL_CON0=0x%08x\n", v);
		v = readl(apmix + 0x214);
		pr_info("mt8163-cpufreq-probe: ARMPLL_CON1=0x%08x\n", v);
		iounmap(apmix);
	}
}

static void mt8163_cpu_mux_switch(u32 sel)
{
	u32 val;

	if (sel == MT8163_CKMUX_MAINPLL || sel == MT8163_CKMUX_UNIVPLL) {
		/* enable gating cell before switching to a divided PLL */
		val = readl(mt8163_cpufreq_topck + MT8163_CLK_MISC_CFG_0);
		val = (val & ~MT8163_GATE_MASK) | (0x3 << MT8163_GATE_SHIFT);
		writel(val, mt8163_cpufreq_topck + MT8163_CLK_MISC_CFG_0);
		udelay(3);
	}

	val = readl(mt8163_cpufreq_infra + MT8163_TOP_CKMUXSEL);
	val = (val & ~0x3) | sel;
	writel(val, mt8163_cpufreq_infra + MT8163_TOP_CKMUXSEL);

	if (sel == MT8163_CKMUX_CLKSQ || sel == MT8163_CKMUX_ARMPLL) {
		/* disable gating cell when running on ARMPLL/CLKSQ */
		val = readl(mt8163_cpufreq_topck + MT8163_CLK_MISC_CFG_0);
		val = (val & ~MT8163_GATE_MASK) | (0x0 << MT8163_GATE_SHIFT);
		writel(val, mt8163_cpufreq_topck + MT8163_CLK_MISC_CFG_0);
	}
}

/* Safe ARMPLL set_rate with CPU clock mux handoff.
 * Switches CPU to MAINPLL before reprogramming ARMPLL, then switches back.
 * This prevents the CPU from running on an unstable PLL during transition. */
static int mt8163_armpll_safe_set_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	int ret;

	if (!mt8163_cpufreq_infra || !mt8163_cpufreq_topck) {
		pr_err("mt8163-armpll: register maps not initialised\n");
		return -ENODEV;
	}

	/* 1. Switch CPU to MAINPLL (safe intermediate clock). */
	mt8163_cpu_mux_switch(MT8163_CKMUX_MAINPLL);

	/* 2. Reprogram ARMPLL while the CPU runs on MAINPLL. */
	ret = mtk_pll_set_rate_regs_safe(hw, rate, parent_rate);

	/* 3. Wait for PLL settle, then switch CPU back to ARMPLL. */
	udelay(20);
	mt8163_cpu_mux_switch(MT8163_CKMUX_ARMPLL);

	return ret;
}

static void __init mtk_apmixedsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	void __iomem *base;
	struct clk_hw *clk;
	int r, i;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%s(): ioremap failed\n", __func__);
		return;
	}

	mt8163_cpufreq_clock_probe();

	pll_data = clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return;

	mtk_clk_register_plls(node, plls, ARRAY_SIZE(plls), clk_data);

	for (i = 0; i < ARRAY_SIZE(apmixed_usb); i++) {
		const struct mtk_clk_usb *cku = &apmixed_usb[i];

		clk = mtk_clk_register_ref2usb_tx(cku->name, cku->parent,
					base + cku->reg_ofs);

		if (IS_ERR(clk)) {
			pr_err("Failed to register clk %s: %ld\n", cku->name,
					PTR_ERR(clk));
			continue;
		}

		clk_data->hws[cku->id] = clk;
	}

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);

	mtk_clk_enable_critical();
}
CLK_OF_DECLARE(mtk_apmixedsys, "mediatek,mt8163-apmixedsys",
		mtk_apmixedsys_init);

static void __init mtk_mfgsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_MFG_NR_CLK);

	mtk_clk_register_gates(NULL, node, mfg_clks, ARRAY_SIZE(mfg_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);

	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_mfgsys, "mediatek,mt8163-mfgsys", mtk_mfgsys_init);

static void __init mtk_imgsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_IMG_NR_CLK);

	mtk_clk_register_gates(NULL, node, img_clks, ARRAY_SIZE(img_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);

	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_imgsys, "mediatek,mt8163-imgsys", mtk_imgsys_init);

static void __init mtk_mmsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_MM_NR_CLK);

	mtk_clk_register_gates(NULL, node, mm_clks, ARRAY_SIZE(mm_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_mmsys, "mediatek,mt8163-mmsys", mtk_mmsys_init);

static void __init mtk_vdecsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_VDEC_NR_CLK);

	mtk_clk_register_gates(NULL, node, vdec_clks, ARRAY_SIZE(vdec_clks),
						clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_vdecsys, "mediatek,mt8163-vdecsys", mtk_vdecsys_init);

static void __init mtk_vencsys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_VENC_NR_CLK);

	mtk_clk_register_gates(NULL, node, venc_clks, ARRAY_SIZE(venc_clks),
						clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_vencsys, "mediatek,mt8163-vencsys", mtk_vencsys_init);

static void __init mtk_audiosys_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_AUDIO_NR_CLK);

	mtk_clk_register_gates(NULL, node, audio_clks, ARRAY_SIZE(audio_clks),
						clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		pr_err("%s(): could not register clock provider: %d\n",
			__func__, r);
}
CLK_OF_DECLARE(mtk_audiosys, "mediatek,mt8163-audiosys", mtk_audiosys_init);

static int __init clk_mt8163_init(void)
{
	timer_ready = true;
	mtk_clk_enable_critical();

	return 0;
}
arch_initcall(clk_mt8163_init);
