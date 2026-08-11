
# AuroraOS Cycle 4
# Runtime 与平台阶段

> 版本：1.0
> 项目：AuroraOS
> 内核：July Kernel
> 阶段：Cycle 4 - Runtime & Platform
> 前置：Cycle 3 - Services & Separation
> 状态：规划中

---

# 1. 阶段概述

Cycle 4 是 AuroraOS 从"系统平台"走向"应用平台"的阶段。

Cycle 3 完成了核心服务的独立化和 Kernel 的瘦身。Cycle 4 在此基础上建立面向应用开发者的运行时环境和平台能力。

本阶段的核心目标：

> 建立 Aurora Runtime、统一应用模型、UI 体系、Sensor Framework 和 Power Management，使 AuroraOS 成为可以承载第三方应用的完整平台。

---

# 2. 阶段目标

目标架构：

```
Application
    ↓
Aurora Runtime API
    ↓
System Services (VFS, Network, UI, Sensor, Power)
    ↓
IPC / Capability
    ↓
July Kernel
```

---

# 3. 时间规划

预计周期：

```
5 ～ 8个月
```

---

# 4. Aurora Runtime

## 4.1 目标

建立统一运行时环境：

```
Aurora Runtime
├── Lua Runtime
├── App Runtime
├── IPC Runtime
├── Event Runtime
└── Resource Runtime
```

## 4.2 分层架构

```
Lua Script
    ↓
Aurora Runtime API
    ↓
IPC / Capability
    ↓
System Service
```

Lua 不应该直接接触 Kernel 内部结构。

## 4.3 Runtime 职责

- 提供统一的应用编程接口
- 管理应用生命周期
- 资源分配与回收
- 事件分发
- 错误处理

---

# 5. Aurora Application Model

## 5.1 目标

建立统一 App 模型：

```
Application
├── Manifest
├── Capability Request
├── Memory Limit
├── CPU Limit
├── IPC Endpoint
├── Lifecycle
└── Runtime
```

## 5.2 App 启动流程

```
App Manifest
    ↓
Capability Request
    ↓
Security Manager 审核
    ↓
App Sandbox 创建
    ↓
Runtime 初始化
    ↓
App 运行
```

## 5.3 App 生命周期

```
Created → Starting → Running → Paused → Stopped → Destroyed
                ↓                    ↑
              Error ─────────────────┘
```

每个状态转换必须有明确的回调。

---

# 6. UI 体系

## 6.1 目标

建立独立 UI Service：

```
UI Service
├── Window Manager
├── Screen Manager
├── View System
├── Renderer
├── Input Handler
├── Animation Engine
└── Display Driver Interface
```

## 6.2 架构

```
Application
    ↓
UI API (IPC)
    ↓
UI Service
    ↓
Renderer
    ↓
Display Driver
```

UI 不进入 July Kernel。

## 6.3 功能目标

- 窗口管理
- 视图层级
- 基础渲染
- 触摸/按键输入
- 简单动画
- 多屏幕支持预留

---

# 7. Sensor Framework 2.0

## 7.1 目标

建立统一 Sensor Service：

```
Sensor Service
├── Sensor Manager
├── Heart Rate Sensor
├── Accelerometer
├── Gyroscope
├── Temperature Sensor
├── PPG Sensor
└── Sensor Fusion
```

## 7.2 数据流

```
Sensor Driver (HAL)
    ↓
Sensor Framework
    ↓
Data Pipeline (filtering, calibration)
    ↓
Algorithm (health, motion)
    ↓
Application
```

健康算法与硬件驱动分离。

## 7.3 功能目标

- 传感器注册与发现
- 数据采样率控制
- 数据融合
- 算法插件机制
- 低功耗传感模式

---

# 8. Power Management 2.0

## 8.1 目标

建立电源管理服务：

```
Power Manager
├── Power State Machine
│   ├── RUN
│   ├── IDLE
│   ├── LIGHT_SLEEP
│   ├── DEEP_SLEEP
│   └── SHUTDOWN
├── Clock Manager
├── Peripheral Power Control
├── Wake-up Source Manager
├── Battery Monitor
└── Thermal Policy
```

## 8.2 架构

