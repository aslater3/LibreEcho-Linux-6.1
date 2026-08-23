#!/usr/bin/env python3
"""Source contracts for MT8163 Wi-Fi state convergence and reporting."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[4]
GL_CFG80211 = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/os/linux/gl_cfg80211.c"
)
GL_WEXT = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/os/linux/gl_wext.c"
)
AIS_FSM = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/mgmt/ais_fsm.c"
)


class WifiStateReportingContractTests(unittest.TestCase):
    @staticmethod
    def _function(source: str, start: str, end: str) -> str:
        return source.split(start, 1)[1].split(end, 1)[0]

    def test_cfg80211_station_query_rejects_disconnected_state_first(self) -> None:
        source = GL_CFG80211.read_text(encoding="utf-8")
        function = self._function(
            source,
            "int mtk_cfg80211_get_station(",
            "/*----------------------------------------------------------------------------*/\n/*!\n * @brief This routine is responsible for",
        )
        state_guard = (
            "if (prGlueInfo->eParamMediaStateIndicated "
            "!= PARAM_MEDIA_STATE_CONNECTED)"
        )
        query = "wlanQueryInformation(prGlueInfo->prAdapter, wlanoidQueryBssid"
        self.assertIn(state_guard, function)
        self.assertIn("return -ENOTCONN;", function)
        self.assertLess(function.index(state_guard), function.index(query))

    def test_wext_essid_query_rejects_disconnected_state_first(self) -> None:
        source = GL_WEXT.read_text(encoding="utf-8")
        function = self._function(
            source,
            "wext_get_essid(IN struct net_device *prNetDev,",
            "/* wext_get_essid */",
        )
        state_guard = (
            "if (prGlueInfo->eParamMediaStateIndicated "
            "!= PARAM_MEDIA_STATE_CONNECTED)"
        )
        query = "wlanoidQuerySsid"
        self.assertIn(state_guard, function)
        self.assertIn("return -ENOTCONN;", function)
        self.assertLess(function.index(state_guard), function.index(query))

    def test_wext_access_point_query_clears_nonconnected_state_first(self) -> None:
        source = GL_WEXT.read_text(encoding="utf-8")
        function = self._function(
            source,
            "wext_get_ap(IN struct net_device *prNetDev,",
            "/* wext_get_ap */",
        )
        state_guard = (
            "if (prGlueInfo->eParamMediaStateIndicated "
            "!= PARAM_MEDIA_STATE_CONNECTED)"
        )
        query = "wlanoidQueryBssid"
        self.assertIn(state_guard, function)
        self.assertIn("memset(prAddr, 0, 6);", function)
        self.assertLess(function.index(state_guard), function.index(query))

    def test_auth_timeout_recovery_resets_divergence_before_retry(self) -> None:
        source = AIS_FSM.read_text(encoding="utf-8")
        helper = self._function(
            source,
            "static BOOLEAN aisFsmResetStaleConnection(",
            "/*----------------------------------------------------------------------------*/",
        )
        join_complete = self._function(
            source,
            "VOID aisFsmRunEventJoinComplete(",
            "/* end of aisFsmRunEventJoinComplete() */",
        )

        self.assertIn("PARAM_MEDIA_STATE_CONNECTED", helper)
        self.assertIn("rCurrBssId.arMacAddress", helper)
        self.assertIn("NULL_MAC_ADDR", helper)
        self.assertIn("fgIsConnReqIssued == FALSE", helper)
        self.assertIn("fgIsDisconnectedByNonRequest = FALSE", helper)
        self.assertIn("aisFsmDisconnect(prAdapter, FALSE)", helper)

        reset_call = "fgResetAndRetry = aisFsmResetStaleConnection(prAdapter);"
        self.assertIn(reset_call, join_complete)
        self.assertIn("if (fgResetAndRetry)", join_complete)
        self.assertLess(
            join_complete.index(reset_call),
            join_complete.index("if (fgResetAndRetry)"),
        )


if __name__ == "__main__":
    unittest.main()
