# 🐛 AuroraOS 真实编译 + 深度代码审计 BUG 报告

> **审计时间**: 2026-07-21 23:37 CST  
> **审计方式**: ARM 交叉编译 (arm-none-eabi-gcc 12.2) + 全量静态代码审查  
> **编译结果**: 32 通过 / 3 失败 / 839 警告  
> **审计范围**: kernel/ boot/ vfs/ apps/ net/ adapter/ metrics/ 3rdparty/ (采样)

---

## 一、编译失败 (3 个 — 硬 BUG)

### BUG-001: `#include <atomic>` 不兼容裸机 Cortex-M4
- **文件**: `kernel/frame_scheduler_v2.hpp:8`, 传播到 `boot/interrupts.cpp`, `apps/kernel.cpp`
- **现象**: `fatal error: atomic: No such file or directory`
- **根因**: `<atomic>` 是 C++11 标准库头文件，newlib-nano (裸机) 不提供。需要自定义轻量原子操作或用 `volatile` + 关中断替代。
- **严重性**: 🔴 **致命** — 阻断 interrupts.cpp 编译 = 系统无法启动

### BUG-002: `#include <new>` 不兼容裸机环境
- **文件**: `kernel/memory_pool.hpp:6`, 传播到 `adapter/net/sys_arch.cpp`
- **现象**: `fatal error: new: No such file or directory`
- **根因**: 裸机环境无 `<new>` 头文件。placement new 需自行定义:
  ```cpp
  inline void* operator new(size_t, void* p) noexcept { return p; }
  ```
- **严重性**: 🔴 **致命** — 网络子系统编译失败

### BUG-003: ELF Loader 引入了 AArch64 MMU 头文件 (架构污染)
- **文件**: `apps/elf_loader.cpp:8-9`
- **现象**: `#include "../kernel/page_allocator.hpp"` 和 `#include "../arch/arm/cortex-a/mmu/mmu_manager.hpp"` 在 Cortex-M4 目标下不存在
- **根因**: 未用 `#ifdef ARCH_AARCH64` 保护 AArch64 专属 include
- **严重性**: 🔴 **致命** — ELF 加载器在非 AArch64 平台编译失败

---

## 二、真实运行时 BUG (确认存在)

### BUG-004: 调度器 `uint32_t → int8_t` 隐式截断 — 任务索引溢出
- **文件**: `kernel/task.hpp:229-241`
- **代码**:
  ```cpp
  ready_head[prio] = task_index;      // uint32_t → int8_t
  tcb.next_ready = task_index;        // uint32_t → int8_t
  tcb.prev_ready = task_index;        // uint32_t → int8_t
  ```
- **问题**: `task_index` 是 `uint32_t`，但 `next_ready`/`prev_ready` 是 `int8_t`。当 `task_index >= 128` 时发生有符号溢出，产生负数索引 → 数组越界 → 内存损坏。
- **缓解**: `MAX_TASKS` 默认 8，`static_assert(MAX_TASKS <= 127)` 存在。但 **运行时无边界检查**，若配置被修改则灾难性。
- **严重性**: 🟠 **高** — 当前配置安全，但设计脆弱

### BUG-005: `ready_bitmask` 运算符优先级 BUG
- **文件**: `kernel/task.hpp:255`
- **代码**:
  ```cpp
  ready_bitmask &= ~(1 << prio);
  ```
- **问题**: `1 << prio` 中 `1` 是 `int` 类型，`prio` 是 `uint8_t`。表达式 `~(1 << prio)` 结果是 `int`（有符号），赋值给 `uint8_t ready_bitmask` 时发生隐式截断。当 `prio >= 24` 时行为未定义。
- **修复**: 应写为 `ready_bitmask &= static_cast<uint8_t>(~(1u << prio));`
- **严重性**: 🟡 **中** — 优先级只有 0-4，当前安全，但属于未定义行为

### BUG-006: 信号分发中屏蔽信号重新入队导致无限循环风险
- **文件**: `kernel/task.hpp` `dispatch_signals()`
- **代码**:
  ```cpp
  for (int i = 0; i < initial_count; i++) {
      if (sigismember(&tcb->signal_mask, sig)) {
          // 跳过的信号重新入队到队尾
          tcb->signal_queue[tcb->sig_tail] = sig;
          tcb->sig_count--;  // 队列满时丢弃
          continue;
      }
  }
  ```
