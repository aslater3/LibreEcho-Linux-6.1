#!/usr/bin/env python3
"""Source contract for the MT8163 production pstore/ramoops boundary.

Issue #6 asks for persistent panic evidence, but this repository does not yet
contain a safe, complete integration.  The production DT already reserves
platform-owned memory, while the product initramfs is owned by the separate
LibreEcho-Platform repository.  Enabling only one layer here would create a
misleading or unsafe partial implementation.

This test deliberately accepts the documented blocked state on main.  If a
future change starts the integration, it must provide all three layers at once:
CONFIG_PSTORE + CONFIG_PSTORE_RAM, a ramoops node with an explicit reserved
region in the production DT, and initramfs archival handling.  The DT contract
also validates that the ramoops range is active, non-overlapping with existing
reserved-memory children, and has at least one nonzero buffer.

Run from the kernel source root:

    python3 tools/mt8163_pstore_contract_test.py
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"
PRODUCTION_DTS = ROOT / "arch/arm/boot/dts/libreecho-radar-puffin.dts"
README = ROOT / "README.md"

_NODE_LINE = re.compile(
    r"^[ \t]*(?:(?P<label>[A-Za-z_][A-Za-z0-9_-]*):\s*)?"
    r"(?P<name>[A-Za-z0-9_.,+\-]+(?:@[A-Za-z0-9_x+\-]+)?)\s*\{"
)


def _preprocess_dts(source: str, extra_include_dirs: tuple[Path, ...] = ()) -> str:
    """Compose DTS includes and honor the C preprocessor's disabled regions."""
    include_args = [arg for directory in extra_include_dirs for arg in ("-I", str(directory))]
    try:
        result = subprocess.run(
            [
                "cpp",
                "-nostdinc",
                "-undef",
                "-D__DTS__",
                "-P",
                "-x",
                "assembler-with-cpp",
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "arch/arm/boot/dts"),
                *include_args,
                "-",
            ],
            input=source,
            text=True,
            capture_output=True,
            check=False,
        )
    except FileNotFoundError as exc:
        raise AssertionError("cpp is required to compose the production DTS") from exc
    if result.returncode:
        raise AssertionError(f"DTS preprocessing failed: {result.stderr.strip()}")
    return result.stdout


