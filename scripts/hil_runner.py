#!/usr/bin/env python3
import pexpect
import socket
import sys
import time


def _dump_qemu_log():
    """Print the tail of QEMU's diagnostic log (enabled via -d int,guest_errors
    in the QEMU command). This surfaces exceptions (with PC) and unassigned
    memory accesses that would otherwise cause a silent hang."""
    try:
        with open("qemu.log", "r") as f:
            lines = f.readlines()
        print("\n===== qemu.log (last 100 lines) =====")
        for line in lines[-100:]:
            print(line, end="")
        print("===== end qemu.log =====")
    except FileNotFoundError:
        print("\n[qemu.log] not found")


def run_hil_test():
    print("Starting auroraOS HIL simulation via QEMU...")

    # 关键修复：用显式 TCP socket 串口，彻底绕开 QEMU 的 stdio/pty 输入链路。
    # 之前无论 -nographic / -display none -serial stdio -monitor none，CI 里 pexpect
    # 写给 QEMU stdin(pty) 的数据都进不了 PL011 RX FIFO（固件 TX 正常、RXFE 恒为 1）。
    # 改为 -serial tcp:... 后，pexpect 通过 socket 直连 PL011，input/output 都走
    # socket，复用(mux)/行规/pty 输入焦点等坑全部规避，这是 QEMU 自动化测试的标准做法。
    SERIAL_PORT = 1234
    qemu_cmd = ("qemu-system-arm -M lm3s6965evb -cpu cortex-m3 "
                "-display none -monitor none "
                "-serial tcp:127.0.0.1:%d,server "
                "-kernel auroraOS.elf -d int,guest_errors -D qemu.log" % SERIAL_PORT)
    print("[HIL] QEMU cmd: %s" % qemu_cmd)

    # 启动 QEMU（-serial tcp 走 socket，QEMU 自身 stdout 只用于 -d 调试，无需观察）
    qemu = pexpect.spawn(qemu_cmd, encoding='utf-8')
    qemu.logfile = None

    # 等待 QEMU 监听串口端口并建立连接（server,nowait 不会阻塞启动）
    serial_sock = None
    for _ in range(50):
        try:
            serial_sock = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if serial_sock is None:
        print("\n[HIL] FAILED: cannot connect to QEMU serial socket 127.0.0.1:%d" % SERIAL_PORT)
        qemu.terminate(force=True)
        sys.exit(1)

    # 用 fdspawn 直接驱动 socket，等价于之前的 pexpect 交互逻辑
    child = pexpect.fdspawn(serial_sock.fileno(), encoding='utf-8')
    child.logfile = sys.stdout

    try:
        # Wait for boot (the shell prints its "aurora> " prompt once it is up)
        child.expect(r"aurora> ", timeout=10)
        print("\n\n[HIL] Boot successful!")

        # Test Shell command
        child.sendline("help")
        child.expect(r"Show this message", timeout=2)
        print("\n[HIL] Shell 'help' command responsive.")

        # Test task state
        child.sendline("ps")
        child.expect(r"shell_task", timeout=2)
        print("\n[HIL] 'ps' command lists tasks correctly.")

        # Let it run for a bit to ensure stability
        print("\n[HIL] Letting it run for 3 seconds...")
        time.sleep(3)
        child.sendline("ps")
        child.expect(r"shell_task", timeout=2)
        print("\n[HIL] System is stable. Test PASSED.")

    except (pexpect.TIMEOUT, pexpect.EOF):
        print("\n[HIL] Test FAILED: Timeout/EOF waiting for expected output.")
        _dump_qemu_log()
        sys.exit(1)
    finally:
        try:
            child.close()
        except Exception:
            pass
        try:
            serial_sock.close()
        except Exception:
            pass
        qemu.terminate(force=True)

if __name__ == "__main__":
    run_hil_test()
