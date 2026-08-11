
# AuroraOS Cycle 1
# July Kernel 基础阶段

> 版本：1.0  
> 项目：AuroraOS  
> 内核：July Kernel  
> 阶段：Cycle 1 - Kernel Foundation  
> 状态：规划中

---

# 1. 阶段概述

Cycle 1 是 AuroraOS 正式进入内核开发的第一个阶段。

本阶段的核心目标：

> 构建 July Kernel 的基础运行能力，使 AuroraOS 拥有一个可启动、可调度、可管理资源的最小内核。

Cycle 1 不追求完整操作系统功能，而是优先建立：

- Boot 流程
- 硬件抽象层
- 内存管理
- 任务系统
- 调度基础
- 中断机制

---

# 2. 阶段目标

Cycle 1 完成后，July Kernel 应具备：

```

启动

↓

初始化硬件

↓

建立内存环境

↓

创建任务

↓

执行任务

↓

进行任务切换

```

最终实现：

> 一个可以运行基础程序的最小 July Kernel。

---

# 3. 时间规划

预计周期：

```

3 ～ 6个月

```

---

# 4. 核心设计原则


## 4.1 Kernel 保持最小化


July Kernel 只负责：

```

调度

内存

中断

任务

基础通信接口

```

不负责：

```

文件系统

网络协议

UI

应用逻辑

高级服务

```

---

## 4.2 机制与策略分离


July Kernel 提供机制：

例如：

```

任务切换能力

内存映射能力

中断响应能力

```

具体策略交给：

```

Service

Runtime

User Space

```

---

## 4.3 架构无关设计


Kernel 通用代码：

```

kernel/

```

架构代码：

```

arch/

```

分离。


目标：

支持：

```

x86_64

ARM

RISC-V

```

---

# 5. Cycle 1 目标架构


整体结构：

```

```
          Application


              ↓


         July Kernel


    ------------------

    Scheduler

    Memory

    Interrupt

    Task


    ------------------


              ↓


            HAL


              ↓


          Hardware
```

```

---

# 6. Boot 系统建设


## 6.1 目标


建立：

```

Bootloader

↓

Kernel Entry

↓

Kernel Initialization

↓

Kernel Main

```


---

## 6.2 Boot 流程


标准流程：

```

CPU Reset

↓

Bootloader

↓

加载 Kernel

↓

设置 CPU 状态

↓

初始化 Stack

↓

进入 Kernel

```

---

## 6.3 Boot 任务


完成：

- Kernel 镜像加载
- CPU 初始化
- Stack 建立
- 基础输出
- Kernel Entry


---

# 7. HAL 硬件抽象层


## 7.1 目标


隔离：

```

Kernel

与

Hardware

```


结构：

```

July Kernel

```
  |

 HAL

  |
```

---

## ARM     RISC-V

```


---

## 7.2 HAL 职责


提供：

```

CPU操作

Timer

Interrupt

Memory Map

Device Access

```


---

## 7.3 禁止事项


禁止：

Kernel 直接调用：

```

ARM寄存器

RISC-V CSR

具体芯片地址

```


必须通过：

```

HAL Interface

```

---

# 8. 内存管理系统


## 8.1 目标


建立：

```

Physical Memory Manager

Virtual Memory Manager

Kernel Heap

```


---

# 8.2 物理内存管理


负责：

```

Page Frame

Memory Region

Allocation

Release

```


---

# 8.3 虚拟内存管理


负责：

```

Address Space

Mapping

Protection

Isolation

```


---

目标：

实现：

```

Task A

无法访问

Task B 内存

```


---

# 8.4 Kernel Heap


用于：

Kernel 内部动态对象。


要求：

- 明确生命周期
- 防止内存泄漏
- 支持失败处理


禁止：

无限动态分配。

---

# 9. Task 任务系统


## 9.1 目标


建立：

```

Task

Thread

Context

```

---

