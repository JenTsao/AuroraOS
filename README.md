<div align="center">

<br>

<pre>
        ___                  ___  ____   ____
       /   |  ________  ____/  _/ / __ \ / __ \__
      / /| | / ___/ _ \/ __// /  / /_/ // / / /
     / ___ |/ /  /  __/ /__/ /  / _, _// /_/ /
    /_/  |_/_/   \___/\___/___//_/ |_| \____/____/
</pre>

# AuroraOS

**面向智能手表与物联网终端的微内核实时操作系统**

> ARM Cortex-M0+/M3/M4 · RISC-V RV32IMAC · lwIP TCP/IP · Lua 5.4.6 · MPU 内存保护

<p>
  <img src="https://img.shields.io/badge/Platform-Cortex--M0%2B%20%7C%20M3%20%7C%20M4%20%7C%20RV32-brightgreen.svg" alt="Platform">
  <img src="https://img.shields.io/badge/Kernel-July%20Microkernel-blue.svg" alt="Kernel">
  <img src="https://img.shields.io/badge/Network-lwIP%20TCP%2FIP-orange.svg" alt="Network">
  <img src="https://img.shields.io/badge/Storage-LittleFS%20%2B%20PhotonCache-purple.svg" alt="Storage">
  <img src="https://img.shields.io/badge/Script-Lua%205.4.6-yellow.svg" alt="Script">
  <img src="https://img.shields.io/badge/CI-13%20Jobs-green.svg" alt="CI">
  <img src="https://img.shields.io/badge/License-CAOSL%20v2.0-blue.svg" alt="License">
</p>

</div>

---

<details open>
<summary><b>📑 目录</b></summary>

