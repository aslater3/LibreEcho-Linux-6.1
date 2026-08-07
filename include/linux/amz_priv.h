/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Kernel callback interface for Amazon hardware privacy control.
 */

#ifndef _LINUX_AMZ_PRIV_H
#define _LINUX_AMZ_PRIV_H

#include <linux/types.h>

enum priv_cb_dest {
	PRIV_CB_MIC = 0,
	PRIV_CB_CAM,
	PRIV_CB_MAX,
};

struct priv_cb_data {
	int (*cb)(void *data, int privacy_on);
	void *cb_priv_data;
	unsigned int cb_dest;
};

int amz_priv_cb_reg(struct priv_cb_data *pcd);
int amz_priv_trigger(int on);
int amz_priv_timer_sysfs(int on);

#endif /* _LINUX_AMZ_PRIV_H */
