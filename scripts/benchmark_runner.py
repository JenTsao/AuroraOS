#!/usr/bin/env python3
"""Real benchmark runner for auroraOS on QEMU lm3s6965evb.

Launches QEMU, executes a microbenchmark workload (heap_stress 20000 after
metrics start), collects /proc/irq, /proc/power, free, and /proc/net over
serial TCP, validates sampled metrics, and writes the raw output to a file
subsequently parsed by parse_metrics.py.

Usage:
    python3 benchmark_runner.py --elf build_lm3s/auroraOS.elf --out metrics.log
"""
import argparse
import collections
import os
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import pexpect
from pexpect.fdpexpect import fdspawn

PROMPT_PATTERN = r"(?:^|\r?\n)aurora> "
DEFAULT_BOOT_TIMEOUT = 20.0
DEFAULT_CMD_TIMEOUT = 10.0
DEFAULT_WORKLOAD_TIMEOUT = 120.0
QEMU_LOG_TAIL_LINES = 60


def _format_cmd(cmd):
    if hasattr(shlex, "join"):
        return shlex.join(cmd)
    return " ".join(shlex.quote(part) for part in cmd)


def _ensure_parent_dir(path):
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)


def _resolve_qemu_log_path(args):
    qemu_log = getattr(args, "qemu_log", None)
    if qemu_log is None:
        out_abs = os.path.abspath(args.out)
        return os.path.splitext(out_abs)[0] + ".qemu.log"
    return qemu_log or None


def _port_accepts_connection(host, port, timeout=0.2):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _read_tail_lines(path, max_lines=QEMU_LOG_TAIL_LINES):
    if not path or not os.path.isfile(path):
        return ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            tail = collections.deque(f, maxlen=max_lines)
        return "".join(tail).rstrip("\n")
    except OSError as exc:
        return "<failed to read %s: %s>" % (path, exc)


def _write_output(path, lines):
    _ensure_parent_dir(path)
    content = "\n".join(lines)
    if content and not content.endswith("\n"):
        content += "\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def build_cmd(args, port, qemu_log=None):
    """Build QEMU argv list safely without shell string splitting."""
    cmd = [
        getattr(args, "qemu", "qemu-system-arm"),
        "-M", "lm3s6965evb",
        "-cpu", "cortex-m3",
        "-display", "none",
        "-monitor", "none",
        "-serial", "tcp:127.0.0.1:%d,server" % port,
        "-kernel", args.elf,
    ]
    if qemu_log:
        cmd += ["-d", "guest_errors", "-D", qemu_log]
    return cmd


def spawn_qemu(args, port, qemu_log, stderr_file):
    """Launch QEMU via Popen without unread pipe deadlocks."""
    cmd = build_cmd(args, port, qemu_log=qemu_log)
    print("[benchmark] QEMU cmd:", _format_cmd(cmd))
    return subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )


def connect_serial(qemu, port, timeout=10.0):
    """Connect to QEMU serial TCP socket while monitoring QEMU liveness."""
    deadline = time.monotonic() + max(0.5, float(timeout))
    last_exc = None
    sock = None
    while True:
        rc = qemu.poll()
        if rc is not None:
            raise RuntimeError(
                "QEMU exited prematurely with code %d before serial was ready" % rc
            )
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            break
        except OSError as exc:
            last_exc = exc
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "serial connect failed on port %d: %s" % (port, last_exc)
                ) from exc
            time.sleep(0.1)

    rc = qemu.poll()
    if rc is not None:
        try:
            sock.close()
        except OSError:
            pass
        raise RuntimeError(
            "QEMU exited prematurely with code %d right after serial connect" % rc
        )
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def _open_serial_child(sock):
    """Attach pexpect fdspawn to a duplicated fd so child and sock close once each."""
    dup_fd = os.dup(sock.fileno())
    try:
        return fdspawn(dup_fd, encoding="utf-8", codec_errors="replace")
    except Exception:
        try:
            os.close(dup_fd)
        except OSError:
            pass
        raise


