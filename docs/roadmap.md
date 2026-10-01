# AuroraOS 长期发展规划
## —— July 微内核与 AuroraOS 操作系统生态演进路线

**项目名称：** AuroraOS  
**内核名称：** July Kernel  
**项目定位：** 面向智能穿戴、IoT 与资源受限设备的安全型微内核实时操作系统  
**规划类型：** 长期技术路线 / 架构演进计划  
**文档状态：** 长期规划基线  
**核心原则：** 稳定内核、明确边界、安全隔离、模块化、可测试、可持续演进

---

# 1. 项目愿景

AuroraOS 的长期目标不是简单增加功能，而是建立一个具有清晰架构边界的现代微内核操作系统。

最终形成：

```text
                         AuroraOS
                    Operating System
                           │
          ┌────────────────┴────────────────┐
          │                                 │
     July Kernel                       User Space
          │                                 │
   ┌──────┼──────┐                 ┌────────┼────────┐
   │      │      │                 │        │        │
Scheduler IPC Capability          VFS    Network   Apps
   │      │      │                 │        │        │
   └──────┼──────┘                 └────────┼────────┘
          │                                 │
          └──────────── IPC / Syscall ──────┘
                           │
                     HAL / Adapter
                           │
                     Device Drivers
                           │
                       Hardware
```

其中：

> **AuroraOS 是完整操作系统，July 是操作系统的微内核。**

July 不应该成为所有功能的容器。

July 的职责是提供：

- 任务与线程管理
- 调度
- IPC
- Capability
- 内存管理
- 地址空间/MPU/MMU
- 系统调用
- 中断与异常
- Kernel Object
- 基础同步原语
- 基础时钟与定时机制

AuroraOS 的复杂能力则逐渐服务化：

- VFS
- Network
- Firewall
- Packet Capture
- Network Scanner
- BLE
- UI
- Lua Runtime
- AI
- Security Services
- Device Services
- Applications

---

# 2. 当前项目基础

当前 AuroraOS 已经形成较大的系统雏形。

README 当前列出的主要组成包括：

```text
kernel/
arch/
boards/
boot/
bootloader/
drivers/
vfs/
net/
ui/
apps/
syscall/
adapter/
metrics/
ai/
experimental/
tests/
3rdparty/
config/
scripts/
```

同时已经存在：

- 优先级抢占调度
- FrameScheduler
- Mutex / Semaphore / MessageQueue / TaskNotify / Signal
- KernelHeap / MemoryPool
- Cortex-M4 MPU
- VFS / RamFS / ProcFS
- LittleFS / PhotonCache
- lwIP
- Firewall
- PacketCapture
- NetworkScanner
- DistributedSoftBus
- Capability / CSpace
- IPC Endpoint
- SecurityMonitor
- Secure Boot / OTA
- UI / Renderer
- Lua Runtime
- Sensor Framework
- Power Management
- 多个 ARM / RISC-V 目标
- GoogleTest
- ASAN / UBSAN
- clang-tidy / cppcheck
- GitHub Actions CI

这些构成了 AuroraOS 后续发展的基础。

但是功能多并不意味着架构已经成熟。

当前最重要的问题不是“再增加多少功能”，而是：

> **如何把已有功能从一个大型功能集合逐步整理成稳定、清晰、可维护的操作系统架构。**

---

# 3. 长期战略目标

未来 AuroraOS 的技术战略分成六条主线。

## 3.1 July Kernel

建立稳定、最小、可验证的微内核核心。

## 3.2 Security

建立完整的：

```text
Capability
+
IPC
+
Memory Isolation
+
Secure Boot
+
Least Privilege
```

安全模型。

## 3.3 Modularity

把网络、VFS、UI、BLE、AI 等复杂系统从 Kernel 中隔离出来。

## 3.4 Hardware Abstraction

建立稳定的：

```text
Architecture
→ HAL
→ Driver
→ Service
```

硬件抽象体系。

## 3.5 Developer Ecosystem