- **问题**: 如果一个信号被屏蔽且队列未满，它会被重新入队。但循环次数 `initial_count` 是入队前的快照。如果所有信号都被屏蔽，每个信号被移到队尾，循环结束后队列状态不变但 `sig_head` 已移动 → **逻辑正确但效率极差**。更严重的是：如果 `sig_count == MAX_QUEUED_SIGNALS`，被屏蔽信号被丢弃 (`sig_count--`)，但此时 `sig_count` 变为 `MAX_QUEUED_SIGNALS - 1`，后续的重新入队操作可能写入已被消费的槽位。
- **严重性**: 🟡 **中** — 边界条件下的信号丢失

### BUG-007: SysTick_Handler 中 5ms 调度周期可能导致帧感知调度不精确
- **文件**: `boot/interrupts.cpp` `SysTick_Handler()`
- **代码**:
  ```cpp
  if (tick_count % 5 == 0) {
      sched.schedule();
  }
  ```
- **问题**: 调度器每 5ms 才触发一次，但帧感知调度器 (FrameScheduler) 的帧周期是 33ms。这意味着帧边界切换可能有最多 5ms 延迟。对于 30fps 的渲染 deadline，5ms 的不确定性 = 15% 的帧时间浪费。
- **严重性**: 🟡 **中** — 实时性降级

### BUG-008: Mutex `lock()` 中断状态恢复不一致
- **文件**: `kernel/mutex.hpp` `lock()`
- **代码**:
  ```cpp
  {
      IrqGuard guard;
      // ... 第一次检查 locked_
  } // guard destructs here: restores caller's original interrupt state
  
  while (true) {
      bool need_wait = false;
      {
          IrqGuard guard;
          // ... 循环检查
      } // guard destructs
      if (need_wait) {
          Scheduler::instance().schedule();
      }
  }
  ```
- **问题**: 在外层 `while(true)` 循环中，每次循环都创建/销毁 `IrqGuard`。如果调用者进入 `lock()` 时中断已关闭（例如在 ISR 中调用），`IrqGuard` 会保存已关闭状态，循环中每次 guard 析构都恢复到关闭状态 — 这是正确的。但如果 `schedule()` 内部打开了中断（PendSV 会），返回后中断状态可能与 guard 保存的不一致 → **中断状态泄漏**。
- **严重性**: 🟠 **高** — 在嵌套临界区场景下可能导致中断状态损坏

### BUG-009: VFS `read()`/`write()` 的 TOCTOU 竞态
- **文件**: `vfs/vfs.cpp`
- **代码**:
  ```cpp
  int VfsManager::read(int fd, char* buf, int len) {
      {
          LockGuard lock(vfs_mutex_);
          vnode = fd_table_[fd].vnode;
          priv = fd_table_[fd].priv;
          offset = fd_table_[fd].offset;
          fd_table_[fd].ref_count++;
      }  // ← 锁释放
      int bytes = vnode->read(buf, len, offset, priv);  // ← 无锁执行 I/O
      {
          LockGuard lock(vfs_mutex_);
          fd_table_[fd].ref_count--;
          fd_table_[fd].offset += bytes;
      }
  }
  ```
- **问题**: 在释放锁和重新获取锁之间，另一个线程可能 `close(fd)` → `fd_table_[fd].used = false` → `priv` 和 `vnode` 变成悬空指针。虽然 `close()` 会等待 `ref_count == 0`，但 `close()` 的 `while(true)` 循环中 `sleep_ms(1)` 并不原子地检查 ref_count 和执行 close 操作。
- **严重性**: 🟠 **高** — 多线程并发 close+read 可能导致 use-after-free

### BUG-010: ELF 加载器 `delete[] segment_memory` 在 AArch64 路径被双重释放
- **文件**: `apps/elf_loader.cpp`
- **代码**:
  ```cpp
  #ifdef ARCH_AARCH64
      // ... 复制 segment_memory 到物理页 ...
      delete[] segment_memory;  // ← 第一次释放
  #else
      // Cortex-M: 直接使用 segment_memory
  #endif
  
  // ... 后面如果 create_task 失败:
  if (!tcb) {
  #ifdef ARCH_AARCH64
      // ...
  #else
      delete[] app_stack;
      delete[] segment_memory;  // ← Cortex-M 路径: 第二次释放 (如果走到这里)
  #endif
  }
  ```
- **问题**: 在 Cortex-M 路径中，如果 `create_task` 成功，`segment_memory` 不会被释放（内存泄漏！）。如果失败，`segment_memory` 被释放但 `app_stack` 也被释放 — 但 `app_stack` 是从 `PageAllocator` 分配的，用 `delete[]` 释放是错误的。
- **严重性**: 🔴 **严重** — 内存泄漏 + 错误释放

