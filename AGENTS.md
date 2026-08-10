# CLAUDE.md — AuroraOS

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
- `experimental/` — research/feature-prototype code (may be incomplete or unstable; do not assume production-readiness).
- `adapter/` — abstraction shims between kernel and board/driver layers.
- `ai/`, `ui/` — AI/ML glue and UI primitives (early stage).
- `syscall/` — syscall definitions and ABI surface.
- `boot/`, `bootloader/` — early boot and bootloader stages.
- `tests/` — host GoogleTest suite (run kernel logic on x86-64). `tests/CMakeLists.txt` is the entry point.
- `scripts/` — build/codegen helpers: `genconfig.py` (Kconfig → header), `run_qemu.py`, `build_miband.ps1`, etc.
- `3rdparty/` — vendored dependencies (lwIP, Lua, CryptoPAn, Catch2, fmt, etc.). Treat as external; do not edit unless updating the vendored version.
- `metrics/` — runtime metrics/telemetry collection.
- `requirements.txt` — Python deps for build scripts (`kconfiglib`, `pexpect`).
- `Kconfig`, `Kconfig.miband` — feature configuration menus.
- `CMakeLists.txt` — main firmware build. `CMakeLists.miband.txt` — MiBand-specific build.

## Primary targets and how to build/run

There are three build targets. Pick the right one for the task.

1. **LM3S6965 QEMU (primary HIL / CI target).** Use this for kernel bring-up, IPC/capability testing, and most development. Default simulator target.
   - Configure then run: invoke `scripts/run_qemu.py` (or the CMake `qemu` target) after configuring with the LM3S6965 board. The host test suite is also built here for fast iteration.
2. **MiBand 8 / nRF52840 (real hardware target).** Wearable deployment path. Built via `CMakeLists.miband.txt` and flashed with `scripts/build_miband.ps1` (Windows-centric flow). There is a dedicated `miband` git branch.
3. **RISC-V RV32 QEMU (secondary simulation target).** Used to validate the RISC-V arch port.

Do not assume a single "default" board at runtime — the active board is selected by Kconfig + the `boards/` and `config/*.ld` linker scripts. Always confirm which target a change is for.

## Configuring the build (Kconfig)

Features are selected via Kconfig, not hardcoded. After editing `Kconfig`/`Kconfig.miband`, regenerate the configuration header with `scripts/genconfig.py` (run via `start_env.bat` or the Python venv with `kconfiglib` installed). Never hand-edit generated config headers; regenerate them.

## Testing

- **Host unit tests (primary verification path).** Build and run with:
  `cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug && cmake --build build_tests -j && ctest --test-dir build_tests --output-on-failure --timeout 30`
  These exercise scheduler, capability, IPC, crypto, and memory logic on the host without flashing hardware.
- **Sanitizers.** CI runs ASAN + UBSAN builds of the host tests to catch memory and undefined-behavior bugs. Prefer running these locally before pushing.
- **Hardware-in-the-loop.** LM3S6965 QEMU and (where available) MiBand 8 flash runs. Use QEMU for fast iteration; reserve hardware for integration checks.
- CI (`.github/workflows/build.yml`) runs on `main` and `miband` branches and PRs: host unit tests, sanitizer builds, and firmware builds.

## Languages and conventions

- **C++ for the kernel, drivers, and apps** (modern C++, namespaces, RAII, no raw `new` in hot paths where an allocator exists). Header files use `.hpp`, sources `.cpp`.
- **C for the lowest-level drivers and 3rdparty glue** (`.h` / `.c`).
- **Assembly (`.s`) for boot, reset vectors, and context switch** under `arch/`.
- **Python for build/codegen** (`scripts/`), requires `kconfiglib` and `pexpect`.
- Keep the kernel dependency-light: prefer vendored `3rdparty/` libraries over pulling new external deps. Avoid dynamic allocation in the critical scheduler/capability paths where feasible.
- Respect the MPU boundary: userspace apps must go through capabilities/IPC — never add a direct kernel-internal shortcut that bypasses the capability check.
- Match existing style in the file you edit (indentation, naming). The codebase favors explicit, readable kernel code over cleverness.