最终提供：

```text
Aurora SDK
Aurora Runtime
Aurora APIs
Aurora Applications
```

形成开发者生态。

## 3.6 Verification

逐步建立：

```text
Unit Test
Integration Test
Architecture Test
Security Test
Hardware Test
CI
```

完整验证体系。

---

# 4. 第一阶段：架构冻结与治理

## 目标

首先停止无边界增加功能。

这一阶段的核心不是写代码，而是建立规则。

### 主要工作

确定：

```text
AuroraOS = OS
July = Kernel
```

建立：

```text
docs/
├── architecture/
├── security/
├── kernel/
├── userspace/
├── hardware/
└── decisions/
```

并建立 Architecture Decision Records：

```text
ADR-001 July Kernel
ADR-002 Kernel Boundary
ADR-003 Kernel Object Model
ADR-004 Capability Model
ADR-005 IPC Model
ADR-006 Memory Model
ADR-007 Userspace Services
ADR-008 Driver Model
```

### 阶段完成标准

- July 名称正式确定
- Kernel 边界明确
- User Space 边界明确
- Experimental 与 Stable 隔离
- AI Agent 遵循统一架构规则
- 所有重大架构变化有 ADR

---

# 5. 第二阶段：July Kernel 核心重构

这是整个长期计划的最高优先级。

---

## 5.1 TaskControlBlock 重构

当前 TCB 是重点治理对象。

目标：

```text
TaskControlBlock
│
├── TaskIdentity
├── TaskContext
├── SchedulerContext
├── MemoryContext
├── SecurityContext
└── IpcContext
```

避免：

```text
TaskControlBlock
├── Network
├── UI
├── VFS
├── Lua
├── Scanner
├── Metrics
└── ...
```

### 原则

> TCB 是 Kernel Object，不是所有系统功能的状态仓库。

---

# 6. Scheduler 2.0

建立独立 Scheduler 架构：

```text
scheduler/
├── scheduler
├── ready_queue
├── priority
├── scheduling_policy
├── timer
└── context_switch
```

长期目标：

- O(1) Ready Queue
- 抢占式调度
- 优先级继承
- 时间片
- Tickless
- Frame-aware Scheduling
- CPU 负载统计
- 最坏响应时间分析

最终形成：

```text
Task
 ↓
Scheduler
 ↓
Scheduling Policy
 ↓
Ready Queue
 ↓
Architecture Context Switch
```

而不是让 Scheduler 承担其他系统职责。

---

# 7. IPC 2.0

建立统一 IPC 模型。

目标：

```text
IPC
├── Endpoint
├── Message
├── Channel
├── WaitQueue
├── Transfer
└── IPC Security
```

形成：

```text
User
 ↓
Syscall
 ↓
Capability Check
 ↓
Endpoint
 ↓
Message
 ↓
Target Task
```

长期目标：

- 类型安全消息
- Capability 验证
- bounded message
- zero-copy/low-copy 优化
- 超时
- 同步 IPC
- 异步 IPC
- IPC tracing

---

# 8. Capability 2.0

Capability 是 July 的核心安全机制之一。

目标模型：

```text
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

支持：

- lookup
- mint
- derive
- revoke
- delete
- grant
- rights reduction
- object reference
- capability space isolation

最终形成明确的：

```text
Authority
    ↓
Capability
    ↓
Object
    ↓
Operation
```

原则：

> 不允许为了方便而绕过 Capability。

---

# 9. Kernel Object Model

长期建立统一 Kernel Object 模型：

```text
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

Capability 不直接代表裸指针。

而应该代表：

