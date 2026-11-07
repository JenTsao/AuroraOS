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

## Apollo3 GPIO 位域与读分组缺陷（F2/F3，硬件阻塞项）

**状态：已按 Apollo3 Blue 官方寄存器表重写（2026-11-07，未真机验证）。**
**仍是硬件阻塞项**，因为主控芯片型号尚未确认，见文末「上真机前置条件」。

### 核查结论：比原记录更严重

原记录只写了「位域编码错」和「读分组错」。逐条核对 AmbiqSuite SDK 后发现
**旧实现的整张寄存器偏移表都是错的**，GPIO 读写从未落到数据寄存器：

| 旧宏 | 偏移 | 该偏移的真实寄存器 |
|---|---|---|
| `AM_REG_GPIO_CFG_A` | 0x010 | PADREGE（pads 16-19 配置） |
| `AM_REG_GPIO_WTS_A` | 0x030 | PADREGM（pads 48-49 配置） |
| `AM_REG_GPIO_WTC_A` | 0x040 | CFGA（pads 0-7 输出类型） |
| `AM_REG_GPIO_WTT_A` | 0x050 | CFGE（pads 32-39 输出类型）；**Apollo3 无 WTT 寄存器** |
| `AM_REG_GPIO_RD_A` | 0x060 | **PADKEY**（配置锁密钥寄存器） |

即旧 `read_pin()` 读的是 PADKEY 的锁存值而非引脚电平，`set_pin()` 写的是
pad 配置字。此外 PADKEY 从未解锁，`PADnFNCSEL` 从未设为 GPIO(0x3)，
`PADnINPEN` 从未使能（按手册此时读回恒 0）。

- **F2（位域编码错误）**：旧 `init_pin` 把 pull 编码成 `(1u<<2)`/`(2u<<2)`
  写进 4bit 字段。该字段是 CFG：`bit0=INCFG, bit[2:1]=OUTCFG, bit3=INTD`，
  所以 PullUp 实际把输出配成开漏、PullDown 实际置位中断判别位。上下拉属于
  **PADREG**（每 pad 8bit）：`bit0=PADnPULL, bit1=PADnINPEN,
  bit2=PADnSTRNG, bit[5:3]=PADnFNCSEL(0x3=GPIO), bit[7:6]=PADnRSEL`。
  注：PADnPULL 只使能**上拉**，PADnRSEL 仅在 I2C 能力引脚上选阻值；
  **Apollo3 没有通用下拉**，`GpioPull::PullDown` 现退化为无上下拉。
- **F3（读分组错误）**：旧 `group_reg()` 按 `pin/16` 分四组、`bit=pin%16`。
  真实布局只有两组：RDA(0x080, GPIO 0-31) / RDB(0x084, GPIO 32-49)，
  位号 `pin & 0x1F`。SDK 的 `am_hal_gpio_input_read(n)` 用
  `RDA + ((n & 0x20) >> 3)` 选寄存器（0x20>>3 == 4），与此一致。
  WTS/WTCA/WTA 同样按 bit5 分 A/B 两组。

### 权威寄存器表（Apollo3 Blue，base 0x40010000）

| 偏移 | 寄存器 | 分组 |
|---|---|---|
| 0x000–0x030 | PADREGA..M | 4 pads/word，8 bit/pad |
| 0x040–0x058 | CFGA..G | 8 pads/word，4 bit/pad |
| 0x060 | PADKEY | 写 0x73 解锁 / 写 0 上锁 |
| 0x080 / 0x084 | RDA / RDB | 输入 0-31 / 32-49 |
| 0x088 / 0x08C | WTA / WTB | 输出数据 |
| 0x090 / 0x094 | WTSA / WTSB | 写后置位 |
| 0x098 / 0x09C | WTCA / WTCB | 写后清零 |

### 已做的修复

- 新增 `boards/xiaomi/miband8/apollo_gpio_regs.hpp`：把偏移表、分组与位域
  编码抽成 header-only `constexpr` 纯函数，供 HAL 与 host 测试共用。
- 重写 `Apollo3GpioHal`：init_pin 走 PADKEY 解锁 → 改 PADREG（FNCSEL=GPIO、
  输入使能、上拉）→ 改 CFG（输出推挽 / INCFG / INTD 归零）→ PADKEY 上锁；
  read/set 改用 RDA/RDB、WTSA/WTSB、WTCA/WTCB 的 bit5 分组；toggle 改为对
  WTA/WTB 做 `^=`（Apollo3 无 WTT，SDK 的 toggle 宏同理，**ISR 与任务并发
  翻转需调用方加临界区**）。
- 新增 `tests/unit/test_miband8_gpio_regs.cpp`：13 项回归测试覆盖偏移表、
  分组、位号、位域编码、PADKEY 序列与影子寄存器不串脚。
- `board.h` 的 `PIN_BUTTON_SIDE` 去掉「必须落在 0-15」限制。

### 核查来源

- AmbiqSuite SDK `utils/am_util_regdump.c`（g_sRegdumpGPIO[] 偏移表）
  https://gitee.com/cocos_yang/hal_ambiq/blob/main/utils/am_util_regdump.c
- AmbiqSuite `mcu/apollo3/hal/am_hal_gpio.h`（input_read / output_set 宏）
  https://gitee.com/Darber-Wear-Middle/Ambiq2ApolloSuiteSDK/blob/master/mcu/apollo3/hal/am_hal_gpio.h
- Apollo3 Blue MCU Datasheet v0.10.0 §11.7（Table 621/623/649/662–679）
  https://cdn.sparkfun.com/assets/c/1/b/7/6/Apollo3_Blue_MCU_Data_Sheet_v0_10_0.pdf
- Apollo3 Porting Guide（am_hal_gpio_pincfg_t 位域映射注释）
  https://contentportal.ambiq.com/documents/20123/388380/Apollo3-Porting-UsersGuide.pdf
- Apollo3 SVD 寄存器字段（PADREGA / CFGA）
  https://www.chipselect.org/peripheral_id.php?id=94871
- Zephyr 邮件列表（RDA 前 32 脚 / RDB 后 18 脚）
  https://lists.zephyrproject.org/g/devel/topic/82365367

### 上真机前置条件（仍是阻塞项）

1. **确认主控型号**。`board.h` 全篇按 Apollo3 Blue 描述（96 MHz、1 MB Flash、
   384 KB SRAM、50 GPIO），但 Mi Band 8 量产机疑为 **Apollo4 Blue Lite
   (AMA4BL)**（192 MHz、2 MB MRAM、1.4 MB SRAM）。两者 GPIO 基址/偏移/位域
   是否兼容**未核实**。型号确认前，本实现只能保证「符合 Apollo3 手册」，
   不能保证真机可用；须按 Apollo4 手册复核后方可改动寄存器常量。
2. `GpioPull::PullDown` 与 `GpioMode::Alternate/Analog` 未实现，接口层无错误
   返回值，调用方需自行知悉退化行为。
3. 尚未真机验证：上拉是否生效、按键读数、输出电平、toggle 并发均需实板确认。