## Architecture invariants (do not break)

- **Capabilities are the only authority mechanism.** All cross-domain access (memory, endpoints, devices, threads) is mediated by capability derivation/revocation. New resources must be exposed as derivable capabilities, not global handles.
- **IPC is synchronous message-passing via endpoints.** The fast-path IPC is the performance-critical path; changes here need benchmarking against the existing fast-path code.
- **MPU sandbox confines userspace.** Apps (including Lua) run with MPU-limited memory; kernel memory is never directly accessible from apps.
- **Secure boot chain** validates firmware before execution; changes to boot/`bootloader/` must preserve chain-of-trust integrity.
- **Cooperative vs preemptive boundaries** are compile-time; altering scheduling assumptions can break timing-determinism guarantees.

## Suggested workflow for changes

1. Reproduce/understand the behavior with the host test suite first (`tests/`).
2. Make the change in the appropriate layer (kernel / arch / drivers / apps). Avoid cross-layer leaks.
3. Add or update a GoogleTest case for kernel-level logic.
4. Run host tests + sanitizers locally, then build for the target board (QEMU first).
5. Keep `experimental/` isolated from production build paths unless explicitly wiring a feature in.

## Notes on scope

This document intentionally focuses on architecture, build, and conventions. Operational/feature specifics that are out of scope for this guide are maintained separately; ask the user if a task touches those areas.

## References

- `README.md` — project overview and getting started.
- `arch/` and `kernel/` — authoritative source for ISA ports and kernel internals.
- `scripts/run_qemu.py`, `scripts/genconfig.py`, `scripts/build_miband.ps1` — build/run entry points.
- `.github/workflows/build.yml` — CI contract (what must stay green).

## Bug Fix Experience: SRAM Overflow and BSS Size Constraints on Apollo3 (MiBand8)

When developing for heavily resource-constrained targets like the MiBand 8 (Apollo3) which enforces a strict 64KB `.bss` limit in its CI build scripts, be extremely careful about static allocations and linker script configurations:

1. **`arm-none-eabi-size` behavior**: GNU `size` categorizes all `SHT_NOBITS` (uninitialized memory) sections into the `bss` column. If your linker script defines an isolated stack/heap section like `._user_heap_stack`, its size (e.g., `_Min_Heap_Size = 0x10000` / 64KB) will be **added** to the final `bss` output. This can cause false-positive CI failures even if your actual `.bss` variables are small. To fix this, shrink `_Min_Heap_Size` (e.g., to 16KB) if you encounter a "BSS exceeds 64KB" error but your static arrays seem within limits.
2. **Conditional Compilation Pitfalls**: Large mock components (like `FlashBlockDevice::memory_` using 128KB default) MUST be explicitly configured via CMake definitions (e.g., `target_compile_definitions` with `CONFIG_BOARD_MIBAND8=1`) to shrink them (e.g., to 16KB). Do not assume `#ifdef CONFIG_BOARD_MIBAND8` works unless the target explicitly injects it in its CMake configurations.
3. **UI FrameBuffer Optimization**: On 384KB SRAM devices, full-screen `FrameBuffer` (e.g., 192x490x2 = 184KB) is untenable. You MUST utilize stripe-rendered chunk buffers (e.g., `AURORA_FB_CHUNK_HEIGHT=30` saving ~170KB) and point static components to a unified memory pool to stay under 64KB limits.

## Bug Fix Experience: Git Submodule CI Failures

When introducing new third-party dependencies (like `NimBLE` or `btstack`) via `git clone --depth 1` into the `3rdparty/` directory, this will silently fail in CI environments (like GitHub Actions) with `fatal: No url found for submodule path...`. 

**Why:** A manually cloned repository contains a `.git` folder, causing Git to treat it as an untracked gitlink (submodule). However, CI workflows typically run `git submodule update --init --recursive` which strictly relies on `.gitmodules`. If the URL isn't in `.gitmodules`, the CI checkout step will crash.

**Fix:** ALWAYS register manually cloned dependencies in the root `.gitmodules` file (or use `git submodule add` instead of `git clone`), ensuring the CI checkout action can properly resolve the submodule URL during automated builds.