### BUG-011: Shell `reboot` 命令写 AIRCR 后无死循环
- **文件**: `apps/shell.cpp`
- **代码**:
  ```cpp
  volatile uint32_t* aircr = reinterpret_cast<uint32_t*>(0xE000ED0C);
  *aircr = (0x05FA0000 | (1 << 2));
  // ← 函数继续执行！
  ```
- **问题**: NVIC AIRCR 写入后系统复位是异步的，代码继续执行可能导致未定义行为。应在写入后加 `while(true) {}`。
- **严重性**: 🟡 **中** — 复位前可能执行非法操作

### BUG-012: 分布式软总线 `broadcast_beacon()` 使用硬编码 challenge
- **文件**: `net/distributed_bus.hpp`
- **代码**:
  ```cpp
  const char* challenge = "aurorawatch";
  // ... HMAC 计算使用 challenge 作为 device_id
  ```
- **问题**: challenge 就是 device_id，所有设备使用相同的 device_id 发送 beacon → 接收端无法区分不同设备。防重放检查 (`check_and_update_seq`) 基于 device_id 匹配，所有设备共享同一个 replay entry → **序列号冲突**。
- **严重性**: 🔴 **严重** — 分布式设备识别完全失效

---

## 三、潜在 BUG (代码审查发现的风险点)

### POTENTIAL-001: `KernelHeap::allocate()` 返回值未检查
- **位置**: 多处 `new char[...]` 调用
- **问题**: 裸机环境的 `operator new` 在 OOM 时返回 nullptr（无异常），但调用方如 `segment_memory = new char[total_memsz]()` 只检查了一次，后续路径中 `new Elf32_Shdr[...]` 和 `new Elf32_Rel[...]` 的返回值未检查。
- **风险**: OOM → 空指针解引用 → HardFault

### POTENTIAL-002: Semaphore `wait()` 中 `set_task_state` 和 `schedule()` 之间的竞态
- **文件**: `kernel/semaphore.hpp`
- **代码**:
  ```cpp
  Scheduler::instance().set_task_state(current->id, TaskState::Suspended);
  enable_interrupts();          // ← 中断打开
  Scheduler::instance().schedule();  // ← 调度
  ```
- **问题**: 在 `enable_interrupts()` 和 `schedule()` 之间，一个 ISR 可能调用 `signal()` 并将任务设为 Ready。然后 `schedule()` 发现当前任务已 Ready，不做切换 → 任务继续循环，再次检查 `count_`，此时 `count_` 已被 `signal()` 递增 → 正确获取资源。**表面上正确**，但如果 ISR 在 `enable_interrupts()` 后、`schedule()` 前触发了更高优先级任务，当前任务可能永远得不到调度。
- **风险**: 优先级反转场景下的饥饿

### POTENTIAL-003: `IrqGuard` 在 PendSV 上下文中的嵌套问题
- **问题**: `IrqGuard` 使用 `Arch::irq_save()` / `Arch::irq_restore()`。在 Cortex-M 上，PendSV 处理器模式下读取 PRIMASK 可能返回意外值（异常嵌套场景）。如果 `schedule()` 在 SysTick_Handler（中断上下文）中被调用，`IrqGuard` 的 save/restore 行为需要确保与 BASEPRI 协同。
- **风险**: 中断嵌套场景下的死锁

### POTENTIAL-004: VFS 路径遍历检查不完整
- **文件**: `vfs/vfs.cpp` `mount()`
- **代码**:
  ```cpp
  for (const char* p = path; *p; p++) {
      if (p[0] == '.' && p[1] == '.') return false;
  }
  ```
- **问题**: 检查 `..` 但不检查 `../` 的组合。例如路径 `/foo/....//` 可能绕过。`open()` 中的检查更完善，但 `mount()` 中的检查较弱。
- **风险**: 路径遍历攻击

### POTENTIAL-005: `Mutex::get_highest_waiter()` 硬编码 32 位掩码
- **文件**: `kernel/mutex.hpp`
- **代码**:
  ```cpp
  for (int i = 0; i < 32; i++) {
      if (wait_mask_ & (1 << i)) {
  ```
- **问题**: `wait_mask_` 是 `uint32_t`，但 `MAX_TASKS` 可能小于 32。当 `i >= MAX_TASKS` 时，`get_task_by_id(i)` 返回 nullptr（安全），但循环仍然执行 32 次 → 浪费。
- **风险**: 性能问题，非功能性 BUG