```text
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

# 10. Memory 2.0

建立分层内存管理：

```text
Memory
├── Physical Memory
├── Kernel Allocator
├── Object Pool
├── Memory Object
├── Address Space
├── MPU
└── MMU
```

长期目标：

### Kernel

优先：

- Static allocation
- Object Pool
- Fixed Block
- Slab-like allocator

避免：

- 无界动态分配
- 热路径 heap allocation
- 不可控 fragmentation

### Userspace

逐步建立独立地址空间和资源限制。

---

# 11. MPU / MMU 统一抽象

目前 ARM MPU 已有基础，而其他架构仍存在不同成熟度。

长期建立：

```text
Memory Protection Interface
             │
      ┌──────┴──────┐
      │             │
     MPU           MMU
      │             │
 Cortex-M       RISC-V / AArch64
```

最终应用层不应该知道具体：

```text
MPU
MMU
PMSAv7
Page Table
```

这些属于架构层。

---

# 12. Syscall ABI 2.0

建立稳定系统调用 ABI。

目标：

```text
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
Kernel Object
```

每个系统调用必须明确：

- 参数
- 返回值
- 错误码
- 权限
- 是否阻塞
- 是否可在特定上下文调用
- 内存访问规则

最终形成稳定 ABI 文档。

---

# 13. 第三阶段：Kernel / Userspace 分离

这是 AuroraOS 从“大型 RTOS”进一步走向真正微内核的重要阶段。

目标：

```text
                July Kernel
                     │
              Syscall / IPC
                     │
       ┌─────────────┼─────────────┐
       ↓             ↓             ↓
   VFS Service   Network Service  Device Service
       │             │             │
       ↓             ↓             ↓
   Filesystems       lwIP         Drivers
```

Kernel 不再直接包含大量高级服务逻辑。

---

# 14. VFS Service

长期目标：

```text
VFS Service
├── VNode
├── File
├── RamFS
├── ProcFS
├── LittleFS
└── PhotonCache
```

July 只提供：

```text
IPC
Memory
Capability
Task
```

VFS 作为服务运行。

---

# 15. Network Service

网络体系最终：

```text
Network Service
├── lwIP
├── Ethernet
├── WiFi
├── BLE
├── Firewall
├── Packet Capture
├── Network Scanner
├── IDS
└── Distributed SoftBus
```

目标是：

```text
July
 │
 └── IPC
       ↓
Network Service
       ↓
      lwIP
       ↓
    Driver
```

---

# 16. NetworkScanner 重构

当前 Scanner 是重点治理的 God Object。

最终：

```text
NetworkScanner
├── Engine
├── Worker
├── Queue
├── Handler Registry
│
├── TCP Handler
├── UDP Handler
├── ARP Handler
├── ICMP Handler
│
├── Service Detection
└── Vulnerability Detection
```

新增扫描方式时：

```text
新增 Handler
```

而不是不断修改：

```text
ScanEngine.cpp
```

---

# 17. Firewall 2.0

长期形成独立 Firewall Service：

```text
Firewall
├── Rule Engine
├── Connection Tracking
├── Rate Limiting
├── Policy
└── Audit
```

安全策略与网络协议实现分离。

最终可以支持：

```text
Network
 ↓
Firewall
 ↓
Policy
 ↓
Application
```

---

# 18. 第四阶段：Driver / HAL 体系重构

建立：

```text
Application
 ↓
Service
 ↓
Driver API
 ↓
HAL
 ↓
Architecture
 ↓
Hardware
```

例如：

```text
Display Service
      ↓
Display Driver
      ↓
SPI HAL
      ↓
Cortex-M SPI
      ↓
Hardware
```

避免：

```text
Application
 ↓
直接操作寄存器
```

---

# 19. Board 支持体系

长期标准化：

```text
boards/
├── lm3s6965/
├── nucleo_l031k6/
├── miband8/
└── qemu_rv32/
```

每个 Board 应包含：

- memory map
- clock
- peripheral mapping
- linker configuration
- HAL configuration
- board initialization

避免将大量 Board 判断写进根目录 CMake。

---

# 20. 第五阶段：Runtime 体系

建立 Aurora Runtime。

目标：

```text
Aurora Runtime
├── Lua Runtime
├── App Runtime
├── IPC Runtime
├── Event Runtime
└── Resource Runtime
```

Lua 不应该直接接触 Kernel 内部结构。

应该：

```text
Lua
 ↓
