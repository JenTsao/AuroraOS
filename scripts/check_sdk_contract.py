#!/usr/bin/env python3
"""SDK 契约头一致性校验：sdk/include/auroraos/** 必须与仓库根同名头保持镜像。

sdk/include/auroraos/ 下的契约头（syscall ABI、services/*_ipc.hpp、runtime/* 等）
是面向用户态 SDK 的逐字节拷贝。拷贝没有同步机制时，内核侧单方面改签名
（历史上 CSpace::cap_insert 5 参改 3 参一类事故）会让 SDK 静默漂移。本脚本
逐对比对 SDK 头与其在仓库根的同名源头，任何内容差异即失败，供 ctest 门禁调用：

    python scripts/check_sdk_contract.py

比对时统一换行符（CRLF/RLF 视为等价），避免跨平台 checkout 误报；其余任何
字节差异（含行尾空白、文件尾换行）都视为真实漂移。
"""

import difflib
import os
import sys

MIRROR_ROOT = os.path.join("sdk", "include", "auroraos")
MAX_DIFF_LINES = 40


def normalize(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def collect_pairs(repo_root: str):
    """遍历 SDK 镜像目录，产出 (sdk_path, source_path)。"""
    base = os.path.join(repo_root, MIRROR_ROOT)
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in sorted(filenames):
            sdk_path = os.path.join(dirpath, name)
            rel = os.path.relpath(sdk_path, base)
            yield sdk_path, os.path.join(repo_root, rel), rel


def compare(sdk_path: str, source_path: str, rel: str) -> list:
    if not os.path.isfile(source_path):
        return [f"[missing-source] {rel}: SDK 头没有对应的仓库根源头"]
    with open(sdk_path, encoding="utf-8", errors="replace") as f:
        sdk_text = normalize(f.read())
    with open(source_path, encoding="utf-8", errors="replace") as f:
        src_text = normalize(f.read())
    if sdk_text == src_text:
        return []
    diff = list(
        difflib.unified_diff(
            src_text.splitlines(keepends=True),
            sdk_text.splitlines(keepends=True),
            fromfile=f"{rel} (repo root source)",
            tofile=f"{rel} (sdk mirror)",
        )
    )
    head = diff[:MAX_DIFF_LINES]
    if len(diff) > MAX_DIFF_LINES:
        head.append(f"... ({len(diff) - MAX_DIFF_LINES} more diff lines)\n")
    return [f"[drift] {rel}: SDK 镜像与仓库根源头不一致"] + ["    " + line.rstrip("\n") for line in head]


def main() -> int:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    problems = []
    pair_count = 0
    for sdk_path, source_path, rel in collect_pairs(repo_root):
        pair_count += 1
        problems.extend(compare(sdk_path, source_path, rel))

    if problems:
        print(f"S sdk contract check FAILED ({pair_count} pairs scanned):")
        for line in problems:
            print(line)
        print("\nSync the sdk mirror with its repo-root source (or vice versa) before merging.")
        return 1
    print(f"OK sdk contract check passed ({pair_count} mirrored headers in sync)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