# 9.2 Task 结构


设计：

```

Task

├── Identity

├── CPU Context

├── Memory Context

├── Security Context

└── IPC Context

```


---

# 9.3 Context Switch


实现：

```

Task A

↓

保存 CPU Context

↓

恢复 Task B Context

↓

Task B运行

```


---

# 10. Scheduler 调度器


## 10.1 初期目标


实现：

基础调度。


第一阶段：

```

Round Robin

```

---

## 10.2 Scheduler职责


负责：

```

选择下一个任务

保存任务状态

恢复任务状态

```


---

禁止加入：

```

网络管理

文件系统

应用逻辑

```


---

# 11. Interrupt 中断系统


## 11.1 目标


建立：

```

Hardware Interrupt

↓

Interrupt Handler

↓

Kernel Event

```


---

## 11.2 中断处理原则


ISR：

必须：

```

快速

简单

不可阻塞

```


禁止：

```

复杂计算

文件操作

等待锁

```


---

# 12. Timer 定时器


Timer 用于：

```

任务切换

时间管理

超时控制

```


流程：

```

Timer Interrupt

↓

Scheduler

↓

Context Switch

```


---

# 13. Kernel Object 基础


建立统一对象模型：

```

Kernel Object

├── Task

├── Memory

├── Timer

└── Device

```


目标：

为 Cycle 2：

```

IPC

Capability

```

提供基础。

---

# 14. 编译系统


Cycle 1 支持：

```

CMake

Cross Compile

QEMU

```


---

目录：

```

kernel/

├── core/

├── arch/

├── mm/

├── task/

├── scheduler/

└── interrupt/

```


---

# 15. 测试体系


## 15.1 Boot Test


验证：

```

Kernel 是否启动

```


---

## 15.2 Memory Test


验证：

```

分配

释放

映射

保护

```


---

## 15.3 Scheduler Test


验证：

```

任务创建

任务切换

任务退出

```


---

## 15.4 Interrupt Test


验证：

```

Timer

Interrupt Handler

```


---

# 16. 开发里程碑


# Milestone 1

## Kernel Boot


任务：

- 完成 Boot流程
- Kernel入口
- 基础日志输出


完成：

```

Hello July Kernel

```


---

# Milestone 2

## Hardware Abstraction Layer


任务：

- 建立HAL接口
- 支持第一种架构


完成：

```

Kernel 不直接依赖硬件

```


---

# Milestone 3

## Memory Management


任务：

- 物理内存管理
- 虚拟内存基础
- Kernel Heap


完成：

```

Kernel拥有独立内存管理

```


---

# Milestone 4

## Task System


任务：

- Task结构
- Context Switch
- Scheduler


完成：

```

多个任务运行

```


---

# Milestone 5

## Interrupt System


任务：

- Interrupt Handler
- Timer
- 调度触发


完成：

```

硬件事件驱动Kernel

```


---

# 17. 完成标准


Cycle 1 完成后：

```

✓ July Kernel 可以启动

✓ 支持硬件初始化

✓ 拥有基础内存管理

✓ 可以创建任务

✓ 可以任务切换

✓ 支持Timer Interrupt

✓ Kernel 与 Hardware 分离

✓ 具备进入微内核阶段基础

```

---

# 18. 下一阶段


进入：

```

Cycle 2

July Microkernel Core

```


重点：

```

IPC

Capability

Syscall

Kernel Object

User Space Isolation

```

---

# 19. 最终目标


Cycle 1 的目标不是创建完整操作系统。

而是建立：

```

稳定

小型

可扩展

架构清晰

跨平台

```

的 July Kernel 基础。


---

# AuroraOS 核心理念


> Kernel 不应该成为功能堆积的位置。

July Kernel 应该保持：

```

小

快

安全

稳定

可验证

```

通过后续：

```

Service

Runtime

Driver

Application

```

扩展 AuroraOS 能力。
```