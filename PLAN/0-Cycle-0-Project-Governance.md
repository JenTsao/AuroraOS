
# AuroraOS Cycle 0
# 项目治理与架构基础阶段

> 版本：1.0  
> 项目：AuroraOS  
> 内核：July Kernel  
> 阶段：Cycle 0 - 基础建设阶段  
> 状态：规划中

---

# 1. 阶段概述

Cycle 0 是 AuroraOS 的基础建设阶段。

本阶段的目标不是增加大量操作系统功能，而是建立一个能够支持 AuroraOS 长期发展的专业工程体系。

核心目标：

> 将 AuroraOS 从一个实验性操作系统项目，转变为一个具有长期维护能力、可扩展能力以及 AI 辅助开发能力的现代操作系统工程项目。

---

# 2. 阶段目标

Cycle 0 主要关注：

- 项目治理体系
- 架构文档体系
- AI 开发规范
- 仓库结构规范
- 构建系统标准化
- 测试基础设施
- 开发流程规范


最终目标：

为 July Kernel 和 AuroraOS 后续所有模块提供稳定基础。

---

# 3. 时间规划

预计周期：

```

1 ～ 2个月

```

---

# 4. 核心原则


## 4.1 架构优先原则

AuroraOS 的开发必须遵循：

```

需求分析

↓

架构设计

↓

代码实现

↓

测试验证

↓

代码审查

```

禁止：

```

发现需求

↓

直接写代码

↓

后期修补架构问题

```

---

## 4.2 长期维护原则

所有代码必须考虑：

- 未来开发者维护
- AI Agent 持续开发
- 多架构支持
- 硬件扩展
- 长期稳定性


禁止为了快速实现而引入：

- 临时代码
- 隐式依赖
- 全局状态
- 架构污染

---

## 4.3 AI 辅助开发原则

AI 在 AuroraOS 中不是简单代码生成工具。

AI 工作流程：

```

理解

↓

分析

↓

设计

↓

实现

↓

验证

↓

审查

```

AI 必须理解：

- 系统架构
- 模块职责
- 代码规范
- 安全边界

---

# 5. 仓库架构规划


目标结构：

```

AuroraOS/

├── kernel/
│
├── arch/
│
├── hal/
│
├── drivers/
│
├── services/
│
├── runtime/
│
├── apps/
│
├── docs/
│
├── tests/
│
├── tools/
│
└── .ai/

```

---

# 6. 目录职责定义


## kernel/

职责：

July Kernel 核心实现。

包含：

```

调度器 Scheduler

内存管理 Memory

IPC 通信

Capability 权限系统

中断管理 Interrupt

系统调用 Syscall

Kernel Object

```

原则：

> Kernel 只提供最核心机制，不承载复杂业务功能。

---

# arch/

职责：

处理不同 CPU 架构相关代码。

结构：

```

arch/

├── x86_64/

├── arm/

└── riscv/

````

规则：

架构相关代码禁止污染通用 Kernel。


例如：

禁止：

```cpp
#ifdef ARM

大量内核逻辑

#endif
````

应该：

```
Kernel

↓

Architecture Interface

↓

ARM / RISC-V 实现
```

---

# hal/

职责：

硬件抽象层。

结构：

```
HAL

↓

硬件接口

↓

具体平台实现
```

目标：

让 July Kernel 不依赖具体硬件。

---

# drivers/

职责：

硬件驱动。

例如：

```
UART

GPIO

SPI

I2C

USB

Network
```

规则：

驱动必须通过统一接口与系统通信。

---

# services/

职责：

用户态系统服务。

例如：

```
文件系统服务

网络服务

设备管理服务

安全服务
```

原则：

复杂功能应该放在 User Space。

---

# runtime/

职责：

运行环境。

例如：

```
C Runtime

Rust Runtime

语言运行时
```

---

# apps/

职责：

应用程序。

例如：

```
系统工具

测试程序

用户应用
```

---

# 7. 文档体系建设

Cycle 0 必须建立：

```
README.md

AGENTS.md

PLAN.md

ARCHITECTURE.md

CONTRIBUTING.md

SECURITY.md
```

---

# 8. 文档职责

## README.md

负责：

当前项目状态。

包含：

