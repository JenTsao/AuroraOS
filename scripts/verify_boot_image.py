#!/usr/bin/env python3
"""Verify an AuroraOS secure-boot flash image against the C++ contract.

Why this exists
---------------
`scripts/build_image.py` packs Bootloader + Partition Table + FirmwareHeader +
application into one ``flash.bin``.  The consumer is `bootloader/boot_main.cpp`,
which re-parses that layout from raw flash addresses using the structs in
``kernel/core/firmware_header.hpp`` and ``config/partition_table.hpp``.

Nothing checked that the packer and the bootloader agreed.  A changed partition
offset, a renamed magic or an off-by-one in the header size would still produce
a file and still pass every existing CI job -- it would only fail on hardware,
as a device that refuses to boot after an OTA.

This script closes that gap in two ways:

  1. ``--self-test`` cross-checks the constants *inside build_image.py* against
     the C++ headers (magic values, partition offsets/sizes) and exercises the
     structural verifier on synthetic good/corrupt images.  It needs no build.
  2. ``<flash.bin> --app <auroraOS.bin>`` validates a real image: partition
     table magic + CRC, header fields, application payload equality, and the
     bounds check the bootloader performs before copying.

Usage
-----
    verify_boot_image.py --self-test
    verify_boot_image.py build_lm3s/flash.bin --app build_lm3s/auroraOS.bin

Exit codes: 0 = valid, 1 = invalid image, 2 = usage / unreadable input.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

BOOTLOADER_BYTES = 28672        # 28 KiB reserved for the bootloader
PARTITION_TABLE_OFFSET = 0x7000
PARTITION_TABLE_BYTES = 0x1000  # 4 KiB
HEADER_OFFSET = 0x8000
HEADER_BYTES = 128

VALID_STATUS = 0x12345678
MOCK_SIGNATURE_BYTE = 0xED

_HEX = r"0x[0-9A-Fa-f]+"


def _parse_hex(text: str) -> int:
    return int(text, 16)


def parse_cpp_constants(header: Path, pt_header: Path) -> dict:
    """Extract the boot contract from the C++ headers.

    Deliberately strict: if a constant cannot be found the verifier fails
    instead of silently falling back to a copy of the value, because a stale
    copy is exactly the drift this script exists to catch.
    """
    text = header.read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"FIRMWARE_MAGIC\s*=\s*({_HEX})", text)
    if not match:
        raise ValueError(f"FIRMWARE_MAGIC not found in {header}")
    firmware_magic = _parse_hex(match.group(1))

    pt_text = pt_header.read_text(encoding="utf-8", errors="replace")
    pt_magic_match = re.search(rf"PT_MAGIC\s*=\s*({_HEX})", pt_text)
    if not pt_magic_match:
        raise ValueError(f"PT_MAGIC not found in {pt_header}")
    pt_magic = _parse_hex(pt_magic_match.group(1))

    entries = {}
    for name in ("BOOTLOADER", "PART_A", "PART_B", "VFS"):
        match = re.search(
            rf"PT_{name}\s*=\s*\{{\s*({_HEX})\s*,\s*({_HEX})\s*\}}", pt_text
        )
        if not match:
            raise ValueError(f"PT_{name} not found in {pt_header}")
        entries[name.lower()] = (_parse_hex(match.group(1)), _parse_hex(match.group(2)))

    return {
        "firmware_magic": firmware_magic,
        "pt_magic": pt_magic,
        "entries": entries,
    }


def parse_packer_constants(packer: Path) -> dict:
    text = packer.read_text(encoding="utf-8", errors="replace")
    # Strip `#` comments so the multi-line struct.pack(...) argument list can be
    # matched with a single regex (the real call annotates each pair inline).
    text_no_comments = re.sub(r"#[^\n]*", "", text)
    values = {}
    for name in ("FIRMWARE_MAGIC", "PT_MAGIC", "VALID_STATUS"):
        match = re.search(rf"^{name}\s*=\s*({_HEX})", text, re.MULTILINE)
        if not match:
            raise ValueError(f"{name} not found in {packer}")
        values[name] = _parse_hex(match.group(1))

    # struct.pack('<I II II II II', PT_MAGIC, bl_off, bl_size, a_off, a_size, ...)
    match = re.search(
        r"struct\.pack\(\s*'<I II II II II'\s*,\s*PT_MAGIC\s*,"
        r"\s*(" + _HEX + r")\s*,\s*(" + _HEX + r")\s*,"
        r"\s*(" + _HEX + r")\s*,\s*(" + _HEX + r")\s*,"
        r"\s*(" + _HEX + r")\s*,\s*(" + _HEX + r")\s*,"
        r"\s*(" + _HEX + r")\s*,\s*(" + _HEX + r")",
        text_no_comments,
    )
    if not match:
        raise ValueError(f"partition table pack() call not found in {packer}")
    nums = [_parse_hex(match.group(i)) for i in range(1, 9)]
    values["entries"] = {
        "bootloader": (nums[0], nums[1]),
        "part_a": (nums[2], nums[3]),
        "part_b": (nums[4], nums[5]),
        "vfs": (nums[6], nums[7]),
    }
    return values


def check_contract(packer: dict, cpp: dict) -> list[str]:
    """Compare the Python packer with the C++ bootloader's view of the world."""
    problems: list[str] = []
    if packer["FIRMWARE_MAGIC"] != cpp["firmware_magic"]:
        problems.append(
            f"FIRMWARE_MAGIC drift: build_image.py=0x{packer['FIRMWARE_MAGIC']:08X} "
            f"firmware_header.hpp=0x{cpp['firmware_magic']:08X}"
        )
    if packer["PT_MAGIC"] != cpp["pt_magic"]:
        problems.append(
            f"PT_MAGIC drift: build_image.py=0x{packer['PT_MAGIC']:08X} "
            f"partition_table.hpp=0x{cpp['pt_magic']:08X}"
        )
    if packer["VALID_STATUS"] != VALID_STATUS:
        problems.append(
            f"VALID_STATUS drift: build_image.py=0x{packer['VALID_STATUS']:08X} "
            f"expected 0x{VALID_STATUS:08X}"
        )
    for name, (offset, size) in cpp["entries"].items():
        packed = packer["entries"].get(name)
        if packed != (offset, size):
            problems.append(
                f"partition '{name}' drift: build_image.py={packed} "
                f"partition_table.hpp=({offset:#x}, {size:#x})"
            )
    return problems


