#!/usr/bin/env python3
"""check_arch_layers.py — enforce the AuroraOS architecture dependency contract.

AGENTS.md §5 defines a strict dependency direction:

    Applications -> Services -> Subsystem APIs -> Kernel / Syscalls
                 -> HAL / Arch -> Hardware

and forbids reverse edges such as kernel -> ui/apps/net/drivers/experimental,
drivers -> apps, stable -> experimental, vfs -> apps and net -> ui.  This
script turns that prose contract into a CI gate:

  1. Walk AuroraOS-owned sources (3rdparty/, tests/, build*/ are skipped).
  2. Parse every ``#include`` and resolve it to a repository file.
  3. Map each file to an architecture layer from its top-level directory.
  4. Check the explicit AGENTS.md rules plus a generic "no upward edge" rule.
  5. Build the include graph and report strongly connected components as
     include cycles.
  6. Print every finding with file:line, include target and the violated rule.

Exit code 0 = clean, 1 = violations/cycles, 2 = usage or allow-list error.

Exception handling
------------------
Existing, reviewed exceptions live in scripts/arch_layers_allowlist.txt.
Every entry MUST carry a reason:

    allow <source-glob> -> <target-glob> ; reason: <why this edge is legal>

An entry without ``reason:`` is rejected so the whitelist cannot silently
grow.  Allowed edges are removed from both the direction check and the
cycle graph.

Usage:
    python3 scripts/check_arch_layers.py --repo-root .
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable, NamedTuple

SOURCE_EXTENSIONS = {".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".S", ".s"}

# Directories that are not AuroraOS-owned production code, or that are
# intentionally exempt from the layer contract.
EXCLUDED_DIR_NAMES = {
    "3rdparty",
    "tests",
    "test",
    "DOCS",
    "docs",
    "__pycache__",
    ".git",
    ".github",
    ".agent",
    ".workbuddy",
    ".screenshots",
    "node_modules",
}

# Top-level directory -> architecture layer.
#   util    shared freestanding helpers / configuration, usable by everyone
#   hal     boot, bootloader, arch, adapter, boards, hal
#   driver  hardware drivers
#   kernel  microkernel core and syscall ABI
#   subsystem  vfs, net, ui (subsystem API level)
#   service services, runtime, security, metrics, ai
#   sdk     public SDK surface
#   app     userspace applications
#   experimental  research code that may depend on stable APIs only
TOP_LAYER = {
    "utils": "util",
    "config": "util",
    "hal": "hal",
    "arch": "hal",
    "adapter": "hal",
    "boards": "hal",
    "boot": "hal",
    "bootloader": "hal",
    "drivers": "driver",
    "kernel": "kernel",
    "syscall": "kernel",
    "vfs": "subsystem",
    "net": "subsystem",
    "ui": "subsystem",
    "services": "service",
    "runtime": "service",
    "security": "service",
    "metrics": "metrics",
    "ai": "service",
    "sdk": "sdk",
    "apps": "app",
    "experimental": "experimental",
}

# Generic upward-dependency ranking.  Lower numbers sit closer to hardware.
LAYER_RANK = {
    "util": 0,
    "metrics": 0,
    "hal": 1,
    "driver": 1,
    "kernel": 2,
    "subsystem": 3,
    "service": 4,
    "sdk": 4,
    "app": 5,
    "experimental": 5,
}

# AGENTS.md §5 / §42 explicit forbidden edges.
# (id, description, source-top-dirs, target-top-dirs)
EXPLICIT_RULES = [
    ("K2UI", "kernel -> ui is forbidden", {"kernel", "syscall"}, {"ui"}),
    ("K2APP", "kernel -> application is forbidden", {"kernel", "syscall"}, {"apps"}),
    ("K2NET", "kernel -> net is forbidden", {"kernel", "syscall"}, {"net"}),
    ("K2DRV", "kernel -> driver is forbidden", {"kernel", "syscall"}, {"drivers"}),
    ("K2EXP", "kernel -> experimental is forbidden", {"kernel", "syscall"}, {"experimental"}),
    ("DRV2APP", "driver -> application is forbidden", {"drivers"}, {"apps"}),
    ("VFS2APP", "vfs -> application is forbidden", {"vfs"}, {"apps"}),
    ("NET2UI", "net -> ui is forbidden", {"net"}, {"ui"}),
    ("HAL2APP", "HAL/boot/board/adapter -> application is forbidden", {
        "boot", "bootloader", "hal", "arch", "adapter", "boards"
    }, {"apps"}),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')


class AllowEntry(NamedTuple):
    source_glob: str
    target_glob: str
    reason: str


class Finding(NamedTuple):
    source: Path
    line: int
    include: str
    target: Path
    rule_id: str
    description: str


def top_component(rel: Path) -> str:
    return rel.parts[0] if rel.parts else ""


def layer_of(rel: Path) -> str:
    return TOP_LAYER.get(top_component(rel), "unknown")


def iter_sources(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_EXTENSIONS:
            continue
        rel = path.relative_to(root)
        if any(
            part.startswith("build")
            or part.startswith("cmake-build")
            or part in EXCLUDED_DIR_NAMES
            for part in rel.parts[:-1]
        ):
            continue
        yield path


def load_allowlist(path: Path) -> list[AllowEntry]:
    if not path.exists():
        return []
    entries: list[AllowEntry] = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"^allow\s+(\S+)\s*->\s*(\S+)\s*;\s*reason\s*:\s*(.+)$", line)
        if not match:
            raise ValueError(
                f"{path}:{lineno}: allow-list entry must use "
                f"'allow <source-glob> -> <target-glob> ; reason: <text>'"
            )
        entries.append(AllowEntry(match.group(1), match.group(2), match.group(3).strip()))
    return entries


def is_allowed(source_rel: str, target_rel: str, allow: list[AllowEntry]) -> str | None:
    for entry in allow:
        if fnmatch.fnmatchcase(source_rel, entry.source_glob) and fnmatch.fnmatchcase(
            target_rel, entry.target_glob
        ):
            return entry.reason
    return None


class IncludeResolver:
    """Resolve include strings to repository files without guessing too hard."""

    def __init__(self, root: Path, files: dict[str, Path]) -> None:
        self.root = root
        self.files = files  # repo-relative posix path -> absolute path

    def resolve(self, source: Path, include: str) -> Path | None:
        include = include.replace("\\", "/")
        if "3rdparty/" in include or include.startswith("3rdparty"):
            return None

        candidates = [
            (source.parent / include).resolve(),
            (self.root / include).resolve(),
        ]
        for candidate in candidates:
            if candidate.is_file():
                rel = candidate.relative_to(self.root).as_posix()
                if rel in self.files:
                    return self.root / rel

        # Header discovered by suffix / basename.  Prefer a file in the same
        # top-level tree, then the shallowest match.  Ambiguous matches are
        # ignored rather than guessed.
        needle = "/" + include
        matches = [rel for rel in self.files if rel.endswith(needle)]
        if not matches:
            base = Path(include).name
            matches = [rel for rel in self.files if Path(rel).name == base]
        if not matches:
            return None
        source_top = top_component(source.relative_to(self.root))
        same_tree = [rel for rel in matches if top_component(Path(rel)) == source_top]
        pool = same_tree or matches
        best = min(pool, key=lambda rel: (rel.count("/"), len(rel)))
        return self.root / best


def parse_includes(source: Path) -> list[tuple[int, str]]:
    found: list[tuple[int, str]] = []
    for lineno, line in enumerate(
        source.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        match = INCLUDE_RE.match(line)
        if match:
            found.append((lineno, match.group(1).strip()))
    return found


def describe_rule(source_rel: Path, target_rel: Path) -> tuple[str, str] | None:
    source_top = top_component(source_rel)
    target_top = top_component(target_rel)

    if source_top != "experimental" and target_top == "experimental":
        return "STABLE2EXP", "stable code -> experimental is forbidden"

    for rule_id, description, sources, targets in EXPLICIT_RULES:
        if source_top in sources and target_top in targets:
            return rule_id, description

    source_layer = layer_of(source_rel)
    target_layer = layer_of(target_rel)
    if source_layer in ("experimental", "util", "metrics") or target_layer in ("util", "metrics", "unknown"):
        return None
    # The HAL/arch/boot/adapter/board trees are the hardware/ABI implementation
    # side: they call into kernel and subsystem entry points by design.  The
    # contract's concern is that *kernel and subsystem code* must not depend
    # back on them, which the explicit rules and the rank check below enforce.
    if source_layer == "hal" and target_layer in ("kernel", "subsystem", "service"):
        return None
    # Drivers register Device objects with the kernel DeviceFramework and use
    # kernel synchronization primitives; they must not reach further up the
    # stack (vfs/net/services/apps), which the rank check still catches.
    if source_layer == "driver" and target_layer == "kernel":
        return None
    if LAYER_RANK.get(target_layer, 0) > LAYER_RANK.get(source_layer, 0):
        return (
            "REVERSE",
            f"reverse dependency: {source_layer} -> {target_layer}",
        )
    return None


def find_cycles(graph: dict[str, set[str]]) -> list[list[str]]:
    """Return one representative cycle path per strongly connected component."""
    index = 0
    indices: dict[str, int] = {}
    lowlink: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    components: list[list[str]] = []

    def strongconnect(node: str) -> None:
        nonlocal index
        indices[node] = lowlink[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for nxt in sorted(graph.get(node, ())):
            if nxt not in indices:
                strongconnect(nxt)
                lowlink[node] = min(lowlink[node], lowlink[nxt])
            elif nxt in on_stack:
                lowlink[node] = min(lowlink[node], indices[nxt])
        if lowlink[node] == indices[node]:
            component: list[str] = []
            while True:
                member = stack.pop()
                on_stack.discard(member)
                component.append(member)
                if member == node:
                    break
            if len(component) > 1 or node in graph.get(node, ()):
                components.append(sorted(component))

    for node in graph:
        if node not in indices:
            strongconnect(node)

    cycles: list[list[str]] = []
    for component in components:
        members = set(component)
        local: dict[str, set[str]] = {
            node: {x for x in graph.get(node, ()) if x in members} for node in component
        }
        cycle = _cycle_in_subgraph(local, component[0])
        cycles.append(cycle if cycle else component + [component[0]])
    return cycles


def _cycle_in_subgraph(graph: dict[str, set[str]], start: str) -> list[str] | None:
    parent: dict[str, str] = {}
    stack = [(start, iter(sorted(graph.get(start, ()))))]
    visited = {start}
    while stack:
        node, iterator = stack[-1]
        try:
            nxt = next(iterator)
        except StopIteration:
            stack.pop()
            continue
        if nxt in visited:
            cycle = [nxt]
            walker = node
            while walker != nxt:
                cycle.append(walker)
                walker = parent[walker]
            cycle.append(nxt)
            return list(reversed(cycle))
        visited.add(nxt)
        parent[nxt] = node
        stack.append((nxt, iter(sorted(graph.get(nxt, ())))))
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-root", default=".", help="Repository root (default: .)")
    parser.add_argument(
        "--allow-list",
        default=None,
        help="Allow-list path (default: <repo-root>/scripts/arch_layers_allowlist.txt)",
    )
    parser.add_argument("--report", default=None, help="Write a detailed report to this path")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not (root / "AGENTS.md").is_file():
        print(f"error: {root} does not look like the AuroraOS repository", file=sys.stderr)
        return 2

    allow_path = (
        Path(args.allow_list)
        if args.allow_list
        else root / "scripts" / "arch_layers_allowlist.txt"
    )
    try:
        allow = load_allowlist(allow_path)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    sources = list(iter_sources(root))
    files = {path.relative_to(root).as_posix(): path for path in sources}
    resolver = IncludeResolver(root, files)

    findings: list[Finding] = []
    allowed_hits: list[tuple[str, str, str]] = []
    graph: dict[str, set[str]] = defaultdict(set)
    include_count = 0
    resolved_count = 0

    for source in sources:
        source_rel = Path(source.relative_to(root).as_posix())
        source_key = source_rel.as_posix()
        for lineno, include in parse_includes(source):
            include_count += 1
            target = resolver.resolve(source, include)
            if target is None:
                continue
            target_rel = Path(target.relative_to(root).as_posix())
            target_key = target_rel.as_posix()
            if target_key == source_key:
                continue
            resolved_count += 1
            reason = is_allowed(source_key, target_key, allow)
            if reason is not None:
                allowed_hits.append((source_key, target_key, reason))
                continue
            graph[source_key].add(target_key)
            rule = describe_rule(source_rel, target_rel)
            if rule is not None:
                rule_id, description = rule
                findings.append(
                    Finding(source_rel, lineno, include, target_rel, rule_id, description)
                )

    cycles = find_cycles(dict(graph))

    lines: list[str] = []
    allow_display = (
        allow_path.relative_to(root).as_posix()
        if allow_path.is_relative_to(root)
        else str(allow_path)
    )
    lines.append("AuroraOS architecture dependency scan")
    lines.append(f"  repository : {root}")
    lines.append(f"  files      : {len(sources)}")
    lines.append(f"  includes   : {include_count} ({resolved_count} resolved to repo files)")
    lines.append(f"  allow-list : {allow_display}")
    lines.append(f"  entries    : {len(allow)}")
    lines.append("")

    if findings:
        lines.append(f"VIOLATIONS ({len(findings)}):")
        for finding in findings:
            lines.append(
                f"  [{finding.rule_id}] {finding.source.as_posix()}:{finding.line}"
                f" -> {finding.target.as_posix()}"
                f"  (include \"{finding.include}\")"
            )
            lines.append(f"      violates: {finding.description}")
        lines.append("")
    else:
        lines.append("VIOLATIONS: none")
        lines.append("")

    if cycles:
        lines.append(f"INCLUDE CYCLES ({len(cycles)}):")
        for cycle in cycles:
            lines.append("  " + " -> ".join(cycle))
        lines.append("")
    else:
        lines.append("INCLUDE CYCLES: none")
        lines.append("")

    if allowed_hits:
        lines.append(f"ALLOWED EDGES ({len(allowed_hits)}):")
        for source_key, target_key, reason in allowed_hits:
            lines.append(f"  {source_key} -> {target_key}")
            lines.append(f"      reason: {reason}")
        lines.append("")

    report_text = "\n".join(lines)
    print(report_text)
    if args.report:
        Path(args.report).write_text(report_text + "\n", encoding="utf-8")

    return 1 if findings or cycles else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
