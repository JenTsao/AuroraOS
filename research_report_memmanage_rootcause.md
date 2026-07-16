# auroraOS 启动期 MemManage 故障根因分析

## Executive Summary

系统一启动就在 `[Security] MPU Memory Protection Unit Activated.` 之后立即触发 `MemManage_Handler`，打印 `Memory Protection Violation Detected! / Access Denied!` 并永久卡死。经 QEMU 单步/异常跟踪与 ELF 反汇编定位，根因是：**第 3 个被创建的任务 `pi_test_low` 在调用 `Scheduler::create_task` 时，传入的栈指针 `stack_space` 为 `0`（NULL）**。于是 `Arch::init_thread_stack` 把伪造的异常返回栈帧写到了 `NULL + 512 = 0x200` 这个地址——而 `0x200` 落在 MPU 区域 0（基地址 `0x0`、256KB、只读 Flash）内，于是产生 DACCVIOL 写违规。该故障发生在 `Scheduler::start()` 之前，导致 `MemManage_Handler` 无法重新调度，违规指令被无限次重放，表现为“启动即崩溃”。

## 证据链

### 1. QEMU 异常跟踪（`-d int`）

```
Loaded reset SP 0x20010000 PC 0x41 from vector table
Taking exception 4 [Data Abort] on CPU 0
...at fault address 0x1f8
...with CFSR.DACCVIOL and MMFAR 0x1f8
...taking pending nonsecure exception 4
...loading from element 4 of non-secure vector table at 0x10
...loaded new PC 0x1f5
```

- 异常 4 = MemManage；`CFSR.DACCVIOL` 置位 → 这是一次**数据写**违规。
- `MMFAR = 0x1f8` → 违规访问的目标地址是 `0x1f8`。
- `0x1f8` 属于代码区 `.text`（`0x40`–`0x37e8c`）。MPU 区域 0 把整个 `0x0`–`0x3FFFF` 配置为 `AP_ALL_RO`（只读、可执行），因此对该地址的**任何写操作**都会触发 DACCVIOL。

### 2. ELF 内存布局（`objdump -h`）

```
.isr_vector  00000040  00000000   （向量表在 0x0）
.text        00037e4c  00000040   （代码在 0x40 起，含 0x1f8）
.data        000001f4  20000000   （已初始化数据在 RAM）
.bss                      200001f4 （未初始化数据在 RAM）
```

`0x1f8` 位于 `.text`（Flash）内部，确认为只读区写违规。

### 3. gdb + QEMU（`-s -S`）精确定位故障指令

在 `MemManage_Handler`（0x1f4）下断点，读取异常入栈帧中保存的返回 PC，得到故障指令：

```
#2  0x0002ddfc in Arch::init_thread_stack (..., task_entry=0x35bd <pi_test_low()>)
#3  Scheduler::create_task (this=0x2000de44 <Scheduler::instance()::sched>,
                            task_entry=0x35bd <pi_test_low()>, stack_space=<opt>) at task.hpp:113

0x0002ddfc <...create_task+96>:  str.w  r1, [r2, #-8]    ; ← 故障指令
```

异常帧现场寄存器：

```
R0 = 0x2000df1c   (&tcb，RAM，正确：this+task_count*108，task_count=2)
R1 = 0x000035bd   (task_entry = pi_test_low)
R2 = 0x00000200   (top 指针)
R3 = 0xfffffffd   (EXC_RETURN_PSP)
```

故障指令 `str.w r1, [r2, #-8]` 写入地址 `R2 - 8 = 0x200 - 8 = 0x1f8`（与 MMFAR 完全吻合）。这里 `R2 = 0x200` 是 `init_thread_stack` 计算出的栈顶 `top`，而 `top = stack_space + (stack_size / sizeof(uint32_t))`（指针算术）。已知 `stack_size = 512` 字节，故 `top = stack_space + 512`，代入得 `stack_space = 0x200 - 512 = 0`。**任务 `pi_test_low` 的 `stack_space` 是 NULL。**

### 4. 为何是 `pi_test_low`、为何 `stack_space = 0`

