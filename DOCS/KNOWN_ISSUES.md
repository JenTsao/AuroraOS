# Known Issues and Bug Fix Patterns

This document records historical bug fix patterns, obscure toolchain behaviors, and strict hardware constraints. **Always consult this file when encountering strange CI failures or seemingly impossible bugs.**

## 1. SRAM Overflow and BSS Size Constraints on Apollo3 (MiBand8)

When developing for heavily resource-constrained targets like the MiBand 8 (Apollo3) which enforces a strict 64KB `.bss` limit in its CI build scripts, be extremely careful about static allocations and linker script configurations:

- **`arm-none-eabi-size` behavior**: GNU `size` categorizes all `SHT_NOBITS` (uninitialized memory) sections into the `bss` column. If your linker script defines an isolated stack/heap section like `._user_heap_stack`, its size (e.g., `_Min_Heap_Size = 0x10000` / 64KB) will be **added** to the final `bss` output. This can cause false-positive CI failures even if your actual `.bss` variables are small. To fix this, shrink `_Min_Heap_Size` (e.g., to 16KB) if you encounter a "BSS exceeds 64KB" error but your static arrays seem within limits.
- **Conditional Compilation Pitfalls**: Large mock components (like `FlashBlockDevice::memory_` using 128KB default) MUST be explicitly configured via CMake definitions (e.g., `target_compile_definitions` with `CONFIG_BOARD_MIBAND8=1`) to shrink them (e.g., to 16KB). Do not assume `#ifdef CONFIG_BOARD_MIBAND8` works unless the target explicitly injects it in its CMake configurations.
- **UI FrameBuffer Optimization**: On 384KB SRAM devices, full-screen `FrameBuffer` (e.g., 192x490x2 = 184KB) is untenable. You MUST utilize stripe-rendered chunk buffers (e.g., `AURORA_FB_CHUNK_HEIGHT=30` saving ~170KB) and point static components to a unified memory pool to stay under 64KB limits.

## 2. Git Submodule CI Failures

When introducing new third-party dependencies (like `NimBLE` or `btstack`) via `git clone --depth 1` into the `3rdparty/` directory, this will silently fail in CI environments (like GitHub Actions) with `fatal: No url found for submodule path...`. 

**Why:** A manually cloned repository contains a `.git` folder, causing Git to treat it as an untracked gitlink (submodule). However, CI workflows typically run `git submodule update --init --recursive` which strictly relies on `.gitmodules`. If the URL isn't in `.gitmodules`, the CI checkout step will crash.

**Fix:** ALWAYS register manually cloned dependencies in the root `.gitmodules` file (or use `git submodule add` instead of `git clone`), ensuring the CI checkout action can properly resolve the submodule URL during automated builds.

## 3. NimBLE Submodule — Upstream Adds Platform-Specific Files; Never Use `file(GLOB)`

The `3rdparty/nimble` submodule tracks Apache Mynewt NimBLE. Upstream commits (e.g. `70da7f3` on 2026-07-28) can add new `.c` files to `porting/nimble/src/` at any time. These files often contain platform-specific includes that do NOT exist in AuroraOS:

| Upstream file | Problem |
|---------------|---------|
| `hal_timer.c` | `#include "nrfx.h"` — Nordic nRF SDK, unavailable |
| `nimble_port.c` | `#include "nimble/transport.h"` plus controller init (`ble_ll.h`) |
| `os_cputime.c` / `os_cputime_pwr2.c` | Hardware timer HAL dependencies |

**Why:** The original `nimble_port/CMakeLists.txt` used `file(GLOB NIMBLE_SRC ".../porting/nimble/src/*.c")`. When upstream added 9 new files (including `hal_timer.c`, `nimble_port.c`, `os_cputime*.c`, `os_mbuf.c`, `os_mempool.c`, `os_msys.c`), the glob silently picked them all up. CI builds for ALL four targets (LM3S6965, RV32, MiBand8, M0+) then failed with `fatal error: nrfx.h: No such file or directory`, because `hal_timer.c` requires the Nordic nRF SDK header which is not vendored.

