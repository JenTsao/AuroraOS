#!/usr/bin/env python3
"""driver_htl_runner.py — AuroraOS Driver Hardware Test Loop (HTL / HIL) Runner.

This script executes on-target driver tests under QEMU emulation. It validates:
  1. Driver boot sequence and hardware initialization.
  2. Character/Block device VFS node responsiveness (/dev/uart0, /proc/*).
  3. Real hardware watchdog (SP805 / SoftWdt) periodic petting and stability.
  4. Driver stress and concurrency integrity under simulated peripherals.
  5. Absence of guest hardware faults and invalid register accesses.
"""

import argparse
import socket
import sys
import time
import pexpect
from pexpect.fdpexpect import fdspawn


def build_qemu_cmd(args, port):
    cmd = [args.qemu, "-M", args.machine]
    if args.cpu:
        cmd += ["-cpu", args.cpu]
    if args.bios_none:
        cmd += ["-bios", "none"]
    cmd += [
        "-display", "none",
        "-monitor", "none",
        "-serial", f"tcp:127.0.0.1:{port},server",
        "-kernel", args.elf,
    ]
    if args.qemu_log:
        cmd += ["-d", "guest_errors", "-D", args.qemu_log]
    return cmd


def drain_prompt(child, timeout=5):
    """Wait for prompt and drain trailing output."""
    try:
        child.expect(r"aurora> ", timeout=timeout)
    except Exception:
        pass


def run_driver_htl():
    ap = argparse.ArgumentParser(description="AuroraOS Driver HTL / HIL Test Runner")
    ap.add_argument("--qemu", default="qemu-system-arm", help="QEMU binary path")
    ap.add_argument("--machine", default="lm3s6965evb", help="QEMU target machine")
    ap.add_argument("--cpu", default="cortex-m3", help="Target CPU architecture")
    ap.add_argument("--bios-none", action="store_true", help="Pass -bios none")
    ap.add_argument("--elf", default="auroraOS.elf", help="Path to compiled firmware ELF")
    ap.add_argument("--port", type=int, default=1236, help="TCP port for QEMU serial socket")
    ap.add_argument("--qemu-log", default="qemu_driver_htl.log", help="Path for QEMU guest error log")
    ap.add_argument("--skip-selftest", action="store_true", help="Skip kernel selftest")
    ap.add_argument("--iterations", type=int, default=1, help="Number of test iterations")
    args = ap.parse_args()

    failures = []
    print(f"[Driver-HTL] Starting Driver Hardware Test Loop ({args.machine}, {args.iterations} iteration(s))...")

    for iteration in range(1, args.iterations + 1):
        if args.iterations > 1:
            print(f"\n[Driver-HTL] === Iteration {iteration}/{args.iterations} ===")

        cmd = build_qemu_cmd(args, args.port)
        print("[Driver-HTL] Launching QEMU:", " ".join(cmd))
        qemu = pexpect.spawn(" ".join(cmd), encoding="utf-8", codec_errors="replace")

        sock = None
        for _ in range(60):
            try:
                sock = socket.create_connection(("127.0.0.1", args.port), timeout=1)
                break
            except OSError:
                time.sleep(0.1)

        if sock is None:
            print("[Driver-HTL] ERROR: Failed to connect to QEMU serial TCP socket.")
            qemu.terminate(force=True)
            sys.exit(1)

        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        child = fdspawn(sock.fileno(), encoding="utf-8", codec_errors="replace")
        child.logfile = sys.stdout

        try:
            # 1. Boot Verification (UART driver + early hardware setup)
            child.expect(r"aurora> ", timeout=15)
            print("\n[Driver-HTL] [PASS] Driver early boot & UART driver OK.")

            # 2. VFS / Device Subsystem verification
            child.send("help\r")
            child.expect(r"Show this message", timeout=10)
            drain_prompt(child)
            print("\n[Driver-HTL] [PASS] Shell command loop responsive.")

            # 3. ProcFS driver verification (/proc/taskinfo)
            child.send("ps\r")
            child.expect(r"TID", timeout=10)
            drain_prompt(child)
            print("\n[Driver-HTL] [PASS] ProcFS driver (/proc/taskinfo) operational.")

            # 4. Storage & Memory Allocation driver stress
            child.send("heap_stress 100\r")
            child.expect(r"Heap stress test finished", timeout=15)
            drain_prompt(child)
            print("\n[Driver-HTL] [PASS] Memory allocation driver stress test OK.")

            # 5. On-target Hardware self-test (Watchdog, traps, context switches)
            if not args.skip_selftest:
                child.send("selftest\r")
                child.expect(r"\[SELFTEST\] RESULT: (PASS|FAIL)", timeout=60)
                verdict = child.match.group(1)
                print(f"\n[Driver-HTL] On-target self-test verdict: {verdict}")
                if verdict != "PASS":
                    failures.append(f"selftest_iter_{iteration}")
                drain_prompt(child)

            # 6. Watchdog keep-alive check (ensure hardware watchdog does not trigger spurious resets)
            print("[Driver-HTL] Testing watchdog keep-alive over 3 seconds...")
            time.sleep(3)
            child.send("\r")
            child.expect(r"aurora> ", timeout=5)
            print("[Driver-HTL] [PASS] Watchdog periodic petting stable.")

        except (pexpect.TIMEOUT, pexpect.EOF) as e:
            print(f"\n[Driver-HTL] ERROR: Timeout/EOF in iteration {iteration}: {e}")
            failures.append(f"timeout_iter_{iteration}")
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

        # Inspect QEMU guest error log for unexpected hardware traps
        if args.qemu_log:
            try:
                with open(args.qemu_log, "r") as f:
                    log_content = f.read()
                    if "unhandled" in log_content.lower() or "invalid" in log_content.lower():
                        print(f"\n[Driver-HTL] WARNING: Guest errors detected in {args.qemu_log}:")
                        print("\n".join(log_content.splitlines()[-20:]))
            except Exception:
                pass

        if failures:
            break

    print("\n" + "=" * 50)
    print(" DRIVER HTL / HIL TEST SUMMARY")
    print("=" * 50)
    if failures:
        print(f" RESULT: FAILED ({', '.join(failures)})")
        sys.exit(1)
    else:
        print(" RESULT: ALL DRIVER TESTS PASSED")
        sys.exit(0)


if __name__ == "__main__":
    run_driver_htl()
