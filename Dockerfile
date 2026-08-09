FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi \
    gcc-riscv64-unknown-elf \
    cmake \
    qemu-system-arm \
    qemu-system-misc \
    python3 \
    make \
    && rm -rf /var/lib/apt/lists/*