def _drain_pending(child):
    """Consume any asynchronous output already buffered before sending a command."""
    for _ in range(16):
        try:
            child.expect(r"[\s\S]+", timeout=0)
        except pexpect.TIMEOUT:
            break


def _clean_command_output(raw_output, cmd):
    """Normalize line endings and remove the echoed command line if present."""
    normalized = raw_output.replace("\r\n", "\n").replace("\r", "\n")
    lines = normalized.split("\n")
    if lines and lines[0].strip() == cmd.strip():
        lines = lines[1:]
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines)


def send_cmd(child, cmd, expect_pattern=None, timeout=DEFAULT_CMD_TIMEOUT,
             prompt_pattern=PROMPT_PATTERN):
    """Send a shell command and return captured output up to the next prompt."""
    _drain_pending(child)
    child.send(cmd + "\r")
    chunks = []
    if expect_pattern:
        child.expect(expect_pattern, timeout=timeout)
        if getattr(child, "before", None):
            chunks.append(child.before)
        after = getattr(child, "after", None)
        if isinstance(after, str):
            chunks.append(after)
        elif getattr(child, "match", None) is not None:
            chunks.append(child.match.group(0))
    child.expect(prompt_pattern, timeout=timeout)
    if getattr(child, "before", None):
        chunks.append(child.before)
    return _clean_command_output("".join(chunks), cmd)


def _extract_heap_alloc_count(irq_output):
    """Extract numeric sample count from '/proc/irq' heap_alloc line, or None."""
    m = re.search(r"(?m)^heap_alloc\b[^\n]*\bcount=(\d+)\b", irq_output)
    if m:
        return int(m.group(1))
    return None


def _terminate_qemu(qemu):
    if qemu is None:
        return None
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=3)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait(timeout=3)
    return qemu.returncode


def build_parser():
    parser = argparse.ArgumentParser(
        description="Run auroraOS microbenchmarks under QEMU lm3s6965evb."
    )
    parser.add_argument("--elf", default="auroraOS.elf", help="Firmware ELF path")
    parser.add_argument("--out", default="metrics.log", help="Output raw log path")
    parser.add_argument("--port", type=int, default=1234, help="QEMU serial TCP port")
    parser.add_argument(
        "--qemu", default="qemu-system-arm", help="QEMU executable name or path"
    )
    parser.add_argument(
        "--qemu-log",
        default=None,
        help="Optional QEMU -D guest_errors log path (defaults beside --out)",
    )
    parser.add_argument(
        "--boot-timeout",
        type=float,
        default=DEFAULT_BOOT_TIMEOUT,
        help="Timeout in seconds waiting for initial shell prompt (default: 20)",
    )
    parser.add_argument(
        "--cmd-timeout",
        type=float,
        default=DEFAULT_CMD_TIMEOUT,
        help="Timeout in seconds for standard shell commands (default: 10)",
    )
    parser.add_argument(
        "--workload-timeout",
        type=float,
        default=DEFAULT_WORKLOAD_TIMEOUT,
        help="Timeout in seconds for heap_stress 20000 workload (default: 120)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run internal unit tests instead of executing QEMU",
    )
    return parser


