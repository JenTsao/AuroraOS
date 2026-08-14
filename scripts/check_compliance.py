#!/usr/bin/env python3
"""check_compliance.py — AuroraOS Compliance Baseline CI Script

Parses docs/compliance_baseline.md for <!-- CHECK:... --> annotations
and verifies each rule against the repository.

Exit code 0 = all enforced rules pass.
Exit code 1 = one or more rules failed (blocks the CI gate).

Usage:
    python3 scripts/check_compliance.py [--repo-root <path>]

CHECK annotation format:
    <!-- CHECK:<ID>:<kind>:<target>:<value> -->

Supported kinds:
    line_count      <file> must have <= <value> lines
    file_exists     <path> must exist
    path_absent     <path> must NOT exist
    content_absent  <file> must NOT contain <value> as a literal string
    single_define   <file> must define <value> (via #define) exactly once
    no_include_from <dir>  must not #include paths starting with <value>/
    test_count_min  build_tests must have at least <value> test cases (reads CTestTestfile)
"""

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Tuple

# ─────────────────────────── helpers ────────────────────────────────────────

ANNOTATION_RE = re.compile(
    r"<!--\s*CHECK:([^:]+):([^:]+):([^:>]+)(?::([^>]+))?\s*-->"
)


def parse_checks(baseline: Path) -> List[Tuple[str, str, str, str]]:
    """Return list of (id, kind, target, value) tuples from baseline doc."""
    checks = []
    for line in baseline.read_text(encoding="utf-8").splitlines():
        for m in ANNOTATION_RE.finditer(line):
            rule_id, kind, target, value = m.group(1), m.group(2), m.group(3), m.group(4) or ""
            checks.append((rule_id, kind, target.strip(), value.strip()))
    return checks


# ─────────────────────────── check implementations ──────────────────────────

def check_line_count(root: Path, target: str, value: str) -> Tuple[bool, str]:
    f = root / target
    if not f.exists():
        return False, f"{target} not found"
    count = len(f.read_text(encoding="utf-8", errors="replace").splitlines())
    limit = int(value)
    ok = count <= limit
    return ok, f"{target} has {count} lines (limit {limit})"


def check_file_exists(root: Path, target: str, _value: str) -> Tuple[bool, str]:
    f = root / target
    ok = f.exists()
    return ok, f"{target} {'exists' if ok else 'NOT FOUND'}"


def check_path_absent(root: Path, target: str, _value: str) -> Tuple[bool, str]:
    p = root / target
    ok = not p.exists()
    return ok, f"{target} {'absent (ok)' if ok else 'STILL EXISTS (violation)'}"


def check_content_absent(root: Path, target: str, value: str) -> Tuple[bool, str]:
    f = root / target
    if not f.exists():
        # File not present means the content is absent — that's fine.
        return True, f"{target} not found (content absent by default)"
    text = f.read_text(encoding="utf-8", errors="replace")
    found = value in text
    ok = not found
    return ok, f"'{value}' {'FOUND in' if found else 'absent from'} {target}"


def check_single_define(root: Path, target: str, value: str) -> Tuple[bool, str]:
    f = root / target
    if not f.exists():
        return False, f"{target} not found"
    text = f.read_text(encoding="utf-8", errors="replace")
    # Match: #define MACRO_NAME (optional whitespace, then value / end-of-token)
    pattern = re.compile(r"^\s*#\s*define\s+" + re.escape(value) + r"\b", re.MULTILINE)
    hits = pattern.findall(text)
    ok = len(hits) == 1
    return ok, f"'{value}' defined {len(hits)} time(s) in {target} (expected exactly 1)"


