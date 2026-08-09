#!/bin/bash
qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -icount shift=3,align=on -rtc clock=vm -device loader,file=build_qemu/flash.bin,addr=0x0
