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

**Additional include-path fix:** `nimble_npl_os.cpp` uses `#include "kernel/mutex.hpp"` (with the `kernel/` prefix). The `nimble_host` library target's `target_include_directories` did not include `${CMAKE_SOURCE_DIR}` (the project root), so kernel headers were unresolvable — causing `fatal error: kernel/mutex.hpp: No such file or directory`. Fixed by adding `${CMAKE_SOURCE_DIR}` as the first include directory for the `nimble_host` target.

**Additional arch-include fix:** `nimble_npl_os.cpp` uses kernel primitives (`Mutex`, `Semaphore`, `Task`, `Arch::*`) that transitively pull in `arch_api.hpp` → `arch_impl.hpp`. The `nimble_host` target was missing `${CMAKE_SOURCE_DIR}/arch/${ARCH_DIR}` in its include paths, causing `fatal error: arch_impl.hpp: No such file or directory`. `ARCH_DIR` is set in the parent `CMakeLists.txt` before `add_subdirectory(3rdparty/nimble_port)` and is thus available. The NPL layer genuinely needs arch-level headers because it calls `Arch::disable_interrupts()`, `Arch::enable_interrupts()`, and `Arch::trigger_context_switch()` for critical section and scheduling operations.

Note: the host unit test build (`tests/CMakeLists.txt`) does NOT build `nimble_port` — it uses stubs under `tests/stubs/` that shadow real kernel headers. This issue only manifests in firmware (cross-compilation) builds.

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

## Apollo3 GPIO init_pin 位域编码与读分组缺陷（F2/F3，硬件阻塞项）

**现状：** `boards/xiaomi/miband8/hal_impl.cpp` 的 `Apollo3GpioHal::init_pin` 的
上/下拉配置与 `group_reg()` 的引脚分组均与 Apollo3 真实寄存器布局不符：

- **F2（位域编码错误）**：`init_pin` 写入的 pull 编码按 `(1u<<2)`/`(2u<<2)` 布局，
  实际命中的是 OUTCFG（`OPENDRAIN`）与 INTD（中断判别）位域，**不是** 上/下拉。
  Apollo3 的内部上/下拉由 `PADREGx` 的 `PADnRSEL` 字段选择（10k/1.5k/上/下）。
- **F3（读分组错误）**：`group_reg()` 按 `pin/16` 分组寻址，真实布局为
  `RDA`(GPIO 0-31) / `RDB`(GPIO 32-49)。**GPIO 16-49 的 `read_pin` 现网读错
  寄存器和位**；GPIO 0-15 读数正确。

**影响：** 依赖 GPIO 16-49 输入读数或真实上/下拉的驱动（如侧键若分配到
GPIO 16+）在真机上不可用。当前按键占位引脚 `PIN_BUTTON_SIDE=6` 落在 0-15
区间（F3 安全），但其上拉依赖 F2 修复前不可用——上电默认浮空，真机验证
前需外部上拉或修复 F2。

**修复前置条件：** 寄存器绝对偏移与 PADKEY（PAD 功能解锁）写序列未核实，
修复需先对照 Apollo3 / Apollo4 Blue datasheet §11.7（p420-426 起的 GPIO
配置寄存器表与 PADKEY 流程）逐项确认后方可改动 `hal_impl.cpp`。

**注意：** Mi Band 8 实际主控为 Apollo4 Blue Lite（AMA4BL），上真机前需按
Apollo4 手册复核（Apollo3 文档仅作近似参考）。

## 9. `config/autoconf.h` 与 `.config` 不同源 —— 重新生成会静默毁掉板级配置

**症状**：本地跑 `scripts/genconfig.py` 重新生成 `config/autoconf.h` 并提交后，CI 的 `build-lm3s6965` 与 `build-miband8` 同时编译失败（未定义引用），而 `build-rv32` / `build-aarch64` / `build-m0plus` / `build-cortex-m7` 全部正常。

**根因**：`genconfig.py` 的逻辑是

```python
if os.path.exists(autoconf_path) and os.path.getsize(autoconf_path) > 0:
    print("config/autoconf.h already exists, skipping kconfig regeneration")   # 跳过
    return
...
if os.path.exists(".config"):
    kconf.load_config(".config")     # ← 用提交的 .config，不是当初生成 autoconf.h 的那份
```

**提交的 `.config` 与生成出当前 `config/autoconf.h` 的那份配置并不同源。** 重新生成后实际丢失的符号：

| 符号 | 提交版 | 重新生成后 |
|------|--------|-----------|
| `AUTOCONF_H`（include guard） | 有 | **丢失** |
| `CONFIG_MAX_TASKS` | `16` | **`4`** |
| `CONFIG_NETWORKING` | `1` | **丢失** |
| `CONFIG_LUA_VM` | `1` | **丢失** |
| `CONFIG_WATCHDOG` | `1` | **丢失** |
| `CONFIG_STEALTH_HP_LASERJET` / `STEALTH_DHCP_FINGERPRINT` | 有 | **丢失** |

`CONFIG_NETWORKING` 一丢，`boards/ti/lm3s6965-qb/board.cmake` 里那些按 `CONFIG_NETWORKING` 硬编的以太网 / 扫描器 / 无线监控源文件仍在编译列表里，但对应的 `CONFIG_*` 分支不再启用配置 —— 链接期未定义引用。

**规避**：

1. **不要提交重新生成的 `config/autoconf.h`**，除非你确认过完整的符号 diff。改动 Kconfig 时先 `git diff` 看这个文件，只应**新增**符号。
2. `genconfig.py` 的「已存在就跳过」不是 bug 而是保护 —— 想干净重生成必须先 `rm -f config/autoconf.h config/autoconf.cmake .config`（CI 的 `build-rv32` / `build-aarch64` job 有这一步，见 `build.yml` 的 "Reset stale Kconfig output"）。
3. 新增 Kconfig 符号时，若运行时需要读到它而固件侧的 `autoconf.h` 还是陈旧的，让代码在「符号缺失」时退回 vendored 组件自己的默认值（见 `ai/february/kconfig_glue.hpp` 的 `FEBRUARY_COMPILED` 处理），而不是 `#ifdef` 整个编译掉 —— 后者会让固件与 host 单测的行为分叉，测试形同虚设。
