# AuroraOS 内核 BUG 审计与修复报告

> 日期：2026-10-02 ｜ 执行：C++ 超级专家团（程执衡 主理；柯溯源/纪序明/楼稳基 三路专项审计）
> 方式：三路并行静态审计（UB/内存安全、并发/中断安全、能力安全/不变量）+ 主理人交叉裁决 + 统一修复 + 全量回归

## 一、验证结果（先看结论）

| 验证项 | 修复前基线 | 修复后 | 结论 |
|---|---|---|---|
| 宿主机测试（GoogleTest） | 571/571 通过 | **571/571 通过** | ✅ 零回归 |
| 固件构建（lm3s6965-qb, ARM GCC） | 通过 | **通过** | ✅ |
| 测试构建（含新增 mutex.cpp） | — | **通过** | ✅ |
| QEMU 启动冒烟 | — | 未执行（本机 Start-Process 环境异常） | ⚠️ 环境受限 |

变更规模：23 个文件修改 + 新增 `kernel/core/mutex.cpp`，+435/-105 行。

## 二、已修复缺陷（按严重度）

### P0（2 项）

| ID | 缺陷 | 修复 |
|---|---|---|
| DS-01 / CC-01 | `send_signal`/`SYS_KILL` 唤醒阻塞在 Endpoint 上的任务时绕过 `cancel_waiter`：任务带着 Sending/Receiving 状态留在端点等待队列里继续运行；重入队造成队列**自环**，`cancel_all` 死循环挂死内核；幽灵队列条目还会被投递消息/reply 唤醒 | 统一经 `cancel_waiter(Interrupted)`（新增 `IpcStatus::Interrupted = -6`）摘队列+复位状态机；信号保持 pending 由 `dispatch_signals` 处理。`handle_kill` 收敛到 `send_signal` 消除重复 |
| AR-01 | `SYS_SIGACTION` 不校验 `sa_handler` 指向：User 任务可注册任意函数指针，`dispatch_signals` 在 SVC/Handler **特权上下文**同步执行 → 任意内核代码执行/提权原语 | User 特权任务注册的 handler 必须落在该任务自身可访问内存（`validate_user_ptr`），Kernel 特权任务豁免 |

### P1（6 项）

| ID | 缺陷 | 修复 |
|---|---|---|
| DS-02 / CC-02 | 任务带互斥锁被杀：锁永不释放（其他等待者永久死锁）；TCB 槽位复用后 `owner_` 悬垂，新任务可"免费"获得未持有的锁 | 新增 `Scheduler::terminate_task()` 统一终止入口；`kernel/core/mutex.cpp`（新文件）实现 `kernel_release_task_mutexes`：强制 `force_unlock` 全部持锁 + `abandon_waiter` 摘除等待位 |
| DS-03 / AR-05 | `free_task` 是不可达死代码（TCB 初始引用计数 1 从不释放）：Terminated 任务永占槽位、能力引用的内核对象被永久 pin 住，16 槽耗尽后 `create_task` 永久失败 | 终止路径统一走 `terminate_task`（立即资源释放，保留 Terminated 可观察）；`create_task` 惰性回收 Terminated 槽位（复用前补跑 `free_task`） |
| AR-04 | 接收方消亡时不唤醒 ReplyBlocked 且 `receiver_id` 指向它的发送方 → 无限超时的客户端永久冻结 | `terminate_task`/`free_task` 扫描并 `cancel_waiter(ReceiverDead)` 全部等它 reply 的发送方 |
| AR-02 | `SYS_CAP_GRANT` 无目标授权检查：任意任务可向**任意任务**的 cspace 任意槽位注入能力（覆盖/销毁既有能力、伪造 badge 破坏来源认证） | dispatcher 层：跨任务授予须持有目标 Thread+Write 能力（与 SYS_KILL 对齐），Kernel 特权豁免 |
| AR-03 | `SYS_DEV_IOCTL` 的 `desc->arg` 原样透传驱动，未校验 → 用户可借驱动写任意内核内存 | User 特权任务的 arg 必须指向自身可访问内存；Kernel 特权/空 arg 不受限 |
| CC-03 / DS-10 | `Semaphore::signal()`/`TaskNotify::give()` 从 **ISR** 里执行完整 `schedule()`（把被打断任务误当当前任务做轮转）；多处无条件 `enable_interrupts()` 破坏外层临界区 | `signal(in_isr)` 仅置 Ready 不调度（SysTick 尾部统一调度）；work_queue/power_manager/frame_scheduler 改 `irq_save/irq_restore` 或 RAII；VSync/SysTick 路径 `give(yield=false)` |

### P2（9 项）

