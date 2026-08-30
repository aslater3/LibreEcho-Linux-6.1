#!/usr/bin/env python3
"""Regression contract for safe MT8163 MUSB role transitions.

The MUSB mode sysfs attribute invokes the platform callback while musb->lock is
held with IRQs disabled.  This contract keeps that callback non-blocking and
requires the blocking role transition to run from a workqueue.  It also checks
the host-to-device path restores a gadget pull-up that the host transition
removed.

This is a host-side source contract; it does not replace runtime USB
host-to-device testing on hardware.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "drivers/usb/musb/mediatek.c"


def function_body(text, signature_fragment):
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
                return text[start : position + 1]
    return None


def main():
    if not DRIVER.exists():
        print(f"MT8163 role-switch contract: FAIL\n  - {DRIVER} is missing")
        return 1

    text = DRIVER.read_text(encoding="utf-8")
    failures = []
    callback = function_body(text, "static int mtk_musb_set_mode(")
    worker = function_body(text, "static void mtk_musb_mode_work(")
    transition = function_body(text, "static int mtk_otg_switch_set(")
    restore = function_body(text, "static void mtk_musb_restore_gadget_pullup(")
    leave_host = function_body(text, "static void mt8163_musb_leave_host(")
    rescan = function_body(text, "static void mt8163_musb_rescan_port(")
    clear_synthetic = function_body(
        text, "static void mt8163_musb_clear_synthetic_connection("
    )

    if callback is None:
        failures.append("mtk_musb_set_mode() not found")
    else:
        for call in ("usleep_range", "msleep", "mutex_lock", "mtk_otg_switch_set("):
            if call in callback:
                failures.append(
                    f"mtk_musb_set_mode() must not call blocking role code: {call}"
                )
        if "schedule_work" not in callback:
            failures.append(
                "mtk_musb_set_mode() must defer the role transition with schedule_work()"
            )
        if "mode_pending" not in callback:
            failures.append("mtk_musb_set_mode() must publish a pending role")

    if worker is None:
        failures.append("mtk_musb_mode_work() not found")
    else:
        for fragment in ("mtk_otg_switch_set", "mode_pending"):
            if fragment not in worker:
                failures.append(
                    f"mtk_musb_mode_work() lacks deferred transition step: {fragment}"
                )

    if transition is None:
        failures.append("mtk_otg_switch_set() not found")
    elif "case USB_ROLE_HOST:" not in transition or "case USB_ROLE_DEVICE:" not in transition:
        failures.append("role transition must retain both host and device cases")
    elif "mutex_lock(&glue->role_lock)" not in transition:
        failures.append("role transition must serialize concurrent requests")
    elif "mtk_musb_restore_gadget_pullup" not in transition:
        failures.append(
            "device-role transition must restore the requested gadget pull-up"
        )
    elif "mt8163_musb_leave_host" not in transition:
        failures.append("role transition must cleanly leave the host role")

    if restore is None:
        failures.append("mtk_musb_restore_gadget_pullup() not found")
    else:
        for fragment in (
            "musb->softconnect",
            "MUSB_POWER_SOFTCONN",
            "musb_writeb",
            "spin_lock_irqsave",
        ):
            if fragment not in restore:
                failures.append(
                    f"gadget pull-up restore lacks required state/register step: {fragment}"
                )

    if clear_synthetic is None:
        failures.append("synthetic connection cleanup helper not found")
    else:
        for fragment in ("spin_lock_irqsave", "synthetic_connection",
                         "USB_PORT_STAT_ENABLE"):
            if fragment not in clear_synthetic:
                failures.append(
                    f"synthetic connection cleanup lacks required guard: {fragment}"
                )

    if leave_host is None:
        failures.append("host-exit cleanup helper not found")
    else:
        for fragment in ("spin_lock_irqsave", "musb_root_disconnect",
                         "USB_PORT_STAT_ENABLE"):
            if fragment not in leave_host:
                failures.append(
                    f"host-exit cleanup lacks required step: {fragment}"
                )

    if rescan is None:
        failures.append("mt8163_musb_rescan_port() not found")
    else:
        for fragment in ("spin_lock_irqsave", "synthetic_connection",
                         "mod_delayed_work"):
            if fragment not in rescan:
                failures.append(
                    f"synthetic rescan lacks required state step: {fragment}"
                )

    if "cancel_work_sync(&glue->mode_work)" not in text:
        failures.append("role-switch worker is not cancelled during teardown")
    if "cancel_delayed_work_sync(&glue->rescan_cleanup_work)" not in text:
        failures.append("synthetic rescan cleanup is not cancelled during teardown")
    if "cancel_delayed_work_sync(&glue->diag_work)" not in text:
        failures.append("recurring diagnostics are not cancelled during child teardown")
    if "INIT_WORK(&glue->mode_work, mtk_musb_mode_work)" not in text:
        failures.append("role-switch worker is not initialized")
    if "INIT_DELAYED_WORK(&glue->rescan_cleanup_work," not in text:
        failures.append("synthetic rescan cleanup is not initialized")

    if failures:
        print("MT8163 role-switch contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("MT8163 role-switch contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