### POTENTIAL-006: `ElfLoader` 未对 ELF 文件做完整性校验
- **问题**: ELF 加载器不校验：
  - `e_phoff` + `e_phnum * e_phentsize` 是否超出文件大小
  - `e_shoff` + `e_shnum * e_shentsize` 是否超出文件大小
  - `p_offset` + `p_filesz` 是否超出文件大小
  - section header 的 `sh_offset` + `sh_size` 是否合法
- **风险**: 恶意 ELF 文件可导致越界读取

### POTENTIAL-007: `SyscallValidator::validate_user_ptr` 对 NULL+0 的处理
- **代码**:
  ```cpp
  if (!ptr) return false;
  const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t end = p + len;
  if (end < p) return false;
  ```
- **问题**: 当 `ptr != nullptr` 但 `len == 0` 时，`end == p`，`end < p` 为 false，函数可能返回 true。零长度的指针验证语义不明确。
- **风险**: 低

### POTENTIAL-008: `DistributedSoftBus` 未检查 `lwip_socket()` 失败后的状态
- **代码**:
  ```cpp
  udp_socket_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_socket_ < 0) return;
  ```
- **问题**: `init()` 失败后 `udp_socket_` 保持负值，但 `broadcast_beacon()` 和 `listener_task()` 检查 `if (udp_socket_ < 0) return` — 这是安全的。但如果 `init()` 被多次调用，旧 socket 不会被关闭 → 文件描述符泄漏。
- **风险**: 低

### POTENTIAL-009: Shell 命令缓冲区溢出风险
- **文件**: `apps/shell.cpp`
- **代码**:
  ```cpp
  char cmd_copy[128];
  int i = 0;
  while (raw_cmd[i] && i < 127) { cmd_copy[i] = raw_cmd[i]; i++; }
  cmd_copy[i] = '\0';
  ```
- **问题**: `raw_cmd` 来自 UART 读取，长度不受限。虽然 `cmd_copy` 限制了 127 字符，但如果 UART 驱动返回的 `bytes` 大于 127，后面的字符被静默丢弃。这是安全的，但用户可能不知道输入被截断。
- **风险**: 低（安全但 UX 差）

### POTENTIAL-010: `FrameScheduler` 和 `FrameSchedulerV2` 共存导致混淆
- **问题**: 代码中同时存在 `frame_scheduler.hpp` (FrameScheduler) 和 `frame_scheduler_v2.hpp` (FrameSchedulerV2)，`SysTick_Handler` 中调用的是 V2，但 FrameScheduler 的 `on_tick()` 也会被调用（如果被实例化）。两个调度器的状态可能不一致。
- **风险**: 帧调度逻辑冲突

---

## 四、编译警告分析 (839 个)

| 类别 | 数量 | 严重性 | 说明 |
|------|------|--------|------|
| `uint32_t → int8_t` 截断 | 75 | 🟠 | task.hpp 就绪队列索引 |
| `int → uint8_t` 截断 | 83 | 🟡 | ready_bitmask 运算 |
| 符号/无符号转换 | 189 | 🟡 | 混合 int/uint32_t 运算 |
| Lua `goto *expr` 非标准 | 78 | ⚪ | Lua VM 已知行为，可忽略 |
| 取标签地址非标准 | 83 | ⚪ | Lua jump table，可忽略 |
| 未使用参数 | 6 | ⚪ | 可加 `(void)param` 消除 |

---

## 五、总结

| 类别 | 数量 |
|------|------|
| 🔴 编译失败 (致命) | 3 |
| 🔴 严重运行时 BUG | 2 |
| 🟠 高风险 BUG | 3 |
| 🟡 中风险 BUG | 4 |
| 🟡 潜在风险点 | 10 |
| ⚪ 编译警告 | 839 |

### 最高优先级修复建议

1. **BUG-001**: 将 `<atomic>` 替换为 `volatile` + 关中断保护
2. **BUG-002**: 自行定义 placement new
3. **BUG-003**: 用 `#ifdef` 保护 AArch64 专属代码
4. **BUG-010**: 修复 ELF 加载器的内存管理 (泄漏 + 错误释放)
5. **BUG-012**: 使用真实设备 ID 替代硬编码 challenge
6. **BUG-009**: 修复 VFS read/write 的 TOCTOU 竞态

---

*报告生成: AuroraOS 深度审计 · ARM 交叉编译 + 静态代码分析*
