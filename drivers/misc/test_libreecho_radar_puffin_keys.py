#!/usr/bin/env python3
"""Regression contract for Radar-Puffin keypad mappings.

This host-side check verifies the semantic DTS contract for the two physical
buttons added by the PR. It does not replace DTC compilation or hardware
validation.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"


def node_body(text, node_name):
    match = re.search(rf"(?m)^\s*{re.escape(node_name)}\s*\{{", text)
    if not match:
        return None
    start = text.find("{", match.start())
    depth = 0
    for position in range(start, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start : position + 1]
    return None


def main():
    if not DTS.exists():
        print(f"Radar-Puffin keypad contract: FAIL\n  - {DTS} is missing")
        return 1

    text = DTS.read_text(encoding="utf-8")
    failures = []
    action = node_body(text, "action_key")
    if action is None:
        failures.append("action_key node is missing")
    else:
        required = (
            ('compatible = "gpio-keys"', "gpio-keys compatibility"),
            ("action@36", "KPCOL0 action key node"),
            ("linux,code = <0x8a>", "KEY_HELP mapping"),
            ("gpios = <&pio 0x24 0x01>", "KPCOL0 GPIO mapping"),
        )
        for fragment, description in required:
            if fragment not in action:
                failures.append(f"action_key lacks {description}: {fragment}")

    pmic = node_body(text, "mt6323keys")
    if pmic is None or "linux,keycodes = <0x71>" not in pmic:
        failures.append("mt6323keys must retain the PMIC KEY_MUTE mapping")

    if failures:
        print("Radar-Puffin keypad contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("Radar-Puffin keypad contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