**Fix:** Replace `file(GLOB ...)` with an explicit source file list. The generic porting files safe to include are: `endian.c`, `mem.c`, `os_mbuf.c`, `os_mempool.c`, `os_msys.c`. Exclude `hal_timer.c`, `nimble_port.c`, `os_cputime.c`, and `os_cputime_pwr2.c`.

**Additional include-path fix:** `nimble_npl_os.cpp` uses `#include "kernel/mutex.hpp"` (with the `kernel/` prefix). The `nimble_host` library target's `target_include_directories` did not include `${CMAKE_SOURCE_DIR}` (the project root), so kernel headers were unresolvable — causing `fatal error: kernel/mutex.hpp: No such file or directory`. Fixed by adding `${CMAKE_SOURCE_DIR}` as the first include directory for the `nimble_host` target. Note: this is different from the `aurora_tests` target in `tests/CMakeLists.txt`, which uses `${AURORA_ROOT}/kernel` and thus resolves `#include "mutex.hpp"` (no `kernel/` prefix) — two different include conventions must be supported.

**`file(GLOB)` audit result:** All other `file(GLOB)` uses in the project are safe because they target vendored libraries with stable file sets (Lua, lwIP) and include explicit `list(REMOVE_ITEM ...)` filtering. Only the NimBLE porting layer glob was dangerous because upstream can add platform-specific files that break cross-platform builds.

## 4. CI Firmware Build Troubleshooting — Common Failure Patterns

The CI (`.github/workflows/build.yml`) builds firmware for four targets. The following pitfalls cause firwmare build failures:

### 4a. `make` versus `cmake --build` generator mismatch
All firmware build jobs invoke `make -j$(nproc)` directly. If CMake's default generator is Ninja (which may be pre-installed on ubuntu-24.04 runners), the build directory contains `build.ninja` rather than a `Makefile`, and `make` fails. Always use `cmake --build <dir> -j$(nproc)`.

### 4b. Stale Kconfig output files tracked in git
`config/autoconf.h`, `config/autoconf.cmake`, and `.config` are tracked in git and currently target `BOARD_LM3S6965_QB`. `scripts/genconfig.py` skips regeneration when `autoconf.h` already exists — it does NOT check which board that file was generated for. This means non-LM3S6965 boards (RV32, MiBand8) silently inherit LM3S6965 Kconfig flags unless the stale files are deleted before cmake configure (as the RV32 CI job does at line 329). The MiBand8 CI job currently does NOT have this step.

### 4c. NimBLE submodule must initialize for all boards
`CMakeLists.txt` line 269 unconditionally does `add_subdirectory(3rdparty/nimble_port)` for every board (including M0+ which has no networking). The NimBLE host sources reference `${CMAKE_SOURCE_DIR}/3rdparty/nimble/nimble/host/src/*.c`. Every CI job MUST use `submodules: recursive` in `actions/checkout@v4`.

### 4d. Per-board resource limits enforced in CI
| Target | Flash | BSS | Hard Error? |
|--------|-------|-----|-------------|
| LM3S6965 | 256 KB | None | Warning only |
| MiBand8 | 512 KB | 64 KB | Yes (BSS includes `._user_heap_stack`) |
| M0+ | 64 KB | 8 KB | Flash: error, BSS: warning |
| RV32 | None | None | N/A |

### 4e. New source files must be conditioned on board
The `CMakeLists.txt` source list (lines 104-269) is organized by `if(BOARD STREQUAL "...")` blocks. Not all sources compile for all boards: networking/lwIP/firewall/scanner are excluded from M0+; OTA and symbol_export are excluded from M0+; `apps/shell.cpp` is excluded from MiBand8 (which uses `apps/watch/miband_main.cpp` instead).

### 4f. Correct toolchain file per board
- LM3S6965 & M0+: `config/toolchain.cmake`
- MiBand8: `config/toolchain_miband.cmake` (hard-float FPU: `-mfloat-abi=hard -mfpu=fpv4-sp-d16`)
- RV32: `config/toolchain_rv32.cmake` (picolibc, not newlib)
