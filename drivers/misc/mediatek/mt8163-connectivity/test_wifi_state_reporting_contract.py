#!/usr/bin/env python3
"""Source contracts for MT8163 Wi-Fi state convergence and reporting."""

from pathlib import Path
import subprocess
import tempfile
import textwrap
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
GL_KAL = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/os/linux/gl_kal.c"
)
AIS_FSM = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/mgmt/ais_fsm.c"
)
AIS_INCLUDE = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/include"
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
        self.assertIn("memset(prAddr->sa_data, 0, ETH_ALEN);", function)
        self.assertLess(function.index(state_guard), function.index(query))

    def test_local_disconnect_notifies_cfg80211(self) -> None:
        source = GL_KAL.read_text(encoding="utf-8")
        disconnect = source.split(
            "case WLAN_STATUS_MEDIA_DISCONNECT:", 1
        )[1].split(
            "prGlueInfo->eParamMediaStateIndicated = "
            "PARAM_MEDIA_STATE_DISCONNECTED;", 1
        )[0]

        self.assertIn("case WLAN_STATUS_MEDIA_DISCONNECT_LOCALLY:", disconnect)
        self.assertIn(
            "if (prGlueInfo->fgIsRegistered == TRUE) {", disconnect
        )
        self.assertNotIn(
            "&& eStatus == WLAN_STATUS_MEDIA_DISCONNECT", disconnect
        )
        self.assertIn(
            "eStatus == WLAN_STATUS_MEDIA_DISCONNECT_LOCALLY", disconnect
        )
        self.assertEqual(disconnect.count("cfg80211_disconnected("), 1)

    def test_auth_timeout_recovery_resets_divergence_before_retry(self) -> None:
        source = AIS_FSM.read_text(encoding="utf-8")
        stale_predicate = self._function(
            source,
            "static BOOLEAN aisFsmIsStaleConnected(",
            "/*----------------------------------------------------------------------------*/",
        )
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

        self.assertIn("PARAM_MEDIA_STATE_CONNECTED", stale_predicate)
        self.assertIn("rCurrBssId.arMacAddress", stale_predicate)
        self.assertIn("NULL_MAC_ADDR", stale_predicate)
        self.assertIn("EQUAL_MAC_ADDR", stale_predicate)
        self.assertNotIn("UNEQUAL_MAC_ADDR", stale_predicate)
        self.assertIn("fgIsConnReqIssued != FALSE", stale_predicate)
        self.assertIn("fgIsDisconnectedByNonRequest = FALSE", helper)
        self.assertIn("aisFsmDisconnect(prAdapter, FALSE)", helper)
        self.assertLess(
            helper.index("if (fgAbortExpired)"),
            helper.index("aisFsmDisconnect(prAdapter, FALSE)"),
        )

        reset_call = "fgResetAndRetry = aisFsmResetStaleConnection(prAdapter, FALSE);"
        self.assertIn(reset_call, join_complete)
        self.assertIn("aisFsmResetStaleConnection(prAdapter, TRUE);", join_complete)
        self.assertIn("if (fgResetAndRetry)", join_complete)
        self.assertLess(
            join_complete.index(reset_call),
            join_complete.index("if (fgResetAndRetry)"),
        )

    def test_compiled_recovery_decision_table_and_deadline(self) -> None:
        harness = textwrap.dedent(
            """
            #include <stdio.h>
            typedef int BOOLEAN;
            #include "mgmt/ais_fsm_recovery.h"

            static int expected_action(int stale, int bss, int expired)
            {
                if (stale)
                    return expired ? AIS_STALE_RECOVERY_ABORT :
                        AIS_STALE_RECOVERY_DISCONNECT_RETRY;
                return bss ? AIS_STALE_RECOVERY_WAIT_ROAMING :
                    AIS_STALE_RECOVERY_NONE;
            }

            int main(void)
            {
                int bss, host, bssid_zero, requested, expired;
                int retries = 0;

                for (bss = 0; bss <= 1; ++bss)
                    for (host = 0; host <= 1; ++host)
                        for (bssid_zero = 0; bssid_zero <= 1; ++bssid_zero)
                            for (requested = 0; requested <= 1; ++requested)
                                for (expired = 0; expired <= 1; ++expired) {
                                    int stale = aisFsmHasStaleConnectedState(
                                        bss, host, bssid_zero, requested);
                                    int actual = aisFsmClassifyJoinFailure(
                                        stale, bss, expired);
                                    if (actual != expected_action(stale, bss,
                                                                   expired))
                                        return 1;
                                }

                for (expired = 0; expired <= 3; ++expired) {
                    int action = aisFsmClassifyJoinFailure(
                        1, 1, expired >= 3);
                    if (action == AIS_STALE_RECOVERY_DISCONNECT_RETRY)
                        ++retries;
                    else if (action != AIS_STALE_RECOVERY_ABORT || retries != 3)
                        return 2;
                }

                if (aisFsmClassifyJoinFailure(0, 1, 1) !=
                    AIS_STALE_RECOVERY_WAIT_ROAMING)
                    return 3;
                if (aisFsmClassifyJoinFailure(0, 0, 1) !=
                    AIS_STALE_RECOVERY_NONE)
                    return 4;
                puts("compiled recovery contract: PASS");
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "recovery_contract.c"
            binary = temp / "recovery_contract"
            source.write_text(harness, encoding="utf-8")
            subprocess.run(
                [
                    "gcc",
                    "-std=c99",
                    "-Wall",
                    "-Werror",
                    f"-I{AIS_INCLUDE}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True
            )
        self.assertEqual(result.stdout.strip(), "compiled recovery contract: PASS")

    def test_production_failure_path_checks_deadline_before_reset_helper(self) -> None:
        source = AIS_FSM.read_text(encoding="utf-8")
        failure_path = source.split(
            "if (aisFsmStateInit_RetryJOIN(prAdapter, prStaRec) == FALSE)",
            1,
        )[1].split("/* end of aisFsmRunEventJoinComplete() */", 1)[0]
        self.assertLess(
            failure_path.index("eStaleRecoveryAction = aisFsmClassifyJoinFailure"),
            failure_path.index("aisFsmResetStaleConnection(prAdapter, TRUE)"),
        )
        self.assertLess(
            failure_path.index("aisFsmResetStaleConnection(prAdapter, TRUE)"),
            failure_path.index("WLAN_STATUS_CONNECT_INDICATION"),
        )


if __name__ == "__main__":
    unittest.main()
