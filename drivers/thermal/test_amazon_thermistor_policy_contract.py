#!/usr/bin/env python3
"""Regression contract for the Radar-Puffin thermal policy.

Two behavioural contracts are asserted here, because the ARM32 workflow only
compiles these paths and would not notice either being undone.

1. The temperature API split. A thermal zone's temperature is defined to be a
   temperature, and every generic consumer -- a trip point, a governor, the UI,
   anyone with cat -- reads it as one. So the zone's .get_temp callback must
   return the raw thermistor reading. The vendor virtual-sensor transform
   (weight/1000 * EMA(raw - offset)) does not produce a temperature at all: it
   produces one sensor's weighted contribution to an aggregate that does not
   exist in this tree. It is retained only for the device-specific sysfs `temp`
   attribute, which vendor tooling expects to keep returning that exact number.

   The failure this prevents is a later refactor quietly reunifying the two
   callbacks. That reads as a cleanup -- two functions doing nearly the same
   thing -- and it would silently put a number that is not a temperature back
   in front of every trip point and governor, at which point thermal throttling
   stops working and nothing fails loudly.

2. The zone policy values. Polling is the only path by which a trip is ever
   noticed on this board (mtk_thermal implements .get_temp and no .set_trips),
   so the passive polling delay is the entire reaction time, and the trip
   temperatures are the policy itself. Both trips were previously hex literals
   that disagreed with their own comments, so the values are asserted here in
   the decimal millicelsius the bindings actually specify.

Run from the kernel source root:

    python3 drivers/thermal/test_amazon_thermistor_policy_contract.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DRIVER = ROOT / "drivers/thermal/amazon_virtual_sensor_thermistor.c"
DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"

# The transform's three ingredients. None may appear in the raw reader.
TRANSFORM_TERMS = ("offset", "filter", "weight")


def strip_comments(text):
    """Remove C/dts block and line comments.

    Essential rather than cosmetic: the comments in both files discuss the
    offset, the weights and the literal trip temperatures, so matching against
    uncommented source would produce both false passes and false failures.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def block_after(text, anchor):
    """Return the brace-matched block that follows anchor, or None."""
    index = text.find(anchor)
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
                return text[start:position + 1]
    return None


def calls(body, function):
    """True if body calls function, matching the name exactly.

    Written as a bounded match because amazon_thermistor_read_temp is a strict
    prefix of amazon_thermistor_read_temp_raw; a substring test would report the
    raw reader as a call to the transform and pass a genuinely broken split.
    """
    return re.search(re.escape(function) + r"\s*\(", body) is not None


def check_driver(failures):
    if not DRIVER.exists():
        failures.append(f"missing source file: {DRIVER}")
        return
    text = strip_comments(DRIVER.read_text(encoding="utf-8"))

    zone_callback = block_after(text, "static int amazon_thermistor_get_temp(")
    if zone_callback is None:
        failures.append("amazon_thermistor_get_temp() not found")
    else:
        if not calls(zone_callback, "amazon_thermistor_read_temp_raw"):
            failures.append(
                "amazon_thermistor_get_temp() does not read the raw "
                "thermistor temperature; the thermal zone must report a real "
                "temperature to trip points and governors"
            )
        if calls(zone_callback, "amazon_thermistor_read_temp"):
            failures.append(
                "amazon_thermistor_get_temp() applies the vendor "
                "virtual-sensor transform; that value is a weighted "
                "contribution, not a temperature, and trip points would be "
                "compared against it"
            )

    sysfs = block_after(text, "static ssize_t temp_show(")
    if sysfs is None:
        failures.append("temp_show() not found")
    elif not calls(sysfs, "amazon_thermistor_read_temp"):
        failures.append(
            "temp_show() no longer applies the vendor virtual-sensor "
            "transform; the device-specific temp attribute is expected by "
            "vendor tooling to keep returning that exact number"
        )

    raw_reader = block_after(
        text, "static int amazon_thermistor_read_temp_raw(")
    if raw_reader is None:
        failures.append("amazon_thermistor_read_temp_raw() not found")
    else:
        for term in TRANSFORM_TERMS:
            if term in raw_reader:
                failures.append(
                    f"amazon_thermistor_read_temp_raw() references '{term}'; "
                    "the raw reader must return the thermistor curve result "
                    "untransformed"
                )

    transform = block_after(text, "static int amazon_thermistor_read_temp(")
    if transform is None:
        failures.append("amazon_thermistor_read_temp() not found")
    else:
        for term in TRANSFORM_TERMS:
            if term not in transform:
                failures.append(
                    f"amazon_thermistor_read_temp() no longer references "
                    f"'{term}'; the vendor transform is offset, EMA filter and "
                    "weight together"
                )

    if not re.search(r"\.get_temp\s*=\s*amazon_thermistor_get_temp", text):
        failures.append(
            "the thermal zone ops no longer bind .get_temp to "
            "amazon_thermistor_get_temp"
        )


def property_value(block, name):
    match = re.search(re.escape(name) + r"\s*=\s*<([^>]*)>", block)
    return match.group(1).strip() if match else None


def check_trip(failures, trips, node, expected_temp, expected_type):
    block = block_after(trips, node)
    if block is None:
        failures.append(f"thermal trip '{node}' not found")
        return
    for name, expected in (("temperature", expected_temp),
                           ("hysteresis", "2000")):
        actual = property_value(block, name)
        if actual is None:
            failures.append(f"trip '{node}' has no {name}")
        elif actual != expected:
            failures.append(
                f"trip '{node}' {name} is {actual}, expected {expected} "
                "(millicelsius, decimal)"
            )
    if not re.search(r'type\s*=\s*"' + expected_type + r'"', block):
        failures.append(f"trip '{node}' is not type \"{expected_type}\"")


def check_dts(failures):
    if not DTS.exists():
        failures.append(f"missing device tree: {DTS}")
        return
    text = strip_comments(DTS.read_text(encoding="utf-8"))

    zone = block_after(text, "cpu-thermal")
    if zone is None:
        failures.append("cpu-thermal zone not found")
        return

    # Polling is the only path by which a trip is noticed, so the passive
    # delay is the entire reaction time.
    for name, expected in (("polling-delay-passive", "250"),
                           ("polling-delay", "1000"),
                           ("sustainable-power", "2000")):
        actual = property_value(zone, name)
        if actual is None:
            failures.append(f"cpu-thermal has no {name}")
        elif actual != expected:
            failures.append(
                f"cpu-thermal {name} is {actual}, expected {expected}"
            )

    trips = block_after(zone, "trips")
    if trips is None:
        failures.append("cpu-thermal has no trips node")
    else:
        check_trip(failures, trips, "trip-point0", "68000", "passive")
        check_trip(failures, trips, "trip-point1", "80000", "passive")
        check_trip(failures, trips, "cpu-critical", "117000", "critical")

    # step_wise regulates toward the target trip; binding the cooling devices
    # to the lower threshold instead would cap the clock while merely warm.
    maps = block_after(zone, "cooling-maps")
    if maps is None:
        failures.append("cpu-thermal has no cooling-maps node")
    elif not re.search(r"trip\s*=\s*<&target>", maps):
        failures.append(
            "the cooling map does not bind the target trip; step_wise "
            "regulates toward target, not toward the lower threshold"
        )


def main():
    failures = []
    check_driver(failures)
    check_dts(failures)

    if failures:
        print("Radar-Puffin thermal policy contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("Radar-Puffin thermal policy contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
