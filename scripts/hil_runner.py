#!/usr/bin/env python3
"""HIL runner for auroraOS on QEMU lm3s6965evb.

Expects auroraOS.elf in the current working directory (typically build/).
"""
import pexpect
from pexpect.fdpexpect import fdspawn
import socket
import sys
import time


def run():
    print("Starting auroraOS HIL simulation via QEMU...")
    port = 1234
    cmd = (
        "qemu-system-arm -M lm3s6965evb -cpu cortex-m3 "
        "-display none -monitor none "
        "-serial tcp:127.0.0.1:%d,server "
        "-kernel auroraOS.elf -d guest_errors -D qemu.log" % port
    )
    print("[HIL] QEMU cmd:", cmd)
    qemu = pexpect.spawn(cmd, encoding="utf-8", codec_errors="replace")
    sock = None
    for _ in range(60):
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
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
    try:
        child.expect(r"aurora> ", timeout=15)
        print("\n[HIL] Boot successful!")

        child.send("help\r")
        child.expect(r"Show this message", timeout=10)
        print("\n[HIL] Shell 'help' command responsive.")

        # Drain residual output to a prompt if present
        try:
            child.expect(r"aurora> ", timeout=4)
        except Exception:
            pass

        child.send("ps\r")
        child.expect(r"TID", timeout=10)
        print("\n[HIL] 'ps' command lists tasks correctly.")

        try:
            child.expect(r"aurora> ", timeout=5)
        except Exception:
            pass
        print("\n[HIL] All checks passed. Test PASSED.")
    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        print("\n[HIL] Test FAILED: Timeout/EOF waiting for expected output.")
        try:
            print("===== qemu.log (last 60) =====")
            print("".join(open("qemu.log").readlines()[-60:]))
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


if __name__ == "__main__":
    run()
