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
import os
import shlex
import socket
import sys
import tempfile
import time
import unittest
from unittest import mock

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
    # 额外 QEMU 参数透传（如 virtio 设备）：--qemu-arg 可重复。
    # 注意 virtio-mmio 槽号 = -device 声明顺序（net→blk→gpu，见 board.h）。
    cmd += list(args.qemu_args)
    return cmd


def _port_accepts_connection(port):
    """Return True if something is already listening on 127.0.0.1:<port>."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.settimeout(0.15)
        return probe.connect_ex(("127.0.0.1", port)) == 0
    except OSError:
        return False
    finally:
        probe.close()


def _print_qemu_log_tail(qemu_log):
    if not qemu_log:
        return
    try:
        with open(qemu_log, encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()[-60:]
        if lines:
            print("===== %s (last 60) =====" % qemu_log)
            print("".join(lines))
    except OSError:
        pass


def _drain_qemu_output(qemu):
    """Collect any remaining console/stderr output after QEMU exits."""
    try:
        qemu.expect(pexpect.EOF, timeout=0.2)
    except Exception:
        pass
    out = getattr(qemu, "before", "") or ""
    if out.strip():
        print("[HIL] QEMU output:\n%s" % out.rstrip())


def _cleanup_qemu(qemu):
    if qemu is None:
        return
    try:
        if qemu.isalive():
            qemu.terminate(force=True)
    except Exception:
        pass
    try:
        qemu.close(force=True)
    except Exception:
        pass


def _connect_serial(qemu, port, qemu_log):
    """Connect to QEMU's TCP serial server, aborting immediately if QEMU dies."""
    sock = None
    qemu_exited = False
    for _ in range(60):
        if not qemu.isalive():
            qemu_exited = True
            break
        try:
            candidate = socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            if not qemu.isalive():
                qemu_exited = True
                break
            time.sleep(0.1)
            continue

        # Ensure our own QEMU is still alive after TCP connect so a lingering
        # listener on the port cannot silently hijack the session while QEMU fails bind().
        if not qemu.isalive():
            try:
                candidate.close()
            except OSError:
                pass
            qemu_exited = True
            break
        sock = candidate
        break

    if qemu_exited:
        _drain_qemu_output(qemu)
        print(
            "[HIL] QEMU exited before serial port %d became ready "
            "(exitstatus=%r, signal=%r)"
            % (port, getattr(qemu, "exitstatus", None), getattr(qemu, "signalstatus", None))
        )
        _print_qemu_log_tail(qemu_log)
        sys.exit(1)

    if sock is None:
        print("[HIL] serial connect failed (timeout waiting for 127.0.0.1:%d)" % port)
        _drain_qemu_output(qemu)
        _print_qemu_log_tail(qemu_log)
        sys.exit(1)

    return sock


def build_arg_parser():
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
    ap.add_argument("--qemu-arg", dest="qemu_args", action="append", default=[],
                    help="extra QEMU argument (repeatable), e.g. "
                         "--qemu-arg=-device --qemu-arg=virtio-gpu-device")
    ap.add_argument("--skip-selftest", action="store_true",
                    help="only check boot + shell responsiveness")
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in unit tests for hil_runner")
    return ap


