#!/usr/bin/env python3
"""Source contract for safe IS31FL32xx controller initialization.

The LibreEcho IS31FL3236 is reset before its channel-enable registers and
software shutdown state are configured.  The reset command does not provide a
source-visible guarantee that the PWM frame is zero, so initialization must
explicitly stage an all-off frame before exposing any enabled channels.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/leds/leds-is31fl32xx.c"


class Is31fl32xxInitContractTests(unittest.TestCase):
    def test_init_stages_all_off_pwm_frame_before_channel_enable(self) -> None:
        source = DRIVER.read_text(encoding="utf-8")
        match = re.search(
            r"static int is31fl32xx_init_regs\(.*?\n}\n\n"
            r"/\*\n \* LibreEcho's userspace",
            source,
            re.DOTALL,
        )
        if match is None:
            self.fail("could not isolate controller init function")
        init = match.group(0)

        reset_end = init.index("ret = is31fl32xx_reset_regs(priv);")
        enable_start = init.index("/*\n\t * Set enable bit for all channels.")
        self.assertLess(reset_end, enable_start)

        clear = init[reset_end:enable_start]
        self.assertIn("if (!cdef->reset_func)", clear)
        self.assertRegex(clear, r"for \(i = 0; i < cdef->channels; i\+\+\)")
        self.assertRegex(
            clear,
            r"is31fl32xx_write\(priv,\s*"
            r"cdef->pwm_register_base\s*\+\s*i,\s*0x00\)",
        )
        self.assertIn(
            "is31fl32xx_write(priv, cdef->pwm_update_reg, 0)",
            clear,
        )


if __name__ == "__main__":
    unittest.main()
