
# AuroraOS Cycle 2
# July 微内核核心阶段

> 版本：1.0
> 项目：AuroraOS
> 内核：July Kernel
> 阶段：Cycle 2 - Microkernel Core
> 前置：Cycle 1 - Kernel Foundation
> 状态：规划中

---

# 1. 阶段概述

Cycle 2 是 July Kernel 从基础内核走向真正微内核的关键阶段。

Cycle 1 建立了 Boot、HAL、Memory、Task、Scheduler、Interrupt 等基础能力。Cycle 2 在此基础上建立微内核的标志性机制：

- IPC 进程间通信
- Capability 权限系统
- Syscall 系统调用 ABI
- Kernel Object 统一对象模型
- User Space 隔离

本阶段的核心目标：

> 建立 July Kernel 作为微内核的核心安全与通信机制，使 Task 之间能够通过 IPC 进行受控通信，并通过 Capability 进行权限管理。

---

# 2. 阶段目标

Cycle 2 完成后，July Kernel 应具备：

```
Task A                    Task B
   │                         │
   │    Capability           │
   │       ↓                 │
   │     CSpace              │
   │       ↓                 │
   └──→ IPC Endpoint ←───────┘
            │
            ↓
       Kernel Object
            │
            ↓
         Syscall
```

最终实现：

> 多个 Task 通过 Capability 授权的 IPC 进行安全通信的最小微内核。

---

# 3. 时间规划

预计周期：

```
4 ～ 8个月
```

---

# 4. IPC 2.0 建设

## 4.1 目标

建立统一 IPC 模型：

```
IPC
├── Endpoint
├── Message
├── Channel
├── WaitQueue
├── Transfer
└── IPC Security
```

---

## 4.2 IPC 通信流程

标准流程：

```
User Task
   ↓
Syscall (IPC Send)
   ↓
Capability Check
   ↓
Endpoint Lookup
   ↓
Message Validation
   ↓
Target Task Wakeup
   ↓
Message Delivery
```

---

## 4.3 IPC 消息模型

消息必须：

- 类型安全
- 大小有界（bounded message）
- 经过验证
- 不可绕过 Capability

禁止：

- 无界消息
- 裸指针传递
- 直接内存共享（除非通过 Memory Object 授权）

---

## 4.4 IPC 长期目标

Cycle 2 实现基础同步 IPC，并为后续保留扩展点：

- 同步 IPC（Cycle 2 实现）
- 异步 IPC（后续扩展）
- zero-copy / low-copy 优化（后续扩展）
- 超时机制
- IPC tracing

---

# 5. Capability 2.0

## 5.1 目标

Capability 是 July 的核心安全机制之一。

目标模型：

```
Task
 │
 ↓
CSpace
 │
 ↓
Capability
 │
 ↓
Kernel Object
```

---

## 5.2 Capability 操作

支持：

- lookup：查找 Capability
- mint：创建新 Capability
- derive：派生受限 Capability
- revoke：撤销 Capability
- delete：删除 Capability
- grant：授予 Capability
- rights reduction：权限缩减

---

## 5.3 权限模型

最终形成明确的：

```
Authority
    ↓
Capability
    ↓
Object Reference
    ↓
Rights
    ↓
Operation
```

原则：

> 不允许为了方便而绕过 Capability。

---

## 5.4 Capability 与 IPC 结合

```
Task A
   │
   ├── CSpace
   │     └── Capability → Endpoint
   │
   └── IPC Send ──→ Endpoint ──→ Task B
```

每次 IPC 调用必须经过 Capability 验证。

---

# 6. Kernel Object Model

## 6.1 目标

建立统一 Kernel Object 模型：

```
Kernel Object
├── Task
├── Thread
├── Endpoint
├── CNode / CSpace
├── MemoryObject
├── AddressSpace
├── Device
├── IRQ
├── Timer
└── ...
```

---

## 6.2 设计原则

Capability 不直接代表裸指针。

而应该代表：

```
Capability
 ↓
Object Reference
 ↓
Rights
 ↓
Operation
```

这样 July 的安全模型会更加统一。

---

## 6.3 对象生命周期

每个 Kernel Object 必须有明确：

- 创建者
- 所有者
- 引用计数
- 销毁流程
- 资源回收

---

# 7. Syscall ABI 2.0

## 7.1 目标

建立稳定系统调用 ABI：

```
Userspace
    ↓
Syscall Stub
    ↓
SVC / ECALL
    ↓
Syscall Dispatcher
    ↓
Capability Check
    ↓
Kernel Object Operation
```

---

## 7.2 Syscall 规范

每个系统调用必须明确：

- 参数类型与范围
- 返回值
- 错误码
- 所需 Capability
- 是否阻塞
- 是否可在中断上下文调用
- 内存访问规则（user pointer validation）

---

## 7.3 Syscall 分类

```
Task Management:
  - task_create
  - task_exit
  - task_suspend
  - task_resume

IPC:
  - ipc_send
  - ipc_receive
  - ipc_call
  - ipc_reply

Capability:
  - cap_mint
  - cap_derive
  - cap_revoke
  - cap_delete
  - cap_grant

Memory:
  - mem_alloc
  - mem_free
  - mem_map
  - mem_unmap

Object:
  - object_create
  - object_delete
  - object_control
```

---

# 8. User Space 隔离

## 8.1 目标

建立 Kernel / User Space 边界：

```
User Space                Kernel Space
    │                          │
    ├── Task A                 │
    │   └── Memory Region      │
    │                          │
    ├── Task B                 │
    │   └── Memory Region      │
    │                          │
    └── MPU/MMU Boundary ──────┤
                               │
                        July Kernel
```

