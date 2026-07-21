#!/usr/bin/env python3
import pexpect
import sys
import time

def run_hil_test():
    print("Starting auroraOS HIL simulation via QEMU...")
    
    # Start QEMU
    qemu_cmd = "qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel auroraOS.elf"
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
        sys.exit(1)
    finally:
        child.terminate(force=True)

if __name__ == "__main__":
    run_hil_test()
