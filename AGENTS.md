# AGENTS.md — AuroraOS AI Assistant Guide

Guidance for AI coding assistants working in this repository. AuroraOS is a security-hardened, capability-based microkernel RTOS for resource-constrained wearables and IoT terminals (smartwatches, sensor nodes). Keep responses concise and ground every change in the architecture described below.

## What this project is

AuroraOS is a from-scratch microkernel OS written primarily in modern C++ (kernel) with C device drivers and assembly startup/boot code. It targets bare-metal ARM Cortex-M0+/M3/M4 and RISC-V RV32 microcontrollers. The kernel implements a seL4-inspired capability system for object-based IPC with formally reasoned (but not machine-verified) isolation, an MPU-based userspace sandbox, compile-time preemptive priority scheduling, and a security-first design (secure boot, encrypted at-rest storage, capability-confined syscalls).

Key differentiators: capability-secured IPC instead of raw syscalls, an MPU sandbox isolating untrusted userspace apps, a Lua interpreter running inside the sandbox for safe app logic, lwIP networking, and BLE connectivity. The repo also ships a host-side unit-test suite (~196 GoogleTest cases) that runs the same kernel algorithms on x86-64 for fast verification.

## Repository layout (read before touching anything)

- `kernel/` — the microkernel: scheduler, capability system, IPC, memory manager, MPU, syscall dispatch, timing. This is the heart of the OS.
- `arch/` — per-architecture startup and context-switch code (`.s` assembly + C++). Subdirs per ISA (arm, riscv).
- `drivers/` — C++ device drivers (UART, SPI, I2C, sensors, display, BLE, flash).
- `boards/` — board definitions (clock, memory map, peripheral wiring) per target.
- `config/` — linker scripts (`.ld`), Kconfig fragments, board CMake configs.
- `apps/` — built-in userspace applications (incl. Lua-based app logic).
- `net/` — lwIP networking stack integration and network services.
- `experimental/` — isolated research/feature-prototype code. Do not use or touch unless specifically requested; it is NOT in the main production build.
- `adapter/` — abstraction shims between kernel and board/driver layers.
- `ai/`, `ui/` — AI/ML glue and UI primitives (early stage).
- `syscall/` — syscall definitions and ABI surface.
- `boot/`, `bootloader/` — early boot and bootloader stages.
- `tests/` — host GoogleTest suite (run kernel logic on x86-64). `tests/CMakeLists.txt` is the entry point.
- `scripts/` — build/codegen helpers: `genconfig.py` (Kconfig → header), `run_qemu.py`, etc.
- `3rdparty/` — vendored dependencies (lwIP, Lua, LittleFS, ed25519, nimble). Treat as external; do not edit unless updating the vendored version.
- `metrics/` — runtime metrics/telemetry collection.
- `requirements.txt` — Python deps for build scripts (`kconfiglib`, `pexpect`).
- `Kconfig` — feature configuration menus.
- `CMakeLists.txt` — main firmware build.

## Primary targets and how to build/run

There are four build targets. Pick the right one for the task.

1. **LM3S6965 QEMU (primary HIL / CI target).** Use this for kernel bring-up, IPC/capability testing, and most development. Default simulator target.
   - Built via `cmake -DBOARD=lm3s6965-qb ...`.
2. **MiBand 8 / Ambiq Apollo3 (Cortex-M4F) (real hardware target).** Wearable deployment path. There is a dedicated `miband` git branch.
   - Built via `cmake -DBOARD=miband8 ...`.
3. **Nucleo-L031K6 (Cortex-M0+).** Ultra-low-power target.
   - Built via `cmake -DBOARD=nucleo_l031k6 ...`.
4. **RISC-V RV32 QEMU (secondary simulation target).** Used to validate the RISC-V arch port.
   - Built via `cmake -DBOARD=qemu_rv32_virt ...`.

Do not assume a single "default" board at runtime — the active board is selected by Kconfig + the `boards/` and `config/*.ld` linker scripts. Always confirm which target a change is for.

## Quick Verification Commands

Copy and run these exact blocks to verify your changes quickly:

```bash
# Host Unit Tests (Do this FIRST for any kernel/logic changes)
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j$(nproc)
ctest --test-dir build_tests --output-on-failure

# One-click QEMU Boot Verification (LM3S6965)
pip install kconfiglib pexpect
mkdir -p build && cd build
cmake -DBOARD=lm3s6965-qb ..
make -j$(nproc)
qemu-system-arm -M lm3s6965evb -nographic -kernel auroraOS.elf
```

## Configuring the build (Kconfig Workflow)

Features are selected via Kconfig, not hardcoded. When you need to alter a config:
1. Edit `Kconfig`.
2. Run `python scripts/genconfig.py` to regenerate `config/autoconf.h`.
3. Re-run `cmake` and `make`.

**NEVER** hand-edit `config/autoconf.h` or `config/autoconf.cmake` directly!

## AI Assistant "Do Not Modify" List

The following areas are off-limits for AI modification unless explicitly instructed by the user:
- `3rdparty/` — Only modify if you are porting/updating a vendored library.
- `.github/workflows/build.yml` — The CI contract. Modify only if you fully understand ALL 9 jobs.
- `config/*.ld` — Linker scripts are highly sensitive; breaking them bricks the target.
- Generated files (`config/autoconf.h`, `config/autoconf.cmake`).

## Testing and CI Contract

The CI pipeline (`.github/workflows/build.yml`) runs on `main` and `miband` branches. It consists of 9 critical jobs:
1. `unit-tests`: ~196 Host GoogleTest cases. (BLOCKER for subsequent analysis jobs)
2. `sanitize`: ASAN + UBSAN builds.
3. `static-analysis`: clang-tidy scanning.
4. `cppcheck`: lightweight C/C++ static analysis.
5. `coverage`: test coverage reports.
6. `build-lm3s6965`: Cortex-M3 build + QEMU HIL test.
7. `build-rv32`: RISC-V QEMU build.
8. `build-miband8`: Apollo3 Cortex-M4F build.
9. `build-m0plus`: Nucleo Cortex-M0+ build.
10. `firmware-size`: Compares final `.bss` and `.text` against strict limits.

**All of these must stay green.** Run unit tests and static analysis locally before declaring a task finished.

## Code Review Checklist for AI

Before finalizing your response, self-verify your code against this checklist:
- [ ] Did this change bypass the capability check for cross-domain access?
- [ ] Did I introduce dynamic allocation (`new` or `malloc`) in hot paths (like the scheduler or IPC)?
- [ ] Does this change respect the MPU boundary? (Userspace cannot touch kernel memory)
- [ ] Are new resources exposed securely as derivable capabilities rather than global handles?
- [ ] Did I add/update a GoogleTest case in `tests/` to cover this new logic?
- [ ] Will this change bloat the `.bss` or `.text` size beyond the strict MiBand8 (64KB SRAM/512KB Flash) limits?

## Bug Fix Patterns & Known Issues

Historical bug fix patterns, common CI failures (like Git submodules), and SRAM optimization tricks have been documented in:
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md). **Always refer to this file when facing mysterious CI or build errors.**
