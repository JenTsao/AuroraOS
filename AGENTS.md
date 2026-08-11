# AGENTS.md — AuroraOS AI Assistant Guide

Guidance for AI coding assistants working in this repository. AuroraOS is a security-hardened, capability-based microkernel RTOS for resource-constrained wearables and IoT terminals (smartwatches, sensor nodes). Keep responses concise and ground every change in the architecture described below.

## 1. What this project is

AuroraOS is a from-scratch microkernel OS written primarily in modern C++ (C++17/20/23) for the kernel, with C device drivers and assembly startup/boot code. It targets bare-metal ARM Cortex-M0+/M3/M4 and RISC-V RV32 microcontrollers. The kernel implements a seL4-inspired capability system for object-based IPC with formally reasoned isolation, an MPU-based userspace sandbox, compile-time preemptive priority scheduling, and a security-first design (secure boot, encrypted at-rest storage, capability-confined syscalls).

Key differentiators: capability-secured IPC instead of raw syscalls, an MPU sandbox isolating untrusted userspace apps, a Lua interpreter running inside the sandbox for safe app logic, lwIP networking, and BLE connectivity. The repo also ships a host-side unit-test suite (~196 GoogleTest cases) that runs the same kernel algorithms on x86-64 for fast verification.

## 2. Repository layout (read before touching anything)

- `kernel/` — the microkernel: scheduler, capability system, IPC, memory manager, MPU, syscall dispatch, timing. This is the heart of the OS.
- `arch/` — per-architecture startup and context-switch code (`.s` assembly + C++). Subdirs per ISA (arm, riscv).
- `drivers/` — C++ device drivers (UART, SPI, I2C, sensors, display, BLE, flash).
- `boards/` — board definitions (clock, memory map, peripheral wiring) per target.
- `config/` — linker scripts (`.ld`), Kconfig fragments, board CMake configs.
- `apps/` — built-in userspace applications (incl. Lua-based app logic).
- `net/` — lwIP networking stack integration and network services.
- `experimental/` — isolated research/feature-prototype code. Do not use or touch unless specifically requested.
- `adapter/` — abstraction shims between kernel and board/driver layers.
- `ai/`, `ui/` — AI/ML glue and UI primitives (early stage).
- `syscall/` — syscall definitions and ABI surface.
- `boot/`, `bootloader/` — early boot and bootloader stages.
- `tests/` — host GoogleTest suite (run kernel logic on x86-64). `tests/CMakeLists.txt` is the entry point.
- `scripts/` — build/codegen helpers: `genconfig.py` (Kconfig → header), `run_qemu.py`, etc.
- `3rdparty/` — vendored dependencies (lwIP, Lua, LittleFS, ed25519, nimble). Treat as external; do not edit unless updating the vendored version.
- `metrics/` — runtime metrics/telemetry collection.

## 3. Primary targets and how to build/run

There are four build targets. Pick the right one for the task.
1. **LM3S6965 QEMU (primary HIL / CI target):** Default simulator target.
   - Built via `cmake -DBOARD=lm3s6965-qb ...`.
2. **MiBand 8 / Ambiq Apollo3 (Cortex-M4F) (real hardware target):** Wearable deployment path.
   - Built via `cmake -DBOARD=miband8 ...`.
3. **Nucleo-L031K6 (Cortex-M0+):** Ultra-low-power target.
   - Built via `cmake -DBOARD=nucleo_l031k6 ...`.
4. **RISC-V RV32 QEMU (secondary simulation target):**
   - Built via `cmake -DBOARD=qemu_rv32_virt ...`.

### Quick Verification Commands
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

### Configuring the build (Kconfig Workflow)
1. Edit `Kconfig`.
2. Run `python scripts/genconfig.py` to regenerate `config/autoconf.h`.
3. Re-run `cmake` and `make`.
**NEVER** hand-edit `config/autoconf.h` or `config/autoconf.cmake` directly!

