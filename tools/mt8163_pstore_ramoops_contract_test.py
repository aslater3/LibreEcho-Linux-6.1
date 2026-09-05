#!/usr/bin/env python3
"""Source contract for the MT8163 pstore/ramoops crash-log path.

This check keeps the reserved-memory binding, ramoops layout, Kconfig closure,
and CI registration together.  It intentionally does not claim that the
layout has been validated on hardware.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"
WORKFLOW = ROOT / ".github/workflows/checks.yml"


class Mt8163PstoreRamoopsContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dts = DTS.read_text(encoding="utf-8")
        self.defconfig = DEFCONFIG.read_text(encoding="utf-8")
        self.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_dts_binds_the_full_reserved_region_to_ramoops(self) -> None:
        node = re.search(
            r"ramoops@44400000\s*\{(?P<body>.*?)\n\s*\};",
            self.dts,
            re.DOTALL,
        )
        if node is None:
            self.fail("ramoops@44400000 node is missing")
        body = node.group("body")
        self.assertIn('compatible = "ramoops";', body)
        self.assertIn("reg = <0x00 0x44400000 0x00 0x200000>;", body)

        zones = {
            name: int(value, 16)
            for name, value in re.findall(
                r"\b(console-size|pmsg-size|record-size|ftrace-size)\s*=\s*<"
                r"(0x[0-9a-f]+)>;",
                body,
            )
        }
        self.assertEqual(
            zones,
            {
                "console-size": 0x80000,
                "pmsg-size": 0x40000,
                "record-size": 0x40000,
                "ftrace-size": 0x20000,
            },
        )
        self.assertLessEqual(sum(zones.values()) + 0x10, 0x200000)

    def test_defconfig_enables_complete_pstore_ftrace_closure(self) -> None:
        required = (
            "CONFIG_DEBUG_FS=y",
            "CONFIG_FTRACE=y",
            "CONFIG_FUNCTION_TRACER=y",
            "CONFIG_PSTORE=y",
            "CONFIG_PSTORE_CONSOLE=y",
            "CONFIG_PSTORE_PMSG=y",
            "CONFIG_PSTORE_FTRACE=y",
            "CONFIG_PSTORE_RAM=y",
        )
        for symbol in required:
            with self.subTest(symbol=symbol):
                self.assertIn(f"{symbol}\n", self.defconfig)

    def test_contract_is_registered_in_the_pull_request_build(self) -> None:
        command = "python3 tools/mt8163_pstore_ramoops_contract_test.py"
        self.assertEqual(self.workflow.count(command), 1)
        self.assertIn("name: ARM32 kernel build and regression checks", self.workflow)


if __name__ == "__main__":
    unittest.main()