---

## 8.2 隔离原则

- Task A 不能访问 Task B 的内存
- User Space 不能访问 Kernel 内存
- 所有跨边界访问必须通过 Syscall
- 指针从 User Space 传入必须验证

---

## 8.3 内存验证

```
User Pointer
    ↓
Validation (address range check)
    ↓
copy_from_user / safe access
    ↓
Kernel Buffer
```

禁止直接解引用 User Space 指针。

---

# 9. TaskControlBlock 重构

## 9.1 当前问题

TCB 是重点治理对象，存在字段膨胀趋势。

## 9.2 目标结构

```
TaskControlBlock
│
├── TaskIdentity        (ID, name, type)
├── TaskContext         (CPU registers, stack)
├── SchedulerContext    (priority, state, time slice)
├── MemoryContext       (address space, heap)
├── SecurityContext     (capability space, privileges)
└── IpcContext          (endpoints, wait queue)
```

## 9.3 禁止事项

禁止将以下内容放入 TCB：

- Network 状态
- UI 状态
- VFS 状态
- Lua 运行时
- Scanner 状态
- Metrics 数据

原则：

> TCB 是 Kernel Object，不是所有系统功能的状态仓库。

---

# 10. Memory 2.0

## 10.1 目标

建立分层内存管理：

```
Memory
├── Physical Memory Manager
├── Kernel Allocator
│   ├── Object Pool
│   ├── Fixed Block
│   └── Slab-like Allocator
├── Memory Object
├── Address Space
└── MPU / MMU Interface
```

---

## 10.2 Kernel 内存策略

优先：

- Static allocation
- Object Pool
- Fixed Block
- Slab-like allocator

避免：

- 无界动态分配
- 热路径 heap allocation
- 不可控 fragmentation

---

## 10.3 Userspace 内存

逐步建立：

- 独立地址空间
- 资源限制（quota）
- 内存映射接口

---

# 11. MPU / MMU 统一抽象

## 11.1 目标

```
Memory Protection Interface
             │
      ┌──────┴──────┐
      │             │
     MPU           MMU
      │             │
 Cortex-M       RISC-V / AArch64
```

最终应用层不应该知道具体是 MPU 还是 MMU。

---

## 11.2 接口设计

统一接口应提供：

- region_create
- region_map
- region_protect
- region_unmap
- region_destroy

---

# 12. 测试体系

## 12.1 IPC Test

验证：

- 消息发送与接收
- 消息大小边界
- 多任务通信
- Capability 验证
- 错误处理

---

## 12.2 Capability Test

验证：

- 创建与销毁
- 权限派生
- 权限缩减
- 撤销传播
- 越权访问拒绝

---

## 12.3 Syscall Test

验证：

- 参数验证
- 错误码
- 权限检查
- User pointer 验证
- 边界条件

---

## 12.4 Memory Test

验证：

- 分配与释放
- 地址空间隔离
- MPU/MMU 边界
- 资源限制

---

## 12.5 Integration Test

验证：

```
Task A ──IPC──→ Task B
  │               │
  └──Capability───┘
         │
    Kernel Object
```

端到端微内核通信流程。

---

# 13. 开发里程碑

# Milestone 1
## IPC 基础

任务：

- Endpoint 实现
- Message 结构
- 同步 IPC 通信
- WaitQueue

完成：

```
两个 Task 可以通过 IPC 通信
```

---

# Milestone 2
## Capability 系统

任务：

- CSpace 实现
- Capability 基本操作
- 权限模型
- Capability 与 IPC 集成

完成：

```
IPC 通信受 Capability 控制
```

---

# Milestone 3
## Syscall ABI

任务：

- Syscall 分发器
- 参数验证
- User pointer 处理
- 系统调用表

完成：

```
User Space 通过 Syscall 调用 Kernel
```

---

# Milestone 4
## Kernel Object Model

任务：

- [x] 统一对象模型
- [x] 引用计数
- [x] 生命周期管理
- [x] 类型系统

完成：

```
所有 Kernel 资源通过统一对象模型管理
```

---

# Milestone 5
## Memory 2.0

任务：

- 内核分配器升级
- Object Pool
- Memory Object
- MPU/MMU 抽象接口

完成：

```
分层内存管理体系建立
```

---

# 14. 完成标准

Cycle 2 完成后：

```
✓ IPC 同步通信可用
✓ Capability 权限系统运行
✓ Syscall ABI 定义清晰
✓ Kernel Object 模型统一
✓ User Space 与 Kernel 隔离
✓ 内存管理分层
✓ MPU/MMU 抽象接口存在
✓ 核心模块有测试覆盖
✓ TCB 重构完成
```

---

# 15. 下一阶段

进入：

```
Cycle 3
Kernel/Userspace Separation & Services
```

重点：

```
VFS Service
Network Service
Firewall Service
Scanner 重构
Driver/HAL 体系
```

---

# 16. 最终目标

Cycle 2 的目标是建立 July Kernel 作为微内核的核心竞争力：

```
稳定 IPC
    +
安全 Capability
    +
清晰 Syscall ABI
    +
统一 Kernel Object
    +
User Space 隔离
```

让 July 成为真正意义上的微内核，而不仅仅是一个功能集合。

---

# AuroraOS 核心理念

> Capability、IPC、Syscall 是架构边界，不是附加功能。

July Kernel 通过这三者建立安全、可验证、可扩展的微内核基础。
