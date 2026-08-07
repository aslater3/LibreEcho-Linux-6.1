/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT8163_CONN_UPMU_COMMON_H
#define _MT8163_CONN_UPMU_COMMON_H

/*
 * Linux 6.1 controls the DT-described VCN rails through the regulator
 * framework.  The old PMIC ON_CTRL sideband API has no upstream equivalent.
 */
static inline void upmu_set_vcn_1v8_lp_mode_set(unsigned int value) {}
static inline void upmu_set_vcn28_on_ctrl(unsigned int value) {}
static inline void upmu_set_vcn33_on_ctrl_bt(unsigned int value) {}
static inline void upmu_set_vcn33_on_ctrl_wifi(unsigned int value) {}

#endif