Aurora Runtime API
 ↓
IPC / Capability
 ↓
System Service
```

---

# 21. Aurora Application Model

长期建立统一 App 模型：

```text
Application
├── Manifest
├── Capability Request
├── Memory Limit
├── CPU Limit
├── IPC Endpoint
├── Lifecycle
└── Runtime
```

例如：

```text
App Manifest
    ↓
Capability Request
    ↓
Security Manager
    ↓
App Sandbox
    ↓
Runtime
```

这样才能逐渐形成真正的应用生态。

---

# 22. UI 体系

当前 UI 功能逐渐增多。

长期拆分：

```text
UI Service
├── Window
├── Screen
├── View
├── Renderer
├── Input
├── Animation
└── Display
```

最终：

```text
Application
 ↓
UI API
 ↓
UI Service
 ↓
Renderer
 ↓
Display Driver
```

UI 不进入 July Kernel。

---

# 23. Sensor Framework 2.0

统一：

```text
Sensor Service
├── Sensor Manager
├── Heart Rate
├── Accelerometer
├── Gyroscope
├── Temperature
└── Future Sensors
```

健康算法与硬件驱动分离：

```text
Sensor Driver
 ↓
Sensor Framework
 ↓
Data Pipeline
 ↓
Algorithm
 ↓
Application
```

---

# 24. Power Management 2.0

建立：

```text
Power Manager
├── RUN
├── IDLE
├── LIGHT_SLEEP
├── DEEP_SLEEP
└── SHUTDOWN
```

并建立：

```text
Scheduler
 ↓
Power Manager
 ↓
Clock
 ↓
Peripheral
```

长期加入：

- CPU utilization
- peripheral usage
- sleep prediction
- wake-up source
- battery state
- thermal policy

---

# 25. 安全体系长期目标

AuroraOS 最终安全模型：

```text
             Security
                 │
 ┌───────────────┼────────────────┐
 │               │                │
Capability      Memory         Secure Boot
 │               │                │
 │              MPU/MMU           │
 │               │                │
 └───────────────┼────────────────┘
                 │
                IPC
                 │
             Audit / Monitor
```

长期加入：

- Capability audit
- syscall audit
- security monitor
- secure boot
- signed firmware
- OTA rollback
- anti-replay
- key management
- least privilege
- application sandbox
- security event logging

---

# 26. Secure Boot / OTA 2.0

最终：

```text
Boot ROM
   ↓
Bootloader
   ↓
Signature Verification
   ↓
Firmware Metadata
   ↓
A/B Partition
   ↓
July
   ↓
AuroraOS Services
```

OTA 必须具备：

- 签名验证
- 版本检查
- 回滚保护
- A/B 分区
- 断电恢复
- 更新状态记录

---

# 27. 可观测性体系

长期建立：

```text
Observability
├── Logging
├── Metrics
├── Tracing
├── Profiling
└── Diagnostics
```

重点监控：

```text
CPU
Memory
Scheduler
IPC
Interrupt
Network
Power
Security
```

但必须遵守：

> 可观测性不能反过来成为 Kernel 的核心依赖。

---

# 28. 测试体系升级

最终：

```text
tests/
├── unit/
├── integration/
├── kernel/
├── security/
├── networking/
├── filesystem/
├── runtime/
├── architecture/
└── hardware/
```

测试覆盖：

### Kernel

- Scheduler
- IPC
- Capability
- Memory
- Object lifecycle
- Syscall

### Security

- Capability bypass
- Invalid pointer
- MPU violation
- privilege boundary
- resource exhaustion

### Network

- TCP
- UDP
- Firewall
- Scanner
- Packet Capture
- BLE

### Runtime

- Lua
- App lifecycle
- API isolation

---

# 29. CI 长期目标

当前 CI 已经覆盖多种构建和分析任务。

长期演进为：

```text
                  CI
                   │
        ┌──────────┼──────────┐
        ↓          ↓          ↓
     Unit       Security    Static
        │          │          │
        ↓          ↓          ↓
   Integration   Fuzzing    Analysis
        │
        ↓
 Architecture Builds
        │
 ┌──────┼─────────┐
 ↓      ↓         ↓
