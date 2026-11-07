#!/usr/bin/env python3
"""qemu_log_check.py — assert the QEMU serial transcript of an AuroraOS image.

The previous QEMU "smoke test" only grepped for the ``aurora> `` shell prompt.
That proves the CPU reached one instruction in the shell task and nothing else.
This script turns the smoke into a per-key-path assertion gate.

Expectation files live in ``scripts/qemu_expectations/<target>.txt`` and use
two directives:

    CHECK-SUCC: <literal text>   # must appear in the serial transcript
    CHECK-FAIL: <literal text>   # must NOT appear (panic / HardFault / ...)
    SEND: <shell command>        # capture mode only: drive the shell first

Every entry is reported with PASS/FAIL.  A CHECK-SUCC miss prints the expected
text and what the log actually contained (line count + tail); a CHECK-FAIL hit
prints the offending log line number.  Exit code is 1 if any entry fails, so CI
fails directly.

Two input modes
---------------
    # Check a transcript captured by `timeout ... | tee boot.log`
    qemu_log_check.py --target qemu_aarch64_virt --log boot_aarch64.log

    # Let this script boot QEMU and drive the shell first
    qemu_log_check.py --target lm3s6965-qb --elf auroraOS.elf \
        --qemu qemu-system-arm --machine lm3s6965evb --cpu cortex-m3 \
        --port 1230 --out ../boot.log

Capture mode uses a TCP serial socket (``-serial tcp:...,server``) so the same
script works on Linux CI and on a Windows/macOS developer machine.
"""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import NamedTuple

PROMPT = "aurora> "


class Expectation(NamedTuple):
    kind: str          # CHECK-SUCC / CHECK-FAIL / SEND
    text: str
    lineno: int


def parse_expectations(path: Path) -> list[Expectation]:
    entries: list[Expectation] = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"^(CHECK-SUCC|CHECK-FAIL|SEND)\s*:\s*(.*)$", line)
        if not match:
            raise ValueError(
                f"{path}:{lineno}: expected 'CHECK-SUCC: ...', 'CHECK-FAIL: ...' or 'SEND: ...'"
            )
        kind, text = match.group(1), match.group(2)
        if not text:
            raise ValueError(f"{path}:{lineno}: empty {kind} entry")
        entries.append(Expectation(kind, text, lineno))
    return entries


