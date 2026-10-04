#!/usr/bin/env python3
"""HIL runner for auroraOS under QEMU.

Boots the firmware, drives the shell over a TCP serial console and asserts
both basic shell responsiveness and the on-target kernel self-test.

The self-test matters because the host unit suite builds against
tests/stubs/arch_api.hpp, which replaces the whole Arch:: layer — so the
context switch, the trap/entry path, the stack-canary enforcement and the
scheduler watchdog feed are only observable here, on the target.

Defaults reproduce the historical lm3s6965 invocation exactly:

    cd build_lm3s && python3 ../scripts/hil_runner.py

Other QEMU targets opt in explicitly, e.g.:

    cd build_rv32 && python3 ../scripts/hil_runner.py \
        --qemu qemu-system-riscv32 --machine virt --bios-none --port 1235
"""
import argparse
import socket
import sys
import time

import pexpect
from pexpect.fdpexpect import fdspawn


def build_cmd(args, port):
    cmd = [args.qemu, "-M", args.machine]
    if args.cpu:
        cmd += ["-cpu", args.cpu]
    if args.bios_none:
        cmd += ["-bios", "none"]
    cmd += [
        "-display", "none",
        "-monitor", "none",
        "-serial", "tcp:127.0.0.1:%d,server" % port,
        "-kernel", args.elf,
    ]
    if args.qemu_log:
        cmd += ["-d", "guest_errors", "-D", args.qemu_log]
    return cmd


def run():
    ap = argparse.ArgumentParser(description="auroraOS QEMU HIL runner")
    ap.add_argument("--qemu", default="qemu-system-arm",
                    help="QEMU binary (default: qemu-system-arm)")
    ap.add_argument("--machine", default="lm3s6965evb",
                    help="QEMU machine (default: lm3s6965evb)")
    ap.add_argument("--cpu", default="cortex-m3",
                    help="CPU model ('' lets QEMU pick the default)")
    ap.add_argument("--bios-none", dest="bios_none", action="store_true",
                    help="pass -bios none (RAM-only targets such as QEMU virt)")
    ap.add_argument("--elf", default="auroraOS.elf")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--qemu-log", default="",
                    help="optional path for the QEMU -D guest error log")
    ap.add_argument("--skip-selftest", action="store_true",
                    help="only check boot + shell responsiveness")
    args = ap.parse_args()

    cmd = build_cmd(args, args.port)
    print("[HIL] QEMU cmd:", " ".join(cmd))
    qemu = pexpect.spawn(" ".join(cmd), encoding="utf-8", codec_errors="replace")

    sock = None
    for _ in range(60):
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if sock is None:
        print("[HIL] serial connect failed")
        qemu.terminate(force=True)
        sys.exit(1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    child = fdspawn(sock.fileno(), encoding="utf-8", codec_errors="replace")
    child.logfile = sys.stdout

    failures = []
    try:
        child.expect(r"aurora> ", timeout=15)
        print("\n[HIL] Boot successful!")

        child.send("help\r")
        child.expect(r"Show this message", timeout=10)
        print("\n[HIL] Shell 'help' command responsive.")

        _drain_prompt(child)

        child.send("ps\r")
        child.expect(r"TID", timeout=10)
        print("\n[HIL] 'ps' command lists tasks correctly.")

        _drain_prompt(child)

        if not args.skip_selftest:
            child.send("selftest\r")
            child.expect(r"\[SELFTEST\] RESULT: (PASS|FAIL)", timeout=60)
            verdict = child.match.group(1)
            print("\n[HIL] On-target self-test verdict: %s" % verdict)
            if verdict != "PASS":
                failures.append("selftest")

        if failures:
            print("\n[HIL] Test FAILED: %s" % ", ".join(failures))
            sys.exit(1)
        print("\n[HIL] All checks passed. Test PASSED.")
    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        print("\n[HIL] Test FAILED: Timeout/EOF waiting for expected output (%r)." % (e,))
        if args.qemu_log:
            try:
                print("===== %s (last 60) =====" % args.qemu_log)
                print("".join(open(args.qemu_log).readlines()[-60:]))
            except Exception:
                pass
        sys.exit(1)
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


def _drain_prompt(child):
    """Absorb trailing output so the next command starts from a clean prompt."""
    try:
        child.expect(r"aurora> ", timeout=5)
    except Exception:
        pass


if __name__ == "__main__":
    run()
