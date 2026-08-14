#!/usr/bin/env python3
"""AuroraOS repository encoding normalizer and formatter.

1. Converts UTF-16 and GBK files to standard UTF-8 (no BOM).
2. Strips UTF-8 BOM from all source/header/markdown/config files.
3. Fixes corrupted characters (e.g. replacement characters in config/autoconf.h).
4. Runs clang-format on all AuroraOS C/C++ files (excluding 3rdparty, .git, build).
"""

import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

EXCLUDE_DIRS = {'.git', '3rdparty', '__pycache__', '.gemini'}

def should_skip(dirpath):
    rel = os.path.relpath(dirpath, REPO_ROOT).replace('\\', '/')
    parts = rel.split('/')
    for p in parts:
        if p in EXCLUDE_DIRS or p.startswith('build'):
            return True
    return False

def convert_utf16_files():
    utf16_files = [
        'DOCS/architecture/README.md',
        'DOCS/porting/README.md',
        'DOCS/security/README.md',
        'DOCS/tutorials/README.md'
    ]
    for rel_path in utf16_files:
        full_path = os.path.join(REPO_ROOT, rel_path)
        if os.path.exists(full_path):
            with open(full_path, 'rb') as f:
                raw = f.read()
            # If starts with UTF-16 BOM or contains null bytes indicating UTF-16
            if raw.startswith(b'\xff\xfe') or raw.startswith(b'\xfe\xff'):
                try:
                    text = raw.decode('utf-16')
                    with open(full_path, 'w', encoding='utf-8', newline='\n') as f:
                        f.write(text)
                    print(f"[UTF-16 -> UTF-8] {rel_path}")
                except Exception as e:
                    print(f"[ERROR] Failed to convert {rel_path}: {e}")

def convert_gbk_files():
    gbk_files = [
        'services/firewall/firewall_client.cpp'
    ]
    for rel_path in gbk_files:
        full_path = os.path.join(REPO_ROOT, rel_path)
        if os.path.exists(full_path):
            with open(full_path, 'rb') as f:
                raw = f.read()
            # If invalid utf-8, try gbk
            try:
                raw.decode('utf-8')
            except UnicodeDecodeError:
                try:
                    text = raw.decode('gbk')
                    with open(full_path, 'w', encoding='utf-8', newline='\n') as f:
                        f.write(text)
                    print(f"[GBK -> UTF-8] {rel_path}")
                except Exception as e:
                    print(f"[ERROR] Failed to convert {rel_path}: {e}")

def fix_replacement_chars():
    autoconf = os.path.join(REPO_ROOT, 'config', 'autoconf.h')
    if os.path.exists(autoconf):
        with open(autoconf, 'rb') as f:
            raw = f.read()
        # Replace \xef\xbf\xbd? with " — "
        fixed = raw.replace(b'\xef\xbf\xbd?', '—'.encode('utf-8'))
        if fixed != raw:
            with open(autoconf, 'wb') as f:
                f.write(fixed)
            print("[FIX] Fixed replacement characters in config/autoconf.h")

def strip_bom_and_normalize():
    count_bom = 0
    for root, dirs, files in os.walk(REPO_ROOT):
        if should_skip(root):
            dirs.clear()
            continue
        for f in files:
            if f.endswith(('.hpp', '.cpp', '.h', '.c', '.txt', '.cmake', '.py', '.md', '.S', '.s', '.json', '.yaml', '.yml')):
                p = os.path.join(root, f)
                with open(p, 'rb') as fp:
                    raw = fp.read()
                if raw.startswith(b'\xef\xbb\xbf'):
                    raw = raw[3:]
                    with open(p, 'wb') as fp:
                        fp.write(raw)
                    count_bom += 1
    if count_bom > 0:
        print(f"[BOM] Stripped BOM from {count_bom} files")

def run_clang_format():
    cpp_files = []
    for root, dirs, files in os.walk(REPO_ROOT):
        if should_skip(root):
            dirs.clear()
            continue
        for f in files:
            if f.endswith(('.hpp', '.cpp', '.h', '.c')):
                cpp_files.append(os.path.join(root, f))
    
    print(f"[CLANG-FORMAT] Formatting {len(cpp_files)} C/C++ files...")
    batch_size = 50
    for i in range(0, len(cpp_files), batch_size):
        batch = cpp_files[i:i + batch_size]
        cmd = ['clang-format', '-i'] + batch
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[ERROR] clang-format failed: {res.stderr}")
            return False
    print("[CLANG-FORMAT] Completed successfully.")
    return True

def main():
    print("=== AuroraOS Formatting & Encoding Normalization ===")
    convert_utf16_files()
    convert_gbk_files()
    fix_replacement_chars()
    strip_bom_and_normalize()
    run_clang_format()
    print("=== All done ===")

if __name__ == '__main__':
    main()
