/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT8163_CONN_AEE_H
#define _MT8163_CONN_AEE_H

#include <linux/compiler.h>
#include <linux/types.h>

#define DB_OPT_WCN_ISSUE_INFO 0

static inline void aee_kernel_warning_api(const char *file, int line,
					  unsigned int options,
					  const char *module,
					  const char *description)
{
}

static inline void aee_kernel_dal_show(const char *message)
{
}

static inline void aee_rr_rec_fiq_step(u8 step)
{
}

static inline int aee_rr_curr_fiq_step(void)
{
	return 0;
}

static inline void aee_sram_fiq_save_bin(const char *data, size_t length)
{
}

static inline void aed_combo_exception(const int *log, int log_size,
				       const int *phy, int phy_size,
				       const char *detail)
{
}

#endif
