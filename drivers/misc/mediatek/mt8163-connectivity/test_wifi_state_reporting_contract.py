#!/usr/bin/env python3
"""Source contracts for MT8163 Wi-Fi state convergence and reporting."""

from dataclasses import dataclass, field
from itertools import product
from pathlib import Path
import unittest




@dataclass
class RecoverySnapshot:
    """Host-only model of the predicates consumed by the AIS recovery helper."""

    bss_connected: bool
    host_connected: bool
    bssid_zero: bool
    connection_requested: bool


@dataclass
class RecoveryResult:
    actions: list[str] = field(default_factory=list)
    retries: int = 0


def _stale_connected(snapshot: RecoverySnapshot) -> bool:
    return (
        snapshot.bss_connected
        and snapshot.host_connected
        and snapshot.bssid_zero
        and snapshot.connection_requested
    )


def _model_auth_failure(
    snapshot: RecoverySnapshot,
    *,
    deadline_expired: bool,
    result: RecoveryResult,
) -> None:
    """Bounded model of the production failure decision, not fake hardware."""
    if _stale_connected(snapshot):
        if deadline_expired:
            result.actions.extend(
                ("disconnect", "cleanup", "clear-connection-request", "abort")
            )
        else:
            result.actions.extend(("disconnect", "cleanup", "retry"))
            result.retries += 1
    elif snapshot.bss_connected:
        result.actions.append("wait-for-roaming")
    elif deadline_expired:
        result.actions.extend(("clear-connection-request", "abort"))
    else:
        result.actions.extend(("retry",))
        result.retries += 1


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
        self.assertIn("memset(prAddr->sa_data, 0, ETH_ALEN);", function)
        self.assertLess(function.index(state_guard), function.index(query))

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

        reset_call = "fgResetAndRetry = aisFsmResetStaleConnection(prAdapter);"
        self.assertIn(reset_call, join_complete)
        self.assertIn("if (fgResetAndRetry)", join_complete)
        self.assertLess(
            join_complete.index(reset_call),
            join_complete.index("if (fgResetAndRetry)"),
        )

    def test_stale_predicate_truth_table_only_resets_the_exact_combination(self) -> None:
        stale_cases = []
        for values in product((False, True), repeat=4):
            snapshot = RecoverySnapshot(*values)
            result = RecoveryResult()
            _model_auth_failure(snapshot, deadline_expired=False, result=result)
            if values == (True, True, True, True):
                stale_cases.append(values)
                self.assertEqual(
                    result.actions,
                    ["disconnect", "cleanup", "retry"],
                )
                self.assertEqual(result.retries, 1)
            elif values[0]:
                self.assertEqual(result.actions, ["wait-for-roaming"])
                self.assertEqual(result.retries, 0)
            else:
                self.assertEqual(result.actions, ["retry"])
                self.assertEqual(result.retries, 1)
        self.assertEqual(stale_cases, [(True, True, True, True)])

    def test_stale_recovery_is_bounded_by_join_deadline(self) -> None:
        snapshot = RecoverySnapshot(True, True, True, True)
        result = RecoveryResult()
        deadline = 3
        for elapsed in range(deadline + 1):
            _model_auth_failure(
                snapshot,
                deadline_expired=elapsed >= deadline,
                result=result,
            )
        self.assertEqual(result.retries, deadline)
        self.assertEqual(
            result.actions[-4:],
            ["disconnect", "cleanup", "clear-connection-request", "abort"],
        )
        self.assertEqual(result.actions[:3], ["disconnect", "cleanup", "retry"])

        valid_roaming = RecoveryResult()
        _model_auth_failure(
            RecoverySnapshot(True, True, False, True),
            deadline_expired=True,
            result=valid_roaming,
        )
        self.assertEqual(valid_roaming.actions, ["wait-for-roaming"])
        self.assertEqual(valid_roaming.retries, 0)

    def test_production_failure_path_checks_deadline_before_reset_helper(self) -> None:
        source = AIS_FSM.read_text(encoding="utf-8")
        failure_path = source.split(
            "if (aisFsmStateInit_RetryJOIN(prAdapter, prStaRec) == FALSE)",
            1,
        )[1].split("/* end of aisFsmRunEventJoinComplete() */", 1)[0]
        self.assertLess(
            failure_path.index("CHECK_FOR_TIMEOUT"),
            failure_path.index("aisFsmResetStaleConnection(prAdapter)"),
        )


if __name__ == "__main__":
    unittest.main()
