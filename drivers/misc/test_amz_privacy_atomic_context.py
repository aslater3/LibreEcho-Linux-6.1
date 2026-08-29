#!/usr/bin/env python3
"""Regression contract for the amz_privacy mute-key handler.

The input core calls handler->event() from input_pass_values() with
dev->event_lock held and interrupts disabled, so amz_privacy_input_event()
runs in atomic context. Toggling privacy sleeps at every step: it takes the
amz_privacy_lock mutex, drives the lines with the gpiod_*_cansleep() family,
and amz_privacy_assert_latch() waits on the hardware with usleep_range().
Doing that work in the handler schedules while atomic, which can stall or
crash the input path.

This contract asserts:

1. amz_privacy_input_event() calls nothing that can sleep, and in particular
   does not toggle privacy itself.
2. It hands the press to a workqueue instead.
3. The deferred worker is the one that takes the mutex and toggles.
4. Presses are counted, not flagged. schedule_work() on an already-queued
   item is a no-op, so a flag would swallow a second press arriving before
   the first ran -- and for a toggle, a swallowed press leaves privacy in
   the opposite state to the one the user asked for.
5. Driver removal drains the queued work, so a toggle cannot run against a
   device that is going away.
6. The input ID table matches the PMIC key device identity, not every device
   that happens to advertise KEY_MUTE.

Run from the kernel source root:

    python3 drivers/misc/test_amz_privacy_atomic_context.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/misc/amz_privacy.c"

# Anything here would sleep, which is illegal under dev->event_lock.
SLEEPING_CALLS = (
    "mutex_lock",
    "__amz_priv_trigger",
    "usleep_range",
    "msleep",
    "_cansleep",
    "amz_privacy_assert_latch",
)


def function_body(text, signature_fragment):
    """Return the body of the function whose signature contains the fragment.

    Brace-matched from the opening brace, so nested blocks are included and
    a following function is not.
    """
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
                return text[start:position + 1]
    return None


def main():
    failures = []

    if not DRIVER.exists():
        print(f"amz_privacy atomic-context contract: FAIL\n  - {DRIVER} is missing")
        return 1
    text = DRIVER.read_text(encoding="utf-8")

    handler = function_body(text, "static void amz_privacy_input_event(")
    if handler is None:
        failures.append("amz_privacy_input_event() not found")
    else:
        for call in SLEEPING_CALLS:
            if call in handler:
                failures.append(
                    f"amz_privacy_input_event() calls {call}, which can sleep; "
                    "the input core holds dev->event_lock with interrupts "
                    "disabled here"
                )
        if "schedule_work" not in handler:
            failures.append(
                "amz_privacy_input_event() does not defer the toggle with "
                "schedule_work()"
            )
        if "atomic_inc" not in handler:
            failures.append(
                "amz_privacy_input_event() does not count the press with "
                "atomic_inc(); schedule_work() on an already-queued item is a "
                "no-op, so a repeat press would be swallowed"
            )

    ids = function_body(text, "static const struct input_device_id amz_privacy_ids[]")
    if ids is None:
        failures.append("amz_privacy_ids[] not found")
    else:
        if "INPUT_DEVICE_ID_MATCH_NAME" not in ids:
            failures.append(
                "amz_privacy_ids[] must match the PMIC input device name so "
                "generic HID KEY_MUTE devices are rejected"
            )
        if not re.search(r'\.name\s*=\s*"mtk-pmic-keys"', ids):
            failures.append(
                "amz_privacy_ids[] must identify the mtk-pmic-keys input device"
            )

    worker = function_body(text, "static void amz_privacy_toggle_work(")
    if worker is None:
        failures.append("amz_privacy_toggle_work() not found")
    else:
        if "atomic_xchg" not in worker:
            failures.append(
                "amz_privacy_toggle_work() does not claim the counted presses "
                "with atomic_xchg()"
            )
        if "mutex_lock" not in worker or "__amz_priv_trigger" not in worker:
            failures.append(
                "amz_privacy_toggle_work() must be the caller that takes "
                "amz_privacy_lock and toggles privacy"
            )

    if not re.search(r"DECLARE_WORK\s*\(\s*\w+\s*,\s*amz_privacy_toggle_work\s*\)",
                     text):
        failures.append("no work item is declared for amz_privacy_toggle_work()")

    remove = function_body(text, "static int amz_privacy_remove(")
    if remove is None:
        failures.append("amz_privacy_remove() not found")
    elif "cancel_work_sync" not in remove:
        failures.append(
            "amz_privacy_remove() does not cancel_work_sync() the toggle, so a "
            "queued press could run against a device being removed"
        )

    if failures:
        print("amz_privacy atomic-context contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("amz_privacy atomic-context contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
