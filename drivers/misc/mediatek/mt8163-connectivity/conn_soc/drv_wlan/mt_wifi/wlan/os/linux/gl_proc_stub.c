/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The vendor /proc/wlan tables are diagnostics, not part of the WMT helper
 * character-device ABI.  Keep their call sites harmlessly optional.
 */
#include "precomp.h"

#ifdef CONFIG_MTK_WIFI_ANTENNA_SWITCH
/*
 * The vendor implementation incorrectly placed this hardware command in the
 * obsolete procfs translation unit. Keep the command while the diagnostic
 * proc tables remain stubbed on Linux 6.1.
 */
WLAN_STATUS antennaSwitch(P_ADAPTER_T prAdapter, UINT32 mode, bool is_oid)
{
	CMD_SW_DBG_CTRL_T rCmdSwCtrl;
	UINT32 debugId = 0xa0340000 | mode;

	rCmdSwCtrl.u4Id = debugId;
	rCmdSwCtrl.u4Data = 0;
	DBGLOG(INIT, TRACE, "antennaSwitch 0x%x, %d\n",
	       rCmdSwCtrl.u4Id, rCmdSwCtrl.u4Data);

	return wlanSendSetQueryCmd(prAdapter, CMD_ID_SW_DBG_CTRL,
				   TRUE, FALSE, is_oid,
				   nicCmdEventSetCommon,
				   nicOidCmdTimeoutCommon,
				   sizeof(CMD_SW_DBG_CTRL_T),
				   (PUINT_8)&rCmdSwCtrl, NULL, 0);
}
#endif

INT_32 procInitFs(VOID)
{
	return 0;
}

INT_32 procUninitProcFs(VOID)
{
	return 0;
}

INT_32 procCreateFsEntry(P_GLUE_INFO_T prGlueInfo)
{
	return 0;
}

INT_32 procRemoveProcfs(VOID)
{
	return 0;
}