def run(argv=None):
    ap = build_arg_parser()
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()
    if not (1 <= args.port <= 65535):
        ap.error("invalid --port: %d (must be 1..65535)" % args.port)

    if _port_accepts_connection(args.port):
        print("[HIL] Port %d already has an active listener before QEMU launch." % args.port)
        sys.exit(1)

    cmd = build_cmd(args, args.port)
    print("[HIL] QEMU cmd:", " ".join(shlex.quote(part) for part in cmd))
    if not hasattr(pexpect, "spawn"):
        print("[HIL] pexpect.spawn is unavailable on this platform.")
        sys.exit(1)

    qemu = None
    sock = None
    child = None
    failures = []
    try:
        try:
            qemu = pexpect.spawn(cmd[0], cmd[1:], encoding="utf-8", codec_errors="replace")
        except (pexpect.ExceptionPexpect, OSError) as e:
            print("[HIL] Failed to start QEMU (%s): %s" % (cmd[0], e))
            sys.exit(1)

        sock = _connect_serial(qemu, args.port, args.qemu_log)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        child = fdspawn(sock.fileno(), encoding="utf-8", codec_errors="replace")
        child.logfile = sys.stdout

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
        return 0
    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        print("\n[HIL] Test FAILED: Timeout/EOF waiting for expected output (%r)." % (e,))
        _print_qemu_log_tail(args.qemu_log)
        sys.exit(1)
    finally:
        if child is not None:
            try:
                child.close()
            except Exception:
                pass
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        _cleanup_qemu(qemu)


def _drain_prompt(child):
    """Absorb trailing output so the next command starts from a clean prompt."""
    try:
        child.expect(r"aurora> ", timeout=5)
    except Exception:
        pass


def _self_test():
    class _FakeQemu:
        def __init__(self, alive_seq):
            self._alive_seq = list(alive_seq)
            self.before = "qemu: Failed to bind socket"
            self.exitstatus = 1
            self.signalstatus = None
            self.terminated = False
            self.closed = False

        def isalive(self):
            if self._alive_seq:
                return self._alive_seq.pop(0)
            return False

        def expect(self, *_args, **_kwargs):
            return 0

        def terminate(self, force=False):
            self.terminated = force or True

        def close(self, force=False):
            self.closed = force or True

    class _HilRunnerTests(unittest.TestCase):
        def test_argv_isolation_and_list_spawn(self):
            captured = {}

            class _Stop(Exception):
                pass

            def _fake_spawn(program, args, **_kwargs):
                captured["program"] = program
                captured["args"] = args
                raise _Stop()

            saved_argv = list(sys.argv)
            sys.argv = ["stress_hil_runner.py", "50"]
            try:
                with mock.patch.object(pexpect, "spawn", _fake_spawn, create=True), \
                     mock.patch(__name__ + "._port_accepts_connection", return_value=False):
                    with self.assertRaises(_Stop):
                        run(["--elf", "/tmp/dir with space/auroraOS.elf", "--port", "2345",
                             "--qemu-arg", "--foo \"bar\""])
            finally:
                sys.argv = saved_argv

            self.assertEqual(captured["program"], "qemu-system-arm")
            self.assertIn("/tmp/dir with space/auroraOS.elf", captured["args"])
            self.assertIn("--foo \"bar\"", captured["args"])
            idx = captured["args"].index("-serial")
            self.assertEqual(captured["args"][idx + 1], "tcp:127.0.0.1:2345,server")

        def test_early_exit_aborts_immediately_and_prints_log(self):
            fake_qemu = _FakeQemu([False])
            with tempfile.NamedTemporaryFile("w", delete=False, encoding="utf-8") as tf:
                tf.write("qemu error line\n")
                log_path = tf.name
            try:
                with mock.patch("time.sleep") as mock_sleep:
                    with self.assertRaises(SystemExit) as cm:
                        _connect_serial(fake_qemu, 2346, log_path)
                    self.assertEqual(cm.exception.code, 1)
                    mock_sleep.assert_not_called()
            finally:
                os.unlink(log_path)

        def test_rejects_hijacked_socket_if_qemu_died_after_connect(self):
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            port = srv.getsockname()[1]
            fake_qemu = _FakeQemu([True, False])
            try:
                with self.assertRaises(SystemExit) as cm:
                    _connect_serial(fake_qemu, port, "")
                self.assertEqual(cm.exception.code, 1)
            finally:
                srv.close()

        def test_rejects_port_already_listening_before_spawn(self):
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            port = srv.getsockname()[1]
            try:
                with self.assertRaises(SystemExit) as cm:
                    run(["--port", str(port)])
                self.assertEqual(cm.exception.code, 1)
            finally:
                srv.close()

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(_HilRunnerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run())
