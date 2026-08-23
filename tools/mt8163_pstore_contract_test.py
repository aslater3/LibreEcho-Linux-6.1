#!/usr/bin/env python3
"""Source contract for the MT8163 production pstore/ramoops boundary.

Issue #6 asks for persistent panic evidence, but this repository does not yet
contain a safe, complete integration.  The production DT already reserves
platform-owned memory, while the product initramfs is owned by the separate
LibreEcho-Platform repository.  Enabling only one layer here would create a
misleading or unsafe partial implementation.

This test deliberately accepts the documented blocked state on main.  If a
future change starts the integration, it must provide all three layers at once:
CONFIG_PSTORE + CONFIG_PSTORE_RAM, a ramoops node with an explicit reserved
region in the production DT, and initramfs archival handling.  The test then
checks that the change is complete rather than allowing a partial contract.

Run from the kernel source root:

    python3 tools/mt8163_pstore_contract_test.py
"""

from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"
PRODUCTION_DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"
README = ROOT / "README.md"
INIT_ROOTS = (ROOT / "init", ROOT / "usr")
INIT_MARKERS = (
    "mount -t pstore",
    "/sys/fs/pstore",
    "console-ramoops",
    "dmesg-ramoops",
)


class Mt8163PstoreContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.config = DEFCONFIG.read_text(encoding="utf-8")
        self.dts = PRODUCTION_DTS.read_text(encoding="utf-8")
        self.readme = README.read_text(encoding="utf-8")

    def test_current_tree_is_not_partially_pstore_enabled(self) -> None:
        enabled = {
            symbol: bool(re.search(rf"^{symbol}=[ym]$", self.config, re.MULTILINE))
            for symbol in ("CONFIG_PSTORE", "CONFIG_PSTORE_RAM")
        }
        self.assertEqual(
            enabled,
            {"CONFIG_PSTORE": False, "CONFIG_PSTORE_RAM": False},
            "do not enable pstore without the DT and initramfs layers",
        )

    def test_production_dt_has_no_unowned_ramoops_region(self) -> None:
        self.assertIn("reserved-memory {", self.dts)
        self.assertIn('compatible = "mediatek,ram_console";', self.dts)
        self.assertNotIn('compatible = "ramoops";', self.dts)
        self.assertNotIn("pstore", self.dts.lower())

    def test_kernel_tree_does_not_claim_product_initramfs_archival(self) -> None:
        self.assertIn(
            "owns ARM32 product tooling, initramfs, feature packaging",
            self.readme,
        )
        for root in INIT_ROOTS:
            for path in root.rglob("*"):
                if not path.is_file() or path.suffix in {".o", ".a", ".cmd"}:
                    continue
                text = path.read_text(encoding="utf-8", errors="ignore")
                for marker in INIT_MARKERS:
                    with self.subTest(path=path.relative_to(ROOT), marker=marker):
                        self.assertNotIn(marker, text)

    def test_any_future_integration_must_supply_all_layers(self) -> None:
        config_enabled = all(
            re.search(rf"^CONFIG_{name}=[ym]$", self.config, re.MULTILINE)
            for name in ("PSTORE", "PSTORE_RAM")
        )
        dt_enabled = 'compatible = "ramoops";' in self.dts
        init_enabled = any(
            marker in path.read_text(encoding="utf-8", errors="ignore")
            for root in INIT_ROOTS
            for path in root.rglob("*")
            if path.is_file()
            for marker in INIT_MARKERS
        )
        self.assertFalse(
            config_enabled or dt_enabled or init_enabled,
            "pstore integration must land as one complete, reviewed contract",
        )


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print(
            "MT8163 pstore source contract: PASS "
            "(integration remains blocked pending a firmware-owned region "
            "and product initramfs change)"
        )
    sys.exit(not result.result.wasSuccessful())
