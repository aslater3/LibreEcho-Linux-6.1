#!/usr/bin/env python3
"""Source contract for the MT8163 AFE period IRQ context.

The Radar-Puffin DAI links are registered ``nonatomic`` (see
``mt8163-radar-puffin.c``), so ``snd_soc_new_pcm()`` sets ``pcm->nonatomic``
and the PCM stream lock resolves to a sleeping mutex
(``_snd_pcm_stream_lock_irqsave()`` -> ``mutex_lock()``).  The AFE period IRQ
must therefore not call ``snd_pcm_period_elapsed()`` -- directly or through
any helper -- from the hard IRQ handler, or the handler schedules inside
interrupt context ("scheduling while atomic") and the kernel panics with a
fatal exception in interrupt.

The contract is:

* the AFE IRQ is registered as a threaded IRQ with ``IRQF_ONESHOT``, so the
  primary handler cannot return with interrupts enabled and the period is
  reported in process context where the PCM mutex is legal;
* the primary (hard IRQ) handler issues the status read/ack only and never
  enters a sleeping path (``snd_pcm_period_elapsed()``, ``mutex_lock()``,
  ``snd_pcm_stream_lock*()``, or a sleeping helper defined in this driver);
* the thread handler still reports the period, so period accounting is not
  silently dropped to make the panic go away.

This is a host source contract.  It does not replace the stress/reproduction
run on the target that the issue requires.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
AFE = HERE / "mt8163-afe.c"
MACHINE = HERE / "mt8163-radar-puffin.c"

# Sleeping primitives that must never appear in the hard IRQ handler.
SLEEPING_PRIMITIVES = (
    "snd_pcm_period_elapsed",
    "snd_pcm_period_elapsed_under_stream_lock",
    "snd_pcm_stream_lock",
    "mutex_lock",
    "mutex_lock_nested",
    "synchronize_irq",
    "synchronize_hardirq",
    "msleep",
    "usleep_range",
    "schedule_work",
)

# Driver-local helpers that sleep and must not be reached from hard IRQ.
SLEEPING_HELPERS = (
    "mt8163_afe_pin",
    "mt8163_afe_select_i2s",
    "mt8163_afe_select_pmic",
    "mt8163_afe_select_amp",
    "mt8163_afe_select_dac",
    "mt8163_afe_select_mclk",
    "mt8163_afe_safe",
    "mt8163_afe_analog_clock",
    "mt8163_afe_clocks_enable",
    "mt8163_afe_clocks_disable",
    "mt8163_afe_stop",
    "mt8163_afe_log_sram",
)


def function_body(text, signature_fragment):
    """Return the brace-balanced body of the function whose signature
    contains ``signature_fragment`` (searching from the definition)."""
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


def call_arguments(text, call):
    """Return the argument list of a top-level ``call(`` invocation."""
    index = text.find(call)
    if index < 0:
        return None
    start = index + len(call) - 1  # position of the opening parenthesis
    if text[start] != "(":
        return None
    depth = 0
    for position in range(start, len(text)):
        if text[position] == "(":
            depth += 1
        elif text[position] == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1 : position]
    return None


def split_arguments(arguments):
    """Split a C argument list on top-level commas."""
    parts = []
    depth = 0
    current = ""
    for character in arguments:
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
    if current.strip():
        parts.append(current.strip())
    return parts


def check_contract(text, machine_text):
    failures = []

    if "snd_pcm_period_elapsed" not in text:
        failures.append(
            "mt8163-afe.c must still report periods via "
            "snd_pcm_period_elapsed()"
        )
        return failures

    if "devm_request_irq(" in text:
        failures.append(
            "AFE must not register a non-threaded IRQ: the PCM stream lock "
            "is a mutex (nonatomic DAI links), so period handling cannot run "
            "in the primary handler"
        )

    arguments = call_arguments(text, "devm_request_threaded_irq(")
    if arguments is None:
        failures.append(
            "AFE must register a threaded IRQ via "
            "devm_request_threaded_irq()"
        )
        return failures

    parts = split_arguments(arguments)
    if len(parts) < 5:
        failures.append(
            "devm_request_threaded_irq() must pass primary handler, thread "
            f"handler and flags (got {len(parts)} arguments)"
        )
        return failures

    primary_name = parts[2].strip()
    thread_name = parts[3].strip()
    flags = parts[4]

    if "IRQF_ONESHOT" not in flags:
        failures.append(
            "devm_request_threaded_irq() must set IRQF_ONESHOT so the "
            "primary handler cannot leave interrupts enabled"
        )

    primary = function_body(text, f"static irqreturn_t {primary_name}(")
    if primary is None:
        failures.append(f"primary handler {primary_name}() is missing")
    else:
        for primitive in SLEEPING_PRIMITIVES:
            if primitive in primary:
                failures.append(
                    f"hard IRQ handler {primary_name}() must not use "
                    f"{primitive}()"
                )
        for helper in SLEEPING_HELPERS:
            if re.search(rf"\b{re.escape(helper)}\s*\(", primary):
                failures.append(
                    f"hard IRQ handler {primary_name}() must not call "
                    f"sleeping helper {helper}()"
                )
        if "IRQ_WAKE_THREAD" not in primary and "IRQ_HANDLED" not in primary:
            failures.append(
                f"hard IRQ handler {primary_name}() must acknowledge the IRQ"
            )

    if thread_name == primary_name:
        failures.append(
            "the IRQ thread handler must be distinct from the primary handler"
        )

    thread = function_body(text, f"static irqreturn_t {thread_name}(")
    if thread is None:
        failures.append(f"IRQ thread handler {thread_name}() is missing")
    elif "snd_pcm_period_elapsed(" not in thread:
        failures.append(
            f"IRQ thread handler {thread_name}() must report the elapsed "
            "period via snd_pcm_period_elapsed()"
        )

    # The mutex-backed stream lock exists because the DAI links are
    # nonatomic.  If that ever changes, the threaded IRQ is still valid, but
    # the contract's premise must be re-read rather than assumed.
    if "nonatomic" not in machine_text:
        failures.append(
            "mt8163-radar-puffin.c no longer marks any DAI link nonatomic: "
            "re-derive whether the PCM stream lock is still a mutex"
        )

    return failures


def main():
    failures = []
    for path in (AFE, MACHINE):
        if not path.exists():
            failures.append(f"missing source: {path}")

    if not failures:
        failures = check_contract(
            AFE.read_text(encoding="utf-8"),
            MACHINE.read_text(encoding="utf-8"),
        )

    if failures:
        print("MT8163 AFE period IRQ atomic contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("MT8163 AFE period IRQ atomic contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
