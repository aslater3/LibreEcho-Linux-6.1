#!/usr/bin/env python3
"""Regression contract for LibreEcho MT8163 USB console diagnostics.

Issue #8: unconditional rate-limited MUSB/FFS "diag" prints flooded the
production UART (~400 KB/h during ADB activity), masking real faults.

This contract asserts:

1. No MT8163 USB diagnostic print site remains at info level
   (dev_info/pr_info/dev_info_ratelimited/pr_info_ratelimited).
2. Every MT8163 USB diagnostic print site is at debug level
   (dev_dbg/pr_debug/dev_dbg_ratelimited/pr_debug_ratelimited), so the
   details remain available under CONFIG_DYNAMIC_DEBUG / debug builds.
3. Genuine failure paths keep a bounded rate-limited diagnostic:
   the synchronous and asynchronous FunctionFS endpoint queue failures and
   the unhandled MUSB IRQ (the latter lives in the MediaTek wrapper, not the
   generic shared MUSB core).

Run from the kernel source root:

    python3 drivers/usb/musb/test_mt8163_console_diag_contract.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

MUSB_MEDIATEK = ROOT / "drivers/usb/musb/mediatek.c"
MUSB_GADGET = ROOT / "drivers/usb/musb/musb_gadget.c"
FFS = ROOT / "drivers/usb/gadget/function/f_fs.c"

DIAG_MARKERS = (
    "MUSB IRQ diag",
    "MUSB delayed diag",
    "MUSB RX no-request diag",
    "MUSB restart diag",
    "MUSB enqueue diag",
    "FFS queue diag",
)
INFO_LEVEL = re.compile(
    r"dev_info(?:_ratelimited)?\s*\(|pr_info(?:_ratelimited)?\s*\("
)
DEBUG_LEVEL = re.compile(
    r"dev_dbg(?:_ratelimited)?\s*\(|pr_debug(?:_ratelimited)?\s*\("
)


def diag_sites(text):
    """Yield (line_number, line) for every MT8163 diag print site."""
    for i, line in enumerate(text.splitlines(), start=1):
        if any(marker in line for marker in DIAG_MARKERS):
            yield i, line


def site_is_print_site(path, lines, line_number):
    """Resolve the print-macro controlling this diag string.

    The macro can be on the same line or on a preceding continuation line
    (multi-line call with the format string wrapped).
    """
    for offset in range(0, 6):
        idx = line_number - 1 - offset
        if idx < 0:
            break
        candidate = lines[idx]
        if INFO_LEVEL.search(candidate):
            return "info", idx + 1, candidate.strip()
        if DEBUG_LEVEL.search(candidate):
            return "debug", idx + 1, candidate.strip()
    return "none", line_number, ""


def main():
    failures = []

    for path in (MUSB_MEDIATEK, MUSB_GADGET, FFS):
        if not path.exists():
            failures.append(f"missing source file: {path}")
            continue
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        for line_number, line in diag_sites(text):
            level, macro_line, macro = site_is_print_site(path, lines, line_number)
            if level == "info":
                failures.append(
                    f"{path}:{macro_line}: diag print at info level: {macro[:80]}"
                )
            elif level == "none":
                failures.append(
                    f"{path}:{line_number}: diag string without recognized "
                    f"print macro: {line.strip()[:80]}"
                )

    # Genuine failure paths must keep a bounded (rate-limited) diagnostic.
    # Both the synchronous and the asynchronous FunctionFS queue failures must
    # be covered, and the check must match pr_warn_ratelimited explicitly: an
    # unbounded pr_warn would re-create the console flooding this contract
    # exists to prevent.
    ffs_text = FFS.read_text(encoding="utf-8") if FFS.exists() else ""
    sync_queue_failure = re.search(
        r"ret\s*=\s*usb_ep_queue\(.*?\);\s*"
        r"if\s*\(ret\s*<\s*0\)\s*\{[^}]*pr_warn_ratelimited",
        ffs_text,
        re.DOTALL,
    )
    if not sync_queue_failure:
        failures.append(
            "f_fs.c: synchronous endpoint queue failure path lacks a "
            "rate-limited pr_warn diagnostic"
        )
    async_queue_failure = re.search(
        r"req->complete\s*=\s*ffs_epfile_async_io_complete;\s*"
        r"ret\s*=\s*usb_ep_queue\(.*?\);\s*"
        r"if\s*\(ret\)\s*\{[^}]*pr_warn_ratelimited",
        ffs_text,
        re.DOTALL,
    )
    if not async_queue_failure:
        failures.append(
            "f_fs.c: asynchronous endpoint queue failure path lacks a "
            "rate-limited pr_warn diagnostic"
        )

    # The unhandled-IRQ diagnostic belongs in the MediaTek platform wrapper,
    # not the generic shared musb_interrupt() core used by other glue drivers.
    mediatek_text = MUSB_MEDIATEK.read_text(encoding="utf-8") if MUSB_MEDIATEK.exists() else ""
    unhandled_irq = re.search(
        r"MT8163 MUSB unhandled IRQ",
        mediatek_text,
    )
    if not unhandled_irq:
        failures.append(
            "mediatek.c: unhandled-IRQ path lacks a bounded dev_warn diagnostic"
        )
    if "dev_warn_ratelimited" not in mediatek_text:
        failures.append(
            "mediatek.c: unhandled-IRQ diagnostic is not rate-limited"
        )

    musb_core = ROOT / "drivers/usb/musb/musb_core.c"
    core_text = musb_core.read_text(encoding="utf-8") if musb_core.exists() else ""
    if "MT8163" in core_text:
        failures.append(
            "musb_core.c: generic shared MUSB core must not contain "
            "platform-specific MT8163 diagnostics"
        )

    if failures:
        print("MT8163 console diag contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("MT8163 console diag contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