| ID | 缺陷 | 修复 |
|---|---|---|
| DS-04 / CC-08 | CSpace 全部修改类操作无中断保护，`cap_revoke` 全表扫描可被 SysTick 打断产生位图/槽位不一致 | `cap_insert/delete/lookup/derive_internal/grant/revoke` 入口加 `IrqGuard` |
| DS-05 / AR-08 | `cap_derive/mint` 的 `src_slot == dst_slot`：先删后拷产生"已占用但 Null"幻影槽位 | 派生入口拒绝；`cap_grant` 仅在**同任务**内拒绝（跨任务两 cspace 数值相等合法——回归测试抓出过初版过严问题） |
| CC-06 | `Mutex::get_highest_waiter` 把已离开等待的任务计入 PIP 恢复值（wait_mask 残留），原 owner 带着不属于自己的高优先级运行 | 仅统计 `waiting_on_mutex == this` 且处于 Sleeping/Suspended 的真实等待者 |
| CC-07 | IPC PIP 与 Mutex PIP 双通道互相覆盖：IPC 应答后 `recalculate_receiver_priority` 把持有天花板锁的接收方优先级打回 base | 新增 `Mutex::held_priority_contribution()`，IPC 恢复时并入锁 PIP 贡献取 max |
| DS-06 | `open()` 把用户 path 指针直接交审计钩子读 15 字节 → 内核态解引用任意地址/泄漏内核内存 | User 特权任务的 path 先过 `validate_user_ptr`，失败传 nullptr 给审计层 |
| DS-07 | `ipc_receive<T>` 栈缓冲 1 字节对齐却按 `IpcMessage<T>*` 访问（对齐 UB，M0+ HardFault） | `alignas(IpcMessage<T>)` |
| DS-08 / AR-10 | 信号位图 32 位但只派发 1..15：16..31 置位后静默丢失 | `send_signal` 收紧为 1..15（与 `handle_kill`、`sig_actions[16]` 对齐） |
| AR-06 | 非阻塞 `nb_call` 成功后置 Ready，`Endpoint::reply` 永远返回 Invalid，reply_buf 形同虚设 | 新增 `IpcState::AwaitReply`：nb 投递后可被 reply 按 `receiver_id` 匹配取回应答 |
| DS-09 | `PageAllocator::free_page` 无任何校验：越界/未对齐/重复释放 → 空闲链自环、同页重复发出 | 补齐边界+对齐+双释放三重防护（步数受 `free_pages_` 约束） |

### P3（6 项）

| ID | 缺陷 | 修复 |
|---|---|---|
| AR-07 | `handle_kill`/`handle_cap_grant` 接受 Terminated/Unallocated 目标 | 增加状态校验拒绝 |
| DS-11 | `ota.cpp` 栈缓冲 `reinterpret_cast<uint32_t*>` 类型双关 UB | `alignas(4)` + `memcpy` |
| AR-待验证 | `part_size < sizeof(FirmwareHeader)` 时比较下溢放行超包写入 | 显式相加方向比较 |
| DS-12 | `set_fps(fps>1000)` → 帧周期 0 → `on_tick` 除零 HardFault | 周期下限夹取 1 tick |
| DS-13 | `signal.hpp raise()` 未判空调指针 | 空检查返回 -1 |
| AR-13 / CC-11 | 心跳表满静默丢弃；IPC 超时换算截断；`start_timer(period=0)` 致守护线程死循环 | 表满 UART 告警+计数器；超时饱和换算；period=0 拒绝 |
| 其他 | `schedule()` 兜底扫描 `tasks[candidate]` 越界防御；`push_ready` prev_ready 失效防御；validator 死代码清理；ISR 唤醒路径绕过端点（task_notify） | 已一并修复 |

## 三、记录在案、暂不修复（防债务误判）

| 项 | 说明 |
|---|---|
| AR-09 | `Capability.type` 与 `KernelObject::get_type()` 双类型系统无交叉校验——当前插入点全在内核内部不可利用，建议后续统一 `cap_insert` 类型校验 |
| AR-12 | `memory.hpp → task.hpp` 层环；IpcContext/SecurityContext 轻度 God Object——属架构重构，超出本次修复边界 |
| DS-14 | `ProcessTimer` Absolute 模式语义与绝对到期解读存在歧义（现行为可解释为"过期即触发"，测试锁定现状） |
| DS-15 | `_sbrk`（newlib）与 KernelHeap 同用 `_heap_start/_heap_end`——需链接配置确认二者互斥后处置 |
| CC-09 | `is_task_allowed` 读 volatile 字段无锁——单核单写者下安全，留档备忘 |
| 架构建议 | 信号 handler 不应在特权上下文同步执行，长期应改为异常返回前 trampoline 以任务特权级投递 |

## 四、修复触达文件

```text
CMakeLists.txt / tests/CMakeLists.txt      新增 kernel/core/mutex.cpp 构建项
kernel/task/task.hpp                       send_signal/terminate_task/free_task/create_task/schedule 防御
kernel/core/mutex.cpp                      【新增】终止安全清理（强释放持锁+摘等待位）
kernel/core/mutex.hpp                      等待者状态过滤/held_priority_contribution/abandon_waiter
kernel/core/ipc.hpp / ipc.cpp              Interrupted + AwaitReply 状态机 / PIP 双通道合流 / alignas
kernel/core/syscall_dispatcher.cpp         SIGACTION 提权修复 / CAP_GRANT 授权 / IOCTL arg 校验 / 统一终止
kernel/core/cspace.cpp                     IrqGuard 中断保护 / src==dst 守卫
kernel/core/semaphore.cpp+hpp              signal(in_isr) ISR 安全
kernel/task/task_notify.hpp / work_queue.hpp  端点守卫 / save-restore / ISR 不调度
kernel/interrupt/timer.hpp                 on_tick ISR 信号 / period=0 拒绝
kernel/core/process_timer.cpp              通知唤醒条件补全（Blocked_On_Notify 且非端点阻塞）
kernel/core/security_monitor.hpp           终止走统一入口 / 心跳表满告警
kernel/core/posix.cpp                      审计路径校验
kernel/mm/page_allocator.hpp               释放三重防护
kernel/core/ota.cpp / signal.hpp / syscall_validator.cpp / watchdog_stub.cpp / power_manager.hpp / frame_scheduler_v2.hpp
```

## 五、复现验证命令

```bash
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=OFF
cmake --build tests/build -j
ctest --test-dir tests/build --output-on-failure   # 571/571 PASS
cmake -S . -B build -DBOARD=lm3s6965-qb && cmake --build build -j   # FW PASS
```
