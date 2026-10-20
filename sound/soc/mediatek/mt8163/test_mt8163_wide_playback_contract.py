#!/usr/bin/env python3
"""Source contract for 24-bit speaker playback on the MT8163 DL1 path.

Userspace owns speaker volume, so a 16-bit stream loses one bit per 6 dB of
attenuation.  S24_LE/S32_LE must reach the TLV320AIC3204 intact: the DL1 fetch
format, the O00/O01/O03/O04 24-bit interconnect bits and the internal DAC word
width all follow the stream format, while S16 keeps the 3.18 oracle values.
"""

from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
AFE = (HERE / "mt8163-afe.c").read_text(encoding="utf-8")
MACHINE = (HERE / "mt8163-radar-puffin.c").read_text(encoding="utf-8")


def function(text: str, name: str) -> str:
    match = re.search(rf"^static [^\n]*\b{name}\(.*?^}}\n", text,
                      re.DOTALL | re.MULTILINE)
    if not match:
        raise SystemExit(f"{name} not found")
    return match.group(0)


def require(condition: bool, what: str) -> None:
    if not condition:
        raise SystemExit(f"wide playback contract: {what}")


def main() -> None:
    formats = re.search(r"#define MT8163_PCM_FORMATS(.*?)\n\n", AFE, re.DOTALL)
    require(formats is not None, "MT8163_PCM_FORMATS missing")
    for fmt in ("S16_LE", "S24_LE", "S32_LE"):
        require(f"SNDRV_PCM_FMTBIT_{fmt}" in formats.group(1), f"{fmt} not advertised")
    require(AFE.count(".formats = MT8163_PCM_FORMATS") == 2,
            "PCM hardware and DAI must both advertise MT8163_PCM_FORMATS")
    require("(1 << 16)" in AFE and "(3 << 16)" in AFE,
            "DL1 fetch formats 1 (S24_LE) and 3 (S32_LE) missing")

    prepare = function(AFE, "mt8163_pcm_prepare")
    require("AFE_DL1_FORMAT_MASK, fetch)" in prepare, "DL1 fetch format is not stream-driven")
    require("SNDRV_PCM_FORMAT_S32_LE ?" in prepare and "AFE_DL1_FORMAT_24BIT_MSB" in prepare,
            "S32_LE must select the MSB-aligned fetch")
    require("AFE_DL1_FORMAT_24BIT_LSB : AFE_DL1_FORMAT_16BIT" in prepare,
            "S24_LE must select the LSB-aligned fetch and S16 stay 16-bit")
    require("wide ? AFE_OUTPUT_FORMAT_24BIT_MASK : 0" in prepare,
            "interconnect 24-bit bits must follow the stream width")
    require("mt8163_afe_i2s_dac_config(afe, wide)" in prepare,
            "internal DAC word width must follow the stream width")
    require("AFE_I2S_WORD_32" in prepare, "external I2S slots must stay 32-bit")

    dac = function(AFE, "mt8163_afe_i2s_dac_config")
    require("(wide ? AFE_I2S_WORD_32 : 0)" in dac,
            "16-bit streams must keep the 3.18 16-bit internal DAC words")

    hw = function(AFE, "mt8163_pcm_hw_params")
    require("mt8163_pcm_format_wide(params_format(params))" in hw,
            "hw_params must accept the wide formats")

    speaker = function(MACHINE, "radar_speaker_hw_params")
    for fmt in ("S16_LE", "S24_LE", "S32_LE"):
        require(f"SNDRV_PCM_FORMAT_{fmt}" in speaker, f"machine driver rejects {fmt}")
    print("mt8163 wide playback contract: PASS")


if __name__ == "__main__":
    main()
