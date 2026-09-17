#!/usr/bin/env python3
"""Source contract tests for the Radar-Puffin mute lamp control."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/misc/amz_privacy.c"
KCONFIG = ROOT / "drivers/misc/Kconfig"
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"
DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"


def body_of(source: str, signature: str, end: str) -> str:
    start = source.index(signature)
    stop = source.index(end, start)
    return source[start:stop]


class MuteLampContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.driver = DRIVER.read_text()

    def test_control_is_built_and_exported(self) -> None:
        self.assertIn("CONFIG_AMZ_PRIVACY=y", DEFCONFIG.read_text())
        # Explicit mode, not DEVICE_ATTR_RW: the mute daemon may reach this
        # through group ownership rather than as root, and 0644 would refuse it.
        self.assertIn(
            "static DEVICE_ATTR(mute_lamp, 0664, mute_lamp_show, mute_lamp_store);",
            self.driver)
        self.assertNotIn("DEVICE_ATTR_RW(mute_lamp)", self.driver)
        self.assertIn("&dev_attr_mute_lamp.attr,", self.driver)
        self.assertIn("mute_lamp", KCONFIG.read_text())

    def test_lamp_helper_drives_the_lines_the_latch_drives(self) -> None:
        lamp = body_of(self.driver, "static void amz_privacy_set_mute_lamp(",
                       "\n}\n")
        self.assertIn("gpiod_set_raw_value_cansleep(priv->privacy_gpio, on);",
                      lamp)
        self.assertIn("amz_privacy_set_bright_state(priv, on);", lamp)
        self.assertIn("priv->mute_lamp = !!on;", lamp)

    def test_board_has_no_separate_lamp_line(self) -> None:
        # The control exists because the lamp shares the privacy indication.
        # A board that grows its own lamp line must revisit this driver.
        node = body_of(DTS.read_text(), "amz_privacy {", "};")
        self.assertIn("hw_latch", node)
        self.assertNotIn("mute-gpio", node)

    def test_store_refuses_to_clear_a_latched_lamp(self) -> None:
        store = body_of(self.driver, "static ssize_t mute_lamp_store(",
                        "\n}\n")
        self.assertIn("if (priv->disabled) {", store)
        self.assertIn("if (priv->cur_priv && !on) {", store)
        self.assertEqual(store.count("ret = -EBUSY;"), 2)
        # Refusing the physical clear must still record the software unmute, or
        # the latch release would re-assert a lamp nobody asked for.
        latched = store[store.index("if (priv->cur_priv && !on) {"):]
        latched = latched[:latched.index("-EBUSY")]
        self.assertIn("priv->mute_lamp = false;", latched)

    def test_latch_release_reasserts_a_software_mute_lamp(self) -> None:
        trigger = body_of(self.driver, "static int __amz_priv_trigger(",
                          "int amz_priv_trigger(int on)")
        self.assertIn("if (!priv->cur_priv && priv->mute_lamp)", trigger)
        self.assertIn("amz_privacy_set_mute_lamp(priv, 1);", trigger)

    def test_latched_boards_are_refused(self) -> None:
        # A latched board runs privacy as an assert/acknowledge/ deassert
        # handshake this control does not perform, so it must not drive it.
        store = body_of(self.driver, "static ssize_t mute_lamp_store(",
                        "\n}\n")
        self.assertIn("if (priv->hw_latch) {", store)
        self.assertIn("ret = -EOPNOTSUPP;", store)
        show = body_of(self.driver, "static ssize_t mute_lamp_show(",
                       "\n}\n")
        self.assertIn("if (priv->hw_latch)", show)
        self.assertIn("-EOPNOTSUPP", show)

    def test_shutdown_mode_does_not_restore_the_lamp(self) -> None:
        shutdown = body_of(self.driver, "static ssize_t shutdown_dialog_state_store(",
                           "\n}\n")
        self.assertLess(shutdown.index("priv->mute_lamp = false;"),
                        shutdown.index("__amz_priv_trigger(priv, 0);"),
                        "the lamp request must be dropped before the trigger "
                        "that deasserts the outputs")

    def test_privacy_trigger_still_cannot_leave_privacy(self) -> None:
        store = body_of(self.driver, "static ssize_t privacy_trigger_store(",
                        "\n}\n")
        self.assertIn("if (value == 1 && !priv->cur_timer_on) {", store)
        self.assertNotIn("__amz_priv_trigger(priv, 0);", store)
        self.assertNotIn("amz_privacy_set_mute_lamp", store)

    def test_lamp_control_does_not_enter_the_latch(self) -> None:
        lamp = body_of(self.driver, "static void amz_privacy_set_mute_lamp(",
                       "\n}\n")
        store = body_of(self.driver, "static ssize_t mute_lamp_store(",
                        "\n}\n")
        for forbidden in ("__amz_priv_trigger", "priv->cur_priv =",
                          "amz_privacy_call_callbacks", "cur_timer_on"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, lamp + store)


if __name__ == "__main__":
    unittest.main()