- 故障发生在 `task_count = 2`（第 3 个任务），`&tcb = 0x2000de44 + 2*108 = 0x2000df1c`（RAM，正确），`task_entry = 0x35bd` 即 `pi_test_low`。
- 在当前源码 `apps/kernel.cpp` 中，`pi_test_low` 的创建语句为：

  ```cpp
  static uint32_t pi_low_stack[STACK_SIZE_TEST];
  Scheduler::instance().create_task(pi_test_low, pi_low_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Low);
  ```

  即 `stack_space` 应为 `pi_low_stack`（一个位于 `.bss` 的合法 RAM 地址，应为 `0x2000xxxx`）。
- 但反汇编显示，对应 `pi_test_low` 的那次 `create_task` 调用（二进制中第一个 `bl 2dd9c`，`0x3dfc`）**没有为 `r2`（`stack_space`）加载任何栈数组地址**，而是沿用了之前的寄存器值（最终为 0）。也就是说，当前正在运行的 `build_qemu/auroraOS.elf` 这个**预编译二进制与当前源码不一致（属于旧构建 / 不同变体）**：在它的源码里 `pi_test_low` 传入的是 `0` 或某个在运行时返回 `NULL` 的堆分配栈，而非当前的静态 `.bss` 数组。

### 5. 为何会“永久卡死”而非“只杀掉一个线程”

`MemManage_Handler`（已含诊断打印 CFSR/MMFAR）的逻辑是：

```cpp
TaskControlBlock* current = Scheduler::instance().get_current_tcb(); // 返回 nullptr
if (current) { ... }            // 跳过
Scheduler::instance().schedule(); // started_==false → 直接 return（空操作）
return;                         // 异常返回，重放违规指令
```

因为故障发生在 `kernel_main` 初始化阶段、**早于 `Scheduler::start()` 把 `started_` 置 true**，所以 `get_current_tcb()` 返回 `nullptr`、`schedule()` 为空操作。异常返回后 CPU 重新执行那条违规指令，再次 MemManage，形成死循环——这就是“黑屏→加 `-nographic` 后只有 MPU 激活+违规一行就不动”的真正原因。

> 补充说明：源码里 `hacker_app_task` 会在启动 3 秒后故意降到用户态并写 `tick_count` 来触发一次 MPU 违规（这是设计内的安全演示，handler 会终结该线程）。但本次启动期故障与 `hacker_app_task` 无关，它发生在更早的 `create_task` 阶段。

## 修复建议

1. **用当前源码重新构建**：当前 `apps/kernel.cpp` 中所有任务栈（`idle_stack`、`shell_stack`、`pi_low/mid/high_stack`、`rx/tx_stack` 等）均为 `static uint32_t X[...]`（落在 `.bss`/RAM），`create_task` 传入的是合法 RAM 指针，应当不会再出现 NULL 栈。请基于当前源码重新编译 `auroraOS.elf` 后运行。
2. **先排除两处既存编译错误**：当前源码存在两处会阻断构建的问题（`ui/ui_manager.hpp` 的 `renderer_` 未声明、`kernel/symbol_export.cpp` 的 `luaL_openlibs` 未声明），需先解决才能成功重建（前者多为头文件包含/顺序问题，后者需包含 `lualib.h` 或加保护宏）。
3. **增加防御性断言（可选但推荐）**：在 `create_task` / `Arch::init_thread_stack` 入口加 `KERNEL_ASSERT(stack_space != nullptr && 落在 RAM 区间)`，避免以后再出现“把栈帧写到 Flash”这类静默致命错误。
4. **让 `MemManage_Handler` 在 `started_` 为假时也能停机而非死循环**：当前 handler 在调度器未启动时无法自救，建议在该分支直接 `KERNEL_ASSERT`/停机，便于早期故障暴露而不是无限重启式卡死。

## 临时改动提示

为完成本次定位，我在仓库中做了两处临时改动，请按需处理：
- `boot/interrupts.cpp`：`MemManage_Handler` 中新增了打印 `CFSR`/`MMFAR` 的十六进制诊断代码（正是这段打印帮助确认了 `0x1f8` 与 DACCVIOL）。
- `CMakeLists.txt`：为单独构建 `auroraOS.elf` 临时禁用了 bootloader 目标及其 `POST_BUILD` 步骤。

确认根因后，建议还原这两处临时改动（诊断代码可保留作为调试辅助，bootloader 目标须恢复以保证完整镜像构建）。
