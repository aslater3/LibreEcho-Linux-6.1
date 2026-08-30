#!/usr/bin/env python3
"""Regression contract for MT8163 playback-state MCLK ownership.

The AFE has one pinctrl handle and pinctrl_select_state() replaces the active
state.  Every playback-side state that can be selected after capture starts
must therefore include the CMMCLK state, or playback teardown releases the
microphone codec master clock.

This host-side check verifies the source contract.  It does not replace DTB
compilation or capture-after-playback validation on hardware.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"
AFE = ROOT / "sound/soc/mediatek/mt8163/mt8163-afe.c"

# These are the states selected by the AFE playback/output helpers.  The PMIC
# states are separate codec-clock modes; this contract covers every output
# and teardown state that can replace the shared pinctrl selection.
PLAYBACK_STATES = (
    "audpmicclk-speaker-mode0",
    "audpmicclk-speaker-mode1",
    "audi2s1-speaker-mode0",
    "audi2s1-speaker-mode1",
    "extamp-pullhigh",
    "extamp-pulllow",
    "extamp-dacmux-pullhigh",
    "extamp-dacmux-pulllow",
)

FUNCTION_STATE_SYMBOLS = {
    "mt8163_afe_select_i2s": ("MT8163_PIN_I2S_ACTIVE", "MT8163_PIN_I2S_IDLE"),
    "mt8163_afe_select_amp": ("MT8163_PIN_AMP_ON", "MT8163_PIN_AMP_OFF"),
    "mt8163_afe_select_dac": ("MT8163_PIN_DAC_ON", "MT8163_PIN_DAC_OFF"),
}


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


def function_body(text, signature_fragment):
    index = text.find(signature_fragment)
    if index < 0:
        return None
    start = text.find("{", index)
    if start < 0:
        return None
    depth = 0
    for position in range(start, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start : position + 1]
    return None


def parse_phandles(value):
    return {
        int(token, 0)
        for token in re.findall(r"0x[0-9a-fA-F]+|\d+", value)
    }


def phandle_node_bodies(text):
    """Return node bodies indexed by their explicit phandle value."""
    nodes = {}
    for match in re.finditer(r"(?m)^\s*([A-Za-z0-9_@,-]+)\s*\{", text):
        body = node_body(text[match.start() :], match.group(1))
        if body is None:
            continue
        phandle = re.search(r"(?:linux,)?phandle\s*=\s*<([^>]+)>\s*;", body)
        if phandle:
            for value in parse_phandles(phandle.group(1)):
                nodes[value] = body
    return nodes


def check_contract(dts_text, afe_text):
    failures = []

    array = re.search(
        r"static const char \* const mt8163_pin_names\[.*?\]\s*=\s*\{(.*?)\n\};",
        afe_text,
        re.DOTALL,
    )
    if not array:
        failures.append("mt8163_pin_names[] is missing")
        pin_names = set()
    else:
        pin_names = set(re.findall(r'"([^"]+)"', array.group(1)))

    for name in PLAYBACK_STATES + ("cmmclk-mclk",):
        if name not in pin_names:
            failures.append(f"AFE pin-name table lacks {name}")

    for function, symbols in FUNCTION_STATE_SYMBOLS.items():
        body = function_body(afe_text, f"int {function}(")
        if body is None:
            failures.append(f"{function}() is missing")
            continue
        for symbol in symbols:
            if symbol not in body:
                failures.append(f"{function}() no longer selects {symbol}")

    pin_helper = function_body(afe_text, "static int mt8163_afe_pin(")
    if pin_helper is None or "pinctrl_select_state" not in pin_helper:
        failures.append("mt8163_afe_pin() must select the shared pinctrl handle")

    pcm = node_body(dts_text, "mt_soc_dl1_pcm@11220000")
    if pcm is None:
        failures.append("mt_soc_dl1_pcm@11220000 node is missing")
        return failures

    names_match = re.search(r'pinctrl-names\s*=\s*"([^"]+)"\s*;', pcm)
    if not names_match:
        failures.append("mt_soc_dl1_pcm has no pinctrl-names property")
        return failures
    names = names_match.group(1).split(r"\0")

    states = {}
    for index, value in re.findall(r"pinctrl-(\d+)\s*=\s*<([^>]+)>\s*;", pcm):
        states[names[int(index)]] = parse_phandles(value)

    missing = [name for name in PLAYBACK_STATES + ("cmmclk-mclk",) if name not in states]
    for name in missing:
        failures.append(f"mt_soc_dl1_pcm has no pinctrl reference for {name}")
    if missing:
        return failures

    mclk_refs = states["cmmclk-mclk"]
    if not mclk_refs:
        failures.append("cmmclk-mclk has no pinctrl phandle")
        return failures

    phandle_nodes = phandle_node_bodies(dts_text)
    for phandle in mclk_refs:
        body = phandle_nodes.get(phandle)
        if body is None:
            failures.append(f"cmmclk-mclk phandle 0x{phandle:x} is not defined")
        elif "pinmux = <0x7701>" not in body:
            failures.append(
                f"cmmclk-mclk phandle 0x{phandle:x} does not configure "
                "CMMCLK pin 119/function 1"
            )

    for name in PLAYBACK_STATES:
        missing_refs = sorted(mclk_refs - states[name])
        if missing_refs:
            formatted = ", ".join(f"0x{value:x}" for value in missing_refs)
            failures.append(
                f"{name} does not preserve cmmclk-mclk phandle(s): {formatted}"
            )

    return failures


def main():
    failures = []
    if not DTS.exists():
        failures.append(f"missing DTS: {DTS}")
    if not AFE.exists():
        failures.append(f"missing AFE source: {AFE}")
    if not failures:
        failures = check_contract(
            DTS.read_text(encoding="utf-8"), AFE.read_text(encoding="utf-8")
        )

    if failures:
        print("MT8163 MCLK pinctrl contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("MT8163 MCLK pinctrl contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
