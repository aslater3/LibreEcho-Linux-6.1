// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal MT8163 WMT platform callback registry.
 *
 * The vendor common-detect implementation also probes removable combo chips
 * and depends on board-specific SDIO glue.  MT8163 CONSYS is integrated and
 * described by a fixed DT node, so only registration lifetime is required by
 * the in-tree WMT platform layer.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "mtk_wcn_cmb_stub.h"

static DEFINE_MUTEX(cmb_stub_lock);
static CMB_STUB_CB cmb_stub_callbacks;
static bool cmb_stub_registered;
static int cmb_stub_chip_id = 0x8163;

int mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB callbacks)
{
	if (!callbacks || callbacks->size != sizeof(*callbacks))
		return -EINVAL;

	mutex_lock(&cmb_stub_lock);
	if (cmb_stub_registered) {
		mutex_unlock(&cmb_stub_lock);
		return -EBUSY;
	}

	cmb_stub_callbacks = *callbacks;
	cmb_stub_registered = true;
	mutex_unlock(&cmb_stub_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_cmb_stub_reg);

int mtk_wcn_cmb_stub_unreg(void)
{
	mutex_lock(&cmb_stub_lock);
	memset(&cmb_stub_callbacks, 0, sizeof(cmb_stub_callbacks));
	cmb_stub_registered = false;
	mutex_unlock(&cmb_stub_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_cmb_stub_unreg);

static int mtk_wcn_cmb_stub_deep_idle(COMBO_IF src, unsigned int enter)
{
	wmt_deep_idle_ctrl_cb callback = NULL;

	if (src < COMBO_IF_UART || src >= COMBO_IF_MAX)
		return -EINVAL;

	mutex_lock(&cmb_stub_lock);
	if (cmb_stub_registered && src == COMBO_IF_BTIF)
		callback = cmb_stub_callbacks.deep_idle_ctrl_cb;
	mutex_unlock(&cmb_stub_lock);

	if (!callback)
		return -EOPNOTSUPP;

	return callback(enter);
}

int mt_combo_plt_enter_deep_idle(COMBO_IF src)
{
	return mtk_wcn_cmb_stub_deep_idle(src, 1);
}
EXPORT_SYMBOL_GPL(mt_combo_plt_enter_deep_idle);

int mt_combo_plt_exit_deep_idle(COMBO_IF src)
{
	return mtk_wcn_cmb_stub_deep_idle(src, 0);
}
EXPORT_SYMBOL_GPL(mt_combo_plt_exit_deep_idle);

int mtk_wcn_wmt_chipid_query(void)
{
	return READ_ONCE(cmb_stub_chip_id);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_chipid_query);

void mtk_wcn_wmt_set_chipid(int chipid)
{
	WRITE_ONCE(cmb_stub_chip_id, chipid);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_set_chipid);