- [项目简介](#项目简介)
- [功能特性](#功能特性)
- [目录结构](#目录结构)
- [功能状态](#功能状态)
- [快速开始](#快速开始)
- [核心架构](#核心架构)
- [AI 运行时 (February)](#ai-运行时-february)
- [显示驱动 (Display)](DOCS/porting/hardware.md)
- [局域网隐身伪装 (Stealth Identity)](#局域网隐身伪装-stealth-identity)
- [BLE 广播隐身伪装 (BleStealth)](#ble-广播隐身伪装-blestealth)
- [开发路线图](#开发路线图)
- [许可证](#许可证)

</details>

---

## 项目简介

auroraOS 是一个面向智能手表与物联网终端的实时操作系统平台，其底层的微内核被正式命名为 **July**（July Kernel）。它在精简的代码体积内实现了优先级抢占调度、完整 TCP/IP 网络协议栈、MPU 内存隔离、Lua 小程序引擎、帧感知渲染、分布式软总线等特性，并在最新架构中完全转向了**基于 Capability 与 IPC 驱动的现代微服务架构 (Microservice Architecture)**。

| 指标 | 说明 |
|------|------|
| 目标架构 | ARM Cortex-M0+/M3/M4 (Thumb-2)、RISC-V 32 (RV32IMAC) |
| 支持板级 | TI LM3S6965-QB (Cortex-M3, QEMU)、QEMU RV32 Virt、ST Nucleo-L031K6 (Cortex-M0+)、小米手环 8 (Apollo3 M4F, 内核已启动) |
| 构建系统 | CMake + Kconfig (Linux 内核风格可裁剪配置) |
| CI/CD | GitHub Actions (13 jobs: 4 目标固件构建 + QEMU 冒烟 + HIL + 单元测试 + ASAN/UBSAN + clang-tidy + cppcheck + 覆盖率 + 模糊测试 + 性能基准 + 固件大小对比 + Release) |
| 开发语言 | C++ (内核) + C (驱动/lwIP/Lua) + ARM/RISC-V Assembly (启动/异常向量) |
| 第三方依赖 | lwIP 2.x · Lua 5.4.6 · LittleFS (git submodule) · ed25519 |

### 设计参考

参考了以下操作系统的公开文档与源码，具体实现均为独立编写：

| 项目 | 参考内容 |
|------|----------|
| **NuttX** | POSIX 兼容层、ProcFS 设计 |
| **FreeRTOS** | TaskNotify、tickless 思路 |
| **Zephyr** | Kconfig、传感器框架接口 |
| **seL4** | Capability 模型、IPC 端点 |
| **Linux** | VFS 设计 |
| **HarmonyOS** | 分布式软总线 |
| **vivo BlueOS** | 帧感知调度、光子缓存、超级渲染树 |
| **watchOS** | 表盘 Complication、应用生命周期 |

---

## 功能特性

auroraOS 围绕「小而全」的嵌入式内核目标构建，核心亮点包括：

- 🧠 **确定性微内核**：O(1) 五级优先级抢占调度 + 帧感知调度，面向 30fps 可穿戴渲染场景。
- 🔐 **能力安全模型**：seL4 风格 CSpace 能力空间、类型化 IPC Endpoint、系统调用审计与 Ed25519 安全启动。
- 🛡️ **MPU 内存隔离**：Cortex-M4 / M4F 下 Flash 只读、RAM 特权隔离与用户栈沙盒的动态切换。
- 🌐 **完整网络栈**：lwIP 2.x 全协议栈、防火墙、包捕获、扫描器、分布式软总线与跨设备 AI 协同。
- 🥷 **隐身伪装**：局域网 (StealthIdentity) 与 BLE (BleStealth) 双层身份欺骗，混入周边设备背景。
- 📜 **Lua 小程序引擎**：Lua 5.4.6 以自定义分配器集成，开放传感器与 UI API 给第三方小程序。
- 🤖 **嵌入式 AI 运行时**：February 跨设备意图引擎，零堆分配、可静态配置，适配 8KB RAM 级别设备。
- 💾 **掉电安全存储**：LittleFS + PhotonCache LRU 页缓存，脏页延迟写与重试。

---

## 目录结构

```
auroraOS/
├── apps/                 # 应用层 (Shell, Lua 引擎, ELF 加载器, 网络应用, MiBand 8 表盘)
├── kernel/               # 内核核心 (调度器, 内存, 同步原语, IPC, CSpace, MPU, 信号, 定时器)
├── boot/                 # 启动与硬件抽象 (Reset_Handler, PendSV, SVC, SysTick)
├── bootloader/           # 安全启动 (Ed25519 验签 + OTA 双分区)
├── vfs/                  # 虚拟文件系统 (VNode, RamFS, ProcFS, LittleFS, PhotonCache)
├── net/                  # 网络子系统 (防火墙, 包捕获, 扫描器, BLE 安全, 软总线, 无线安全审计)
├── drivers/              # 驱动层 (显示[帧缓冲/SSD1306/ST7789/OLED-Mock], 输入, 传感器, USB, 存储, 看门狗, 电源)
├── ui/                   # UI 框架 (ScreenNavigator, View, Complication, 基础控件)
├── arch/                 # 架构抽象层 (ARM Cortex-M0+/M3/M4/M4F, ARMv8-A AArch64 探索, RISC-V RV32)
├── boards/               # 板级支持包 (LM3S6965, Nucleo-L031K6, MiBand 8, QEMU RV32)
├── adapter/net/          # lwIP OSAL 适配层 (Mutex/Sem/Mbox/Thread 映射, 以太网接口)
├── syscall/              # SVC/ECALL 系统调用定义
├── services/             # 独立服务 (vfs/firewall 接入固件构建；net/sensor/power 参与 host 测试；UI 归并至 ui/ 体系)
├── runtime/              # 应用运行时 (app_base, aurora_runtime, app_sandbox)
├── metrics/              # 性能度量 (DWT 周期计数器, 延迟记录器, 功耗分析)
├── utils/                # 工具 (HMAC-SHA256, JSON 解析器)
├── ai/                   # AI 运行时 (February 跨设备意图引擎：EventBus / Planner / Persona / SoftBus)
├── experimental/         # 实验性代码 (BLE 协议栈, 相机, GPU, NFC, GUIX, 通知中心)
├── config/               # 构建配置 (Kconfig, 链接脚本, 分区表)
├── scripts/              # 构建脚本 (Kconfig 生成, QEMU 启动, HIL 测试, 固件打包)
├── tests/                # 测试 (GoogleTest 单元测试, 集成测试, 压力测试)
├── 3rdparty/             # 第三方依赖 (lwIP, Lua, LittleFS, ed25519)
├── .github/workflows/    # GitHub Actions CI 配置
├── Kconfig               # Kconfig 配置定义
├── CMakeLists.txt        # CMake 构建脚本
├── Dockerfile            # Docker 构建环境
└── LICENSE               # CAOSL v2.0 许可证
```

### 目录详解

| 目录 | 层级 | 核心职责 | 关键内容 / 稳定度 |
|------|------|----------|-------------------|
| `apps/` | 应用层 | 用户态/准用户态应用与入口 | Shell、Lua 小程序引擎、ELF 加载器、网络应用 (`net_app`)、MiBand 8 表盘与 `kernel_main` 启动链；依赖内核 ABI，不反向被内核依赖 |
| `kernel/` | 内核核心 | 微内核本体（仅含需特权的功能） | 调度器、内存管理、同步原语、IPC Endpoint、CSpace 能力、MPU、审计、安全监控、看门狗；高敏感，改动需额外评审 |
| `boot/` | 架构启动 | 上电到进入内核的引导与异常向量 | `Reset_Handler`、PendSV/SVC/SysTick 处理、早期硬件初始化；与 `arch/` 紧密配合 |
| `bootloader/` | 安全启动 | 固件验签与 OTA | Ed25519 验签 + A/B 双分区断电安全；生产构建强制真实密钥 |
| `vfs/` | 子系统 | 虚拟文件系统 | VNode 多态抽象、RamFS、ProcFS、LittleFS 落盘 + PhotonCache LRU 页缓存；路径遍历防护 |
| `net/` | 子系统 | 网络协议栈与安全 | lwIP 2.x、防火墙、包捕获、扫描器、分布式软总线、Stealth/Ble 隐身、无线安全审计（部分 🚧） |
| `drivers/` | 驱动层 | 硬件外设驱动 | 显示（帧缓冲/SSD1306/ST7789/OLED-Mock）、输入、传感器、存储、USB、看门狗、电源；全部经 `hal/` 抽象 |
| `ui/` | 框架 | 可穿戴 UI 框架 | ScreenNavigator 页面栈、View、Complication 表盘引擎、基础控件；归并了 services 中的 UI 部分 |
| `arch/` | 架构抽象 | 多架构汇编与寄存器适配 | Cortex-M0+/M3/M4/M4F、ARMv8-A 探索、RISC-V RV32；`Arch::` 命名空间与 `arch_impl.hpp` |
| `boards/` | 板级支持 | 具体硬件绑定 | LM3S6965、Nucleo-L031K6、QEMU RV32 Virt、MiBand 8；含 `board.h/.cpp` 与 `get_*_hal()` 工厂 |
| `adapter/net/` | 适配层 | lwIP OSAL 映射 | 将 lwIP 的 Mutex/Sem/Mbox/Thread 映射到内核原语，并接入以太网 MAC |
| `syscall/` | 边界 | 系统调用 ABI 定义 | SVC (ARM) / ECALL (RISC-V) 调用号与参数约定；用户态进入内核的唯一契约 |
| `services/` | 服务 | 独立后台服务 | vfs/firewall 接入固件构建，net/sensor/power 参与 host 测试；UI 已归并 `ui/` |
| `runtime/` | 运行时 | 应用运行支撑 | `app_base`、aurora_runtime、app_sandbox 沙盒；提供应用生命周期与安全边界 |
| `metrics/` | 度量 | 运行时指标 | DWT 周期计数器、延迟记录器、功耗分析；供 benchmark 套件采集 |
| `utils/` | 工具 | 通用算法 | HMAC-SHA256、JSON 解析器；内核与 host 均可复用 |
| `ai/` | 运行时 | 嵌入式 AI | 遗留 `intent_engine.hpp` 与 February 跨设备意图引擎（EventBus/Planner/Persona/SoftBus）；header-only，零堆分配 |
| `experimental/` | 实验性 | 探索性代码 | BLE 协议栈、相机、GPU、NFC、GUIX、通知中心；**不进入稳定内核依赖**（见 `AGENTS.md` §4） |
| `config/` | 构建 | Kconfig/链接/分区 | 源 Kconfig、链接脚本 (`*.ld`)、分区表；生成产物不手工编辑 |
| `scripts/` | 构建 | 自动化脚本 | `genconfig.py`、QEMU 启动、HIL 测试、固件打包 |
| `tests/` | 测试 | 验证 | 221 个 GoogleTest 单元/集成/压力测试，覆盖率与模糊测试支撑 |
| `3rdparty/` | 依赖 | 第三方库 | lwIP、Lua 5.4.6、LittleFS (submodule)、ed25519；vendor 代码不手工改 |

---

## 功能状态

图例：<code>✅ 已完成</code> &nbsp;·&nbsp; <code>🚧 进行中</code> &nbsp;·&nbsp; <code>❌ 未实现</code>

| 子系统 | 功能 | 状态 | 说明 |
|--------|------|:----:|------|
| 内核调度 | O(1) 优先级抢占调度器 (5 级: Idle/Low/Normal/High/Realtime) | ✅ | 就绪位图 + 侵入式双向链表，O(1) 入队/出队/最高优先级检索 |
| 内核调度 | 帧感知调度 FrameSchedulerV2 | ✅ | 30fps 帧内/帧间窗口分级，`volatile bool` 实现，不依赖 `<atomic>` |
| 同步原语 | Mutex (优先级继承 PIP) | ✅ | 传递性优先级继承、递归加锁、超时机制、RAII UniqueLock |
| 同步原语 | Semaphore / MessageQueue SPSC / TaskNotify / Signal | ✅ | 计数信号量 ISR 安全；无锁 SPSC 环形队列；32 位零开销通知；POSIX signal/kill/raise |
| 内存管理 | KernelHeap (First-Fit + Split + Lazy Coalesce) | ✅ | 线程安全 (IrqGuard RAII)，8 字节对齐，魔数校验，OOM 懒合并 |
| 内存管理 | MemoryPool (O(1) 固定块分配器) | ✅ | 空闲链表，边界检查，双重释放检测 |
| 内存保护 | MPU (Cortex-M4, PMSAv7) | ✅ | 8 区域配置，Flash 只读 + RAM 特权态 + 用户栈沙盒，PendSV 动态切换 |
| 内存保护 | MPU (Apollo3 M4F) | ✅ | `arch/arm/cortex-m/cm4f/arch_impl.hpp` PMSAv7 完整实现：RNR/RBAR/RASR 配置、AP/XN/Device 属性、PRIVDEFENA + MemFault 使能 |
| 内存保护 | AArch64 MMU + VAS | ❌ | 仅抽象接口 (`kernel/mm/vasp.hpp`)，无 CMake 构建目标 |
| 存储 | VFS (VNode 多态) + RamFS + ProcFS | ✅ | open/read/write/close/lseek/ioctl 完整接口，路径遍历防护 |
| 存储 | LittleFS + PhotonCache (LRU 页缓存) | ✅ | 掉电安全日志式文件系统，8 槽 LRU 缓存，脏页延迟写，3 次重试 |
| 存储 | SoftBus (UART RPC 总线) | ✅ | 有 `.cpp` 实现，非 M0+ 目标编译时包含，带凭证验证 |
| 网络 | lwIP 2.x 全栈 (IPv4/TCP/UDP/ICMP/ARP/DHCP) | ✅ | Socket + Netconn 双 API，`LWIP_TCPIP_CORE_LOCKING` 核心锁 |
| 网络 | 以太网驱动 (StellarisEth, LM3S6965) | ✅ | 独立 RX/TX 互斥锁，4 字节对齐 memcpy，异常包 FIFO 排空 |
| 网络 | 防火墙 FirewallEngine | ✅ | 规则匹配 (IP+Port) + 流量整形 (抗 DDoS) + Shell 命令 + Lua 绑定 |
| 网络 | 数据包捕获 PacketCapture | ✅ | `/dev/pcap0` 字符设备，BPF 风格过滤器，Wireshark .pcap 格式 |
| 网络 | 网络扫描 NetworkScanner | ✅ | 端口/主机/服务/漏洞 4 模块，TaskNotify Worker 池，Lua `aurora.scan.*` 绑定 |
| 网络 | 分布式软总线 DistributedSoftBus | ✅ | HMAC-SHA256 挑战应答 + 防重放 + 能力白名单 + LRU 路由表 + DDoS 限速 |
| 安全 | Secure Storage HAL (密钥供应) | ✅ | `hal/secure_storage_hal.hpp` 抽象 + `secure_storage_stub.cpp` 弱符号 fail-closed；miband8 从 customer OTP 读取每设备唯一 SoftBus 密钥，未烧录时拒绝返回（`#error` 已移除） |
| 网络 | BLE 协议栈 (基础) | 🚧 | 连接状态机 + HCI 命令编码 + GATT + Ed25519；`net/ble/` 4 个 header-only 安全模块 |
| 网络 | BLE 真实硬件驱动路径 | ✅ | `hal_ble_impl.cpp` 把 HalBle 抽象映射为 HCI Command；`hci_packet.hpp` 编解码 + `hci_event_dispatch.hpp` 将 Controller 事件投递到 BleScanner/BleIds/BleMitmDetector/GattAuditor；`hci_uart_transport.cpp` H4 UART 打通 `on_hardware_rx`；已接入 miband8 构建 |
| 网络 | 局域网隐身伪装 StealthIdentity | ✅ | MAC OUI 厂商欺骗 + DHCP 主机名伪装 + DHCP Option 55 指纹伪装，Kconfig 可选 7 种身份预设 |
| 网络 | BLE 隐身伪装 BleStealth | ✅ | GAP Flags 隐藏 (不可发现) + iBeacon 制造商数据伪造 (Apple 0x004C)，Kconfig 可选 4 种 Apple 外设预设 |
| 网络 | WiFi 安全审计 WirelessIDS | 🚧 | 5 模块 header-only 完整；驱动 .cpp 已实现，但 **未加入 CMakeLists.txt SOURCES**，不参与编译 |
| IPC/安全 | IPC (seL4 风格 Endpoint) + 类型化消息 | ✅ | Endpoint::call/receive/reply，IpcMessage<T> 模板，编译期类型安全 |
| IPC/安全 | 能力空间 CSpace (lookup/delete/derive/mint/revoke/grant) | ✅ | 16 槽位，权限降级检测，全局撤销 |
| IPC/安全 | 安全监控 SecurityMonitor | ✅ | 心跳监考 + 看门狗联动 + 堆压力检测 + 栈溢出计数 |
| IPC/安全 | 看门狗管理 WatchdogManager | ✅ | 80% idle 阈值喂狗，弱符号透明接入调度循环 |
| IPC/安全 | 系统调用审计 AuditEngine | ✅ | 128 槽环形缓冲 + 规则引擎 + `/proc/audit_log` |
| IPC/安全 | 安全启动 (Ed25519 + OTA) | ✅ | Ed25519 验签 + A/B 双分区断电安全，生产构建 `#error` 强制真实密钥 |
| 显示 | 帧缓冲 + 脏区域渲染 | ✅ | set_pixel/fill_rect 自动标记脏矩形，flush 只刷新变动区域 |
| 显示 | OLED 驱动 (Mock) | ✅ | SPI 接口框架 + 窗口化局部更新协议，无真实 SPI/DMA |
| 显示 | SSD1306 驱动 (I2C OLED) | ✅ | 0.96" 单色 128×64 SSD1306 I2C 屏真实驱动，复用 `II2cHal`，页式显存 + 脏页刷新，内嵌 5×7 字模，零动态分配 |
| 显示 | ST7789 驱动 (MiBand) | 🚧 | 半实现，DMA 忙等 + 注释 |
| 显示 | Renderer2D 2D 引擎 | ✅ | 完整实现 |
| 输入 | InputEvent / TouchDriver / GestureRecognizer | ✅ | 统一事件抽象，触摸驱动，7 种手势识别 (Tap/双按/长按/上下左右滑) |
| 输入 | 触摸驱动 (真实硬件) | ❌ | QEMU 仿真状态机，非真实硬件 |
| 电源 | 5 级功耗管理 (ACTIVE→DIM→IDLE→SLEEP→CRITICAL) | ✅ | 固件实际状态机 (`kernel/core/power/power_manager.hpp`)，联动 30/15/1/0fps 帧率，含抬腕唤醒与 BLE 状态绑定 |
| 电源 | 充电管理 | ✅ | 电池状态机 (DISCHARGING/PRE_CHARGE/FAST_CHARGE/CHARGE_DONE/FAULT) |
| 传感器 | 传感器框架 (Zephyr 风格) | ✅ | SensorDriver 抽象，HeartRateSensor (模拟 75 BPM)，Accelerometer |
| 传感器 | 健康算法 (PPG 滤波 + 计步 + 活动识别) | ✅ | 滑动窗口 + IIR 低通滤波器，活动状态识别 (静止/行走/跑步/睡眠) |
| UI | 页面栈导航 ScreenNavigator | ✅ | Push/Pop/Replace，平移转场动画，页面生命周期 |
| UI | 表盘 Complication 引擎 | ✅ | 数据驱动 UI，预定义心率和计步回调（数据变化时才触发局部重绘） |
| UI | 基础控件 (button, text_view, arc_progress) | ✅ | 3 种基础控件 |
| 运行时 | Lua 5.4.6 小程序引擎 | ✅ | 自定义 KernelHeap 分配器，Lua ↔ UI 绑定，传感器 API 暴露 |
| 运行时 | ELF 动态加载器 | ✅ | ARM Thumb ELF 加载，地址回绕校验，W^X 保护，MPU 沙盒 |
| 运行时 | 应用生命周期 ACB | ✅ | FOREGROUND/BACKGROUND/SUSPENDED 状态机，动态优先级调整 |
| 运行时 | 意图引擎 IntentEngine (legacy) | ✅ | 基于传感器步数规则决策，自动提升/降级健身应用优先级 (`ai/intent_engine.hpp`) |
| AI 运行时 | February 意图引擎 (EventBus + 规则表 + 唤醒词门控) | ✅ | 传感器驱动 (步数/心率/电量) + 文本规则匹配，CooldownGate + LevelLatch 防抖，经 EventBus 发布意图 |
| AI 运行时 | February Planner (固定深度规划器) | ✅ | 单意图 → 有序动作序列 (≤4 步)，静态 PlanRule 表，`set_rules()` 运行时替换，零堆分配 |
| AI 运行时 | February Persona (人格/语音) | ✅ | 固定回复模板 + 4 种语调 (Calm/Friendly/Professional/Minimal)，负责语音文本输出 |
| AI 运行时 | February SoftBus (跨设备传输) | ✅ | 二进制意图帧编解码 + 传输适配层 + OpenHarmony 适配器，远程意图可让渡给本地处理 |
| AI 运行时 | February PeerTable + 板级绑定 | ✅ | 固定容量对等节点表 (last-seen/TX-RX/会话)，`board_bind.hpp` 10 行完成板级接入，Kconfig 可裁剪 |
| 移植 | Cortex-M3 (LM3S6965, QEMU) | ✅ | 主 HIL 平台，完整可运行 |
| 移植 | RISC-V RV32 (QEMU) | ✅ | 独立异常向量，完整可运行 |
| 移植 | Cortex-M0+ (Nucleo-L031K6) | ✅ | 裸板适配，64KB Flash / 8KB RAM 限制，最大任务数 4 |
| 移植 | Cortex-M4F (MiBand 8) | ✅ | `apps/watch/miband_main.cpp` `kernel_main` → `miband_kernel_main()` 完整启动：时钟树初始化、UI 渲染线程 + 传感器/BLE 守护线程 + Idle 线程、SysTick 1ms tick、首次上下文切换进入调度器；CI build-miband8 构建并通过 576KB Flash 大小检查 |
| 移植 | AArch64 (ARMv8-A) | ❌ | 仅探索代码，无 CMake 构建目标 |
| 实验性 | 通知中心 NotificationCenter | ✅ | 优先级堆队列 + BLE 协议解析 + Overlay 横幅/全屏绘制 |
| 实验性 | NFC 卡模拟 | 🚧 | 控制器抽象，有 .cpp 实现 |
| 实验性 | 摄像头 | ❌ | 仅抽象接口，占位 |
| 实验性 | SoftGPU | ❌ | 源存在，无 CMake 目标 |
| 实验性 | GUIX 图形框架 | 🚧 | 合成器 + 窗口，部分实现 |
| 实验性 | WiFi 驱动 (RTL8187L/RTL8812AU) | 🚧 | 驱动已实现，缺物理 USB 硬件 |
| 工程 | 主机单元测试 | ✅ | 221 个测试 (GoogleTest, ctest 发现) |
| 工程 | CI/CD (GitHub Actions) | ✅ | 13 jobs：4 目标固件构建 + QEMU 冒烟 + HIL + 单元测试 + ASAN+UBSAN + clang-tidy + cppcheck + 覆盖率 + 模糊测试 + 性能基准 + 固件大小对比 + Release |
| 工程 | 性能度量 Metrics (DWT) | ✅ | DWT 采样 + QEMU 基准测试套件 (benchmark_runner.py 自动化采集 ProcFS 指标输出 benchmark_report.md) |

---

## 快速开始

### 🛠️ 环境准备

| 工具 | 说明 |
|------|------|
| `arm-none-eabi-gcc` / `arm-none-eabi-g++` | ARM GNU Toolchain |
| `gcc-riscv64-unknown-elf` | RISC-V GNU Toolchain (可选) |
| `CMake` >= 3.20 | 构建系统 |
| `Make` / `Ninja` | 构建后端 |
| `QEMU` | qemu-system-arm + qemu-system-misc |
| `Python 3` + `kconfiglib` | Kconfig 配置生成 |

```bash
pip install kconfiglib
```

### 🚀 构建并运行 (LM3S6965, QEMU)

```bash
git clone --recursive https://github.com/jencaoking/auroraOS.git
cd auroraOS

# 生成 Kconfig 配置
python scripts/genconfig.py

# 构建
mkdir build && cd build
cmake -DBOARD=lm3s6965-qb ..
make -j8

# 在 QEMU 中运行
qemu-system-arm -M lm3s6965evb -nographic -kernel auroraOS.elf
```

启动后进入 `aurora>` 终端，支持 `help`、`ps`、`free`、`cat`、`ifconfig`、`ping`、`udpsend` 等命令。

### 🔧 构建其他目标

```bash
# RISC-V RV32
cmake -DBOARD=qemu_rv32_virt -DCMAKE_TOOLCHAIN_FILE=../config/toolchain_rv32.cmake ..
make -j8
qemu-system-riscv32 -M virt -nographic -kernel auroraOS.elf

# Cortex-M0+ (Nucleo-L031K6)
cmake -DBOARD=nucleo_l031k6 -DCMAKE_TOOLCHAIN_FILE=../config/toolchain.cmake ..
make -j8

# MiBand 8 (Apollo3 M4F)
cmake -DBOARD=miband8 -DCMAKE_TOOLCHAIN_FILE=../config/toolchain_miband.cmake ..
make -j8
```

### 🧪 构建并运行主机单元测试

```bash
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j
ctest --test-dir build_tests --output-on-failure
```

### ⚙️ 持续集成流水线 (CI/CD 13 Jobs)

GitHub Actions 工作流包含 13 个独立 Job，保证多架构固件与算法质量：

| 分类 | Job 名称 | 触发与验证目标 | 门禁性质 |
| :--- | :--- | :--- | :--- |
| **固件构建** | `build-lm3s6965` | TI LM3S6965 (Cortex-M3) 固件 + QEMU 自动化 HIL 冒烟测试 | 阻塞门禁 |
| | `build-rv32` | RISC-V RV32IMAC (QEMU Virt) 固件编译 | 阻塞门禁 |
| | `build-miband8` | 小米手环 8 (Ambiq Apollo3 Blue / M4F) 固件编译 + 576KB 显存/Flash 检查 | 阻塞门禁 |
| | `build-m0plus` | ST Nucleo-L031K6 (Cortex-M0+) 固件编译 + 8KB SRAM 资源检查 | 阻塞门禁 |
| **质量与安全** | `unit-tests` | 221 个 GoogleTest 单元与集成测试 (`ctest`) | 阻塞门禁 |
| | `sanitize` | ASAN (AddressSanitizer) + UBSAN 运行时内存安全检查 | 阻塞门禁 |
| | `static-analysis` | `clang-tidy` 全固件源码静态检查，生成并归档诊断报告制品 | 报告归档 |
| | `cppcheck` | `cppcheck` 驱动与 OSAL 适配层静态代码分析 | 报告归档 |
| **度量与测试** | `coverage` | `lcov` / `genhtml` 代码覆盖率报告生成并上传 | 报告归档 |
| | `fuzz` | LibFuzzer 针对 IPC 与系统调用解析器的模糊测试 (30s) | 阻塞门禁 |
| | `benchmark` | QEMU 中运行真实基准工作负载，自动化提取 ProcFS 延迟生成报告 | 报告归档 |
| | `size-compare` | 跟踪 Flash / RAM 段大小变动与预算上限 | 报告归档 |
| **发布** | `release` | Git Tag (v*) 触发自动化打包全目标固件与 Release 发布 | 自动化发布 |

---

## 核心架构

auroraOS 采用经典的**纵向分层 + 横向能力隔离**架构：自顶向下分为应用、内核、子系统（VFS/网络）、驱动、架构抽象、板级六层，层间仅通过明确定义的边界（系统调用、HAL 接口、Kconfig 裁剪）通信，严格禁止反向依赖（如内核不依赖应用或实验性代码，详见 `AGENTS.md` 架构边界约束）。下方给出分层全景图与关键的数据/控制流向。

```
                                        ┌─────────────────────────────────────────┐
                                        │              应用层  apps/               │
                                        │  Shell · Lua MiniProgram · ELF Loader   │
                                        │  February AI 运行时 · WatchFace 表盘     │
                                        │  AppLifecycle · DistributedSoftBus       │
                                        └───────────────────┬─────────────────────┘
                                                            │  ║ SVC / ECALL 系统调用边界 ║
                                                            │  (用户态 → 内核态 唯一入口)
                                        ┌───────────────────▼─────────────────────┐
                                        │           内核核心  kernel/               │
                                        │  调度: Scheduler(5级抢占)·FrameScheduler  │
                                        │  同步: MutexPI·Semaphore·MsgQueue·Signal │
                                        │  内存: KernelHeap·MemoryPool·MPU         │
                                        │  安全: IPC Endpoint·CSpace·AuditEngine   │
                                        │        SecurityMonitor·Watchdog         │
                                        └───────────────────┬─────────────────────┘
                                            ┌───────────────┼────────────────┐
                                            │               │                │
                              ┌─────────────▼──────┐  ┌──────▼────────┐  ┌────▼──────────┐
                              │  文件系统  vfs/    │  │  网络  net/    │  │  服务 services/│
                              │ VNode·RamFS·ProcFS│  │ lwIP·Firewall  │  │ vfs/fw 接入   │
                              │ LittleFS+Photon   │  │ Scan·SoftBus   │  │ 固件构建      │
                              │ Cache             │  │ Stealth·Ble    │  │               │
                              └─────────────┬──────┘  └──────┬────────┘  └────┬──────────┘
                                            │               │                │
                                        ┌───────────────────▼─────────────────────┐
                                        │            驱动层  drivers/              │
                                        │  display(帧缓冲/SSD1306/ST7789/OLED-Mock)│
                                        │  input · sensor · storage · usb · power  │
                                        │  watchdog   (全部经 HAL 抽象解耦)          │
                                        └───────────────────┬─────────────────────┘
                                                            │  HAL 抽象边界 (II2cHal/ISpiHal/IGpioHal)
                                        ┌───────────────────▼─────────────────────┐
                                        │        架构抽象层  arch/ + hal/          │
                                        │  Arch::(开关中断/wfi/触发切换)·arch_impl   │
                                        │  start_first_task · systick · 栈初始化    │
                                        └───────────────────┬─────────────────────┘
                                                            │
                                        ┌───────────────────▼─────────────────────┐
                                        │        板级支持  boards/ + boot/         │
                                        │  ti/lm3s6965 · st/nucleo-l031k6          │
                                        │  qemu/rv32_virt · xiaomi/miband8          │
                                        └─────────────────────────────────────────┘
```

设计要点：

- 依赖方向单向向下：应用 → 服务 → 子系统 → 内核/系统调用 → HAL/架构 → 硬件。内核核心只暴露接口供上层使用，自身不持有应用、UI 或实验性代码引用（`AGENTS.md` §5）。
- 能力隔离：用户态应用只能通过 `syscall/` 定义的能力化系统调用进入内核，能力由 `CSpace` 签发与回收，越权访问在 IPC 入口即被拒绝。
- HAL 解耦：驱动不直接触碰寄存器，统一经 `hal/i2c_hal.hpp`、`hal/spi_hal.hpp`、`hal/gpio_hal.hpp` 抽象，板级在 `boards/` 中实现 `get_*_hal()` 工厂，因此同一驱动可跨 MCU 复用（如 SSD1306 驱动在 Cortex-M0+ 与 M4F 上无需改动）。
- 可裁剪：所有子系统和 AI 模块由 Kconfig 特性开关控制，未启用的目标不产生代码体积开销。

---

## 🤖 AI 运行时 (February)

auroraOS 内置一套轻量级、可裁剪的**嵌入式 AI 运行时** (`ai/`)，代号 **February**，面向智能手表与物联网终端，负责把传感器信号与跨设备消息转化为可执行的意图 (Intent)。它由两套实现并存：遗留的 `ai/intent_engine.hpp`（传感器步数规则直接升降应用优先级），以及重新设计的模块化运行时 `ai/february/`（Phase 2.2 跨设备意图引擎）。February 的设计目标是**确定性、零堆分配、固定容量、可静态配置**，不使用任何大模型推理，全部路径可在资源受限目标 (Cortex-M0+, 8KB RAM) 上编译运行。

### 模块构成

February 运行时由五个协作模块组成，全部为 header-only 设计，通过 `EventBus` 解耦：

- **EventBus**：中枢事件总线，订阅者以 `callback(Event)` 注册，发布零拷贝广播；所有模块通过它异步通信，避免直接耦合。
- **IntentEngine**：意图引擎。订阅平台事件（步数、心率、电量、消息通知等），用文本规则表做关键词匹配，经 `CooldownGate`（冷却门控）与 `LevelLatch`（电平锁存）两级防抖后，通过 EventBus 发布结构化意图。规则表由 `add_rule()` 在编译期/运行期填充，引擎自身不持有任何外部状态副本。
- **Planner**：固定深度规划器。接收单个意图，按静态 `PlanRule` 表展开为有序动作序列（单意图最多 4 步动作，展开深度固定为 1 层），`set_rules()` 可在运行时整体替换规则表。规划过程无堆分配，输出直接供执行层消费。
- **Persona**：人格/语音层。持有固定回复模板与四种语调（Calm / Friendly / Professional / Minimal），将意图映射为面向用户的自然语言文本，是语音/提示内容的来源。
- **SoftBus**：跨设备传输层。提供二进制意图帧的编解码 (`codec.hpp`)、传输适配层 (`transport.hpp`) 与 OpenHarmony 适配器 (`oh_adapter.hpp`)；远端到达的意图可被让渡 (defer) 给本地引擎处理，实现设备间意图协同。

对等节点信息由 **PeerTable** 维护（固定容量，记录 last-seen、TX/RX 计数与会话状态），接入具体硬件只需在 `board_bind.hpp` 中约 10 行即可完成板级绑定。所有 AI 模块均通过 Kconfig 特性开关裁剪，未启用的目标不产生任何代码体积开销。

### 关键文件

| 文件 | 职责 |
|------|------|
| `ai/intent_engine.hpp` | 遗留意图引擎：传感器步数规则驱动的应用优先级升降 |
| `ai/february/types.hpp` | 通用类型：Event / Intent / Action / DeviceId |
| `ai/february/event_bus.hpp` | 订阅/发布事件总线 |
| `ai/february/intent_engine.hpp` | 规则表 + 唤醒词门控的意图识别 |
| `ai/february/planner.hpp` | 静态规则表的固定深度规划器 |
| `ai/february/persona.hpp` | 模板回复与语调管理 |
| `ai/february/softbus/*.hpp` | 意图帧编解码、传输适配、OpenHarmony 适配 |
| `ai/february/peer_table.hpp` | 固定容量对等节点表 |
| `ai/february/board_bind.hpp` | 板级接入适配（约 10 行绑定） |

---

## 🖥️ 显示驱动 (Display)

auroraOS 的显示驱动集中在 `drivers/display/`，按协议与屏型分为三层：**帧缓冲 + 脏区域渲染**内核、`Renderer2D` 2D 引擎，以及具体的屏驱动（SSD1306 I2C OLED、ST7789 SPI LCD、OLED Mock）。所有已适配硬件的完整规格、初始化序列、接线与最小接入示例，请参阅 **[硬件适配指南](DOCS/porting/hardware.md)**。

---

## 🥷 局域网隐身伪装 (Stealth Identity)

auroraOS 内置一套局域网隐身伪装引擎 (`net/stealth_identity.hpp`)，设备在接入有线/无线局域网时，会在 `netif_add` 与 `dhcp_start` 之前自动伪装成局域网中最无害的普通办公设备，使路由器 DHCP 客户端列表与网络行为分析仪无法识别其真实内核身份 (STM32/lwIP)。伪装由 Kconfig 的 "Stealth Identity Preset" choice 编译期选定，包含三层：

- **MAC 地址厂商欺骗 (OUI Spoofing)**：在网卡初始化前，将 MAC 前三个字节改写为设备厂商 OUI。可选前缀包括 `10:DD:B1` (Apple Inc.)、`00:26:55` (HP 惠普打印机)、`8C:F5:A3` (Samsung 三星)。后三个字节由 DWT 硬件高精度周期计数器混合位旋转生成高熵随机值，并确保第 7 位为 0 (单播)、第 8 位为 0 (全局唯一地址)，避免局域网 MAC 冲突。MAC 通过 `NetDevice::set_mac_address()` 写入 `StellarisEth` 的 MAC 地址过滤寄存器。
- **DHCP 主机名伪装 (Hostname Masking)**：开启 `LWIP_NETIF_HOSTNAME` 后，将默认主机名由 `lwip`/`stm32` 改写为所选身份，例如 `HP-LaserJet-M402dn`、`iPad-of-Staff`、`iPhone-15`、`MacBook-Pro`、`Galaxy-S24`，作为 DHCP Option 12 在路由器客户端列表中隐藏真实身份。
- **DHCP Option 55 指纹伪装**：专业网络行为分析仪通过 DHCP 请求的参数列表 (Option 55 参数顺序) 判定操作系统类型。auroraOS 通过 `AURORA_DHCP_OPTION55_CUSTOM` 宏 (在 `3rdparty/lwip/.../dhcp.c` 最小化 patch) 让 `adapter/net/aurora_dhcp_opts.c` 接管参数请求列表，提供 iOS 15.x (10 参数，含 WPAD)、Windows 10/11 (12 参数，含 NetBIOS/MS Classless Route)、HP 打印机固件 (6 参数极简列表) 等指纹，参数顺序与条目数量均匹配真实设备。

可用预设 (Kconfig choice `Stealth Identity Preset`，默认 `STEALTH_HP_LASERJET`)：

| 预设 | MAC OUI | DHCP 主机名 | Option 55 指纹 |
|------|---------|-------------|----------------|
| `STEALTH_APPLE_IPAD` | 10:DD:B1 | iPad-of-Staff | iOS 15.x |
| `STEALTH_APPLE_IPHONE` | 10:DD:B1 | iPhone-15 | iOS 15.x |
| `STEALTH_APPLE_MACBOOK` | 10:DD:B1 | MacBook-Pro | iOS 15.x |
| `STEALTH_HP_LASERJET` (默认) | 00:26:55 | HP-LaserJet-M402dn | HP Printer |
| `STEALTH_HP_OFFICEJET` | 00:26:55 | HP-OfficeJet-Pro9010 | HP Printer |
| `STEALTH_SAMSUNG_GALAXY` | 8C:F5:A3 | Galaxy-S24 | lwIP 默认 |
| `STEALTH_NONE` | 关闭伪装 | 关闭伪装 | lwIP 默认 |

集成位置：`apps/net_app.cpp` 的 `tcpip_init_done_cb` 在 `netif_add` 前调用 `StealthIdentity::apply()` + `eth.init()` (Layer 1)，在 `netif_set_up` 后、`dhcp_start` 前设置 `g_netif.hostname` (Layer 2)，Layer 3 由编译期 C 文件自动注入。RISC-V 目标通过 `#ifndef ARCH_RISCV32` 守卫跳过 `StellarisEth` 相关代码，仅保留主机名与 Option 55 伪装。

---

## 📡 BLE 广播隐身伪装 (BleStealth)

auroraOS 内置一套 BLE 蓝牙广播隐身引擎 (`net/ble/ble_stealth.hpp`)，当设备开启蓝牙以便手机连接和控制时，自动伪装成周边最常见的 Apple 智能小外设，混入蓝牙无线电背景噪音中，避免被 BLE IDS 与周边手机的系统蓝牙设置列表识别为可疑设备。伪装由 Kconfig 的 "BLE Stealth Identity Preset" choice 编译期选定，包含两层：

- **GAP Flags 隐藏 (非可发现模式)**：在构建广播 AD 数据时，故意不广播 Limited Discoverable (`0x01`) 和 General Discoverable (`0x02`) 标志位，仅保留 `BR/EDR Not Supported` (`0x04`)。效果是周边任何人的手机进入系统蓝牙设置扫描列表时，完全搜不到该设备；但主人的手机控制端 APP 知道其物理 MAC 地址，可通过定向连接强行连入并正常使用 GATT 服务。
- **iBeacon 指纹欺骗 (Apple iBeacon Spoofing)**：在广播包中插入 Manufacturer Specific Data (AD Type `0xFF`)，公司 ID 设为 `0x004C` (Apple Inc.，小端序 `0x4C, 0x00`)，payload 完全符合 Apple iBeacon 规范 (iBeacon Type `0x02`、21 字节 UUID + Major/Minor + TX Power)。周边蓝牙 IDS 和安全检测器将其归类为合法 Apple AirTag 追踪器或 AirPods，过路行人的 iPhone 甚至会弹出无害的 Find My 配对提示。

隐身模式下广播的完整 AD 数据包 (≤30 字节，BLE 规范上限 31 字节)：

```
[0x02, 0x01, 0x04,           ← Flags: 非可发现 (仅 BR/EDR Not Supported)
 0x1A, 0xFF, 0x4C, 0x00,     ← Manufacturer Data: Apple 公司 ID
 0x02, 0x15,                  ← iBeacon Type + Data Length
 UUID(16), Major(2), Minor(2), TX_Power(1)]
```

`BleStealth::build_advertisement()` 在 `BleManager::init()` 被调用前构建整个 AD buffer，通过新增的 `HalBle::start_advertising_raw()` 直接注入底层 HAL，不做任何包装。断开重连 (`EVENT_DISCONNECT`) 同样走隐身路径，确保射频生命周期内身份一致。

可用预设 (Kconfig choice `BLE Stealth Identity Preset`，默认 `STEALTH_BLE_AIRTAG`)：

| 预设 | GAP Flags | iBeacon TX Power | 模拟设备 |
|------|-----------|------------------|----------|
| `STEALTH_BLE_AIRTAG` (默认) | 非可发现 | -59 dBm | Apple AirTag 追踪器 |
| `STEALTH_BLE_AIRPODS_PRO` | 非可发现 | -54 dBm | Apple AirPods Pro |
| `STEALTH_BLE_AIRPODS` | 非可发现 | -55 dBm | Apple AirPods (标准版) |
| `STEALTH_BLE_APPLE_PENCIL` | 非可发现 | -62 dBm | Apple Pencil (第 2 代) |
| `STEALTH_BLE_NONE` | 可发现 (正常) | 无 iBeacon | 正常广播 Aurora_MiBand8 |

集成位置：`experimental/net/ble/ble_stack.cpp` 的 `ble_start_advertising()` 辅助函数在 `BleManager::init()` 和 `daemon_task()` 断开事件中统一调用，根据 `ble_stealth_preset_from_config()` 选择正常路径 (`HalBle::start_advertising`) 或隐身路径 (`BleStealth::build_advertisement` → `HalBle::start_advertising_raw`)。BLE 隐身依赖 `CONFIG_BLE_ENABLED` Kconfig 开关，由板级 `ENABLE_BLE_5_2` 宏自动激活。

---

## 🗺️ 开发路线图

基于当前功能状态，以下方向仍是进行中或尚未落地，可作为后续贡献重点：

- ✅ **SoftBus 密钥供应 (Secure Storage)**：`net/distributed_bus.hpp` 的密钥加载 `#error` 已移除，改为 `hal/secure_storage_hal.hpp` 抽象；miband8 从 customer OTP 读取每设备唯一密钥，其余板级走弱符号 fail-closed。
- 🚧 **BLE 协议栈完整化**：真实硬件驱动路径（HalBle → HCI → 安全模块）已打通并接入 miband8 构建；剩余工作为板级 UART 中断喂数 (`feed_rx_byte`) 与 NimBLE Host 桥接。
- 🚧 **WiFi 安全审计 WirelessIDS 接入编译**：驱动 `.cpp` 已实现，需加入 `CMakeLists.txt SOURCES` 以参与构建。
- 🚧 **ST7789 显示驱动 (MiBand)**：完成 DMA 路径并移除忙等占位。
- 🚧 **GUIX 图形框架**：推进合成器与窗口实现。
- 🚧 **WiFi 驱动 (RTL8187L / RTL8812AU)**：等待物理 USB 硬件接入。
- ❌ **触摸驱动真实硬件**：当前为 QEMU 仿真状态机。
- ❌ **AArch64 MMU + VAS**：仅抽象接口，无 CMake 构建目标。
- ❌ **摄像头 / SoftGPU**：仅抽象接口或占位源，无构建目标。

---

## 📜 许可证

本项目采用 **CAOSL v2.0 (JENCAO Custom Advanced Open Source License v2.0)** 开源许可证。详见 [LICENSE](LICENSE) 文件。

这是一个自定义开源协议，包含强 Copyleft、专利保护、道德使用限制和商业双授权条款。

第三方依赖保留各自许可证：

- lwIP: BSD-3-Clause
- LittleFS: BSD-3-Clause
- Lua 5.4.6: MIT
- ed25519: MIT

---

<div align="center">

**auroraOS** · 从学习演示到多分支 Lua 化智能手表 RTOS 平台

<p>
  <a href="https://github.com/jencaoking/auroraOS">Repository</a> ·
  <a href="LICENSE">License</a> ·
  <a href="https://github.com/jencaoking/auroraOS/issues">Issues</a>
</p>

</div>
