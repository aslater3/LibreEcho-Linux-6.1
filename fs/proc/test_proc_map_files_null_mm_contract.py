#!/usr/bin/env python3
"""Source contract for the procfs map_files mm_access() NULL guard.

``mm_access()`` (kernel/fork.c) returns a plain ``NULL`` -- not an error
pointer -- when the target task has no ``mm``: a kernel thread, or a task
whose ``mm`` was torn down while it exits.  Only ``-EACCES`` and the
``exec_update_lock`` failure are wrapped in ``ERR_PTR()``.

``proc_map_files_readdir()`` and ``proc_map_files_lookup()`` used to check
only ``IS_ERR(mm)``, which is false for ``NULL``, so ``NULL`` reached
``mmap_read_lock_killable(mm)`` and the kernel dereferenced
``NULL->mmap_lock`` (fault address ``0x34``) inside ``down_read_killable()``.
An unprivileged enumeration of ``/proc/<pid>/map_files`` was therefore a
kernel panic and a hard reboot.

The contract is:

* neither site may pass a possibly-NULL ``mm`` to
  ``mmap_read_lock_killable()``: both must guard with ``IS_ERR_OR_NULL(mm)``
  (or an equivalent explicit ``!mm`` test) before taking the mmap lock;
* ``proc_map_files_readdir()`` must treat a missing ``mm`` as an empty
  directory -- success with no entries, not an error;
* ``proc_map_files_lookup()`` must treat a missing ``mm`` as ``-ENOENT``,
  matching ``map_files_d_revalidate()``.

This is a host source contract.  It does not replace the device check (a
``/proc/*/map_files`` enumeration completing without an Oops) that the issue
requires.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE_C = HERE / "base.c"
FORK_C = HERE.parent.parent / "kernel" / "fork.c"

# A guard that rejects NULL as well as error pointers.
NULL_SAFE_GUARD = re.compile(
    r"IS_ERR_OR_NULL\s*\(\s*mm\s*\)"  # canonical form
    r"|!\s*mm\s*\|\|"  # !mm || IS_ERR(mm)
    r"|\bmm\s*==\s*NULL\b"
)


def function_body(text, signature_fragment):
    """Return the brace-balanced body of a function definition."""
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


def guard_segment(body):
    """Return (region between mm_access() and the mmap lock, text after it).

    That region is where the NULL check has to live.  On the vulnerable code
    it holds only the ``IS_ERR()`` test, and the NULL value flows straight
    into ``mmap_read_lock_killable()``.
    """
    access = body.find("mm_access(")
    if access < 0:
        return None, None
    lock = body.find("mmap_read_lock_killable(mm)", access)
    if lock < 0:
        return None, None
    return body[access:lock], body[lock:]


def check_site(body, name, null_outcome):
    """Check one map_files call site.

    ``null_outcome`` is the required NULL result: ``"0"`` for readdir (empty
    directory) or ``"-ENOENT"`` for lookup.
    """
    failures = []
    segment, tail = guard_segment(body)
    if segment is None or tail is None:
        failures.append(
            f"{name}() no longer calls mm_access() before "
            "mmap_read_lock_killable(mm): re-derive this contract"
        )
        return failures

    if not NULL_SAFE_GUARD.search(segment):
        failures.append(
            f"{name}() guards mm_access() with IS_ERR() only: mm_access() "
            "returns NULL when the task has no mm, so NULL reaches "
            "mmap_read_lock_killable(mm) and faults on NULL->mmap_lock"
        )

    # The guard must map the NULL case to the documented outcome explicitly.
    # Accepted: a ternary on IS_ERR(mm), an else branch assignment, or a
    # conditional assignment guarded by !mm.  Rejected: an unconditional
    # PTR_ERR(mm)/ERR_CAST(mm), where a NULL mm silently becomes 0 no matter
    # what the site means.
    flattened = re.sub(r"\s+", " ", segment)
    outcome = re.escape(null_outcome)
    # The outcome may be written bare, or wrapped in ERR_PTR() where the
    # site returns a pointer (the lookup path returns a struct dentry *).
    spelled = rf"(?:ERR_PTR\s*\(\s*{outcome}\s*\)|{outcome})(?!\w)"
    maps_null = (
        re.search(rf"IS_ERR\s*\(\s*mm\s*\)\s*\?[^;]*?:\s*{spelled}", flattened)
        or re.search(rf"\belse\b[^;]*?=\s*{spelled}", flattened)
        or re.search(rf"!\s*mm\b[^;]*?=\s*{spelled}", flattened)
    )
    if not maps_null:
        failures.append(
            f"{name}() does not map a NULL mm to {null_outcome}: the no-mm "
            "case must be handled explicitly, not left to PTR_ERR(NULL)"
        )

    if "mmap_read_lock_killable(mm)" not in tail:
        failures.append(
            f"{name}() no longer takes mmap_read_lock_killable(mm): "
            "re-derive this contract"
        )
    return failures


def check_mm_access_premise(text):
    """mm_access() still returns a bare NULL for a task without an mm."""
    failures = []
    body = function_body(text, "struct mm_struct *mm_access(")
    if body is None:
        failures.append("kernel/fork.c: mm_access() definition is missing")
        return failures
    if "get_task_mm(task)" not in body:
        failures.append(
            "kernel/fork.c: mm_access() no longer obtains the mm via "
            "get_task_mm(): re-derive whether it can still return NULL"
        )
    if "ERR_PTR(-ESRCH)" in body:
        failures.append(
            "kernel/fork.c: mm_access() now reports a missing mm as "
            "ERR_PTR(-ESRCH): the map_files NULL handling must be re-derived"
        )
    return failures


def main():
    failures = []
    for path in (BASE_C, FORK_C):
        if not path.exists():
            failures.append(f"missing source: {path}")
    if failures:
        print("procfs map_files NULL mm contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    failures.extend(check_mm_access_premise(FORK_C.read_text(encoding="utf-8")))

    text = BASE_C.read_text(encoding="utf-8")
    sites = (
        ("proc_map_files_readdir", "static int\nproc_map_files_readdir(", "0"),
        (
            "proc_map_files_lookup",
            "static struct dentry *proc_map_files_lookup(",
            "-ENOENT",
        ),
    )
    for name, signature, null_outcome in sites:
        body = function_body(text, signature)
        if body is None:
            failures.append(f"fs/proc/base.c: {name}() body not found")
            continue
        failures.extend(check_site(body, name, null_outcome))

    if failures:
        print("procfs map_files NULL mm contract: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("procfs map_files NULL mm contract: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