* 项目介绍
* 当前功能
* 编译方法
* 使用方式
* 已知限制

---

## AGENTS.md

负责：

AI 和开发者行为规范。

包含：

* 编码规范
* 架构限制
* 安全规则
* 测试要求
* 提交规范

---

## PLAN.md

负责：

长期发展路线。

包含：

* 六周期规划
* 架构目标
* 技术路线

---

## ARCHITECTURE.md

负责：

系统架构说明。

包含：

* 模块关系
* 数据流
* 依赖关系
* Kernel 设计

---

## CONTRIBUTING.md

负责：

贡献指南。

包含：

* 分支规则
* Commit 规范
* PR 流程

---

## SECURITY.md

负责：

安全管理。

包含：

* 漏洞报告
* 安全原则
* 安全审查流程

---

# 9. AI 开发体系

AuroraOS 使用：

```
开发者

+

AI Agent

↓

统一工程标准
```

---

# 10. AI Skill 结构

推荐：

```
.ai/

└── skills/

    └── auroraos-development/

        └── SKILL.md
```

---

Skill 负责：

```
代码生成规则

Bug预防

架构检查

测试要求

安全检查
```

---

# 11. 构建系统

AuroraOS 使用：

```
CMake

+

交叉编译

+

CI 自动化
```

---

支持：

```
GCC

Clang

Cross Compiler

QEMU

Static Analysis
```

---

# 12. CI 流程

目标：

```
提交代码

↓

自动构建

↓

静态分析

↓

单元测试

↓

集成测试

↓

代码审查
```

---

# 13. 测试体系

## 单元测试

测试：

```
内存管理

队列

IPC

调度器
```

---

## 集成测试

测试：

```
Kernel + Service

IPC + Application

Driver + HAL
```

---

## 硬件测试

未来支持：

```
ARM开发板

RISC-V开发板
```

---

# 14. 编码规范

## 14.1 单一职责

每个模块必须拥有明确职责。

禁止：

```
万能管理类

超级对象

大量全局变量
```

---

## 14.2 依赖方向

正确：

```
Application

↓

Runtime

↓

Service

↓

Kernel API

↓

July Kernel

↓

HAL

↓

Hardware
```

---

禁止：

```
Kernel → Application

Kernel → UI

Driver → Application
```

---

# 15. Git 工作流

推荐分支：

```
main

develop

feature/*

bugfix/*

release/*
```

---

# 16. Commit 规范

推荐：

正确：

```
fix(ipc): 修复消息大小检查
```

错误：

```
修改代码
```

---

# 17. 代码审查要求

重要修改必须检查：

```
架构影响

安全影响

性能影响

测试情况

文档同步
```

---

# 18. Cycle 0 里程碑

## Milestone 1

## 项目结构整理

任务：

* 创建目录结构
* 整理现有代码
* 明确模块职责

---

## Milestone 2

## 文档体系建立

任务：

* 创建 AGENTS.md
* 创建 PLAN.md
* 创建 ARCHITECTURE.md

---

## Milestone 3

## AI 开发框架

任务：

* 创建 SKILL.md
* 定义 AI 规则
* 建立审核流程

---

## Milestone 4

## 构建测试体系

任务：

* 配置 CMake
* 建立 CI
* 建立测试框架

---

# 19. 完成标准

Cycle 0 完成条件：

```
✓ 仓库结构标准化

✓ 文档体系建立

✓ AI开发规范完成

✓ 构建流程稳定

✓ CI运行正常

✓ 测试框架存在

✓ 开发流程明确
```

---

# 20. 下一阶段

完成 Cycle 0 后进入：

```
Cycle 1

July Kernel Foundation
```

重点：

```
Boot

HAL

Memory

Scheduler

Task System
```

---

# 21. 最终目标

Cycle 0 建立：

```
专业工程环境

+

可维护系统架构

+

AI辅助开发体系
```

为 AuroraOS 后续发展提供基础。

---

# AuroraOS 核心理念

不要通过不断增加代码让系统变大。

应该通过：

```
清晰架构

模块隔离

稳定接口

严格规范

长期规划
```

让 AuroraOS 持续成长。

---

**AuroraOS 应该通过架构成长，而不是通过复杂度膨胀。**

````
