
# AuroraOS Cycle 3
# Kernel/Userspace 分离与服务化阶段

> 版本：1.0
> 项目：AuroraOS
> 内核：July Kernel
> 阶段：Cycle 3 - Services & Separation
> 前置：Cycle 2 - Microkernel Core
> 状态：规划中

---

# 1. 阶段概述

Cycle 3 是 AuroraOS 从"大型 RTOS"进一步走向真正微内核的重要阶段。

Cycle 2 建立了 IPC、Capability、Syscall 等微内核核心机制。Cycle 3 在此基础上将复杂功能从 Kernel 中分离出来，以 Service 形式运行。

本阶段的核心目标：

> 将 VFS、Network、Firewall、Scanner 等复杂系统从 Kernel 中隔离，作为独立的 User Space Service 运行，通过 IPC 与 Kernel 和其他 Task 通信。

---

# 2. 阶段目标

目标架构：

```
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

# 3. 时间规划

预计周期：

```
5 ～ 8个月
```

---

# 4. VFS Service

## 4.1 目标

将文件系统从 Kernel 中分离为独立 Service：

```
VFS Service
├── VNode
├── File
├── RamFS
├── ProcFS
├── LittleFS
└── PhotonCache
```

## 4.2 架构

```
Application
    ↓
VFS API (IPC)
    ↓
VFS Service
    ↓
Filesystem Driver
    ↓
Storage Driver
```

## 4.3 Kernel 职责

July 只提供：

- IPC 通道
- Memory 共享
- Capability 授权
- Task 管理

VFS 作为 Service 运行，不进入 Kernel。

---

# 5. Network Service

## 5.1 目标

建立独立网络服务：

```
Network Service
├── lwIP Stack
├── Ethernet Driver Interface
├── WiFi Driver Interface
├── BLE Stack
├── Firewall Engine
├── Packet Capture
├── Network Scanner
├── IDS
└── Distributed SoftBus
```

## 5.2 架构

```
Application
    ↓
Socket API (IPC)
    ↓
Network Service
    ↓
lwIP
    ↓
Network Driver
```

## 5.3 分离原则

```
July Kernel
 │
 └── IPC (only)
       ↓
Network Service
       ↓
      lwIP
       ↓
    Driver
```

网络协议栈不进入 Kernel。

---

# 6. NetworkScanner 重构

## 6.1 当前问题

Scanner 是重点治理的 God Object，所有扫描类型耦合在一起。

## 6.2 目标架构

```
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

## 6.3 扩展方式

新增扫描方式时：

```
新增 Handler
```

而不是不断修改：

```
ScanEngine.cpp
```

采用策略/Handler 接口模式，保持扩展性。

---

# 7. Firewall 2.0

## 7.1 目标

形成独立 Firewall Service：

```
Firewall Service
├── Rule Engine
├── Connection Tracking
├── Rate Limiting
├── Policy Manager
└── Audit Log
```

## 7.2 安全策略分离

```
Network Packet
    ↓
Firewall Service
    ↓
Policy Engine
    ↓
Rule Matching
    ↓
Accept / Drop / Log
```

安全策略与网络协议实现分离。

## 7.3 能力目标

- 规则引擎（匹配、优先级）
- 连接追踪
- 速率限制
- 审计日志
- 动态规则更新

---

# 8. Driver / HAL 体系重构

## 8.1 目标

建立清晰的分层驱动体系：

```
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

## 8.2 示例

```
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

## 8.3 禁止事项

禁止：

```
Application
 ↓
直接操作寄存器
```

所有硬件访问必须通过 HAL → Driver → Service 层次。

---

# 9. Board 支持体系标准化

## 9.1 目标

```
boards/
├── lm3s6965/
├── nucleo_l031k6/
├── miband8/
└── qemu_rv32/
```

## 9.2 每个 Board 应包含

- memory map
- clock configuration
- peripheral mapping
- linker configuration
- HAL configuration
- board initialization

## 9.3 CMake 规范

避免将大量 Board 判断写进根目录 CMake。

Board 配置应放在 `boards/` 目录下。

---

# 10. 安全服务

## 10.1 Security Monitor Service

```
Security Monitor
├── Syscall Audit
├── Capability Audit
├── IPC Monitor
├── Resource Usage Tracking
└── Alert System
```

## 10.2 Secure Boot Service

```
Secure Boot Service
├── Signature Verification
├── Firmware Metadata
├── A/B Partition Management
├── Rollback Protection
└── OTA Update Manager
```

---

# 11. 测试体系

## 11.1 Service Test

验证：

- VFS Service 文件操作
- Network Service 通信
- Firewall 规则匹配
- Scanner 扫描流程

## 11.2 Integration Test

验证：

```
Application
    ↓ IPC
VFS Service
    ↓ IPC
Storage Driver
```

端到端服务调用流程。

## 11.3 Security Test

验证：

- Service 隔离
- Capability 边界
- 资源限制
- 错误传播

---

# 12. 开发里程碑

# Milestone 1
## VFS 服务化

任务：

- [x] VFS Service 独立进程
- [x] IPC 文件操作接口
- [x] RamFS / ProcFS 迁移
- [x] LittleFS 适配

完成：

```
文件操作通过 IPC 完成，VFS 不在 Kernel 中
```

---

# Milestone 2
## Network 服务化

任务：

- [x] Network Service 独立进程
- [x] Socket IPC 接口
- [x] lwIP 集成
- [x] 网络驱动接口

完成：

```
网络通信通过 Network Service 代理
```

---

# Milestone 3
## Driver/HAL 重构

任务：

- 统一 Driver API
- HAL 接口标准化
- 显示驱动分层
- Board 配置标准化

完成：

```
硬件访问层次清晰，无跨层调用
```

---

# Milestone 4
## Scanner 重构

任务：

- Handler 接口设计
- TCP/UDP/ARP/ICMP Handler 拆分
- Engine 与 Worker 分离
- Service Detection 模块化

完成：

```
Scanner 不再是一个 God Object
```

---

# Milestone 5
## Firewall 2.0

任务：

- 独立 Firewall Service
- 规则引擎
- 连接追踪
- 审计日志

完成：

```
网络安全策略独立管理
```

---

# 13. 完成标准

Cycle 3 完成后：

```
✓ VFS 作为独立 Service 运行
✓ Network 作为独立 Service 运行
✓ Firewall 作为独立 Service 运行
✓ Scanner 完成模块化重构
✓ Driver/HAL 层次清晰
✓ Board 配置标准化
✓ Service 之间通过 IPC 通信
✓ Kernel 不再包含高级服务逻辑
```

---

# 14. 下一阶段

进入：

```
Cycle 4
Runtime & Platform
```

重点：

```
Aurora Runtime
App Model
UI Service
Sensor Framework
Power Management
```

---

# 15. 最终目标

Cycle 3 的目标是验证微内核架构的关键假设：

```
复杂功能可以且应该在 Kernel 之外运行。
```

通过将 VFS、Network、Firewall 等服务化，证明 July Kernel 的 IPC/Capability 机制足够支撑实际系统需求，同时保持 Kernel 自身的小型化和安全性。

---

# AuroraOS 核心理念

> 能放到 User Space，就不要放进 July。

Kernel 越小，越容易验证、越安全、越可维护。