## 4. AuroraOS C++ Coding Standards
Code generated for this repository MUST adhere to modern C++ principles based on the C++ Core Guidelines.

### 4.1 Cross-Cutting Principles & Immutability
- **RAII everywhere:** Bind resource lifetime to object lifetime. Never leak resources.
- **Immutability by default:** Maximize use of `const` and `constexpr`. Variables should be declared `const` or `constexpr` unless they are explicitly meant to be modified.
- **Type safety:** Use the type system (e.g., `enum class` over `enum`) to prevent errors at compile time. Avoid magic numbers and `void*`.
- **Value semantics:** Prefer returning by value (using structs for multiple return values) rather than output parameters.

### 4.2 Interfaces & Functions
- **Naming:** Express intent clearly. Use `underscore_style` for classes, functions, and variables. Use `ALL_CAPS` only for macros.
- **Compile-time checks:** Prefer compile-time checks (`static_assert`, `constexpr`) over run-time checks.
- **Parameters:** Pass cheap-to-copy types by value, and expensive types by `const&`.
- **Noexcept:** If a kernel function cannot throw or fail, declare it `noexcept`.

### 4.3 Classes & Resource Management
- **Rule of Zero/Five:** If you can avoid defining default operations, let the compiler do it (Rule of Zero). If you define or `=delete` any copy/move/destructor, handle all five.
- **Constructors:** Declare single-argument constructors `explicit`. A constructor should create a fully initialized object. Always initialize members, preferring `{}` syntax.
- **Hierarchy:** Base classes must have a `public virtual` or `protected non-virtual` destructor. Use `override` or `final` for virtual functions.
- **Smart Pointers:** Avoid raw `new`/`delete` or `malloc`/`free`. Represent exclusive ownership with `std::unique_ptr` and shared ownership with `std::shared_ptr`. Raw pointers (`T*`) are strictly for non-owning observation.

### 4.4 Concurrency & Error Handling
- **Synchronization:** Never use plain `lock()`/`unlock()`. Always use RAII (`std::lock_guard`, `std::unique_lock`, `std::scoped_lock`) and name your lock variables to prevent immediate destruction.
- **Wait conditions:** Never wait on a condition variable without a condition predicate.
- **Errors:** Throw an exception by value and catch by reference to signal failure (using purpose-designed exception types), unless operating in a strictly `noexcept` kernel hot-path.

## 5. Testing and CI Contract
The CI pipeline (`.github/workflows/build.yml`) consists of 9 critical jobs (e.g., `unit-tests`, `sanitize`, `static-analysis`, `cppcheck`, `coverage`, and builds for LM3S6965, RV32, MiBand8, M0+). 
**All of these must stay green.** Run unit tests and static analysis locally before declaring a task finished.

## 6. AI Assistant "Do Not Modify" List
The following areas are off-limits for AI modification unless explicitly instructed by the user:
- `3rdparty/`
- `.github/workflows/build.yml`
- `config/*.ld`
- Generated files (`config/autoconf.h`, `config/autoconf.cmake`)

## 7. Code Review Checklist for AI
Before finalizing your response, self-verify your code against this checklist:
- [ ] **Architecture:** Did this change bypass the capability check for cross-domain access? Does it respect the MPU boundary?
- [ ] **Memory & Size:** Did I avoid dynamic allocation (`new`/`malloc`) in hot paths like the scheduler/IPC? Will this change bloat `.bss` or `.text` beyond limits (e.g., 384KB SRAM / 576KB Flash for MiBand8)?
- [ ] **C++ Standards:** Is RAII used universally? Are variables `const` by default? Are naming conventions (`underscore_style`) followed? Is the Rule of Zero/Five respected?
- [ ] **Security:** Are new resources exposed securely as derivable capabilities rather than global handles?
- [ ] **Testing:** Did I add/update a GoogleTest case in `tests/`? Have I checked `docs/KNOWN_ISSUES.md` for historical bug fix patterns?
