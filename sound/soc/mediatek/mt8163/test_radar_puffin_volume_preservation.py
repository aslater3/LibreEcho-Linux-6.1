#!/usr/bin/env python3
"""Source contract for preserving user PCM volume during speaker teardown."""

from pathlib import Path
import re


SOURCE = Path(__file__).with_name("mt8163-radar-puffin.c")


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        r"static int radar_speaker_safe\(.*?^}\n",
        text,
        re.DOTALL | re.MULTILINE,
    )
    if not match:
        raise SystemExit("radar_speaker_safe not found")
    safe = match.group(0)

    write_sites = re.findall(
        r"snd_soc_component_(?:write|update_bits)\([^;]*RADAR_(?:L|R)DACVOL",
        text,
        re.DOTALL,
    )
    if write_sites:
        raise SystemExit(
            "machine driver must not overwrite user PCM volume registers"
        )

    required_mutes = (
        "snd_soc_dai_digital_mute",
        "RADAR_HPLGAIN",
        "RADAR_HPRGAIN",
        "RADAR_DAC_MFP2_MUTE",
        "mt8163_afe_select_amp(&priv->afe_pdev->dev, false)",
    )
    missing = [fragment for fragment in required_mutes if fragment not in safe]
    if missing:
        raise SystemExit(
            "speaker teardown lost required safety mute: " + ", ".join(missing)
        )

    print("radar_puffin_volume_preservation: teardown preserves PCM volume PASS")


if __name__ == "__main__":
    main()
