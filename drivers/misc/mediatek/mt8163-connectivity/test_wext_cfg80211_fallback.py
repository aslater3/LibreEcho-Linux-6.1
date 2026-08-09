#!/usr/bin/env python3
"""Source contracts for MT8163 WEXT userspace over cfg80211."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[4]
WEXT_CORE = ROOT / "net/wireless/wext-core.c"
GL_INIT = ROOT / (
    "drivers/misc/mediatek/mt8163-connectivity/conn_soc/drv_wlan/"
    "mt_wifi/wlan/os/linux/gl_init.c"
)
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"


class WextCfg80211FallbackTests(unittest.TestCase):
    def test_mt8163_enables_cfg80211_wext_compatibility(self) -> None:
        config = DEFCONFIG.read_text(encoding="utf-8")
        self.assertIn("CONFIG_CFG80211_WEXT=y", config)

    def test_standard_handlers_fall_back_to_cfg80211(self) -> None:
        source = WEXT_CORE.read_text(encoding="utf-8")
        get_handler = source.split("static iw_handler get_handler", 1)[1].split(
            "static int ioctl_standard_iw_point", 1
        )[0]
        self.assertIn("cfg_handlers", get_handler)
        self.assertIn("if (handler)", get_handler)
        self.assertIn("cfg_handlers->standard[index]", get_handler)

    def test_driver_does_not_override_cfg80211_standard_handlers(self) -> None:
        source = GL_INIT.read_text(encoding="utf-8")
        self.assertNotIn("prWiphy->wext = &wext_handler_def", source)

    def test_private_handlers_remain_device_owned(self) -> None:
        source = WEXT_CORE.read_text(encoding="utf-8")
        get_handler = source.split("static iw_handler get_handler", 1)[1].split(
            "static int ioctl_standard_iw_point", 1
        )[0]
        private_block = get_handler.split(
            "/* Private commands remain owned by the device handler table. */", 1
        )[1]
        self.assertIn("handlers->private[index]", private_block)
        self.assertNotIn("cfg_handlers->private[index]", private_block)


if __name__ == "__main__":
    unittest.main()
