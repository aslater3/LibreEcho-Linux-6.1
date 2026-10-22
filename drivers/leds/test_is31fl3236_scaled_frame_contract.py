#!/usr/bin/env python3
"""Source contract for the IS31FL3236 current-scaled frame (LibreEcho UI #364).

At sleep-light brightness the 8-bit PWM alone has only a couple of usable
steps, so the sleep pulse visibly steps.  Each IS31FL3236 output also has a
current setting (SL bits of its LED control register: Imax, /2, /3, /4).  The
``scaled_frame`` attribute lets userspace set PWM and current together.

The contract:
- ``scaled_frame`` takes exactly 36 PWM bytes then 36 scale bytes (144 hex
  characters, one optional newline) and rejects scales above 3;
- LED control registers are written with OUT kept on and SL in bits 2:1, at
  the same channel offset as the PWM registers, before the single Update
  write that latches PWM and control together;
- unchanged scales are skipped, and the cache is only trusted after a
  successful Update, so a failed write rewrites every control register;
- the legacy ``frame`` attribute keeps its 72-character format and drives
  every channel at full current.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/leds/leds-is31fl32xx.c"


def function(source: str, name: str) -> str:
    match = re.search(
        r"\n(?:static )?[^\n]*\b" + re.escape(name) + r"\([^)]*\)\n\{.*?\n\}\n",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"could not isolate {name}")
    return match.group(0)


class Is31fl3236ScaledFrameContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = DRIVER.read_text(encoding="utf-8")

    def test_attribute_is_published(self) -> None:
        self.assertIn("static DEVICE_ATTR_RW(scaled_frame);", self.source)
        attrs = re.search(
            r"static struct attribute \*is31fl3236_attrs\[\] = \{(.*?)\};",
            self.source,
            re.DOTALL,
        )
        if attrs is None:
            self.fail("could not isolate the attribute list")
        self.assertIn("&dev_attr_frame.attr", attrs.group(1))
        self.assertIn("&dev_attr_scaled_frame.attr", attrs.group(1))

    def test_control_register_encoding(self) -> None:
        self.assertRegex(self.source, r"#define IS31FL3236_CTRL_OUT_ON\s+BIT\(0\)")
        self.assertRegex(self.source, r"#define IS31FL3236_CTRL_SL_SHIFT\s+1\b")
        self.assertRegex(self.source, r"#define IS31FL3236_SCALE_MAX\s+3\b")

    def test_update_writes_control_then_pwm_then_one_latch(self) -> None:
        body = function(self.source, "is31fl3236_update_frame")
        ctrl = body.index("cdef->led_control_register_base + channel")
        pwm = body.index("cdef->pwm_register_base + channel")
        latch = body.index("cdef->pwm_update_reg, 0")
        self.assertLess(ctrl, pwm)
        self.assertLess(pwm, latch)
        self.assertEqual(body.count("pwm_update_reg"), 1)
        self.assertIn("IS31FL3236_CTRL_OUT_ON |", body)
        self.assertIn("scale[i] << IS31FL3236_CTRL_SL_SHIFT", body)
        self.assertEqual(
            body.count("(i + priv->channel_offset) % ARRAY_SIZE(priv->frame)"), 2
        )

    def test_scale_cache_is_trusted_only_after_latch(self) -> None:
        body = function(self.source, "is31fl3236_update_frame")
        self.assertIn("priv->scale_valid && priv->scale[i] == scale[i]", body)
        self.assertIn("priv->scale_valid = false;", body)
        latch = body.index("cdef->pwm_update_reg, 0")
        valid = body.index("priv->scale_valid = true;")
        self.assertLess(latch, valid)
        self.assertLess(latch, body.index("memcpy(priv->scale, scale"))
        self.assertLess(latch, body.index("memcpy(priv->frame, frame"))

    def test_scaled_store_validates_length_and_range(self) -> None:
        body = function(self.source, "scaled_frame_store")
        self.assertIn("ARRAY_SIZE(priv->frame) + ARRAY_SIZE(priv->scale)", body)
        self.assertIn("is31fl3236_parse_hex(buf, count, data, sizeof(data))", body)
        self.assertIn("scale[i] > IS31FL3236_SCALE_MAX", body)
        self.assertIn("-EINVAL", body)
        self.assertLess(
            body.index("IS31FL3236_SCALE_MAX"), body.index("mutex_lock")
        )

    def test_parse_hex_allows_only_one_newline(self) -> None:
        body = function(self.source, "is31fl3236_parse_hex")
        self.assertIn("buf[hex_len - 1] == '\\n'", body)
        self.assertIn("hex_len != len * 2", body)

    def test_legacy_frame_runs_at_full_current(self) -> None:
        body = function(self.source, "frame_store")
        self.assertIn("static const u8 full[ARRAY_SIZE(priv->scale)];", body)
        self.assertIn("is31fl3236_update_frame(priv, frame, full)", body)
        self.assertIn("is31fl3236_parse_hex(buf, count, frame, sizeof(frame))", body)

    def test_scaled_show_reports_pwm_then_scale(self) -> None:
        body = function(self.source, "scaled_frame_show")
        self.assertLess(
            body.index("bin2hex(buf, priv->frame"),
            body.index("bin2hex(buf + len, priv->scale"),
        )


if __name__ == "__main__":
    unittest.main()