def build_image(contract: dict, app: bytes, version: int = 1) -> bytes:
    """Synthetic image builder used by --self-test (mirrors build_image.py)."""
    entries = contract["entries"]
    pt = struct.pack(
        "<I II II II II",
        contract["pt_magic"],
        *entries["bootloader"],
        *entries["part_a"],
        *entries["part_b"],
        *entries["vfs"],
    )
    pt += struct.pack("<I", zlib.crc32(pt) & 0xFFFFFFFF)
    pt = pt.ljust(PARTITION_TABLE_BYTES, b"\xFF")

    header = struct.pack(
        "<I I I I 64s 48s",
        contract["firmware_magic"],
        version,
        len(app),
        VALID_STATUS,
        bytes([MOCK_SIGNATURE_BYTE] * 64),
        bytes(48),
    )
    assert len(header) == HEADER_BYTES, len(header)
    return b"\xFF" * BOOTLOADER_BYTES + pt + header + app


def verify_image(blob: bytes, contract: dict, app: bytes | None = None) -> list[str]:
    """Return a list of problems; empty means the image is structurally valid."""
    problems: list[str] = []
    entries = contract["entries"]

    if len(blob) < HEADER_OFFSET + HEADER_BYTES:
        return [f"image too short: {len(blob)} bytes"]

    pt = blob[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES]
    magic, = struct.unpack_from("<I", pt, 0)
    if magic != contract["pt_magic"]:
        problems.append(f"partition table magic 0x{magic:08X} != 0x{contract['pt_magic']:08X}")

    expected = struct.pack(
        "<I II II II II",
        contract["pt_magic"],
        *entries["bootloader"],
        *entries["part_a"],
        *entries["part_b"],
        *entries["vfs"],
    )
    if pt[:len(expected)] != expected:
        problems.append("partition table entries do not match partition_table.hpp")
    stored_crc, = struct.unpack_from("<I", pt, len(expected))
    actual_crc = zlib.crc32(pt[:len(expected)]) & 0xFFFFFFFF
    if stored_crc != actual_crc:
        problems.append(f"partition table CRC 0x{stored_crc:08X} != 0x{actual_crc:08X}")

    header = blob[HEADER_OFFSET:HEADER_OFFSET + HEADER_BYTES]
    fmagic, version, image_size, status = struct.unpack_from("<I I I I", header, 0)
    signature = header[16:80]
    reserved = header[80:128]

    if fmagic != contract["firmware_magic"]:
        problems.append(f"firmware magic 0x{fmagic:08X} != 0x{contract['firmware_magic']:08X}")
    if version == 0:
        problems.append("firmware version must be >= 1")
    if status != VALID_STATUS:
        problems.append(f"firmware status 0x{status:08X} != VALID (0x{VALID_STATUS:08X})")
    if reserved != bytes(48):
        problems.append("firmware header reserved bytes are not zero")
    if signature == bytes(64):
        problems.append("firmware signature is all zero (bootloader would reject it)")

    part_a_size = entries["part_a"][1]
    if image_size == 0 or image_size > part_a_size - HEADER_BYTES:
        problems.append(
            f"image_size {image_size} outside bootloader bounds (1..{part_a_size - HEADER_BYTES})"
        )

    expected_len = HEADER_OFFSET + HEADER_BYTES + image_size
    if len(blob) != expected_len:
        problems.append(
            f"image length {len(blob)} != header-declared {expected_len} "
            f"(trailing data or truncated image)"
        )

    payload = blob[HEADER_OFFSET + HEADER_BYTES:HEADER_OFFSET + HEADER_BYTES + image_size]
    if app is not None and payload != app:
        problems.append(
            f"application payload mismatch: image carries {len(payload)} bytes, "
            f"{len(app)} bytes supplied"
        )

    return problems