def run(args=None):
    """Execute the benchmark workflow. Returns True on success, False on failure."""
    if args is None:
        args = build_parser().parse_args(sys.argv[1:])
    elif isinstance(args, (list, tuple)):
        args = build_parser().parse_args(args)

    port = args.port
    if not (1 <= port <= 65535):
        msg = "invalid TCP port %r (must be 1..65535)" % port
        print("[benchmark] FAILED: %s" % msg)
        _write_output(args.out, ["=== benchmark failure ===", msg])
        return False

    if not os.path.isfile(args.elf):
        msg = "ELF file not found: %s" % args.elf
        print("[benchmark] FAILED: %s" % msg)
        _write_output(args.out, ["=== benchmark failure ===", msg])
        return False

    if _port_accepts_connection("127.0.0.1", port):
        msg = "TCP port %d is already in use" % port
        print("[benchmark] FAILED: %s" % msg)
        _write_output(args.out, ["=== benchmark failure ===", msg])
        return False

    qemu_log = _resolve_qemu_log_path(args)
    if qemu_log:
        _ensure_parent_dir(qemu_log)

    stderr_fh = tempfile.NamedTemporaryFile(mode="wb+", delete=False)
    stderr_path = stderr_fh.name

    qemu = None
    sock = None
    child = None
    collected = []
    ok = False

    try:
        qemu = spawn_qemu(args, port, qemu_log=qemu_log, stderr_file=stderr_fh)
        sock = connect_serial(qemu, port, timeout=min(10.0, float(args.boot_timeout)))
        child = _open_serial_child(sock)

        child.expect(PROMPT_PATTERN, timeout=args.boot_timeout)
        print("[benchmark] Boot successful!")

        # 1. Enable metrics sampling (firmware supports start|report only).
        out_metrics_start = send_cmd(
            child, "metrics start", timeout=args.cmd_timeout
        )
        collected.append("=== metrics start ===")
        collected.append(out_metrics_start)

        # 2. Run heap workload and synchronize on completion marker.
        out_heap_stress = send_cmd(
            child,
            "heap_stress 20000",
            expect_pattern=r"Heap stress test finished\.",
            timeout=args.workload_timeout,
        )
        collected.append("=== heap_stress ===")
        collected.append(out_heap_stress)

        # 3. Capture latency metrics (irq_latency / ctx_switch / heap_alloc).
        irq_out = send_cmd(child, "cat /proc/irq", timeout=args.cmd_timeout)
        collected.append("=== cat /proc/irq ===")
        collected.append(irq_out)

        # 4. Capture power / dirty-ratio metrics.
        power_out = send_cmd(child, "cat /proc/power", timeout=args.cmd_timeout)
        collected.append("=== cat /proc/power ===")
        collected.append(power_out)

        # 5. Capture heap memory stats.
        free_out = send_cmd(child, "free", timeout=args.cmd_timeout)
        collected.append("=== free ===")
        collected.append(free_out)

        # 6. Capture network / softbus counters.
        net_out = send_cmd(child, "cat /proc/net", timeout=args.cmd_timeout)
        collected.append("=== cat /proc/net ===")
        collected.append(net_out)

        heap_count = _extract_heap_alloc_count(irq_out)
        if heap_count is None or heap_count <= 0:
            raise RuntimeError(
                "heap_alloc sample validation failed in /proc/irq (count=%r)"
                % (heap_count,)
            )

        print("[benchmark] Capture complete (heap_alloc count=%d)." % heap_count)
        ok = True
    except (pexpect.TIMEOUT, pexpect.EOF, RuntimeError, OSError) as exc:
        print("[benchmark] FAILED: %s" % exc)
        partial_buf = getattr(child, "before", None) if child is not None else None
        if partial_buf:
            collected.append("=== partial serial buffer ===")
            collected.append(str(partial_buf).replace("\r\n", "\n"))

        rc = _terminate_qemu(qemu)
        if stderr_fh is not None:
            try:
                stderr_fh.close()
            except OSError:
                pass
            stderr_fh = None

        collected.append("=== benchmark failure ===")
        if rc is not None:
            collected.append("QEMU exit code: %d" % rc)
        collected.append(str(exc))

        stderr_tail = _read_tail_lines(stderr_path, QEMU_LOG_TAIL_LINES)
        if stderr_tail:
            collected.append("=== qemu stderr tail ===")
            collected.append(stderr_tail)

        log_tail = _read_tail_lines(qemu_log, QEMU_LOG_TAIL_LINES)
        if log_tail:
            collected.append("=== qemu log tail ===")
            collected.append(log_tail)
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
        _terminate_qemu(qemu)
        if stderr_fh is not None:
            try:
                stderr_fh.close()
            except OSError:
                pass
        if stderr_path and os.path.exists(stderr_path):
            try:
                os.unlink(stderr_path)
            except OSError:
                pass

    _write_output(args.out, collected)
    print("[benchmark] Raw metrics written to %s" % args.out)
    return ok


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.self_test:
        return 0 if _self_test() else 1
    return 0 if run(args) else 1


