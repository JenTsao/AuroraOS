#!/usr/bin/env python3
import pexpect
from pexpect.fdpexpect import fdspawn  # fdspawn 在子模块 pexpect.fdpexpect 里，不在 pexpect 顶层
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

    # 关掉 Nagle，避免小包(尤其行尾换行符)被合并/延迟发送
    serial_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # 用 fdspawn 直接驱动 socket，等价于之前的 pexpect 交互逻辑
    child = fdspawn(serial_sock.fileno(), encoding='utf-8')
    child.logfile = sys.stdout

    try:
        # Wait for boot (the shell prints its "aurora> " prompt once it is up)
        child.expect(r"aurora> ", timeout=10)
        print("\n\n[HIL] Boot successful!")

        # Test Shell command
        # 只发送 \r（不发送 \n）：shell 的 read() 在收到 \r 时即 break 并执行命令，
        # 不会把 \n 残留在 FIFO 中。若发送 \r\n，read() 在 \r 处 break 后，\n 留在
        # FIFO 中被下一轮循环读到，触发一次空 read（bytes=0）和多一轮 aurora> 提示
        # 符输出，污染 pexpect 缓冲区导致后续 expect 超时。
        child.send("help\r")
        child.expect(r"Show this message", timeout=2)
        print("\n[HIL] Shell 'help' command responsive.")

        # Test task state
        child.send("ps\r")
        child.expect(r"TID", timeout=2)
        print("\n[HIL] 'ps' command lists tasks correctly.")

        # 等待 aurora> 提示符，消费掉上一条 ps 的全部残留输出，清空 pexpect 缓冲区。
        child.expect(r"aurora> ", timeout=2)

        # Let it run for a bit to ensure stability (1s 足够，原 3s 纯等待)
        print("\n[HIL] Letting it run for 1 second...")
        time.sleep(1)
        child.send("ps\r")
        child.expect(r"TID", timeout=1)
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