def _balanced_body(source: str, opening: int) -> str:
    """Return the text inside the brace at *opening*."""
    if source[opening] != "{":
        raise AssertionError("internal parser error: expected an opening brace")
    depth = 0
    for index in range(opening, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError("unclosed DT node")


_RESERVED_MEMORY_FRAGMENT = re.compile(
    r"(?m)^[ \t]*(?:reserved-memory|&\{/?reserved-memory\}|&reserved-memory)\s*\{"
)


def _reserved_memory_bodies(dts: str) -> list[str]:
    """Return the bodies of the declaration and all reopened fragments."""
    dts = _preprocess_dts(dts)
    bodies = []
    for match in _RESERVED_MEMORY_FRAGMENT.finditer(dts):
        opening = dts.rfind("{", match.start(), match.end())
        bodies.append(_balanced_body(dts, opening))
    return bodies


def _reserved_memory_body(dts: str) -> str | None:
    bodies = _reserved_memory_bodies(dts)
    return "\n".join(bodies) if bodies else None


def _direct_children(reserved_body: str) -> list[tuple[str, str, str | None]]:
    """Extract direct child node names and bodies from reserved-memory."""
    children: list[tuple[str, str, str | None]] = []
    index = 0
    while index < len(reserved_body):
        line_end = reserved_body.find("\n", index)
        if line_end == -1:
            line_end = len(reserved_body)
        line = reserved_body[index:line_end]
        match = _NODE_LINE.match(line)
        if match is not None:
            opening = index + match.end() - 1
            children.append(
                (
                    match.group("name"),
                    _balanced_body(reserved_body, opening),
                    match.group("label"),
                )
            )
            node_end = opening + len(_balanced_body(reserved_body, opening)) + 2
            index = node_end
            continue
        index = line_end + 1
    return children


def _reserved_children(dts: str) -> list[tuple[str, str]]:
    """Merge child nodes declared across all reserved-memory fragments."""
    dts = _preprocess_dts(dts)
    fragments: dict[str, list[str]] = {}
    labels: dict[str, str] = {}
    for reserved_body in _reserved_memory_bodies(dts):
        for name, body, label in _direct_children(reserved_body):
            fragments.setdefault(name, []).append(body)
            if label is not None:
                labels[label] = name

    override_pattern = re.compile(r"(?m)^[ \t]*&(?P<label>[A-Za-z_][A-Za-z0-9_-]*)\s*\{")
    for match in override_pattern.finditer(dts):
        name = labels.get(match.group("label"))
        if name is None:
            continue
        opening = dts.find("{", match.start(), match.end())
        fragments[name].append(_balanced_body(dts, opening))
    return [(name, "\n".join(bodies)) for name, bodies in fragments.items()]


def _cells_with_width(body: str, property_name: str) -> tuple[list[int], int] | None:
    pattern = (
        rf"(?m)^[ \t]*{re.escape(property_name)}[ \t]*=[ \t]*"
        r"(?:(?:/bits/[ \t]+(?P<bits>\d+))[ \t]*)?"
        r"<(?P<cells>[^>]*)>;"
    )
    matches = list(re.finditer(pattern, body))
    if not matches:
        return None
    match = matches[-1]
    try:
        cells = [int(token, 0) for token in match.group("cells").split()]
    except ValueError as exc:
        raise AssertionError(f"{property_name} contains a non-integer cell") from exc
    return cells, int(match.group("bits") or 32)


def _cells(body: str, property_name: str) -> list[int] | None:
    parsed = _cells_with_width(body, property_name)
    return parsed[0] if parsed is not None else None


def _u32_property(body: str, property_name: str) -> int | None:
    parsed = _cells_with_width(body, property_name)
    if parsed is None:
        return None
    values, bits = parsed
    if bits != 32:
        raise AssertionError(f"ramoops {property_name} must use 32-bit cells")
    if len(values) != 1 or not 0 <= values[0] <= 0xFFFFFFFF:
        raise AssertionError(f"ramoops {property_name} must contain one u32 cell")
    return values[0]


def _string_property(body: str, property_name: str) -> str | None:
    matches = list(re.finditer(
        rf"(?m)^\s*{re.escape(property_name)}\s*=\s*\"([^\"]+)\"\s*;",
        body,
    ))
    return matches[-1].group(1) if matches else None


def _fold_cells(cells: list[int]) -> int:
    value = 0
    for cell in cells:
        if not 0 <= cell <= 0xFFFFFFFF:
            raise AssertionError("DT cell is outside the 32-bit range")
        value = (value << 32) | cell
    return value


def _reg_ranges(body: str, address_cells: int, size_cells: int) -> list[tuple[int, int]]:
    cells = _cells(body, "reg")
    if cells is None:
        return []
    tuple_cells = address_cells + size_cells
    if tuple_cells <= 0 or len(cells) % tuple_cells:
        raise AssertionError("reg must contain complete address/size tuples")
    ranges = []
    for offset in range(0, len(cells), tuple_cells):
        address = _fold_cells(cells[offset : offset + address_cells])
        size = _fold_cells(cells[offset + address_cells : offset + tuple_cells])
        ranges.append((address, size))
    return ranges


def _ramoops_children(dts: str) -> list[tuple[str, str]]:
    return [
        (name, body)
        for name, body in _reserved_children(dts)
        if _string_property(body, "compatible") == "ramoops"
    ]


def validate_ramoops_contract(dts: str) -> None:
    """Validate every ramoops child without assuming an address when absent."""
    reserved_body = _reserved_memory_body(dts)
    ramoops_nodes = _ramoops_children(dts)
    if not ramoops_nodes:
        return
    if reserved_body is None:
        raise AssertionError("ramoops requires a reserved-memory parent")

    address_cells = _cells(reserved_body, "#address-cells")
    size_cells = _cells(reserved_body, "#size-cells")
    if address_cells is None or size_cells is None or len(address_cells) != 1 or len(size_cells) != 1:
        raise AssertionError("reserved-memory must declare scalar address/size cell counts")
    address_count = address_cells[0]
    size_count = size_cells[0]
    if address_count <= 0 or size_count <= 0:
        raise AssertionError("reserved-memory cell counts must be positive")

    children = _reserved_children(dts)

    for name, body in ramoops_nodes:
        status = _string_property(body, "status")
        if status is not None and status not in {"okay", "ok"}:
            raise AssertionError(f"ramoops node {name} must be enabled, got status={status!r}")

        ramoops_ranges = _reg_ranges(body, address_count, size_count)
        if not ramoops_ranges:
            raise AssertionError(f"ramoops node {name} needs an explicit nonempty reg")
        if len(ramoops_ranges) != 1:
            raise AssertionError(f"ramoops node {name} needs exactly one reg tuple")
        address, reserved_size = ramoops_ranges[0]
        if reserved_size == 0:
            raise AssertionError(f"ramoops node {name} needs a nonzero reg size")
        end = address + reserved_size
        for other_name, other_body in children:
            if other_name == name:
                continue
            for other_address, other_size in _reg_ranges(
                other_body, address_count, size_count
            ):
                if other_size:
                    other_end = other_address + other_size
                    if address < other_end and other_address < end:
                        raise AssertionError(
                            f"ramoops range {address:#x}-{end:#x} overlaps "
                            f"reserved-memory child {other_name}"
                        )

        buffer_sizes = []
        for property_name in ("record-size", "console-size", "ftrace-size", "pmsg-size"):
            value = _u32_property(body, property_name)
            if value is not None:
                buffer_sizes.append((property_name, value))
        if not buffer_sizes or not any(size > 0 for _, size in buffer_sizes):
            raise AssertionError(f"ramoops node {name} needs a nonzero buffer size")
        if sum(size for _, size in buffer_sizes) > reserved_size:
            raise AssertionError(
                f"ramoops buffers exceed the reserved region for node {name}"
            )
        for property_name in ("mem-type", "flags", "max-reason"):
            value = _u32_property(body, property_name)
            if value is not None and value > 0x7FFFFFFF:
                raise AssertionError(f"ramoops {property_name} must fit signed int")
        ecc_size = _u32_property(body, "ecc-size")
        if ecc_size is not None:
            if ecc_size > 0x7FFFFFFF:
                raise AssertionError("ramoops ecc-size must fit signed int")
            if ecc_size:
                for property_name, buffer_size in buffer_sizes:
                    if not buffer_size:
                        continue
                    if buffer_size <= ecc_size:
                        raise AssertionError(
                            f"ramoops ecc-size is too large for {property_name}"
                        )
                    ecc_blocks = (
                        buffer_size - ecc_size + 128 + ecc_size - 1
                    ) // (128 + ecc_size)
                    ecc_total = (ecc_blocks + 1) * ecc_size
                    if ecc_total >= buffer_size:
                        raise AssertionError(
                            f"ramoops ecc-size is unusable for {property_name}"
                        )


class Mt8163PstoreContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.config = DEFCONFIG.read_text(encoding="utf-8")
        self.dts = PRODUCTION_DTS.read_text(encoding="utf-8")
        self.readme = README.read_text(encoding="utf-8")

    @staticmethod
    def _with_ramoops(dts: str, node: str) -> str:
        marker = "\t\tconsys-reserve-memory {"
        if marker not in dts:
            raise AssertionError("test fixture lost the reserved-memory insertion point")
        return dts.replace(marker, node + "\n\n" + marker, 1)

    @staticmethod
    def _with_ramoops_fragment(dts: str, node: str) -> str:
        return dts + "\n&{/reserved-memory} {\n" + node + "\n};\n"

    def test_pstore_config_layers_are_consistent(self) -> None:
        enabled = {
            symbol: bool(re.search(rf"^{symbol}=[ym]$", self.config, re.MULTILINE))
            for symbol in ("CONFIG_PSTORE", "CONFIG_PSTORE_RAM")
        }
        if any(enabled.values()):
            self.assertEqual(
                enabled,
                {"CONFIG_PSTORE": True, "CONFIG_PSTORE_RAM": True},
                "pstore config must enable both symbols together",
            )

    def test_production_dt_has_no_unowned_ramoops_region(self) -> None:
        self.assertIn("reserved-memory {", self.dts)
        dt_enabled = bool(_ramoops_children(self.dts))
        validate_ramoops_contract(self.dts)
        if not dt_enabled:
            self.assertIn('compatible = "mediatek,ram_console";', self.dts)
            self.assertNotIn("pstore", self.dts.lower())

    def test_ramoops_rejects_overlap_with_existing_reserved_child(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@43000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x43000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "overlaps"):
            validate_ramoops_contract(dts)

    def test_ramoops_reads_reopened_reserved_memory_fragment(self) -> None:
        existing_names = {name for name, _ in _ramoops_children(self.dts)}
        dts = self._with_ramoops_fragment(
            self.dts,
            """\t ramoops@4f000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x4f000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        discovered_names = {name for name, _ in _ramoops_children(dts)}
        self.assertEqual(discovered_names - existing_names, {"ramoops@4f000000"})
        validate_ramoops_contract(dts)

    def test_ramoops_ignores_commented_out_nodes(self) -> None:
        existing_names = {name for name, _ in _ramoops_children(self.dts)}
        dts = self._with_ramoops(
            self.dts,
            """/*
\t\tramoops@4f000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x4f000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};
\t\t*/""",
        )
        self.assertEqual(existing_names, {name for name, _ in _ramoops_children(dts)})
        validate_ramoops_contract(dts)

    def test_ramoops_composes_included_reserved_memory_fragment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fragment = Path(directory) / "ramoops-fragment.dtsi"
            fragment.write_text(
                """&{/reserved-memory} {
\t ramoops@4f000000 {
\t\tcompatible = \"ramoops\";
\t\treg = <0x00 0x4f000000 0x00 0x200000>;
\t\trecord-size = <0x10000>;
\t};
};
""",
                encoding="utf-8",
            )
            composed = _preprocess_dts(
                self.dts + f'\n#include "{fragment.name}"\n',
                (Path(directory),),
            )
        self.assertIn("ramoops@4f000000", [name for name, _ in _ramoops_children(composed)])
        validate_ramoops_contract(composed)

    def test_ramoops_applies_labeled_child_override(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops_label: ramoops@4f000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x4f000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        dts += "\n&ramoops_label {\n\tstatus = \"disabled\";\n};\n"
        with self.assertRaisesRegex(AssertionError, "enabled"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_disabled_node(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\tstatus = \"disabled\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "enabled"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_all_zero_buffers(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0>;
\t\t\tconsole-size = <0>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "nonzero buffer"):
            validate_ramoops_contract(dts)

    def test_valid_ramoops_range_and_single_nonzero_buffer_are_accepted(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        validate_ramoops_contract(dts)

    def test_ramoops_rejects_mixed_case_status(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\tstatus = \"OKAY\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "enabled"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_multicell_buffer_property(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0 0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "one u32"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_non_32_bit_buffer_encoding(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = /bits/ 64 <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "32-bit"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_buffers_larger_than_reserved_region(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t\tconsole-size = <0x300000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "exceed"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_ecc_size_above_signed_int(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t\tecc-size = <0xffffffff>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "signed int"):
            validate_ramoops_contract(dts)

    def test_ramoops_rejects_ecc_size_that_consumes_a_buffer(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t\tecc-size = <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "too large"):
            validate_ramoops_contract(dts)

    def test_ramoops_allows_zero_sized_optional_ecc_buffer(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t\tconsole-size = <0>;
\t\t\tecc-size = <0x10>;
\t\t};""",
        )
        validate_ramoops_contract(dts)

    def test_ramoops_rejects_driver_u32_values_above_signed_int(self) -> None:
        for property_name in ("mem-type", "flags", "max-reason"):
            with self.subTest(property_name=property_name):
                dts = self._with_ramoops(
                    self.dts,
                    f"""\t\tramoops@45000000 {{
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t\t{property_name} = <0xffffffff>;
\t\t}};""",
                )
                with self.assertRaisesRegex(AssertionError, "signed int"):
                    validate_ramoops_contract(dts)

    def test_ramoops_rejects_multiple_reg_tuples(self) -> None:
        dts = self._with_ramoops(
            self.dts,
            """\t\tramoops@45000000 {
\t\t\tcompatible = \"ramoops\";
\t\t\treg = <0x00 0x45000000 0x00 0x10000
\t\t\t       0x00 0x46000000 0x00 0x200000>;
\t\t\trecord-size = <0x10000>;
\t\t};""",
        )
        with self.assertRaisesRegex(AssertionError, "exactly one reg tuple"):
            validate_ramoops_contract(dts)

    def test_kernel_tree_does_not_claim_product_initramfs_archival(self) -> None:
        self.assertIn(
            "owns ARM32 product tooling, initramfs, feature packaging",
            self.readme,
        )
        self.assertIn(
            "production initramfs and its `/data` archival/rotation policy are owned",
            self.readme,
        )

    def test_any_future_integration_must_supply_all_layers(self) -> None:
        config_enabled = all(
            re.search(rf"^CONFIG_{name}=[ym]$", self.config, re.MULTILINE)
            for name in ("PSTORE", "PSTORE_RAM")
        )
        dt_enabled = bool(_ramoops_children(self.dts))
        complete = config_enabled and dt_enabled
        absent = not config_enabled and not dt_enabled
        self.assertTrue(
            complete or absent,
            "pstore integration must land as one complete, reviewed contract",
        )


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print(
            "MT8163 pstore source contract: PASS "
            "(integration remains blocked pending a firmware-owned region "
            "and product initramfs change)"
        )
    sys.exit(not result.result.wasSuccessful())