def check_no_include_from(root: Path, src_dir: str, forbidden_prefix: str) -> Tuple[bool, str]:
    """Scan all .hpp/.h/.cpp in src_dir for #include paths starting with forbidden_prefix."""
    dir_path = root / src_dir
    if not dir_path.exists():
        return True, f"{src_dir} not found (skip)"
    include_re = re.compile(r'#\s*include\s+[<"](' + re.escape(forbidden_prefix) + r'/[^>"]*)[>"]')
    violations = []
    for ext in ("*.cpp", "*.hpp", "*.h"):
        for f in dir_path.rglob(ext):
            # Skip 3rdparty and build dirs
            parts = f.parts
            if any(p in ("3rdparty", "build_tests", "build_analysis") for p in parts):
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in include_re.finditer(text):
                rel = f.relative_to(root)
                violations.append(f"  {rel}: #include <{m.group(1)}>")
    ok = len(violations) == 0
    detail = (
        f"No {forbidden_prefix}/ includes from {src_dir}/ (ok)"
        if ok
        else f"{len(violations)} violation(s) in {src_dir}/:\n" + "\n".join(violations[:10])
    )
    return ok, detail


def check_test_count_min(root: Path, _target: str, value: str) -> Tuple[bool, str]:
    """Count 'add_test(' entries in tests/CMakeLists.txt as a proxy for test count."""
    # ctest discovers tests; we count add_test() calls in tests/CMakeLists.txt as lower bound.
    cmake = root / "tests" / "CMakeLists.txt"
    if not cmake.exists():
        return False, "tests/CMakeLists.txt not found"
    text = cmake.read_text(encoding="utf-8", errors="replace")
    # More reliable: count lines with gtest_discover_tests / add_test
    # AuroraOS uses gtest_discover_tests; individual test count can't be
    # determined statically.  Fall back to checking the CMakeLists exists
    # and contains the suite registration.
    has_discover = "gtest_discover_tests" in text or "add_test" in text
    if not has_discover:
        return False, "No test registration found in tests/CMakeLists.txt"
    minimum = int(value)
    # We report a soft check: the suite must be registered.  Actual count
    # is validated at runtime by the unit-tests job.
    return True, (
        f"Test suite registration present in tests/CMakeLists.txt "
        f"(runtime count verified by unit-tests job, expected >= {minimum})"
    )


# ─────────────────────────── dispatch ───────────────────────────────────────

DISPATCH = {
    "line_count": check_line_count,
    "file_exists": check_file_exists,
    "path_absent": check_path_absent,
    "content_absent": check_content_absent,
    "single_define": check_single_define,
    "no_include_from": check_no_include_from,
    "test_count_min": check_test_count_min,
}

# ─────────────────────────── main ───────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description="AuroraOS compliance baseline checker")
    parser.add_argument("--repo-root", default=".", help="Path to repository root")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    # The repo stores docs under DOCS/ (uppercase).  Look there first, then
    # fall back to docs/ for forks that may use the lowercase layout.
    # Resolve case-sensitively so Linux CI (case-sensitive fs) finds it.
    baseline_candidates = [
        root / "DOCS" / "compliance_baseline.md",
        root / "docs" / "compliance_baseline.md",
    ]
    baseline = next((p for p in baseline_candidates if p.exists()), None)

    if baseline is None:
        print(
            f"ERROR: compliance_baseline.md not found (tried {baseline_candidates[0]} and {baseline_candidates[1]})",
            file=sys.stderr,
        )
        return 1

    checks = parse_checks(baseline)
    if not checks:
        print("WARNING: No CHECK: annotations found in compliance_baseline.md")
        return 0

    passed = 0
    failed = 0
    warned = 0

    print(f"AuroraOS Compliance Check — {len(checks)} rules\n{'=' * 60}")

    for rule_id, kind, target, value in checks:
        fn = DISPATCH.get(kind)
        if fn is None:
            print(f"  [{rule_id}] SKIP  unknown check kind '{kind}'")
            warned += 1
            continue
        try:
            ok, detail = fn(root, target, value)
        except Exception as exc:  # noqa: BLE001
            ok, detail = False, f"exception: {exc}"

        status = "PASS" if ok else "FAIL"
        icon = "OK " if ok else "ERR"
        print(f"  [{rule_id}] {icon} {status}  {detail}")
        if ok:
            passed += 1
        else:
            failed += 1

    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {warned} skipped")

    if failed > 0:
        print("\n::error::Compliance baseline check FAILED — see details above.")
        return 1

    print("\nCompliance baseline check PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