class SerialCapture:
    """Minimal TCP serial reader/writer; works where pexpect.spawn does not."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.sock: socket.socket | None = None
        self.buffer = ""
        self.transcript = ""

    def connect(self) -> bool:
        for _ in range(80):
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                sock.settimeout(0.2)
                self.sock = sock
                return True
            except OSError:
                time.sleep(0.1)
        return False

    def _pump(self) -> None:
        assert self.sock is not None
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return
        if not data:
            raise EOFError
        text = data.decode("utf-8", "replace")
        self.buffer += text
        self.transcript += text

    def wait_for(self, needle: str, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.buffer:
                index = self.buffer.index(needle) + len(needle)
                self.buffer = self.buffer[index:]
                return True
            try:
                self._pump()
            except EOFError:
                return False
        return False

    def send(self, command: str) -> None:
        assert self.sock is not None
        self.sock.sendall((command + "\r").encode())

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass


def capture(args: argparse.Namespace, expectations: list[Expectation]) -> tuple[str, bool]:
    """Boot QEMU, run SEND commands, return (transcript, reached_prompt)."""
    cmd = [args.qemu, "-M", args.machine]
    if args.cpu:
        cmd += ["-cpu", args.cpu]
    if args.bios_none:
        cmd += ["-bios", "none"]
    cmd += [
        "-display", "none",
        "-monitor", "none",
        "-serial", f"tcp:127.0.0.1:{args.port},server",
        "-kernel", args.elf,
    ]
    cmd += args.qemu_arg

    print("[QEMU] " + " ".join(cmd))
    process = subprocess.Popen(cmd)
    serial = SerialCapture(args.port)
    try:
        if not serial.connect():
            print("[QEMU] serial connection failed", file=sys.stderr)
            return "", False
        reached_prompt = serial.wait_for(PROMPT, args.boot_timeout)
        if not reached_prompt:
            print(f"[QEMU] shell prompt not reached within {args.boot_timeout:g}s")
            return serial.transcript, False

        for entry in expectations:
            if entry.kind != "SEND":
                continue
            timeout = args.selftest_timeout if entry.text == "selftest" else args.cmd_timeout
            serial.send(entry.text)
            if not serial.wait_for(PROMPT, timeout):
                print(f"[QEMU] no prompt after command: {entry.text!r}")
                break
        return serial.transcript, True
    finally:
        serial.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


def check_expectations(log: str, expectations: list[Expectation], target: str) -> int:
    lines = log.splitlines()
    failures = 0
    passes = 0

    print(f"\nQEMU serial assertions for {target}")
    print(f"  transcript: {len(log)} chars, {len(lines)} lines")

    for entry in expectations:
        if entry.kind == "SEND":
            continue
        if entry.kind == "CHECK-SUCC":
            if entry.text in log:
                print(f"[PASS] CHECK-SUCC  {entry.text}")
                passes += 1
            else:
                failures += 1
                print(f"[FAIL] CHECK-SUCC  {entry.text}")
                print(f"       expected: must appear in serial output")
                print(f"       captured: no matching line ({len(lines)} lines captured)")
                _print_tail(lines)
        else:  # CHECK-FAIL
            hit = next((i for i, line in enumerate(lines, 1) if entry.text in line), None)
            if hit is None:
                print(f"[PASS] CHECK-FAIL  {entry.text}")
                passes += 1
            else:
                failures += 1
                print(f"[FAIL] CHECK-FAIL  {entry.text}")
                print(f"       expected: must NOT appear in serial output")
                print(f"       captured: {entry.text!r} at log line {hit}")
                print(f"                 {lines[hit - 1].rstrip()}")

    print(f"\nRESULT: {passes} passed, {failures} failed")
    return 1 if failures else 0


def _print_tail(lines: list[str], count: int = 8) -> None:
    printable = [line.rstrip() for line in lines if line.strip()]
    if not printable:
        print("       log tail: <empty>")
        return
    print("       log tail:")
    for line in printable[-count:]:
        print(f"         | {line}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--target", required=True, help="Expectation profile name, e.g. lm3s6965-qb")
    parser.add_argument("--expect-dir", default=None, help="Directory with <target>.txt profiles")
    parser.add_argument("--log", default=None, help="Check an existing transcript (no QEMU boot)")
    parser.add_argument("--out", default=None, help="Write the captured transcript here")
    parser.add_argument("--elf", default=None, help="Firmware ELF to boot in capture mode")
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--machine", default="lm3s6965evb")
    parser.add_argument("--cpu", default="cortex-m3")
    parser.add_argument("--bios-none", dest="bios_none", action="store_true")
    parser.add_argument("--port", type=int, default=1230)
    parser.add_argument("--boot-timeout", type=float, default=30.0)
    parser.add_argument("--cmd-timeout", type=float, default=10.0)
    parser.add_argument("--selftest-timeout", type=float, default=90.0)
    parser.add_argument(
        "--qemu-arg",
        action="append",
        default=[],
        help="Extra QEMU argument (repeatable), e.g. --qemu-arg=-D --qemu-arg=qemu.log",
    )
    args = parser.parse_args(argv)

    if not args.log and not args.elf:
        parser.error("either --log or --elf is required")

    root = Path(__file__).resolve().parent.parent
    expect_dir = Path(args.expect_dir) if args.expect_dir else root / "scripts" / "qemu_expectations"
    profile = expect_dir / f"{args.target}.txt"
    if not profile.is_file():
        print(f"error: expectation profile not found: {profile}", file=sys.stderr)
        return 2
    try:
        expectations = parse_expectations(profile)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.log:
        log_path = Path(args.log)
        if not log_path.is_file():
            print(f"error: transcript not found: {log_path}", file=sys.stderr)
            return 2
        log = log_path.read_text(encoding="utf-8", errors="replace")
    else:
        log, reached_prompt = capture(args, expectations)
        if args.out:
            with Path(args.out).open("w", encoding="utf-8", newline="") as handle:
                handle.write(log)
        if not reached_prompt:
            print("[QEMU] WARNING: capture ended before the shell prompt", file=sys.stderr)

    return check_expectations(log, expectations, args.target)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
