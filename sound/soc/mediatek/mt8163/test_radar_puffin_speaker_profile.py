#!/usr/bin/env python3
"""Source contract: selectable speaker codec profile (Radar crossover / Flat).

The Radar two-way crossover must stay the default, and the Flat profile must
replace exactly the left/right DAC biquad coefficients with unity (N0 =
0x7fffff, all other coefficients zero) while leaving the rest of the stock
profile untouched.  The Flat mapping is re-derived here from the C table and
the C helper's documented layout, so a change to either fails this check.
"""

from pathlib import Path
import re


SOURCE = Path(__file__).with_name("mt8163-radar-puffin.c")


def flat(page: int, reg: int, value: int) -> int:
    base = {44: 12, 45: 20}.get(page)
    if base is None or reg < base:
        return value
    off = (reg - base) % 20
    return (0x7F, 0xFF, 0xFF)[off] if off < 3 else 0


def s24(table: dict, page: int, reg: int) -> int:
    x = (table[(page, reg)] << 16) | (table[(page, reg + 1)] << 8) | table[(page, reg + 2)]
    return x - (1 << 24) if x & 0x800000 else x


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")

    rows = [tuple(map(int, m)) for m in re.findall(
        r"RADAR_PUFFIN_PROFILE\((\d+), (\d+), (\d+)\)", text)]
    if len(rows) != 117:
        raise SystemExit(f"expected the 117-value stock profile, found {len(rows)}")

    stock = {(p, r): v for p, r, v in rows}
    table = {(p, r): flat(p, r, v) for p, r, v in rows}
    for page, base in ((44, 12), (45, 20)):
        for biquad in range(3):
            coefs = [s24(table, page, base + 20 * biquad + 4 * i) for i in range(5)]
            if coefs != [0x7FFFFF, 0, 0, 0, 0]:
                raise SystemExit(f"page {page} biquad {biquad} is not unity: {coefs}")
        # The stock Radar crossover is not flat (guards against an all-unity table).
        stock_n0 = s24(stock, page, base)
        if stock_n0 == 0x7FFFFF and s24(stock, page, base + 4) == 0:
            raise SystemExit(f"page {page} stock profile unexpectedly flat")
    for (page, reg), value in stock.items():
        if page not in (44, 45) and table[(page, reg)] != value:
            raise SystemExit(f"non-biquad register {page}/{reg} changed by Flat")

    helper = re.search(r"static unsigned int radar_flat_profile_value\(.*?^}\n",
                       text, re.DOTALL | re.MULTILINE)
    if not helper:
        raise SystemExit("radar_flat_profile_value not found")
    body = helper.group(0)
    for fragment in ("page == 44", "base = 12", "page == 45", "base = 20",
                     "% 20", "0x7f, 0xff, 0xff", "return stock;"):
        if fragment not in body:
            raise SystemExit(f"flat helper layout changed: missing {fragment!r}")

    if 'SOC_ENUM_EXT("Speaker Codec Profile"' not in text:
        raise SystemExit("Speaker Codec Profile control is not registered")
    if 'radar_speaker_profiles[] = { "Radar", "Flat" }' not in text:
        raise SystemExit("profile enum must be Radar (default, index 0) then Flat")
    if "RADAR_SPEAKER_PROFILE_RADAR = 0" not in text:
        raise SystemExit("Radar must remain profile 0 (the zero-initialised default)")
    if "priv->speaker_profile =" in text.split("static int radar_card_probe", 1)[1].split("\n}\n", 1)[0]:
        raise SystemExit("probe must not override the Radar default")
    if "radar_speaker_apply_profile(component, priv->speaker_profile)" not in text:
        raise SystemExit("speaker prepare must apply the selected profile")

    print("radar_puffin_speaker_profile: Radar default, Flat unity biquads PASS")


if __name__ == "__main__":
    main()
