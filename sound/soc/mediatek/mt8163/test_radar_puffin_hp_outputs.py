#!/usr/bin/env python3
"""Source contract for Radar-Puffin's two-output crossover policy."""

from pathlib import Path
import re


SOURCE = Path(__file__).with_name("mt8163-radar-puffin.c")


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        r"static int radar_speaker_prepare\(.*?^}\n",
        text,
        re.DOTALL | re.MULTILINE,
    )
    if not match:
        raise SystemExit("radar_speaker_prepare not found")
    prepare = match.group(0)

    forbidden = (
        "priv->speaker_mono ? RADAR_HP_DRIVER_MUTE : 0",
        "one-channel path is HPR-only",
    )
    present = [fragment for fragment in forbidden if fragment in prepare]
    if present:
        raise SystemExit(
            "speaker crossover must not mute HPL: " + ", ".join(present)
        )

    for register in ("RADAR_HPLGAIN", "RADAR_HPRGAIN"):
        pattern = re.compile(
            rf"snd_soc_component_update_bits\(component, {register},"
            rf".*?RADAR_HP_DRIVER_MUTE, 0\);",
            re.DOTALL,
        )
        if not pattern.search(prepare):
            raise SystemExit(f"{register} is not explicitly unmuted")

    if "params_channels(params) != 2" not in text:
        raise SystemExit("speaker hw_params must reject non-stereo transport")
    if "speaker_mono" in text:
        raise SystemExit("mono speaker state must not remain in the production path")

    print("radar_puffin_hp_outputs: HPL and HPR unmuted PASS")


if __name__ == "__main__":
    main()