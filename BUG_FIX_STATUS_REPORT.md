# AuroraOS BUG 修复状态检查报告

**检查时间**: 2026-07-31
**原始审计时间**: 2026-07-21
**检查范围**: BUG_REPORT.md 中全部 12 个确认 BUG + 10 个潜在风险点

---

## 一、已修复的 BUG (共 5 个)

### BUG-001: `#include <atomic>` 不兼容裸机 Cortex-M4 — 已修复
- **文件**: `kernel/frame_scheduler_v2.hpp`
- **修复方式**: 移除了 `<atomic>` 头文件，改用 `volatile` 变量 + `disable_interrupts()`/`enable_interrupts()` 关中断保护方案。注释明确说明"单核裸机：volatile 足够保证可见性，所有写入均在关中断保护下进行"。

### BUG-002: `#include <new>` 不兼容裸机环境 — 已修复
- **文件**: `kernel/memory_pool.hpp`
- **修复方式**: 使用条件编译 `__has_include(<new>)` 检测宿主环境是否提供 `<new>`。在裸机环境下，自定义了 placement new 和 placement new[] 运算符。

### BUG-003: ELF Loader AArch64 MMU 头文件架构污染 — 已修复
- **文件**: `apps/elf_loader.cpp`
- **修复方式**: `#include "../arch/arm/cortex-a/mmu/mmu_manager.hpp"` 已用 `#ifdef ARCH_AARCH64` 包裹，AArch64 专属代码路径也被正确的条件编译保护。

### BUG-008: Mutex `lock()` 中断状态恢复不一致 — 已修复
- **文件**: `kernel/mutex.hpp`
- **修复方式**: 重构了 `lock()` 函数，将 `IrqGuard` 的作用域限制在每次循环迭代的临界区内，在调用 `schedule()` 之前析构 guard，让中断恢复到进入临界区前的状态。`schedule()` 自身也有独立的 `IrqGuard` 保护。注释说明"不再有无条件的 enable/disable ping-pong"。

### POTENTIAL-007: Syscall Validator NULL+0 指针处理 — 已修复
- **文件**: `boot/interrupts.cpp`
- **修复方式**: `validate_user_ptr()` 函数明确检查 `!ptr` 并返回 false，NULL 指针（包括 NULL+0 长度）被正确拒绝。

---

## 二、部分修复的 BUG (共 1 个)

### BUG-009: VFS `read()`/`write()` TOCTOU 竞态 — 部分修复
- **文件**: `vfs/vfs.cpp`
- **修复方式**: 添加了 ref_count 引用计数机制。`close()` 会等待 `ref_count == 0` 才真正关闭文件。offset 更新前会验证 `fd_table_[fd].priv == priv`。
- **仍存在的问题**: vnode 对象本身在 I/O 期间没有被引用计数保护。如果底层设备被卸载，vnode 可能变成悬空指针。不过如果 vnode 在 mount 期间一直存在，则此修复可以认为是充分的。

---

## 三、未修复的 BUG (共 16 个)

### BUG-004: 调度器 `uint32_t → int8_t` 隐式截断 — 未修复
- **文件**: `kernel/task.hpp`
- **现状**: `next_ready` 和 `prev_ready` 仍然是 `int8_t` 类型，`push_ready()` 和 `remove_ready()` 中的隐式截断依然存在。唯一的新增措施是 `static_assert(MAX_TASKS <= 127)` 编译期检查，但无运行时边界保护。

### BUG-005: `ready_bitmask` 运算符优先级 BUG — 未修复
- **文件**: `kernel/task.hpp` 第256行
- **现状**: 代码仍为 `ready_bitmask &= ~(1 << prio)`，未采用建议的 `static_cast<uint8_t>(~(1u << prio))` 修复方式。虽然当前优先级范围(0-4)碰巧安全，但这仍然是未定义行为。

### BUG-006: 信号分发中屏蔽信号重新入队逻辑 — 未修复
- **文件**: `kernel/task.hpp` `dispatch_signals()`
- **现状**: 虽然循环现在受 `initial_count` 限制不会在同一次调用中无限循环，但核心问题仍未解决：跨调度的重复处理、队列满时的静默丢弃、被屏蔽信号的无效轮转等。

### BUG-007: SysTick_Handler 5ms 调度周期问题 — 未修复
- **文件**: `boot/interrupts.cpp` 第606-608行
- **现状**: 代码仍为 `if (tick_count % 5 == 0) { sched.schedule(); }`，5ms 调度触发逻辑未改变，帧边界切换仍有最多 5ms 的延迟不确定性。

### BUG-010: ELF 加载器内存管理 BUG — 未修复
- **文件**: `apps/elf_loader.cpp`
- **现状**: (1) `segment_memory` 在 Cortex-M 成功路径中仍然泄漏（未释放）；(2) `app_stack`（通过 PageAllocator::alloc_page() 分配）在 Cortex-M 失败路径中仍然用 `delete[]` 错误释放，应使用 `PageAllocator::free_page()`。