```
Scheduler (idle detection)
    ↓
Power Manager
    ↓
Clock Control
    ↓
Peripheral Power
```

## 8.3 长期目标

- CPU utilization 统计
- peripheral usage 监控
- sleep prediction 预测
- wake-up source 管理
- battery state 监测
- thermal policy 温控策略

---

# 9. 可观测性体系

## 9.1 目标

```
Observability
├── Logging
├── Metrics
├── Tracing
├── Profiling
└── Diagnostics
```

## 9.2 监控重点

```
CPU Usage
Memory Usage
Scheduler Stats
IPC Stats
Interrupt Stats
Network Stats
Power Stats
Security Events
```

## 9.3 原则

> 可观测性不能反过来成为 Kernel 的核心依赖。

日志和监控应该通过 Service 实现，不应侵入 Kernel 热路径。

---

# 10. 安全增强

## 10.1 Application Sandbox

```
App A                    App B
  │                        │
  ├── Manifest             ├── Manifest
  ├── Capability Set       ├── Capability Set
  ├── Memory Quota         ├── Memory Quota
  └── CPU Quota            └── CPU Quota
```

## 10.2 Least Privilege

每个 App 默认：

- 无网络权限
- 无文件系统权限
- 无传感器权限
- 无 UI 权限

所有权限必须通过 Manifest 声明，经 Security Manager 审核。

---

# 11. 测试体系

## 11.1 Runtime Test

验证：

- App 生命周期管理
- Lua Runtime API
- 事件分发
- 资源管理

## 11.2 UI Test

验证：

- 窗口创建与销毁
- 渲染输出
- 输入事件
- 动画帧率

## 11.3 Sensor Test

验证：

- 传感器数据采集
- 数据管道
- 算法输出

## 11.4 Power Test

验证：

- 电源状态切换
- 唤醒源
- 低功耗模式

## 11.5 Security Test

验证：

- App Sandbox 隔离
- Capability 限制
- 资源配额

---

# 12. 开发里程碑

# Milestone 1
## Aurora Runtime

任务：

- Runtime API 设计
- App 生命周期管理
- Lua Runtime 集成
- IPC Runtime 封装

完成：

```
Lua 脚本通过 Runtime API 调用系统服务
```

---

# Milestone 2
## App Model

任务：

- Manifest 格式定义
- Capability Request 机制
- App Sandbox 基础
- 资源配额

完成：

```
应用通过 Manifest 声明权限并受限运行
```

---

# Milestone 3
## UI Service

任务：

- UI Service 独立进程
- Window Manager
- Renderer
- Input Handler

完成：

```
应用通过 UI API 创建窗口和渲染界面
```

---

# Milestone 4
## Sensor Framework

任务：

- Sensor Service
- 统一传感器接口
- 数据管道
- 基础算法集成

完成：

```
应用通过 Sensor API 获取传感器数据
```

---

# Milestone 5
## Power Management

任务：

- Power Manager Service
- 电源状态机
- Clock 管理
- 低功耗策略

完成：

```
系统根据负载自动进入低功耗模式
```

---

# 13. 完成标准

Cycle 4 完成后：

```
✓ Aurora Runtime 可用
✓ App 模型定义清晰
✓ Lua 脚本可调用系统服务
✓ UI Service 提供基础界面能力
✓ Sensor Service 统一传感器访问
✓ Power Manager 管理电源状态
✓ App Sandbox 隔离生效
✓ 可观测性基础设施就绪
```

---

# 14. 下一阶段

进入：

```
Cycle 5
Ecosystem & Production
```

重点：

```
Aurora SDK
Developer APIs
Security Audit
Fuzzing
HIL Testing
Multi-Architecture
Production Readiness
```

---

# 15. 最终目标

Cycle 4 的目标是让 AuroraOS 成为一个可编程的平台：

```
开发者
    ↓
Aurora SDK / Runtime API
    ↓
App (Lua / Native)
    ↓
Aurora Services
    ↓
July Kernel
```

从"操作系统开发者视角"转向"应用开发者视角"，为最终的开发者生态奠定基础。

---

# AuroraOS 核心理念

> 平台的价值不在于它有什么功能，而在于开发者能用它做什么。

Cycle 4 建立从 Kernel 到 Application 的完整价值链条。
