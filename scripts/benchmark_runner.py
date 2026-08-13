#!/usr/bin/env python3
"""Real benchmark runner for auroraOS on QEMU lm3s6965evb.

Boots the firmware, enables metrics sampling, runs an on-target workload to
produce real measurements, then captures the ProcFS metrics output to a log
file.  The log is subsequently parsed by parse_metrics.py.

Usage:
    python3 benchmark_runner.py --elf build_lm3s/auroraOS.elf --out metrics.log
"""
import argparse
import socket
import sys
import time

import pexpect
from pexpect.fdpexpect import fdspawn


def spawn_qemu(elf_path, port):
    cmd = (
        "qemu-system-arm -M lm3s6965evb -cpu cortex-m3 "
        "-display none -monitor none "
        "-serial tcp:127.0.0.1:%d,server "
        "-kernel %s -d guest_errors -D qemu.log" % (port, elf_path)
    )
    print("[benchmark] QEMU cmd:", cmd)
    qemu = pexpect.spawn(cmd, encoding="utf-8", codec_errors="replace")
    sock = None
    for _ in range(60):
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if sock is None:
        print("[benchmark] serial connect failed")
        qemu.terminate(force=True)
        sys.exit(1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return qemu, sock


def send_cmd(child, cmd, expect_pattern=None, timeout=10):
    """Send a shell command and return captured output up to the prompt."""
    child.send(cmd + "\r")
    if expect_pattern:
        child.expect(expect_pattern, timeout=timeout)
    child.expect(r"aurora> ", timeout=timeout)
    return child.before if hasattr(child, "before") else ""


def run(args):
    port = args.port
    qemu, sock = spawn_qemu(args.elf, port)
    child = fdspawn(sock.fileno(), encoding="utf-8", codec_errors="replace")
    collected = []

    try:
        child.expect(r"aurora> ", timeout=20)
        print("[benchmark] Boot successful!")

        # 1. Enable metrics sampling.
        send_cmd(child, "metrics start")
        collected.append("=== metrics start ===")
        collected.append("metrics start")

        # 2. Run a heap workload to populate heap_alloc latency samples.
        send_cmd(child, "heap_stress 20000")
        collected.append("=== heap_stress ===")
        collected.append("heap_stress 20000")

        # 3. Capture latency metrics (ctx_switch / heap_alloc, irq if sampled).
        out = send_cmd(child, "cat /proc/irq")
        collected.append("=== cat /proc/irq ===")
        collected.append(out)

        # 4. Capture power / dirty-ratio metrics.
        out = send_cmd(child, "cat /proc/power")
        collected.append("=== cat /proc/power ===")
        collected.append(out)

        # 5. Capture heap memory stats.
        out = send_cmd(child, "free")
        collected.append("=== free ===")
        collected.append(out)

        # 6. Capture network / softbus counters (may be zero if unused).
        out = send_cmd(child, "cat /proc/net")
        collected.append("=== cat /proc/net ===")
        collected.append(out)

        print("[benchmark] Capture complete.")
    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        print("[benchmark] FAILED: %s" % e)
        try:
            collected.append("".join(open("qemu.log").readlines()[-60:]))
        except Exception:
            pass
    finally:
        try:
            child.close()
        except Exception:
            pass
        try:
            sock.close()
        except Exception:
            pass
        qemu.terminate(force=True)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(collected))
    print("[benchmark] Raw metrics written to %s" % args.out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", default="auroraOS.elf", help="Firmware ELF path")
    parser.add_argument("--out", default="metrics.log", help="Output raw log path")
    parser.add_argument("--port", type=int, default=1234, help="QEMU serial TCP port")
    args = parser.parse_args()
    run(args)


if __name__ == "__main__":
    main()
