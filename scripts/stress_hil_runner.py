#!/usr/bin/env python3
import argparse
import os
import socket
import sys
import time
import unittest
from unittest import mock

_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)

import hil_runner


def _strip_port_args(hil_args):
    """Remove any forwarded --port option so each stress iteration uses its own port."""
    cleaned = []
    skip_next = False
    for arg in hil_args:
        if skip_next:
            skip_next = False
            continue
        if arg == "--port":
            skip_next = True
            continue
        if arg.startswith("--port="):
            continue
        cleaned.append(arg)
    return cleaned


def _can_bind_loopback(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


def alloc_free_port(preferred_port, used_ports):
    """Pick an unused TCP port distinct from previously used ports in this run."""
    for offset in range(100):
        candidate = preferred_port + offset
        if not (1 <= candidate <= 65535) or candidate in used_ports:
            continue
        if _can_bind_loopback(candidate):
            used_ports.add(candidate)
            return candidate

    for _ in range(32):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", 0))
            candidate = sock.getsockname()[1]
        finally:
            sock.close()
        if candidate not in used_ports:
            used_ports.add(candidate)
            return candidate

    raise RuntimeError("unable to allocate a free loopback TCP port")


def build_arg_parser():
    ap = argparse.ArgumentParser(
        description="Run repeated HIL checks via hil_runner with per-iteration unique ports."
    )
    ap.add_argument(
        "iterations",
        nargs="?",
        type=int,
        default=50,
        help="positive number of HIL iterations to execute (default: 50)",
    )
    ap.add_argument(
        "--base-port",
        type=int,
        default=1234,
        help="starting TCP port incremented per iteration (default: 1234)",
    )
    ap.add_argument(
        "--pause",
        type=float,
        default=0.5,
        help="seconds to wait between iterations (default: 0.5)",
    )
    ap.add_argument(
        "--keep-going",
        action="store_true",
        help="continue running remaining iterations after a failure",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="run built-in unit tests for stress_hil_runner",
    )
    return ap


def main(argv=None, runner=None):
    ap = build_arg_parser()
    args, forwarded_args = ap.parse_known_args(argv)
    if args.self_test:
        return _self_test()

    if args.iterations <= 0:
        ap.error("iterations must be a positive integer (got %d)" % args.iterations)
    if not (1 <= args.base_port <= 65535):
        ap.error("invalid --base-port: %d (must be 1..65535)" % args.base_port)
    if args.pause < 0:
        ap.error("--pause must be non-negative (got %s)" % args.pause)

    run_fn = runner if runner is not None else hil_runner.run
    hil_base_args = _strip_port_args(forwarded_args)

    print(f"Starting HIL Stress Test ({args.iterations} iterations)...")

    runs_completed = 0
    passed_count = 0
    failed_count = 0
    used_ports = set()

    for i in range(1, args.iterations + 1):
        runs_completed = i
        port = alloc_free_port(args.base_port + (i - 1), used_ports)
        iter_args = list(hil_base_args) + ["--port", str(port)]

        print("\n==============================================")
        print(f" HIL STRESS TEST: ITERATION {i}/{args.iterations} (port={port})")
        print("==============================================")

        try:
            # hil_runner.run() returns normally/0 on pass and calls sys.exit(1) on failure.
            rc = run_fn(iter_args)
            if rc not in (0, None):
                failed_count += 1
                print(f"--> Iteration {i} FAILED (return code {rc}).")
                if not args.keep_going:
                    break
            else:
                passed_count += 1
                print(f"--> Iteration {i} PASSED.")
        except SystemExit as e:
            failed_count += 1
            print(f"--> Iteration {i} FAILED (exit code {e.code}).")
            if not args.keep_going:
                break
        except Exception as e:
            failed_count += 1
            print(f"--> Iteration {i} FAILED with exception: {e}")
            if not args.keep_going:
                break

        if i < args.iterations and args.pause > 0:
            time.sleep(args.pause)

    print("\n==============================================")
    print(" STRESS TEST SUMMARY")
    print(f" Target Runs: {args.iterations}")
    print(f" Total Runs:  {runs_completed}")
    print(f" Passed:      {passed_count}")
    print(f" Failed:      {failed_count}")
    print("==============================================")

    return 1 if failed_count > 0 else 0


def _self_test():
    class _StressRunnerTests(unittest.TestCase):
        def test_rejects_bad_and_non_positive_iterations(self):
            for bad in (["abc"], ["0"], ["-4"]):
                with self.assertRaises(SystemExit) as cm:
                    main(bad)
                self.assertEqual(cm.exception.code, 2)

        def test_assigns_distinct_free_ports_and_passes_isolated_argv(self):
            busy = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            busy.bind(("127.0.0.1", 0))
            busy.listen(1)
            busy_port = busy.getsockname()[1]
            seen_argvs = []

            def _fake_run(argv=None):
                seen_argvs.append(list(argv or []))
                return 0

            try:
                rc = main(
                    ["3", "--base-port", str(busy_port), "--pause", "0", "--elf", "fw.elf", "--port", "9999"],
                    runner=_fake_run,
                )
            finally:
                busy.close()

            self.assertEqual(rc, 0)
            self.assertEqual(len(seen_argvs), 3)
            ports = []
            for a in seen_argvs:
                self.assertIn("--elf", a)
                self.assertIn("fw.elf", a)
                self.assertNotIn("9999", a)
                ports.append(int(a[a.index("--port") + 1]))
            self.assertNotIn(busy_port, ports)
            self.assertEqual(len(set(ports)), 3)

        def test_stops_on_first_system_exit_failure(self):
            calls = []

            def _fake_run(argv=None):
                calls.append(argv)
                if len(calls) == 2:
                    sys.exit(1)
                return 0

            rc = main(["5", "--pause", "0"], runner=_fake_run)
            self.assertEqual(rc, 1)
            self.assertEqual(len(calls), 2)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(_StressRunnerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
