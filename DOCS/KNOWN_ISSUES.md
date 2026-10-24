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

### 4b. Stale Kconfig output files tracked in git — RESOLVED
Historically `config/autoconf.h` / `config/autoconf.cmake` were tracked in git and `scripts/genconfig.py` skipped regeneration whenever `autoconf.h` already exists, so non-default boards silently inherited whichever board last wrote that file. Now resolved at the mechanism level: `genconfig.py` regenerates the full autoconf trio into the build directory on every configure (single source of truth = Kconfig, with a compile-time board guard rejecting stale/wrong-board `autoconf.h`), the tracked `config/autoconf.h` has been removed, and `scripts/gen_m0plus_config.py` delegates to `genconfig.py` instead of hardcoding M0+ values. The `rm -f config/autoconf.* .config` lines still present in some CI jobs are legacy belt-and-braces and are no longer required for correctness.

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

## 5. `-m32` (ILP32) 主机测试暴露的两类可移植性缺陷

`unit-tests-32bit` 用 `-m32` 构建主机测试。64 位主机上全绿的用例，在 ILP32 下曾整组
SEGFAULT / 失败。这两类缺陷与"指针截断"无关，值得单独记录：

### 5a. TLSF 尺寸分级：<32B 与 32..63B 共用 `fl = 0`

`runtime/lua_heap.hpp` 的 `mapping_insert()` 把 `fl` 算成 `msb - FL_INDEX_SHIFT`，
于是 32..63B 的块与 16..31B 的块落进同一个 `fl = 0`，而 `sl` 仍按 `size ^ (1<<msb)`
计算——`sl` 随尺寸单调递增的前提被破坏。一次 40B 请求会命中 `sl = 12` 的 24B 空闲块，
返回只有 16B payload 的块。**32 位下 `MIN_BLOCK_SIZE = 16`，16/24B 小块远多于 64 位**，
Lua GC 第一次 `lua_close` 释放就写穿相邻块头（`remove_free_block` 里
`node->prev_free` 是垃圾指针），表现为 15 个 Lua 用例 SEGFAULT。

**修复：** `fl = msb - FL_INDEX_SHIFT + 1`（槽位 0 专用于 <32B 小块），
`FL_INDEX_COUNT` 相应 `+2`。回归用例：
`LuaHeapMappingTest.SmallFreeBlockDoesNotSatisfyLargerRequest`（40B 写入后校验 64B
guard 区未被破坏）。

### 5b. 32 位 `uintptr_t` 上的 `vaddr >> 39` 是未定义行为

`arch/arm/cortex-a/mmu/mmu_manager.cpp` 按 AArch64 四级页表取索引时直接写
`vaddr >> 39/30/21/12`。`-m32` 下 `uintptr_t` 只有 32 位，移位量 ≥ 位宽是 UB；
x86 只取移位量低 5 位，`>> 39` 实际等价于 `>> 7`。于是 L0 索引变成 `vaddr` 的
bit7..15——每 128 字节就换一项、每 64KB 回绕一次；128MB 内核映射会反复重建
页表子树，4MB 页表池瞬间耗尽，`map_kernel_regions()` 整体失败（5 个 MMU 用例
报 `translate()` 失败）。

**修复：** 引入 `split_va()`，先把 `vaddr` 提升为 `uint64_t` 再移位，四级索引统一
由它产出。AArch64 固件行为不变（`uintptr_t` 本就是 64 位），只是让主机测试能真实
覆盖页表逻辑。

**教训：** 这两个缺陷都只在 ILP32 下可见，64 位主机测试无法替代 `-m32` 门禁；
新增位运算/尺寸分级逻辑时，应同时检查移位量是否可能超过 `sizeof(uintptr_t) * 8`。

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
