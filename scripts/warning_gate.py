#!/usr/bin/env python3
"""Compiler-warning ratchet gate for AuroraOS firmware builds.

Why this exists
---------------
The firmware toolchain files (`config/toolchain*.cmake`) carry only
architecture flags (`-mcpu`, `-march`, `-ffreestanding`, ...).  No
`-Wall`/`-Wextra` anywhere, so the compiler is currently silent about
uninitialised use, sign/size mismatches and unused parameters -- exactly
the defect classes that corrupt kernel state (AGENTS.md sections 22 and 47).
`clang-tidy` cannot cover them: it is non-blocking by design and cannot parse
the bare-metal translation units.  This gate is the only signal that reaches
*every* source file of *every* target.

How it works
------------
The build runs with `-Wall -Wextra` injected via the `CFLAGS`/`CXXFLAGS`
environment variables -- NOT via `-DCMAKE_C_FLAGS`, which would *replace*
`CMAKE_C_FLAGS_INIT` and silently drop the architecture flags (see the note in
`.github/workflows/build.yml`).  The resulting log is parsed, normalised into
`path [-Wflag] message` keys, and compared against a per-board baseline under
`config/warning_baseline/`.

A diagnostic that is not in the baseline fails the gate, so the warning count
can only ever ratchet down.  Renumbering lines or reformatting a function does
not churn the baseline because line/column numbers are deliberately dropped
from the key.

Vendored code under `3rdparty/` is excluded: AGENTS.md section 26 forbids
patching third-party sources, so a warning we are not allowed to fix must not
be able to break the build.  Linker/toolchain chatter (e.g. LTO's "using serial
compilation of N LTRANS jobs") carries no line number and is filtered by the
diagnostic regex itself.

Baseline bootstrap
------------------
When the baseline file for a board does not exist the gate runs in bootstrap
mode: it reports every diagnostic, uploads the normalised list as an artifact,
and exits 0 with a `::warning::`.  This mirrors the `COVERAGE_MIN_LINE=0.0`
bootstrap already used by the coverage job.  To adopt a baseline:

    python3 scripts/warning_gate.py --update-baseline \\
        config/warning_baseline/<board>.txt build.log

Usage
-----
    warning_gate.py --baseline <file> <build.log>       # enforce (CI)
    warning_gate.py --report         <build.log>        # never fail
    warning_gate.py --update-baseline <file> <build.log># rewrite baseline
    warning_gate.py --self-test                         # parser unit checks

Exit codes: 0 = within baseline, 1 = new warnings, 2 = usage/IO error.
"""

from __future__ import annotations

import argparse
import codecs
import posixpath
import re
import sys
from pathlib import Path

# `path:line:col: warning: msg [-Wflag]` and the two-column variant
# `path:line: warning: msg`.  Anchored at line start so that continuation
# lines and `note:` diagnostics are ignored.  The file group must tolerate
# colons (Windows `C:/...` paths); non-greedy matching makes the engine
# settle on the last `:digits:` pair before `warning:`.  Requiring a line
# number is what filters out toolchain chatter such as
# `lto-wrapper: warning: ...`.
_DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+)"
    r"(?::(?P<col>\d+))?:\s+warning:\s+(?P<msg>.+?)\s*$"
)

# Trailing ` [-Wsome-flag]` (GCC >= 6, Clang).  `#warning` directives report as
# `[-Wcpp]`, so they are captured like any other diagnostic.
_FLAG_RE = re.compile(r"\s+\[-W(?P<flag>[A-Za-z0-9=_-]+)\]\s*$")

_WS_RE = re.compile(r"\s+")

# Windows drive-letter prefix (`C:`), stripped from *both* a diagnostic path
# and the repository root before comparison, so `C:/repo/a.cpp` matches a root
# reported as `C:\repo`. Only the letter and colon are removed -- the separator
# stays, which keeps the leading slash intact.
_DRIVE_RE = re.compile(r"^[A-Za-z]:")

# Path fragments whose diagnostics are never AuroraOS's to fix.
_VENDORED_PREFIXES = ("3rdparty/", "tests/",)