ARM   RISC-V     QEMU
 │
 ↓
HIL Hardware
```

最终做到：

> 每一次 Kernel 变化都必须经过自动验证。

---

# 30. Fuzzing 计划

在中长期加入：

```text
Fuzzing
├── IPC Parser
├── Syscall Arguments
├── VFS Path
├── Network Packet
├── Firewall Rules
├── Lua Binding
└── Config Parser
```

重点寻找：

- buffer overflow
- integer overflow
- invalid state
- parser crash
- resource exhaustion
- privilege boundary bugs

---

# 31. 静态分析与代码质量

长期目标：

```text
Compiler Warnings
+
clang-tidy
+
cppcheck
+
ASAN
+
UBSAN
+
Fuzzing
+
Coverage
```

并逐渐建立：

```text
Architecture Lint
```

例如检测：

```text
kernel → ui       ❌
kernel → app      ❌
stable → experimental ❌
driver → app      ❌
```

---

# 32. CMake 长期重构

根 CMake 必须逐渐瘦身。

目标：

```text
CMakeLists.txt
        │
        ├── kernel
        ├── arch
        ├── boards
        ├── drivers
        ├── services
        ├── apps
        └── tests
```

而不是让所有 Board / Driver / Feature 条件不断堆积。

长期建立：

```text
cmake/
├── AuroraKernel.cmake
├── AuroraBoard.cmake
├── AuroraArch.cmake
├── AuroraTests.cmake
└── AuroraServices.cmake
```

---

# 33. 代码技术债治理

长期禁止以下趋势：

```text
God Object
God Function
Global Singleton
void* 扩散
裸指针扩散
CMake if/else 爆炸
Subsystem Circular Dependency
Stable → Experimental
复制粘贴
无边界全局状态
无测试核心代码
```

每个新功能都需要考虑：

```text
它属于哪里？
谁拥有它？
谁可以访问？
它依赖谁？
谁依赖它？
生命周期是什么？
如何测试？
如何删除？
```

---

# 34. AGENTS.md 长期治理

AGENTS.md 本身也必须维护。

它不是一次写完永久不变的文档。

每当：

- Kernel 架构变化
- Build 系统变化
- 测试系统变化
- API 变化
- 安全模型变化

都应该同步检查 AGENTS.md。

最终 AGENTS.md 的职责是：

> **阻止 AI 在 AuroraOS 中制造新的架构债务。**

---

# 35. 版本路线

建议采用长期版本体系：

```text
AuroraOS 0.x
    │
    ├── Architecture Development
    ├── Kernel Refactoring
    └── Feature Stabilization
            ↓
AuroraOS 1.0
    │
    ├── July Kernel Stable
    ├── Stable Syscall ABI
    ├── Capability Security
    ├── Core Services
    └── Supported Hardware
            ↓
AuroraOS 2.0
    │
    ├── Advanced Isolation
    ├── Multi-core exploration
    ├── Advanced networking
    ├── Application ecosystem
    └── Developer SDK
