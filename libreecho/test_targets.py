#!/usr/bin/env python3
"""Host-only regression tests for the target table and CI matrix planner."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "validate_targets.py"
PARITY: dict[str, dict[str, object]] = {
    "radar_puffin": {
        "defconfig": "mt8163_arm32_defconfig",
        "dtb": "libreecho-radar-puffin.dtb",
    },
    "biscuit": {
        "defconfig": "mt8163_arm32_defconfig",
        "dtb": "libreecho-radar-puffin.dtb",
    },
}


class TargetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.table = self.root / "targets.json"
        self.config = self.root / "arch/arm/configs/mt8163_arm32_defconfig"
        self.dts = self.root / "arch/arm/boot/dts/libreecho-radar-puffin.dts"
        for path in (self.config, self.dts):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n")

    def run_table(self, table):
        self.table.write_text(json.dumps(table))
        return subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--root", str(self.root),
             "--table", str(self.table), "--matrix"],
            text=True, capture_output=True, check=False,
        )

    def test_validator_exists(self):
        self.assertTrue(SCRIPT.is_file(), "target validator is not implemented")

    def test_committed_table_validates_and_retains_radar_recipe(self):
        self.assertTrue((HERE / "targets.json").is_file(), "target table is missing")
        table = json.loads((HERE / "targets.json").read_text())
        self.assertEqual(set(table), {"radar_puffin", "biscuit"})
        self.assertEqual(table["radar_puffin"], PARITY["radar_puffin"])
        # Do not pin Biscuit to parity forever: divergence must be a data change.
        result = subprocess.run([sys.executable, "-B", str(SCRIPT)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_github_output_matches_cli_matrices(self):
        result = self.run_table(PARITY)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = json.loads(result.stdout)
        output = self.root / "github-output"
        output.write_text("existing=value\n")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--root", str(self.root),
             "--table", str(self.table), "--github-output", str(output)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = output.read_text().splitlines()
        self.assertEqual(lines[0], "existing=value")
        for line in lines[1:]:
            name, value = line.split("=", 1)
            self.assertEqual(json.loads(value), expected[name.removesuffix("_matrix")])

    def test_json_key_order_does_not_change_build_owner(self):
        result = self.run_table(dict(reversed(list(PARITY.items()))))
        self.assertEqual(result.returncode, 0, result.stderr)
        matrices = json.loads(result.stdout)
        self.assertEqual(matrices["build"]["include"][0]["target"], "radar_puffin")
        self.assertTrue(all(lane["build_target"] == "radar_puffin"
                            for lane in matrices["publish"]["include"]))

    def test_parity_builds_once_and_publishes_each_target(self):
        result = self.run_table(PARITY)
        self.assertEqual(result.returncode, 0, result.stderr)
        matrices = json.loads(result.stdout)
        self.assertEqual(matrices["build"]["include"], [
            {"target": "radar_puffin", **PARITY["radar_puffin"]}])
        self.assertEqual(matrices["publish"]["include"], [
            {"target": target, "build_target": "radar_puffin", **settings}
            for target, settings in PARITY.items()])

    def test_divergent_tuple_adds_a_build_without_code_changes(self):
        table = copy.deepcopy(PARITY)
        table["biscuit"] = {"defconfig": "biscuit_defconfig", "dtb": "biscuit.dtb"}
        (self.config.parent / "biscuit_defconfig").write_text("fixture\n")
        (self.dts.parent / "biscuit.dts").write_text("fixture\n")
        result = self.run_table(table)
        self.assertEqual(result.returncode, 0, result.stderr)
        matrices = json.loads(result.stdout)
        self.assertEqual(len(matrices["build"]["include"]), 2)
        self.assertEqual(matrices["publish"]["include"][1]["build_target"], "biscuit")

    def test_missing_defconfig_fails(self):
        self.config.unlink()
        self.assertNotEqual(self.run_table(PARITY).returncode, 0)

    def test_missing_dts_fails(self):
        self.dts.unlink()
        self.assertNotEqual(self.run_table(PARITY).returncode, 0)

    def test_invalid_tables_fail_closed(self):
        cases = [{}, [], {"unknown": PARITY["radar_puffin"]}]
        for field, value in (("defconfig", "../escape_defconfig"),
                             ("dtb", "../../escape.dtb"),
                             ("dtb", "board.dts"),
                             ("defconfig", None)):
            table = copy.deepcopy(PARITY)
            table["biscuit"][field] = value
            cases.append(table)
        extra = copy.deepcopy(PARITY)
        extra["biscuit"]["fragments"] = []
        cases.append(extra)
        for table in cases:
            with self.subTest(table=table):
                self.assertNotEqual(self.run_table(table).returncode, 0)

    def test_duplicate_keys_fail_closed(self):
        self.table.write_text('{"radar_puffin": {}, "radar_puffin": {}}')
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--root", str(self.root),
             "--table", str(self.table)], capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