# Bare-metal toolchains emit `#warning` with host-style absolute paths and
# MSYS backslashes; normalise before comparing.
_PATH_SEP_RE = re.compile(r"[\\/]+")


class Diagnostic:
    """One normalised compiler warning."""

    __slots__ = ("path", "flag", "message", "key")

    def __init__(self, path: str, flag: str, message: str) -> None:
        self.path = path
        self.flag = flag
        self.message = message
        self.key = f"{path} [-W{flag}] {message}" if flag else f"{path} {message}"

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Diagnostic({self.key!r})"


def normalise_path(raw: str, repo_root: Path) -> str | None:
    """Return a repo-relative POSIX path, or None if the path is not ours.

    Returns None for vendored code, toolchain headers and anything outside the
    repository -- those diagnostics are either unfixable by policy or noise
    from the build environment.
    """
    path = _PATH_SEP_RE.sub("/", raw.strip()).replace("\x08", "")
    root = _PATH_SEP_RE.sub("/", str(repo_root)).replace("\x08", "")

    # Drop a Windows drive letter from *both* sides before comparing,
    # otherwise `C:/repo/a.cpp` never matches the root `C:/repo/`.
    path = _DRIVE_RE.sub("", path)
    root = _DRIVE_RE.sub("", root)
    if not root.endswith("/"):
        root += "/"

    if path.startswith(root):
        path = path[len(root):]
    elif path.startswith("/"):
        # Absolute and outside the repository (newlib, /usr/include, a build
        # directory living elsewhere): not attributable to AuroraOS source.
        return None
    # Otherwise the path is already repo-relative -- keep it as-is.

    # Collapse `.` and `..` segments BEFORE any prefix test. GCC reports a
    # header using the spelling of the #include that pulled it in, so a
    # cross-directory include surfaces as `apps/../net/bus.hpp` rather than
    # `net/bus.hpp`. Normalising first also stops `apps/../3rdparty/...` from
    # slipping past the vendored-prefix filter below -- otherwise code we are
    # forbidden to modify could still enter the baseline.
    #
    # The path is treated as POSIX text, not a native filesystem path, so the
    # result does not depend on the host OS.
    path = posixpath.normpath(path)
    if path in (".", ""):
        return None
    if path.startswith(("/", "../")) or path == "..":
        # Escapes the repository root: not ours.
        return None
    if path.startswith(_VENDORED_PREFIXES):
        return None
    # Generated Kconfig header and CMake scratch output.
    if path.startswith(("config/autoconf.", "build", "cmake-build")):
        return None
    if not re.search(r"\.(c|cc|cpp|cxx|h|hh|hpp|hxx|s|S)$", path):
        # Linker scripts, map files, archives, tool drivers.
        return None
    return path


def parse_log(text: str, repo_root: Path) -> list[Diagnostic]:
    """Extract normalised diagnostics from a compiler log."""
    found: dict[str, Diagnostic] = {}
    for raw_line in text.splitlines():
        # GCC prefixes each diagnostic with the source path on its own line
        # when the path contains spaces; the `^` anchor keeps us safe because
        # such lines never carry `:<digits>:` themselves.
        match = _DIAG_RE.match(raw_line)
        if not match:
            continue
        path = normalise_path(match.group("file"), repo_root)
        if path is None:
            continue
        message = _WS_RE.sub(" ", match.group("msg")).strip()
        flag_match = _FLAG_RE.search(message)
        flag = flag_match.group("flag") if flag_match else ""
        if flag_match:
            message = _FLAG_RE.sub("", message).strip()
        diag = Diagnostic(path, flag, message)
        # First occurrence wins so the report keeps the compiler's own wording.
        found.setdefault(diag.key, diag)
    return [found[k] for k in sorted(found)]


def read_baseline(path: Path) -> set[str]:
    """Load accepted diagnostic keys; blank lines and `#` comments ignored."""
    if not path.exists():
        return set()
    keys: set[str] = set()
    for line in read_text(path).splitlines():
        entry = line.strip()
        if entry and not entry.startswith("#"):
            keys.add(entry)
    return keys