```

---

# 36. 近期目标

## P0

首先解决：

- July Kernel 命名与边界
- TCB 重构
- Scheduler 边界
- IPC 边界
- Capability 边界
- Kernel Object 模型
- CMake 架构治理

---

# 37. 中期目标

## P1

完成：

- Memory 2.0
- MPU/MMU 抽象
- Syscall ABI
- VFS Service
- Network Service
- Driver/HAL
- ScanEngine 重构
- Firewall Service
- Runtime Service
- UI Service

---

# 38. 长期目标

## P2

建立：

- Aurora SDK
- App Manifest
- Application Sandbox
- Aurora Runtime
- Developer API
- Security Audit
- Fuzzing
- Architecture Lint
- HIL 自动化
- 性能分析
- Power Profiling

---

# 39. 远期探索方向

以下方向不应过早进入稳定 Kernel：

### 多核

```text
SMP
AMP
```

### 更强 MMU

```text
RV32 MMU
AArch64
```

### GPU

```text
GPU abstraction
Display compositor
```

### 高级无线

```text
BLE
Wi-Fi
NFC
```

### AI

```text
On-device AI
Sensor inference
TinyML
```

### 分布式

```text
Aurora Distributed Services
```

这些应该先以：

```text
experimental/
```

形式研究，再逐步毕业。

---

# 40. 功能毕业制度

所有新功能建议经历：

```text
Experimental
      ↓
Prototype
      ↓
Tested
      ↓
Incubating
      ↓
Stable
      ↓
Production
```

进入 Stable 必须满足：

- API 明确
- 依赖稳定
- 测试存在
- 安全边界明确
- 资源限制明确
- 文档存在
- CI 验证
- 无明显架构债务

---

# 41. 硬件支持策略

不要追求“支持越多越好”。

优先建立三个黄金平台：

```text
LM3S6965
    ↓
QEMU / CI / Kernel Development

Nucleo-L031K6
    ↓
Cortex-M0+ / Low Power

MiBand 8
    ↓
Real Wearable / Cortex-M4F
```

再建立：

```text
RISC-V QEMU
    ↓
Architecture Portability
```

最终形成：

```text
Primary ARM
+
Low-end ARM
+
Real Wearable
+
RISC-V
```

四个核心验证方向。

---

# 42. 性能目标

长期关注：

### Scheduler

```text
O(1) ready queue
low context switch latency
predictable preemption
```

### IPC

```text
low latency
bounded message size
minimal copy
```

### Memory

```text
bounded allocation
low fragmentation
fast object allocation
```

### Power

```text
low idle power
tickless
fast wakeup
```

### Network

```text
bounded packet processing
stable throughput
low memory overhead
```

---

# 43. 资源约束

AuroraOS 面向资源受限设备。

因此任何新功能必须评估：

```text
Flash
RAM
Stack
Heap
CPU
Power
Latency
```

特别是 MiBand 等目标。

禁止因为“功能实现方便”而直接引入大型桌面级依赖。

---

# 44. 开发流程

长期推荐：

```text
Issue
 ↓
Architecture Analysis
 ↓
ADR
 ↓
Design
 ↓
Implementation
 ↓
Unit Test
 ↓
Integration Test
 ↓
Target Build
 ↓
Security Review
 ↓
CI
 ↓
Merge
```

而不是：

```text
想法
 ↓
直接改代码
 ↓
能编译
 ↓
完成
```

---

# 45. AI Agent 开发流程

AI Agent 必须遵守：

```text
Read
 ↓
Understand
 ↓
Search Callers
 ↓
Design
 ↓
Minimal Change
 ↓
Test
 ↓
Review
 ↓
