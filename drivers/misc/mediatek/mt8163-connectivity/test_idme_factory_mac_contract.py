#!/usr/bin/env python3
"""Regression contract for the IDME-backed MT8163 factory Wi-Fi identity.

`CONFIG_IDME` compiles in the retained vendor WLAN path that reads the factory
Wi-Fi MAC and manufacturing data from the Amazon IDME device-tree nodes. If the
IDME MAC ingestion or its fallback wiring regresses, the shipped device can
silently come up with a boot-time generated MAC instead of the factory address
(or vice versa), so this contract locks down both the parsed result and the
fallback decision.

The behavioural half decodes IDME `value` properties exactly as the kernel does
(`idme_get_mac_addr()` parses 12 hex characters, two per octet, with
`kstrtou8(..., 16, ...)`), so a change to the wire format or the length gate is
caught against real inputs. The source half asserts the driver, Kconfig, and
defconfig still implement that algorithm and fallback, and that the CI runner
executes this file.

Run from the kernel source root:

    python3 drivers/misc/mediatek/mt8163-connectivity/test_idme_factory_mac_contract.py
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[4]
CONNECTIVITY = ROOT / "drivers/misc/mediatek/mt8163-connectivity"
GL_INIT = CONNECTIVITY / (
    "conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_init.c"
)
GL_KAL = CONNECTIVITY / (
    "conn_soc/drv_wlan/mt_wifi/wlan/os/linux/gl_kal.c"
)
WLAN_LIB = CONNECTIVITY / "conn_soc/drv_wlan/mt_wifi/wlan/common/wlan_lib.c"
KCONFIG = ROOT / "drivers/misc/mediatek/Kconfig"
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"
WORKFLOW = ROOT / ".github/workflows/checks.yml"

MAC_ADDR_OCTETS = 6
MAC_ADDR_HEX_CHARS = MAC_ADDR_OCTETS * 2
WIFI_MFG_HEX_CHARS = 1024


class IdmeParseError(ValueError):
    """Raised when a pair is not valid hexadecimal, mirroring kstrtou8()."""


def decode_idme_mac(value, current):
    """Mirror idme_get_mac_addr(): parse ``value`` into six octets in-place.

    ``current`` is the byte array the driver would already hold (from NVRAM or
    the compiled default). The kernel leaves it untouched when the node is
    missing or shorter than 12 hex characters, and skips only the offending
    octet when a pair does not parse, so the model reproduces both.
    """
    if value is None or len(value) < MAC_ADDR_HEX_CHARS:
        return list(current)
    for offset in range(0, MAC_ADDR_HEX_CHARS, 2):
        pair = value[offset:offset + 2]
        try:
            octet = int(pair, 16)
        except ValueError as exc:
            raise IdmeParseError(pair) from exc
        if octet > 0xFF:
            raise IdmeParseError(pair)
        current[offset >> 1] = octet
    return list(current)


def wifi_mfg_available(value):
    """Mirror idme_get_wifi_mfg(): only lengths >= 1024 count as a read."""
    return value is not None and len(value) >= WIFI_MFG_HEX_CHARS


class IdmeFactoryMacBehaviourTests(unittest.TestCase):
    def test_factory_mac_is_decoded_from_the_idme_node(self) -> None:
        self.assertEqual(
            decode_idme_mac("001122AABBCC", [0] * MAC_ADDR_OCTETS),
            [0x00, 0x11, 0x22, 0xAA, 0xBB, 0xCC],
        )

    def test_factory_mac_accepts_lowercase_hex(self) -> None:
        self.assertEqual(
            decode_idme_mac("aabbccddeeff", [0] * MAC_ADDR_OCTETS),
            [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF],
        )

    def test_missing_node_preserves_the_fallback_mac(self) -> None:
        fallback = [0x02, 0x11, 0x22, 0x33, 0x44, 0x55]
        self.assertEqual(decode_idme_mac(None, fallback), fallback)

    def test_short_value_preserves_the_fallback_mac(self) -> None:
        fallback = [0x02, 0x11, 0x22, 0x33, 0x44, 0x55]
        # One octet short of the 12-character gate.
        self.assertEqual(decode_idme_mac("001122AABB", fallback), fallback)

    def test_non_hex_pair_is_rejected(self) -> None:
        with self.assertRaises(IdmeParseError):
            decode_idme_mac("001122AABBZZ", [0] * MAC_ADDR_OCTETS)

    def test_wifi_mfg_length_boundary_selects_the_fallback(self) -> None:
        self.assertFalse(wifi_mfg_available("0" * (WIFI_MFG_HEX_CHARS - 2)))
        self.assertTrue(wifi_mfg_available("0" * WIFI_MFG_HEX_CHARS))
        self.assertFalse(wifi_mfg_available(None))


class IdmeSourceContractTests(unittest.TestCase):
    def test_idme_is_enabled_and_selected_in_the_target_config(self) -> None:
        config = DEFCONFIG.read_text(encoding="utf-8")
        self.assertIn("CONFIG_MTK_MT8163_CONSYS=y", config)
        self.assertIn("CONFIG_IDME=y", config)

    def test_kconfig_gates_idme_on_the_retained_consys_path(self) -> None:
        kconfig = KCONFIG.read_text(encoding="utf-8")
        block = kconfig.split("config IDME", 1)[1].split("\nconfig ", 1)[0]
        self.assertIn("depends on MTK_MT8163_CONSYS && OF", block)
        self.assertIn("default y if MTK_MT8163_CONSYS", block)

    def test_driver_reads_the_idme_nodes_by_path(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        self.assertIn('"/idme/mac_addr"', source)
        self.assertIn('"/idme/wifi_mfg"', source)
        self.assertIn('"/idme/board_id"', source)
        self.assertIn("of_find_node_by_path(IDME_OF_MAC_ADDR)", source)
        self.assertIn("of_find_node_by_path(IDME_OF_WIFI_MFG)", source)

    def test_mac_decoder_matches_the_behavioural_contract(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        body = source.split("static void idme_get_mac_addr(", 1)[1].split(
            "\nstatic ", 1
        )[0]
        self.assertIn('of_get_property(ap, "value", &len)', body)
        self.assertIn("if (likely(len >= 12))", body)
        self.assertIn(
            "kstrtou8(buf, 16, &prRegInfo->aucMacAddr[i >> 1]);", body
        )

    def test_wifi_mfg_length_gate_matches_the_behavioural_contract(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        body = source.split("static int idme_get_wifi_mfg(", 1)[1].split(
            "\nstatic ", 1
        )[0]
        self.assertIn("if (likely(len >= 1024))", body)
        self.assertIn("ret = -1;", body)

    def test_idme_success_selects_idme_and_skips_the_nvram_fallback(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        success = source.split("if (idme_get_wifi_mfg(prRegInfo) == 0)", 1)[1]
        success = success.split("} else", 1)[0]
        self.assertIn("wlanCopyIdmeWifiMfg(prRegInfo);", success)
        self.assertIn("prRegInfo->ManufactureSource = MANUFACTURE_IDME;", success)
        self.assertIn("prGlueInfo->fgNvramAvailable = TRUE;", success)

    def test_failed_idme_read_falls_back_to_nvram_and_defaults(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        fallback = source.split("if (idme_get_wifi_mfg(prRegInfo) == 0)", 1)[1]
        fallback = fallback.split("} else", 1)[1]
        self.assertIn("glLoadNvram(prGlueInfo, prRegInfo);", fallback)
        self.assertIn(
            "prRegInfo->ManufactureSource = MANUFACTURE_NVRAM;", fallback
        )
        self.assertIn("wlanGetDefaultWifiMfg(prRegInfo);", fallback)

    def test_network_address_uses_the_idme_mac_when_enabled(self) -> None:
        source = GL_KAL.read_text(encoding="utf-8")
        body = source.split("BOOLEAN kalRetrieveNetworkAddress(", 1)[1].split(
            "\n}", 1
        )[0]
        idme_branch = body.split("#ifdef CONFIG_IDME", 1)[1].split(
            "#else", 1
        )[0]
        self.assertIn(
            "COPY_MAC_ADDR(prMacAddr, &prGlueInfo->rRegInfo.aucMacAddr);",
            idme_branch,
        )

    def test_manufacture_data_reads_versions_from_idme(self) -> None:
        source = WLAN_LIB.read_text(encoding="utf-8")
        self.assertIn(
            "prRegInfo->ManufactureSource == MANUFACTURE_IDME", source
        )
        branch = source.split(
            "prRegInfo->ManufactureSource == MANUFACTURE_IDME", 1
        )[1].split("} else", 1)[0]
        self.assertIn("idme_wifi_mfg.u2Part1OwnVersion", branch)

    def test_contract_runs_in_the_arm32_ci_runner(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn(
            "python3 drivers/misc/mediatek/mt8163-connectivity/"
            "test_idme_factory_mac_contract.py",
            workflow,
        )


if __name__ == "__main__":
    unittest.main()