### BUG-011: Shell `reboot` 命令写 AIRCR 后无死循环 — 未修复
- **文件**: `apps/shell.cpp` 第288-293行
- **现状**: `*aircr = (0x05FA0000 | (1 << 2))` 写入后，代码直接继续执行下一个 else-if 分支，没有添加 `while(true) {}` 或 `__asm volatile("dsb; b .;")` 死循环。

### BUG-012: 分布式软总线硬编码 challenge — 未修复
- **文件**: `net/distributed_bus.hpp` 第267行
- **现状**: `const char* challenge = "aurorawatch"` 仍是硬编码字符串，被同时用作 HMAC challenge 和 JSON 中的 device_id。所有设备共享同一 device_id，设备识别完全失效。

### POTENTIAL-001: `KernelHeap::allocate()` 返回值未检查 — 未修复
- **文件**: `apps/elf_loader.cpp`
- **现状**: `new Elf32_Shdr[...]`、符号表、`new Elf32_Rel[...]` 的返回值仍未检查 nullptr，OOM 时会导致 HardFault。

### POTENTIAL-002: Semaphore `wait()` 竞态条件 — 未修复
- **文件**: `kernel/semaphore.hpp`
- **现状**: `enable_interrupts()` 和 `schedule()` 之间仍存在窗口期，ISR 可能在此间隙触发更高优先级任务导致饥饿。

### POTENTIAL-004: VFS `mount()` 路径遍历检查不完整 — 未修复
- **文件**: `vfs/vfs.cpp`
- **现状**: `mount()` 中仍是简陋的 `..` 字符检测，未像 `open()` 那样修复为更完善的路径遍历检查。

### POTENTIAL-005: `Mutex::get_highest_waiter()` 硬编码 32 位掩码 — 未修复
- **文件**: `kernel/mutex.hpp`
- **现状**: `get_highest_waiter()`、`wake_highest_waiter()`、`Semaphore::signal()` 等函数中仍然硬编码 `i < 32` 循环，未使用 `MAX_TASKS`。

### POTENTIAL-006: ELF 文件完整性校验缺失 — 未修复
- **文件**: `apps/elf_loader.cpp`
- **现状**: `e_phoff`、`e_shoff`、`p_offset` 等字段没有针对文件大小的边界检查，恶意 ELF 文件可导致越界读取。

### POTENTIAL-008: DistributedSoftBus `init()` fd 泄漏 — 未修复
- **文件**: `net/distributed_bus.hpp`
- **现状**: `init()` 函数在创建新 socket 前不关闭旧的 `udp_socket_`，多次调用导致文件描述符泄漏。

### POTENTIAL-009: Shell 命令缓冲区溢出无警告 — 未修复
- **文件**: `apps/shell.cpp`
- **现状**: 超长命令（>127字符）被静默截断，未向用户发出任何警告提示。

### POTENTIAL-010: FrameScheduler 双版本共存 — 未修复
- **文件**: `kernel/frame_scheduler.hpp` 和 `kernel/frame_scheduler_v2.hpp`
- **现状**: 两个帧调度器类同时存在，`boot/interrupts.cpp` 同时包含两个头文件。运行时只使用 V2，但旧版未移除或标记为 deprecated。

---

## 四、无需修复的潜在问题 (共 1 个)

### POTENTIAL-003: `IrqGuard` PendSV 嵌套问题 — 无需修复
- **现状**: `IrqGuard` 使用 save/restore 模式，在构造函数中保存 PRIMASK 状态，析构时恢复，天然支持嵌套上下文。在 PendSV 处理器中读取 PRIMASK 的行为是确定的（Cortex-M 架构保证），不存在报告所述的死锁风险。

---

## 五、总结

| 状态 | 数量 | 详情 |
|------|------|------|
| 已修复 | 5 | BUG-001, BUG-002, BUG-003, BUG-008, POTENTIAL-007 |
| 部分修复 | 1 | BUG-009 |
| 未修复 | 16 | BUG-004, BUG-005, BUG-006, BUG-007, BUG-010, BUG-011, BUG-012, POTENTIAL-001, POTENTIAL-002, POTENTIAL-004, POTENTIAL-005, POTENTIAL-006, POTENTIAL-008, POTENTIAL-009, POTENTIAL-010 |

**修复率**: 5/22 ≈ 22.7%

最高优先级仍未修复的项目：BUG-010（ELF 加载器内存泄漏+错误释放）、BUG-012（分布式设备识别失效）、BUG-004（调度器类型截断）、BUG-009（VFS TOCTOU 竞态，仅部分修复）。