def _self_test() -> int:
    failures = 0

    def check(name: str, condition: bool, detail: str = "") -> None:
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL [{name}] {detail}")

    packer_path = REPO_ROOT / "scripts" / "build_image.py"
    header_path = REPO_ROOT / "kernel" / "core" / "firmware_header.hpp"
    pt_path = REPO_ROOT / "config" / "partition_table.hpp"

    try:
        contract = parse_cpp_constants(header_path, pt_path)
        packer = parse_packer_constants(packer_path)
    except (OSError, ValueError) as exc:
        print(f"FAIL [contract parse] {exc}")
        return 1

    drift = check_contract(packer, contract)
    check("packer matches C++ headers", not drift, "; ".join(drift))

    app = bytes(range(256)) * 4
    blob = build_image(contract, app)
    check("synthetic image is valid", verify_image(blob, contract, app) == [],
          str(verify_image(blob, contract, app)))

    corrupt = bytearray(blob)
    corrupt[HEADER_OFFSET] ^= 0xFF
    check("bad firmware magic detected",
          any("firmware magic" in p for p in verify_image(bytes(corrupt), contract)))

    corrupt = bytearray(blob)
    corrupt[PARTITION_TABLE_OFFSET + 4] ^= 0xFF
    check("bad partition entry detected",
          any("entries do not match" in p for p in verify_image(bytes(corrupt), contract)))

    corrupt = bytearray(blob)
    corrupt[PARTITION_TABLE_OFFSET + 36] ^= 0xFF
    check("bad partition CRC detected",
          any("CRC" in p for p in verify_image(bytes(corrupt), contract)))

    check("payload mismatch detected",
          any("payload mismatch" in p for p in verify_image(blob, contract, app + b"x")))

    check("truncated image detected",
          any("length" in p for p in verify_image(blob[:-1], contract)))

    zero_sig = bytearray(blob)
    zero_sig[HEADER_OFFSET + 16:HEADER_OFFSET + 80] = bytes(64)
    check("all-zero signature detected",
          any("all zero" in p for p in verify_image(bytes(zero_sig), contract)))

    total = 8
    if failures:
        print(f"\n{failures}/{total} self-test case(s) failed.")
        return 1
    print(f"self-test OK ({total} cases)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify an AuroraOS secure-boot flash image.")
    parser.add_argument("image", nargs="?", type=Path, help="flash.bin produced by build_image.py")
    parser.add_argument("--app", type=Path, help="application binary that was packed")
    parser.add_argument("--firmware-header", type=Path,
                        default=REPO_ROOT / "kernel" / "core" / "firmware_header.hpp")
    parser.add_argument("--partition-header", type=Path,
                        default=REPO_ROOT / "config" / "partition_table.hpp")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.image:
        parser.error("an image path is required (or use --self-test)")

    try:
        contract = parse_cpp_constants(args.firmware_header, args.partition_header)
        blob = args.image.read_bytes()
        app = args.app.read_bytes() if args.app else None
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    problems = verify_image(blob, contract, app)
    if problems:
        for problem in problems:
            print(f"::error::boot image invalid -- {problem}")
        return 1

    print(
        f"::notice::Boot image OK: {args.image} "
        f"({len(blob)} bytes, app {len(blob) - HEADER_OFFSET - HEADER_BYTES} bytes, "
        f"mock signature)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
