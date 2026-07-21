#!/usr/bin/env python3
import pexpect
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
    
    # Start QEMU
    # 注意：-nographic 隐含的是 "-serial mon:stdio"，即把串口与 monitor 复用(mux)
    # 到同一 stdio。仅用 -monitor none 关掉 monitor 并不能解除这个 mux：stdio 的输入
    # 焦点仍在 mux 上，pexpect 写入的 stdin 永远进不了 PL011 的 RX FIFO
    # (表现为 RXFE 恒为 1、固件收不到任何字符，但固件 TX 输出正常)。
    # 正确做法：用 -display none 替代 -nographic(避免它再塞 mon:stdio)，并显式
    # "-serial stdio -monitor none"，使 stdio 纯粹走串口、monitor 彻底关闭。
    qemu_cmd = ("qemu-system-arm -M lm3s6965evb -cpu cortex-m3 "
                "-display none -serial stdio -monitor none "
                "-kernel auroraOS.elf -d int,guest_errors -D qemu.log")
    child = pexpect.spawn(qemu_cmd, encoding='utf-8')
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
        
    except pexpect.TIMEOUT:
        print("\n[HIL] Test FAILED: Timeout waiting for expected output.")
        _dump_qemu_log()
        sys.exit(1)
    finally:
        child.terminate(force=True)

if __name__ == "__main__":
    run_hil_test()
