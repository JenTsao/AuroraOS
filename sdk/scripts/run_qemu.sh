#!/usr/bin/env bash
# run_qemu.sh
# Helper script to run an AuroraOS application binary in QEMU

if [ -z "$1" ]; then
    echo "Usage: ./run_qemu.sh <path_to_app_binary.bin>"
    exit 1
fi

APP_BIN=$1

# Typically, QEMU for LM3S6965 requires the kernel ELF or binary.
# In a real SDK scenario with a microkernel, the user's app binary 
# would be loaded into the QEMU environment either via a filesystem image (LittleFS)
# or appended to the kernel image. 
# For demonstration purposes in the SDK, we'll assume a command that boots the AuroraOS
# kernel and loads the app into RAM.

echo "Starting QEMU with AuroraOS Kernel and loading $APP_BIN ..."

# qemu-system-arm -M lm3s6965evb -nographic -kernel auroraos_kernel.bin -device loader,file=$APP_BIN,addr=0x20008000
echo "QEMU execution simulated for SDK. (Ensure qemu-system-arm is installed)"