Report
```

AI 不应该：

- 为一个小功能重写整个系统
- 随意修改 3rdparty
- 破坏 Capability
- 扩大 TCB
- 添加全局变量
- 绕过测试
- 把 experimental 代码直接变成 stable
- 为了编译通过而修改架构边界

---

# 46. 最终目录目标

长期希望 AuroraOS 演进为：

```text
AuroraOS/
│
├── kernel/
│   └── july/
│       ├── scheduler/
│       ├── task/
│       ├── ipc/
│       ├── capability/
│       ├── memory/
│       ├── syscall/
│       ├── interrupt/
│       ├── object/
│       └── panic/
│
├── arch/
│   ├── arm/
│   └── riscv/
│
├── hal/
│
├── drivers/
│
├── services/
│   ├── vfs/
│   ├── network/
│   ├── firewall/
│   ├── security/
│   ├── power/
│   ├── display/
│   └── sensor/
│
├── runtime/
│   ├── lua/
│   └── aurora/
│
├── apps/
│
├── ui/
│
├── boards/
│
├── tests/
│
├── experimental/
│
├── docs/
│
├── scripts/
│
└── 3rdparty/
```

最终形成：

```text
AuroraOS
│
├── July Kernel
│
├── Aurora Services
│
├── Aurora Runtime
│
├── Aurora SDK
│
└── Aurora Applications
```

---

# 47. 最终架构原则

AuroraOS 长期发展必须坚持以下原则：

### 原则一：Kernel 最小化

> 能放到 User Space，就不要放进 July。

### 原则二：安全边界优先

> Capability、IPC、MPU/MMU 是架构边界，不是附加功能。

### 原则三：不要制造 God Object

> 核心对象应该保持小而明确。

### 原则四：不要为了功能破坏架构

> 功能可以延后，架构债务很难偿还。

### 原则五：稳定接口优先

> Kernel ABI 和核心对象接口不能频繁为了上层便利而改变。

### 原则六：实验与稳定分离

> Experimental 可以快速试错，但不能污染 Stable。

### 原则七：测试驱动核心演进

> Scheduler、IPC、Capability、Memory 等核心模块必须有测试。

### 原则八：硬件抽象

> Application 不应该知道硬件寄存器，Service 不应该依赖具体 MCU。

### 原则九：AI 必须服从架构

> AI 是开发工具，不是架构决策者。

### 原则十：每次修改都要问一句

> **“这次修改会不会让下一次修改更困难？”**

如果答案是“会”，就应该重新设计。

---

# 48. 最终愿景

AuroraOS 最终不是：

```text
一个拥有很多功能的 RTOS
```

而应该成为：

```text
                 AuroraOS
                     │
             ┌───────┴───────┐
             │               │
        July Kernel       User Space
             │               │
        Security Core     Services
             │               │
        Capability          Runtime
             │               │
             IPC             Apps
             │               │
             └───────┬───────┘
                     │
                  Hardware
```

其中：

**July 是稳定、最小、安全的核心。**

**AuroraOS 是建立在 July 之上的完整系统。**

最终目标不是让 July “什么都会”，而是让 July **足够小、足够可靠、足够安全**，然后让 AuroraOS 在这个核心之上不断扩展。

---

# 49. 长期成功标准

AuroraOS 真正达到成熟阶段，不应该只用“代码量”和“功能数量”衡量。

应重点衡量：

```text
Kernel Size
        ↓
Security Boundary
        ↓
IPC Reliability
        ↓
Scheduler Determinism
        ↓
Memory Safety
        ↓
Architecture Portability
        ↓
Test Coverage
        ↓
Hardware Stability
        ↓
Developer Experience
```

最终理想状态：

```text
         小型 July Kernel
                 │
          清晰 Capability
                 │
           高可靠 IPC
                 │
       强隔离 User Space
                 │
        模块化 System Services
                 │
         稳定 Aurora Runtime
                 │
          Aurora Application
                 │
             AuroraOS
```

**这就是 AuroraOS 从当前代码库走向长期可维护操作系统的主路线。**

---

# 50. 总路线图

```text
现在
 │
 │  架构治理
 ↓
AuroraOS Architecture Baseline
 │
 │  TCB / Scheduler / IPC / Capability
 ↓
July Kernel Core
 │
 │  Memory / Syscall / Object Model
 ↓
July Kernel Stable
 │
 │  VFS / Network / Driver / Security Services
 ↓
AuroraOS Services
 │
 │  Runtime / UI / Sensor / Power
 ↓
AuroraOS Platform
 │
 │  SDK / App Model / Developer APIs
 ↓
AuroraOS Ecosystem
 │
 │  Security / Fuzzing / HIL / Multi-Architecture
 ↓
AuroraOS 1.0+
```

**核心路线只有一句话：**

> **先把 July 做小、做稳、做安全，再让 AuroraOS 变强。**

而不是反过来把越来越多功能塞进 Kernel。