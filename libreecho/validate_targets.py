#!/usr/bin/env python3
"""Validate kernel targets and plan one CI build per distinct config/DTB tuple."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import sys

TARGETS = ("radar_puffin", "biscuit")


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key: {key}")
        result[key] = value
    return result


def load_targets(root, table):
    targets = json.loads(table.read_text(), object_pairs_hook=unique_keys)
    if not isinstance(targets, dict) or set(targets) != set(TARGETS):
        raise ValueError(f"target IDs must be exactly {', '.join(TARGETS)}")
    for target, settings in targets.items():
        if not isinstance(settings, dict) or set(settings) != {"defconfig", "dtb"}:
            raise ValueError(f"{target}: expected only defconfig and dtb")
        config = settings["defconfig"]
        dtb = settings["dtb"]
        if not isinstance(config, str) or not re.fullmatch(r"[A-Za-z0-9_]+_defconfig", config):
            raise ValueError(f"{target}: invalid defconfig name")
        if (not isinstance(dtb, str)
                or not re.fullmatch(r"[A-Za-z0-9_/-]+\.dtb", dtb)
                or any(part in {"", ".", ".."} for part in dtb.split("/"))
                or PurePosixPath(dtb).is_absolute()):
            raise ValueError(f"{target}: invalid dtb path")
        for source in (root / "arch/arm/configs" / config,
                       root / "arch/arm/boot/dts" / PurePosixPath(dtb).with_suffix(".dts")):
            if not source.is_file() or not source.resolve().is_relative_to(root.resolve()):
                raise ValueError(f"{target}: missing or out-of-tree source: {source}")
    return targets


def plan_matrices(targets):
    builds = []
    publish = []
    owners = {}
    # Radar remains the canonical build/legacy artifact owner at parity,
    # independent of JSON key order. A distinct Biscuit tuple gets its own lane.
    for target in TARGETS:
        settings = targets[target]
        key = (settings["defconfig"], settings["dtb"])
        if key not in owners:
            owners[key] = target
            builds.append({"target": target, **settings})
        publish.append({"target": target, "build_target": owners[key], **settings})
    return {"build": {"include": builds}, "publish": {"include": publish}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--table", type=Path)
    parser.add_argument("--matrix", action="store_true", help="print CI matrices as JSON")
    parser.add_argument("--github-output", type=Path, help="append matrices to GITHUB_OUTPUT")
    args = parser.parse_args()
    try:
        targets = load_targets(args.root, args.table or args.root / "libreecho/targets.json")
        matrices = plan_matrices(targets)
        if args.github_output:
            with args.github_output.open("a") as output:
                for name, matrix in matrices.items():
                    output.write(f"{name}_matrix={json.dumps(matrix, separators=(',', ':'))}\n")
    except (OSError, ValueError) as error:
        print(f"target validation failed: {error}", file=sys.stderr)
        return 1
    if args.matrix:
        print(json.dumps(matrices, separators=(",", ":")))
    else:
        print(f"Validated {len(targets)} targets; {len(matrices['build']['include'])} unique build(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