def _self_test():
    class _FakeChild:
        def __init__(self, script):
            self._script = list(script)
            self.before = ""
            self.after = ""
            self.match = None
            self.sent = []
            self.closed = False

        def send(self, data):
            self.sent.append(data)

        def expect(self, pattern, timeout=None):
            if pattern == r"[\s\S]+":
                # Emit async stale chunk once when scripted as ("DRAIN", chunk)
                if self._script and self._script[0][0] == "DRAIN":
                    _, text = self._script.pop(0)
                    self.before = ""
                    self.after = text
                    return 0
                raise pexpect.TIMEOUT("no pending bytes")

            if not self._script:
                raise pexpect.TIMEOUT("unexpected EOF in fake script for %r" % (pattern,))

            action, payload = self._script.pop(0)
            if action == "TIMEOUT":
                self.before = payload
                raise pexpect.TIMEOUT("simulated timeout waiting for %r" % (pattern,))
            if action == "EOF":
                self.before = payload
                raise pexpect.EOF("simulated EOF")
            before, after = payload
            self.before = before
            self.after = after
            return 0

        def close(self):
            self.closed = True

    class _BenchmarkRunnerTests(unittest.TestCase):
        def test_build_cmd_preserves_spaces_and_quotes(self):
            args = build_parser().parse_args([
                "--elf", "/tmp/dir with space/auroraOS \"1\".elf",
                "--qemu-log", "/tmp/log dir/q.log",
            ])
            cmd = build_cmd(args, 2345, qemu_log=args.qemu_log)
            self.assertIsInstance(cmd, list)
            self.assertIn("/tmp/dir with space/auroraOS \"1\".elf", cmd)
            self.assertIn("/tmp/log dir/q.log", cmd)
            idx = cmd.index("-serial")
            self.assertEqual(cmd[idx + 1], "tcp:127.0.0.1:2345,server")

        def test_prompt_pattern_does_not_match_midline(self):
            self.assertIsNone(re.search(PROMPT_PATTERN, "hello aurora> world\r\n"))
            self.assertIsNotNone(re.search(PROMPT_PATTERN, "aurora> "))
            self.assertIsNotNone(re.search(PROMPT_PATTERN, "done\r\naurora> "))
            self.assertIsNotNone(re.search(PROMPT_PATTERN, "done\naurora> "))

        def test_send_cmd_preserves_output_before_expect_pattern_and_strips_echo(self):
            fake_child = _FakeChild([
                ("DRAIN", "[async] noise\r\n"),
                ("MATCH", (
                    "heap_stress 20000\r\nRunning heap stress test...\r\n",
                    "Heap stress test finished.",
                )),
                ("MATCH", ("\r\nelapsed_ms=4200", "\r\naurora> ")),
            ])
            out = send_cmd(
                fake_child,
                "heap_stress 20000",
                expect_pattern=r"Heap stress test finished\.",
                timeout=120,
            )
            self.assertEqual(fake_child.sent, ["heap_stress 20000\r"])
            self.assertEqual(
                out,
                "Running heap stress test...\nHeap stress test finished.\nelapsed_ms=4200",
            )

        def test_connect_serial_aborts_immediately_on_early_exit(self):
            class _DeadQemu:
                def poll(self):
                    return 2

            with mock.patch("time.sleep") as mock_sleep:
                with self.assertRaises(RuntimeError) as cm:
                    connect_serial(_DeadQemu(), 2345, timeout=10)
                self.assertIn("code 2", str(cm.exception))
                mock_sleep.assert_not_called()

        def test_preflight_failures_exit_nonzero(self):
            with tempfile.TemporaryDirectory() as tmpdir:
                out_path = os.path.join(tmpdir, "metrics.log")
                self.assertEqual(
                    main(["--port", "0", "--out", out_path]), 1
                )
                self.assertEqual(
                    main(["--elf", os.path.join(tmpdir, "missing.elf"), "--out", out_path]),
                    1,
                )
                elf_path = os.path.join(tmpdir, "auroraOS.elf")
                with open(elf_path, "wb") as f:
                    f.write(b"\x7fELF")
                with mock.patch(
                    __name__ + "._port_accepts_connection", return_value=True
                ):
                    self.assertEqual(
                        main(["--elf", elf_path, "--out", out_path, "--port", "2456"]),
                        1,
                    )

        def test_timeout_captures_partial_buffer_and_tail_of_qemu_logs(self):
            with tempfile.TemporaryDirectory() as tmpdir:
                elf_path = os.path.join(tmpdir, "auroraOS.elf")
                out_path = os.path.join(tmpdir, "metrics.log")
                qemu_log = os.path.join(tmpdir, "qemu.log")
                with open(elf_path, "wb") as f:
                    f.write(b"\x7fELF")
                with open(qemu_log, "w", encoding="utf-8") as f:
                    for i in range(100):
                        f.write("guest-line-%03d\n" % i)

                class _FakeQemu:
                    def __init__(self, stderr_file):
                        stderr_file.write(b"qemu stderr boom\n")
                        stderr_file.flush()
                        self.returncode = None
                        self.terminated = False

                    def poll(self):
                        return self.returncode

                    def terminate(self):
                        self.terminated = True
                        self.returncode = -15

                    def wait(self, timeout=None):
                        return self.returncode

                class _FakeSock:
                    def __init__(self):
                        self.closed = False

                    def close(self):
                        self.closed = True

                fake_child = _FakeChild([
                    ("MATCH", ("Booted\r\n", "aurora> ")),
                    ("MATCH", ("metrics start\r\nMetrics started.\r\n", "\r\naurora> ")),
                    ("TIMEOUT", "heap_stress 20000\r\nRunning heap stress test...\r\n"),
                ])
                fake_sock = _FakeSock()

                def _fake_spawn(args, port, qemu_log, stderr_file):
                    return _FakeQemu(stderr_file)

                with mock.patch(__name__ + "._port_accepts_connection", return_value=False), \
                     mock.patch(__name__ + ".spawn_qemu", side_effect=_fake_spawn), \
                     mock.patch(__name__ + ".connect_serial", return_value=fake_sock), \
                     mock.patch(__name__ + "._open_serial_child", return_value=fake_child):
                    rc = main([
                        "--elf", elf_path,
                        "--out", out_path,
                        "--qemu-log", qemu_log,
                        "--port", "2457",
                    ])

                self.assertEqual(rc, 1)
                self.assertTrue(fake_child.closed)
                self.assertTrue(fake_sock.closed)
                with open(out_path, "r", encoding="utf-8") as f:
                    text = f.read()
                self.assertIn("=== partial serial buffer ===", text)
                self.assertIn("Running heap stress test...", text)
                self.assertIn("qemu stderr boom", text)
                self.assertIn("guest-line-099", text)
                self.assertNotIn("guest-line-000", text)

        def test_zero_heap_alloc_samples_fails_gate(self):
            with tempfile.TemporaryDirectory() as tmpdir:
                elf_path = os.path.join(tmpdir, "auroraOS.elf")
                out_path = os.path.join(tmpdir, "metrics.log")
                with open(elf_path, "wb") as f:
                    f.write(b"\x7fELF")

                fake_child = _FakeChild([
                    ("MATCH", ("Boot\r\n", "aurora> ")),
                    ("MATCH", ("metrics start\r\nMetrics measurement started.\r\n", "\r\naurora> ")),
                    ("MATCH", ("heap_stress 20000\r\nRunning...\r\n", "Heap stress test finished.")),
                    ("MATCH", ("", "\r\naurora> ")),
                    ("MATCH", (
                        "cat /proc/irq\r\n"
                        "irq_latency avg=1us max=3us p99=2us count=5\r\n"
                        "heap_alloc avg=0us max=0us count=0\r\n",
                        "\r\naurora> ",
                    )),
                    ("MATCH", ("cat /proc/power\r\nsleep_ratio 0% sleep_count 0\r\n", "\r\naurora> ")),
                    ("MATCH", ("free\r\nfree_bytes=4096\r\n", "\r\naurora> ")),
                    ("MATCH", ("cat /proc/net\r\nrx_bytes=0\r\n", "\r\naurora> ")),
                ])

                class _FakeQemu:
                    returncode = 0
                    def poll(self):
                        return None
                    def terminate(self):
                        self.returncode = 0
                    def wait(self, timeout=None):
                        return 0

                class _FakeSock:
                    def close(self):
                        pass

                with mock.patch(__name__ + "._port_accepts_connection", return_value=False), \
                     mock.patch(__name__ + ".spawn_qemu", return_value=_FakeQemu()), \
                     mock.patch(__name__ + ".connect_serial", return_value=_FakeSock()), \
                     mock.patch(__name__ + "._open_serial_child", return_value=fake_child):
                    rc = main(["--elf", elf_path, "--out", out_path, "--port", "2458"])

                self.assertEqual(rc, 1)
                with open(out_path, "r", encoding="utf-8") as f:
                    self.assertIn("heap_alloc sample validation failed", f.read())

        def test_happy_path_writes_clean_sections_and_exits_zero(self):
            with tempfile.TemporaryDirectory() as tmpdir:
                elf_path = os.path.join(tmpdir, "auroraOS.elf")
                out_path = os.path.join(tmpdir, "metrics.log")
                with open(elf_path, "wb") as f:
                    f.write(b"\x7fELF")

                fake_child = _FakeChild([
                    ("MATCH", ("Boot\r\n", "aurora> ")),
                    ("MATCH", ("metrics start\r\nMetrics measurement started.\r\n", "\r\naurora> ")),
                    ("MATCH", ("heap_stress 20000\r\nRunning heap stress test...\r\n", "Heap stress test finished.")),
                    ("MATCH", ("\r\n", "\r\naurora> ")),
                    ("MATCH", (
                        "cat /proc/irq\r\n"
                        "irq_latency avg=2us max=7us p99=5us count=12\r\n"
                        "ctx_switch avg=3us max=9us count=24\r\n"
                        "heap_alloc avg=4us max=11us count=20000\r\n",
                        "\r\naurora> ",
                    )),
                    ("MATCH", ("cat /proc/power\r\nsleep_ratio 60% sleep_count 30\r\ndirty_ratio 10%\r\n", "\r\naurora> ")),
                    ("MATCH", ("free\r\nused=1024 free=7168\r\n", "\r\naurora> ")),
                    ("MATCH", ("cat /proc/net\r\ntx=1 rx=2\r\n", "\r\naurora> ")),
                ])

                class _FakeQemu:
                    returncode = None
                    def poll(self):
                        return None
                    def terminate(self):
                        self.returncode = -15
                    def wait(self, timeout=None):
                        return self.returncode

                class _FakeSock:
                    def close(self):
                        pass

                with mock.patch(__name__ + "._port_accepts_connection", return_value=False), \
                     mock.patch(__name__ + ".spawn_qemu", return_value=_FakeQemu()), \
                     mock.patch(__name__ + ".connect_serial", return_value=_FakeSock()), \
                     mock.patch(__name__ + "._open_serial_child", return_value=fake_child):
                    rc = main(["--elf", elf_path, "--out", out_path, "--port", "2459"])

                self.assertEqual(rc, 0)
                with open(out_path, "r", encoding="utf-8") as f:
                    text = f.read()
                self.assertIn("=== metrics start ===\nMetrics measurement started.", text)
                self.assertIn("Heap stress test finished.", text)
                self.assertIn("heap_alloc avg=4us max=11us count=20000", text)
                self.assertNotIn("\rcat /proc/irq", text)

        @unittest.skipUnless(os.name == "posix", "fdspawn sockets require POSIX fds")
        def test_posix_end_to_end_popen_dup_fdspawn_no_ebadf(self):
            with tempfile.TemporaryDirectory() as tmpdir:
                elf_path = os.path.join(tmpdir, "auroraOS with space.elf")
                out_path = os.path.join(tmpdir, "sub dir", "metrics.log")
                fake_qemu_py = os.path.join(tmpdir, "fake_qemu.py")
                with open(elf_path, "wb") as f:
                    f.write(b"\x7fELF")

                # Reserve an unused ephemeral port then close before spawning.
                probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
                probe.close()

                with open(fake_qemu_py, "w", encoding="utf-8") as f:
                    f.write(
                        "import socket, sys\n"
                        "port = None\n"
                        "for a in sys.argv[1:]:\n"
                        "    if a.startswith('tcp:127.0.0.1:'):\n"
                        "        port = int(a.split(':')[2].split(',')[0])\n"
                        "srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n"
                        "srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
                        "srv.bind(('127.0.0.1', port))\n"
                        "srv.listen(1)\n"
                        "conn, _ = srv.accept()\n"
                        "conn.sendall(b'Booting auroraOS\\r\\naurora> ')\n"
                        "buf = b''\n"
                        "while True:\n"
                        "    data = conn.recv(1024)\n"
                        "    if not data:\n"
                        "        break\n"
                        "    buf += data\n"
                        "    while b'\\r' in buf:\n"
                        "        line, buf = buf.split(b'\\r', 1)\n"
                        "        cmd = line.decode('utf-8', 'replace').strip()\n"
                        "        if cmd == 'metrics start':\n"
                        "            resp = b'metrics start\\r\\nMetrics measurement started.\\r\\naurora> '\n"
                        "        elif cmd == 'heap_stress 20000':\n"
                        "            resp = b'heap_stress 20000\\r\\nRunning heap stress test...\\r\\nHeap stress test finished.\\r\\naurora> '\n"
                        "        elif cmd == 'cat /proc/irq':\n"
                        "            resp = b'cat /proc/irq\\r\\nirq_latency avg=1us max=4us p99=3us count=5\\r\\nctx_switch avg=2us max=5us count=8\\r\\nheap_alloc avg=3us max=9us count=20000\\r\\n\\r\\naurora> '\n"
                        "        elif cmd == 'cat /proc/power':\n"
                        "            resp = b'cat /proc/power\\r\\nsleep_ratio 50% sleep_count 10\\r\\ndirty_ratio 5%\\r\\n\\r\\naurora> '\n"
                        "        elif cmd == 'free':\n"
                        "            resp = b'free\\r\\nTotal: 8192 Free: 4096\\r\\n\\r\\naurora> '\n"
                        "        elif cmd == 'cat /proc/net':\n"
                        "            resp = b'cat /proc/net\\r\\nrx=0 tx=0\\r\\n\\r\\naurora> '\n"
                        "        else:\n"
                        "            resp = (cmd + '\\r\\nunknown\\r\\naurora> ').encode()\n"
                        "        conn.sendall(resp)\n"
                    )

                wrapper = os.path.join(tmpdir, "fake_qemu.sh")
                with open(wrapper, "w", encoding="utf-8") as f:
                    f.write("#!/bin/sh\nexec %s %s \"$@\"\n" % (
                        shlex.quote(sys.executable), shlex.quote(fake_qemu_py)
                    ))
                os.chmod(wrapper, 0o755)

                rc = main([
                    "--qemu", wrapper,
                    "--elf", elf_path,
                    "--out", out_path,
                    "--port", str(port),
                    "--boot-timeout", "5",
                    "--cmd-timeout", "5",
                    "--workload-timeout", "5",
                ])
                self.assertEqual(rc, 0)
                with open(out_path, "r", encoding="utf-8") as f:
                    text = f.read()
                self.assertIn("Metrics measurement started.", text)
                self.assertIn("Heap stress test finished.", text)
                self.assertIn("heap_alloc avg=3us max=9us count=20000", text)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(_BenchmarkRunnerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    sys.exit(main())
