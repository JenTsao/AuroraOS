#!/usr/bin/env python3
"""Per-board firmware size budget gate for AuroraOS.

Why this exists
---------------
`firmware-size` historically produced a Markdown table and nothing else: a
board could grow by 20 KB and the report would still say "OK".  The real
constraints live in the linker scripts (``config/linker*.ld``) and in the
board datasheets, and they differ by more than an order of magnitude --
MiBand 8 has 384 KB SRAM while Nucleo-L031K6 has 8 KB.  A single global
threshold either lets the small boards overflow or blocks the large ones.

This gate enforces, per board:

  * hard region limits (``flash_bytes`` / ``ram_bytes``) taken from the
    linker script MEMORY map, so an overflowing image fails here instead of
    silently producing a non-bootable binary;
  * section ratchets (``text`` / ``data`` / ``bss``), so growth is a
    deliberate act: the budget is raised in the same commit that spends it.

Ratchet budgets are "rounded up" measurements (see ``--update``), not
guesses: 4 KiB granularity for .text, 1 KiB for .data/.bss.  That leaves
just enough headroom for ordinary refactoring while still catching a
stray buffer or an accidentally linked subsystem.

Usage
-----
    size_budget.py --board lm3s6965-qb --elf build/auroraOS.elf \
                   --size-cmd arm-none-eabi-size
    size_budget.py --board lm3s6965-qb --size-text size.txt   # offline
    size_budget.py --board lm3s6965-qb --elf ... --update     # raise budget
    size_budget.py --self-test

Exit codes: 0 = within budget (or no budget configured), 1 = over budget,
2 = usage / parse error.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

BUDGET_PATH = Path(__file__).resolve().parent.parent / "config" / "size_budget.json"

# `arm-none-eabi-size` prints a header line then one line per section:
#    text    data     bss     dec     hex filename
#   57596    6400   41424  105420   19bcc auroraOS.elf
_SIZE_RE = re.compile(
    r"^\s*(?P<text>\d+)\s+(?P<data>\d+)\s+(?P<bss>\d+)\s+"
    r"(?P<dec>\d+)\s+(?P<hex>[0-9a-fA-F]+)\s+(?P<file>.+?)\s*$"
)

TEXT_GRANULARITY = 4096
SMALL_GRANULARITY = 1024

# Section order is fixed so the report is stable across runs.
SECTIONS = ("text", "data", "bss")


def parse_size_output(text: str) -> dict[str, int]:
    """Return {'text','data','bss'} from the last `size` data line.

    Only the final matching line is used: multi-object invocations print one
    line per object followed by a total, and the total is what we budget.
    """
    match = None
    for line in text.splitlines():
        candidate = _SIZE_RE.match(line)
        if candidate:
            match = candidate
    if match is None:
        raise ValueError("no 'text data bss dec hex file' line found in size output")
    return {name: int(match.group(name)) for name in SECTIONS}


def round_up(value: int, granularity: int) -> int:
    """Round `value` up to the next multiple of `granularity` (never to 0)."""
    if value <= 0:
        return granularity
    return ((value + granularity - 1) // granularity) * granularity


def ratchet_budget(measured: dict[str, int]) -> dict[str, int]:
    """Derive section budgets from a measurement (used by --update)."""
    return {
        "text": round_up(measured["text"], TEXT_GRANULARITY),
        "data": round_up(measured["data"], SMALL_GRANULARITY),
        "bss": round_up(measured["bss"], SMALL_GRANULARITY),
    }


def evaluate(measured: dict[str, int], budget: dict) -> list[str]:
    """Return human-readable violations; empty list means the image fits.

    `budget` may omit any key: a missing key means "not enforced yet", which
    keeps boards without a measured baseline (rv32/aarch64 at the time of
    writing) reportable without turning the job red.
    """
    violations: list[str] = []

    flash_used = measured["text"] + measured["data"]
    ram_used = measured["data"] + measured["bss"]

    flash_limit = budget.get("flash_bytes")
    if flash_limit is not None and flash_used > flash_limit:
        violations.append(
            f"flash: text+data = {flash_used} B exceeds region limit {flash_limit} B "
            f"by {flash_used - flash_limit} B"
        )

    ram_limit = budget.get("ram_bytes")
    if ram_limit is not None and ram_used > ram_limit:
        violations.append(
            f"ram: data+bss = {ram_used} B exceeds region limit {ram_limit} B "
            f"by {ram_used - ram_limit} B"
        )

    for section in SECTIONS:
        cap = budget.get(section)
        if cap is None:
            continue
        used = measured[section]
        if used > cap:
            violations.append(
                f"{section}: {used} B exceeds ratchet budget {cap} B "
                f"by {used - cap} B"
            )

    return violations


def load_budgets(path: Path) -> dict[str, dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    boards = data.get("boards")
    if not isinstance(boards, dict):
        raise ValueError(f"{path}: missing 'boards' object")
    return boards


def save_budgets(path: Path, boards: dict[str, dict]) -> None:
    payload = {
        "_comment": (
            "Per-board firmware size budgets. flash_bytes/ram_bytes mirror the "
            "linker-script MEMORY map; text/data/bss are ratchets raised by "
            "`python scripts/size_budget.py --board <b> --elf <elf> --update`."
        ),
        "boards": boards,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def read_size(elf: Path, size_cmd: str) -> dict[str, int]:
    result = subprocess.run(
        [size_cmd, str(elf)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise ValueError(
            f"'{size_cmd} {elf}' failed (exit {result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return parse_size_output(result.stdout)


def _self_test() -> int:
    failures = 0

    def check(name: str, condition: bool, detail: str = "") -> None:
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL [{name}] {detail}")

    sample = (
        "   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
        "  57596\t   6400\t  41424\t 105420\t  19bcc\tauroraOS.elf\n"
    )
    measured = parse_size_output(sample)
    check("parse text", measured["text"] == 57596, str(measured))
    check("parse data", measured["data"] == 6400, str(measured))
    check("parse bss", measured["bss"] == 41424, str(measured))

    # Multiple objects: the total (last line) wins.
    multi = sample + "   1000\t     16\t    200\t   1216\t    4c0\ttotal\n"
    check("last line wins", parse_size_output(multi)["text"] == 1000)

    try:
        parse_size_output("garbage\n")
    except ValueError:
        pass
    else:
        check("garbage rejected", False)

    budget = ratchet_budget(measured)
    check("text rounded to 4 KiB", budget["text"] == 61440, str(budget))
    check("data rounded to 1 KiB", budget["data"] == 7168, str(budget))
    check("bss rounded to 1 KiB", budget["bss"] == 41984, str(budget))

    check("exact fit passes", evaluate(measured, {**budget, "flash_bytes": 63996, "ram_bytes": 47824}) == [])

    over = evaluate(measured, {"text": measured["text"] - 1})
    check("text over budget detected", len(over) == 1 and "text" in over[0], str(over))

    over = evaluate(measured, {"flash_bytes": 63995})
    check("flash over limit detected", len(over) == 1 and "flash" in over[0], str(over))

    over = evaluate(measured, {"ram_bytes": 47823})
    check("ram over limit detected", len(over) == 1 and "ram" in over[0], str(over))

    check("missing budget keys are not enforced", evaluate(measured, {}) == [])

    check("round_up never returns 0", round_up(0, 1024) == 1024)

    total = 14
    if failures:
        print(f"\n{failures}/{total} self-test case(s) failed.")
        return 1
    print(f"self-test OK ({total} cases)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="AuroraOS per-board size budget gate.")
    parser.add_argument("--board", help="board id as used by -DBOARD= (e.g. lm3s6965-qb)")
    parser.add_argument("--elf", type=Path, help="firmware ELF to measure")
    parser.add_argument("--size-cmd", default="size",
                        help="size tool to invoke (default: size)")
    parser.add_argument("--size-text", type=Path,
                        help="use a captured `size` output instead of running the tool")
    parser.add_argument("--budget-file", type=Path, default=BUDGET_PATH)
    parser.add_argument("--update", action="store_true",
                        help="rewrite the ratchet budgets from this measurement")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.board:
        parser.error("--board is required")
    if not args.elf and not args.size_text:
        parser.error("either --elf or --size-text is required")

    try:
        boards = load_budgets(args.budget_file)
    except (OSError, ValueError) as exc:
        print(f"ERROR: cannot load {args.budget_file}: {exc}", file=sys.stderr)
        return 2

    budget = boards.get(args.board)
    if budget is None:
        print(f"::notice::No size budget configured for '{args.board}' -- reporting only.")
        return 0

    try:
        if args.size_text:
            measured = parse_size_output(args.size_text.read_text(encoding="utf-8", errors="replace"))
        else:
            measured = read_size(args.elf, args.size_cmd)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.update:
        budget.update(ratchet_budget(measured))
        boards[args.board] = budget
        save_budgets(args.budget_file, boards)
        print(f"Budget for '{args.board}' updated from measurement: {budget}")
        return 0

    violations = evaluate(measured, budget)
    flash_used = measured["text"] + measured["data"]
    ram_used = measured["data"] + measured["bss"]
    print(
        f"{args.board}: text={measured['text']} data={measured['data']} "
        f"bss={measured['bss']} | flash={flash_used} ram={ram_used}"
    )
    for section in SECTIONS:
        cap = budget.get(section)
        if cap is not None:
            print(f"  {section:<4} {measured[section]:>8} / {cap:>8} B")
    if budget.get("flash_bytes") is not None:
        print(f"  flash {flash_used:>8} / {budget['flash_bytes']:>8} B")
    if budget.get("ram_bytes") is not None:
        print(f"  ram   {ram_used:>8} / {budget['ram_bytes']:>8} B")

    if violations:
        for violation in violations:
            print(f"::error::size budget exceeded -- {violation}")
        print(
            "\nIf the growth is intentional, raise the budget in the same commit:\n"
            f"  python scripts/size_budget.py --board {args.board} --elf <elf> --update"
        )
        return 1

    print(f"::notice::Size budget OK for '{args.board}'.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
