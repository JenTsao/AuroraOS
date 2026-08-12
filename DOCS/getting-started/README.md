# Getting Started with AuroraOS

Welcome to AuroraOS! This guide will help you set up the development environment, build the OS, and run your first application on the July Microkernel.

## 1. Environment Setup

### Required Tools
- **CMake** (3.10+)
- **Cross Compiler**: `arm-none-eabi-gcc` or `riscv64-unknown-elf-gcc`
- **QEMU**: `qemu-system-arm` or `qemu-system-riscv32` (for simulation)
- **Python 3**: For build scripts and metrics parsing

## 2. Building the OS

Use CMake to configure and build the kernel for your target platform:

```bash
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../config/toolchain-arm.cmake
make
```

## 3. Creating Your First App

AuroraOS provides a standard SDK for developing third-party applications. See `sdk/template/` for a basic scaffold.

```cpp
#include <auroraos/runtime/app_base.hpp>
// ... your app logic here
```

For a detailed tutorial on AppManifests and Capabilities, see the [Tutorials](../tutorials/README.md).