def read_text(path: Path) -> str:
    """Decode a log or baseline, refusing encodings we cannot parse.

    A build log that is not decodable yields *zero* diagnostics, which would
    turn the gate into a silent pass -- the exact failure this gate exists to
    prevent.  UTF-16 is common because Windows shells default `>` redirection
    to it, so it is decoded explicitly; anything else containing NUL bytes is
    rejected rather than silently mis-parsed.
    """
    raw = path.read_bytes()
    if raw.startswith(codecs.BOM_UTF16_LE) or raw.startswith(codecs.BOM_UTF16_BE):
        text = raw.decode("utf-16")
    elif raw.startswith(codecs.BOM_UTF8):
        text = raw.decode("utf-8-sig")
    else:
        text = raw.decode("utf-8", errors="replace")
    if "\x00" in text:
        raise ValueError(
            f"{path} contains NUL bytes: unrecognised encoding, refusing to "
            "report zero diagnostics from an unparsable log"
        )
    return text


def write_baseline(path: Path, diags: list[Diagnostic]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = "\n".join(d.key for d in diags)
    path.write_text(body + ("\n" if body else ""), encoding="utf-8")


def report(diags: list[Diagnostic], title: str) -> None:
    print(f"{title}: {len(diags)}")
    for d in diags:
        print(f"  {d.key}")


def _self_test() -> int:
    """Parser unit checks -- run with `python3 scripts/warning_gate.py --self-test`.

    Each case feeds a whole log snippet to the parser, so path normalisation,
    flag extraction and de-duplication are exercised together.
    """
    root = Path("/repo")
    cases: list[tuple[str, str, list[str]]] = [
        (
            "three-column and two-column forms",
            "kernel/mm/memory.cpp:12:3: warning: unused parameter 'size' [-Wunused-parameter]\n"
            "apps/kernel.cpp:7: warning: unused variable 'X' [-Wunused-variable]\n",
            [
                "apps/kernel.cpp [-Wunused-variable] unused variable 'X'",
                "kernel/mm/memory.cpp [-Wunused-parameter] unused parameter 'size'",
            ],
        ),
        (
            "vendored, toolchain, build-tree and generated paths dropped",
            "/repo/3rdparty/lwip/src/include/lwip/errno.h:159: warning: \"ETIMEDOUT\" redefined [-Wmacro-redefined]\n"
            "/usr/lib/gcc/arm-none-eabi/12.2.0/include/newlib.h:1: warning: x [-Wfoo]\n"
            "/repo/build_lm3s/CMakeFiles/x.dir/foo.cpp:1: warning: y [-Wbar]\n"
            "config/autoconf.h:10:1: warning: z [-Wundef]\n",
            [],
        ),
        (
            "POSIX and Windows paths unify onto one key",
            "/repo/kernel/task.cpp:9:1: warning: x [-Wshadow]\n"
            "C:\\repo\\kernel\\task.cpp:400:7: warning: x [-Wshadow]\n"
            "D:\\repo\\net\\scan.cpp:4: warning: y [-Wshadow]\n",
            [
                "kernel/task.cpp [-Wshadow] x",
                "net/scan.cpp [-Wshadow] y",
            ],
        ),
        (
            "cross-directory include spelling is collapsed",
            # GCC reports a header with the spelling of the #include that pulled
            # it in, so CI emits `apps/../net/...` where a local build may emit
            # the already-relative path. Both must key identically.
            "apps/../net/distributed_bus.hpp:3:1: warning: \"hardcoded key\" [-Wcpp]\n"
            "net/distributed_bus.hpp:3:1: warning: \"hardcoded key\" [-Wcpp]\n"
            "./apps/../ui/complications.hpp:9:1: warning: unused parameter 'max_len' [-Wunused-parameter]\n",
            [
                "net/distributed_bus.hpp [-Wcpp] \"hardcoded key\"",
                "ui/complications.hpp [-Wunused-parameter] unused parameter 'max_len'",
            ],
        ),
        (
            "'..' cannot smuggle vendored code past the filter",
            # `apps/../3rdparty/...` must be recognised as vendored, otherwise
            # third-party diagnostics enter the baseline even though
            # AGENTS.md section 26 forbids fixing them.
            "apps/../3rdparty/lwip/src/core/tcp.c:10:1: warning: x [-Wfoo]\n"
            "apps/../tests/stubs/x.cpp:1:1: warning: y [-Wbar]\n"
            "apps/../kernel/task.cpp:5:1: warning: z [-Wunused-variable]\n",
            ["kernel/task.cpp [-Wunused-variable] z"],
        ),
        (
            "paths escaping the repository root are rejected",
            "../outside/evil.cpp:1:1: warning: x [-Wfoo]\n"
            "/etc/passwd:1:1: warning: y [-Wbar]\n",
            [],
        ),
        (
            "linker chatter, notes and include banners ignored",
            "lto-wrapper: warning: using serial compilation of 2 LTRANS jobs\n"
            "/repo/kernel/sched.cpp:5:2: note: declared here\n"
            "In file included from /repo/kernel/sched.cpp:1:\n"
            "                 from /repo/vfs/vfs.hpp:2:\n",
            [],
        ),
        (
            "#warning directive retained",
            "/repo/net/distributed_bus.hpp:3:1: warning: \"hardcoded key\" [-Wcpp]\n",
            ["net/distributed_bus.hpp [-Wcpp] \"hardcoded key\""],
        ),
        (
            "key order does not depend on discovery order",
            "/repo/z/z.cpp:1: warning: b [-Wunused]\n"
            "/repo/a/a.cpp:1: warning: a [-Wunused]\n",
            ["a/a.cpp [-Wunused] a", "z/z.cpp [-Wunused] b"],
        ),
    ]
    failures = 0
    for name, log, expected in cases:
        got = [d.key for d in parse_log(log, root)]
        if got != expected:
            failures += 1
            print(f"FAIL [{name}]\n  expected {expected}\n  got      {got}")
    if failures:
        print(f"\n{failures}/{len(cases)} self-test case(s) failed.")
        return 1
    print(f"self-test OK ({len(cases)} cases)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="AuroraOS compiler-warning ratchet gate.",
    )
    parser.add_argument("log", nargs="?", help="compiler build log to analyse")
    parser.add_argument("--baseline", type=Path,
                        help="accepted-diagnostics baseline for this board")
    parser.add_argument("--report", action="store_true",
                        help="always exit 0 (reporting only)")
    parser.add_argument("--update-baseline", type=Path, metavar="PATH",
                        help="write the observed diagnostics to PATH as baseline")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd(),
                        help="repository root used to relativise paths")
    parser.add_argument("--self-test", action="store_true",
                        help="run the parser unit checks and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.log:
        parser.error("a build log path is required (or use --self-test)")

    try:
        text = read_text(Path(args.log))
    except OSError as exc:
        print(f"ERROR: cannot read build log {args.log}: {exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    diags = parse_log(text, args.repo_root.resolve())

    if args.update_baseline:
        write_baseline(args.update_baseline, diags)
        report(diags, f"baseline written to {args.update_baseline}")
        return 0

    if args.report:
        report(diags, "diagnostics (report-only)")
        return 0

    if not args.baseline:
        parser.error("--baseline is required unless --report/--update-baseline is used")

    if not args.baseline.exists():
        report(diags, "diagnostics")
        print(
            f"::warning::No baseline at {args.baseline} -- bootstrap mode. "
            "This board is reported but not gated. Adopt the baseline with:\n"
            f"  python3 scripts/warning_gate.py --update-baseline "
            f"{args.baseline} {args.log}"
        )
        return 0

    baseline = read_baseline(args.baseline)
    new = [d for d in diags if d.key not in baseline]
    resolved = sorted(baseline - {d.key for d in diags})

    report(diags, "diagnostics")
    report(new, "NEW (not in baseline)")
    if resolved:
        print(f"resolved (now unused baseline entries): {len(resolved)}")
        for key in resolved:
            print(f"  {key}")

    if new:
        print(
            f"\n::error::{len(new)} new compiler warning(s) outside the baseline. "
            "Fix them, or -- if a warning is intentional and in AuroraOS-owned "
            "code -- regenerate the baseline for this board:\n"
            f"  python3 scripts/warning_gate.py --update-baseline "
            f"{args.baseline} {args.log}"
        )
        return 1

    print("\n::notice::Compiler warning gate PASSED (no new warnings).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
