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

**万物互联智能 AIOS —— 面向全场景智能终端的 AI 驱动微内核操作系统**

> ARM Cortex-M0+/M3/M4 · RISC-V RV32IMAC · lwIP TCP/IP · Lua 5.4.6 · MPU 内存保护 · 分布式软总线 · 端侧 AI 意图引擎

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
- [射频频谱感知 (Spectrum Sensing)](#射频频谱感知-spectrum-sensing)
- [GUIX 图形框架 (GUIX Engine)](#guix-图形框架-guix-engine)
- [开发路线图](#开发路线图)
- [发展时间线](#发展时间线)
- [许可证](#许可证)

</details>

---

## 项目简介

auroraOS 是一个万物互联的智能 AIOS 平台，其底层的微内核被正式命名为 **July**（July Kernel）。它在精简的代码体积内实现了优先级抢占调度、完整 TCP/IP 网络协议栈、MPU 内存隔离、Lua 小程序引擎、帧感知渲染、分布式软总线等特性，并在最新架构中完全转向了**基于 Capability 与 IPC 驱动的现代微服务架构 (Microservice Architecture)**。依托端侧 AI 意图引擎与分布式软总线，auroraOS 能够让智能手表、物联网终端与各类智能设备无缝互联、协同感知并自主决策，成为面向全场景的万物互联智能 AIOS。

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

auroraOS 作为万物互联智能 AIOS，以「小而全、可互联、有智能」的嵌入式内核为目标，核心亮点包括：

- 🧠 **确定性微内核**：$O(1)$ 32 级优先级抢占调度（单指令硬件 CLZ 检索，含 ISR-DPC / Audio / Sensor / Net-RX 可穿戴细分层级） + Cortex-M4F 硬件 FPU 惰性保存（Lazy Stacking，减少 30% 上下文切换耗时） + BASEPRI 选择性中断掩蔽 + 动态自适应 VSync 帧感知调度。
- 🔐 **能力安全模型**：seL4 风格 CSpace 能力空间（16 槽位 `uint16_t` 硬件位图管理与 CTZ $O(1)$ 空闲分配）、优先级有序 IPC Endpoint 与同步调用 PIP 优先级继承、系统调用审计与 Ed25519 安全启动。
- 🛡️ **MPU 内存隔离**：Cortex-M0+/M3/M4F 下 Flash 只读与特权态隔离，创新采用 8×512B 子区域禁用（SRD）硬件栈哨兵，实现零内存浪费的高可靠栈溢出拦截。
- ⚡ **实时内存管理**：内核全量引入 TLSF（Two-Level Segregated Fit）$O(1)$ 分配器（384 分箱） + FastRAM 8 字节对齐加速（TCB/VNode 适配 DTCM/CCMRAM），为 Lua 5.4.6 配备独立 32KB 私有隔离堆（`LuaHeap`），彻底杜绝 GC 内存碎片污染内核。
- 🌐 **万物互联 · 分布式软总线**：基于 lwIP 2.x 全协议栈、防火墙、包捕获、扫描器之上构建的分布式软总线，支持设备间 HMAC 鉴权、防重放与意图协同，让手表、IoT 终端与智能设备无缝组网。
- 🥷 **隐身伪装**：局域网 (StealthIdentity) 与 BLE (BleStealth) 双层身份欺骗，混入周边设备背景。
- 📜 **Lua 小程序引擎**：Lua 5.4.6 以专用 `LuaHeap` 隔离运行，开放传感器与 UI API 给第三方小程序。
- 🤖 **嵌入式 AI 运行时**：February 跨设备意图引擎，零堆分配、可静态配置，适配 8KB RAM 级别设备，为互联终端注入本地智能。
- 💾 **掉电安全存储**：LittleFS + PhotonCache LRU 页缓存，脏页延迟写与重试。
- 📡 **射频频谱感知**：`drivers/rf/` 提供频谱传感器抽象（Q8 定点功率）、异常信号检测（噪声底 EMA + 四类异常）与干扰识别（连续波/窄带/宽带/扫频/脉冲五类物理层干扰），全定点零堆分配，可与 SecurityMonitor 联动告警。

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
├── drivers/              # 驱动层 (显示[帧缓冲/SSD1306/ST7789/OLED-Mock], 输入, 传感器, 射频[频谱感知], USB, 存储, 看门狗, 电源)
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
| `drivers/` | 驱动层 | 硬件外设驱动 | 显示（帧缓冲/SSD1306/ST7789/OLED-Mock）、输入、传感器、射频（频谱感知 `drivers/rf/`）、存储、USB、看门狗、电源；全部经 `hal/` 抽象 |
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
| `tests/` | 测试 | 验证 | 362 个 GoogleTest 单元/集成/压力测试，覆盖率与模糊测试支撑 |
| `3rdparty/` | 依赖 | 第三方库 | lwIP、Lua 5.4.6、LittleFS (submodule)、ed25519；vendor 代码不手工改 |

---

## 功能状态

图例：<code>✅ 已完成</code> &nbsp;·&nbsp; <code>🚧 进行中</code> &nbsp;·&nbsp; <code>❌ 未实现</code>

| 子系统 | 功能 | 状态 | 说明 |
|--------|------|:----:|------|
| 内核调度 | O(1) 32 级优先级抢占调度器 (含 ISR-DPC/Audio/Sensor/Net-RX) | ✅ | 就绪位图 + 单指令硬件 CLZ 检索，侵入式循环链表 O(1) 调度 |
| 内核调度 | 帧感知调度 FrameSchedulerV2 (自适应 VSync) | ✅ | 真实 VSync 周期实时测量 + EMA 平滑，动态预测 expected_idle_ticks |
| 内核调度 | Cortex-M4F FPU 惰性保存 (Lazy Stacking) | ✅ | SCB_FPCCR ASPEN/LSPEN 硬件惰性压栈，PendSV 检查 EXC_RETURN bit 4，节省 30% 上下文切换时间 |
| 同步原语 | BASEPRI 中断选择性掩蔽 (Cortex-M3/M4/M4F) | ✅ | 仅掩蔽低于等于 `0x50U` 的系统调用中断，高优先级实时中断 (BLE) 零抖动直达 |
| 同步原语 | Mutex (PIP 优先级继承 + IPCP 优先级天花板 + 死锁检测) | ✅ | 传递性优先级继承、立即优先级天花板协议 (IPCP)、跨任务等待图闭环死锁检测 (EDEADLK)、递归加锁、超时机制、RAII UniqueLock |
| 同步原语 | Semaphore (零堆分配池 + 定时唤醒) | ✅ | 基于 `IrqGuard` 的计数信号量，支持超时等待与调度器休眠唤醒；POSIX `sem_*` 接口采用静态内存池 (`s_posix_sem_pool[16]`) 杜绝动态分配 |
| 同步原语 | MessageQueue SPSC / TaskNotify / Signal | ✅ | 无锁 SPSC 环形队列；32 位零开销通知；POSIX signal/kill/raise 与调度器安全信号分发 |
| 定时器 | 进程级/任务级内核定时器 ProcessTimer | ✅ | 零堆分配静态管理，支持相对/绝对、单次/周期 (零漂移) 定时，支持 POSIX 信号、IPC 通知与事件唤醒，与 SysTick 和 Tickless 深度协同，任务退出自动级联回收 |
| 内存管理 | TLSF 实时内核堆分配器 KernelHeap | ✅ | $O(1)$ 分配与释放 (<2μs)，384 个独立分箱，物理双向边界标记即时合并，无碎片整理开销 |
| 内存管理 | FastRAM 8 字节对齐与高速 RAM 优化 | ✅ | `memory_attributes.hpp` 强制 TCB/VNode 8 字节对齐，适配 Cortex-M LDRD/STRD 与 DTCM/CCMRAM |
| 内存管理 | MemoryPool (O(1) 固定块分配器) | ✅ | 空闲链表，边界检查，双重释放检测 |
| 内存保护 | MPU Sub-Region Disable (SRD) 硬件栈哨兵 | ✅ | 4KB 区域切分 8×512B 子区域，禁用 subregion 0 实现零内存浪费的硬件栈溢出防御 |
| 内存保护 | AArch64 MMU + VAS | ✅ | 4级4KB页表管理 (`arch/arm/cortex-a/mmu/mmu_manager.cpp`)、PTE位域与MAIR配置、虚实转换、权限修改与递归回收，支持 QEMU virt 目标与 CI 构建测试 |
| 存储 | VFS (VNode 多态) + RamFS + ProcFS | ✅ | open/read/write/close/lseek/ioctl 完整接口，路径遍历防护，8KB M0+ 目标自适应缩容 |
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
| 网络 | WiFi 安全审计 WirelessIDS | ✅ | 5 模块 header-only 完整；USB 驱动与监控任务 .cpp 已合入 CMakeLists.txt SOURCES 并参与编译，支持 Lua 绑定与单元测试 |
| IPC/安全 | IPC (seL4 风格 Endpoint) + 优先级队列 + PIP | ✅ | 优先级有序等待队列杜绝队头阻塞，同步 IPC 优先级继承协议 (PIP) 与应答/撤销自动恢复 |
| IPC/安全 | 能力空间 CSpace (16 槽位硬件位图加速) | ✅ | uint16_t 位图管理，CTZ $O(1)$ 快速分配，单指令空闲检测，全局撤销位图剪枝加速 |
| IPC/安全 | 安全监控 SecurityMonitor | ✅ | 心跳监考 + 看门狗联动 + 堆压力检测 + 栈溢出计数 |
| IPC/安全 | 看门狗管理 WatchdogManager | ✅ | 80% idle 阈值喂狗，弱符号透明接入调度循环 |
| IPC/安全 | 系统调用审计 AuditEngine | ✅ | 128 槽环形缓冲 + 规则引擎 + `/proc/audit_log` |
| IPC/安全 | 安全启动 (Ed25519 + OTA) | ✅ | Ed25519 验签 + A/B 双分区断电安全，生产构建 `#error` 强制真实密钥 |
| 显示 | 帧缓冲 + 脏区域渲染 | ✅ | set_pixel/fill_rect 自动标记脏矩形，flush 只刷新变动区域 |
| 显示 | OLED 驱动 (Mock) | ✅ | SPI 接口框架 + 窗口化局部更新协议，无真实 SPI/DMA |
| 显示 | SSD1306 驱动 (I2C OLED) | ✅ | 0.96" 单色 128×64 SSD1306 I2C 屏真实驱动，复用 `II2cHal`，页式显存 + 脏页刷新，内嵌 5×7 字模，零动态分配 |
| 显示 | ST7789 驱动 (MiBand) | ✅ | 完整初始化序列 (RGB565/MADCTL/时序/Gamma/反相) + DMA 路径 (WFI 替代忙等) + 硬件复位/偏移/亮度/休眠，板级 Apollo3 IOM SPI/GPIO HAL 已打通；待真机验证 |
| 显示 | Renderer2D 2D 引擎 | ✅ | 完整实现 |
| 输入 | InputEvent / TouchDriver / GestureRecognizer | ✅ | 统一事件抽象，触摸驱动，7 种手势识别 (Tap/双按/长按/上下左右滑) |
| 输入 | 触摸驱动 (真实硬件) | ❌ | QEMU 仿真状态机，非真实硬件 |
| 电源 | 5 级功耗管理 (ACTIVE→DIM→IDLE→SLEEP→CRITICAL) | ✅ | 固件实际状态机 (`kernel/core/power/power_manager.hpp`)，联动 30/15/1/0fps 帧率，含抬腕唤醒与 BLE 状态绑定 |
| 电源 | 充电管理 | ✅ | 电池状态机 (DISCHARGING/PRE_CHARGE/FAST_CHARGE/CHARGE_DONE/FAULT) |
| 传感器 | 传感器框架 (Zephyr 风格) | ✅ | SensorDriver 抽象，HeartRateSensor (模拟 75 BPM)，Accelerometer |
| 射频 | 频谱传感器抽象 `spectrum_sensor.hpp` | ✅ | `ISpectrumSensor` 接口 (init/sweep/set_freq_range/功率上下电) + Q8 定点功率类型，附 `MockSpectrumSensor` 可编程注入器 |
| 射频 | 异常信号检测 `rf_analyzer.hpp` | ✅ | 逐分箱噪声底 EMA + 绝对偏差，检测 AboveNoiseFloor/AbsoluteHigh/SuddenBurst/WidebandRise 四类异常，宽带压制优先输出 |
| 射频 | 干扰信号识别 `jamming_detector.hpp` | ✅ | 组合复用 RfAnalyzer，跨帧环形缓冲识别 ContinuousWave/Narrowband/BroadbandNoise/SweepingChirp/Pulsed 五类物理层干扰，含中心频率/带宽/置信度，上报告警由调用方决定 |
| 传感器 | 健康算法 (PPG 滤波 + 计步 + 活动识别) | ✅ | 滑动窗口 + 中值预滤波 + 动态 IIR 低通（按活动态切换 α），自适应基线校准计步 + 能量二次校验，活动状态识别 (静止/行走/跑步/睡眠) 含睡眠置信度计数与缓冲退出 |
| UI | 页面栈导航 ScreenNavigator | ✅ | Push/Pop/Replace，平移转场动画，页面生命周期 |
| UI | 表盘 Complication 引擎 | ✅ | 数据驱动 UI，预定义心率和计步回调（数据变化时才触发局部重绘） |
| UI | 基础控件 (button, text_view, arc_progress) | ✅ | 3 种基础控件 |
| 运行时 | Lua 5.4.6 独立私有堆 LuaHeap | ✅ | 32KB 独立 TLSF 私有池，8 字节紧凑头部与就地扩容，GC 抖动零污染内核堆 |
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
| 移植 | Cortex-M0+ (Nucleo-L031K6) | ✅ | 8KB SRAM / 64KB Flash 极简优化适配，BSS 精简至 4.8KB，稳固运行 Shell 与 VFS |
| 移植 | Cortex-M4F (MiBand 8) | ✅ | `apps/watch/miband_main.cpp` `kernel_main` → `miband_kernel_main()` 完整启动：时钟树初始化、UI 渲染线程 + 传感器/BLE 守护线程 + Idle 线程、SysTick 1ms tick、首次上下文切换进入调度器；CI build-miband8 构建并通过 576KB Flash 大小检查 |
| 实验性 | 通知中心 NotificationCenter | ✅ | 优先级堆队列 + BLE 协议解析 + Overlay 横幅/全屏绘制 |
| 实验性 | NFC 卡模拟 | 🚧 | 控制器抽象，有 .cpp 实现 |
| 驱动 | 摄像头子系统 (OV2640 / OV7670 / Mock) | ✅ | `ICameraHal` 硬件抽象、OV2640 SCCB 探测与 DSP 缩放/特效寄存器表、VFS `/dev/video0` 节点与 IOCTL 集、乒乓双缓冲 DMA、SMPTE 8 色彩条/动态小球 Mock 驱动 |
| 实验性 | SoftGPU | ❌ | 源存在，无 CMake 目标 |
| 实验性 | GUIX 图形框架 | ✅ | 窗口合成器 + 多态窗口 + 脏矩形差量合并 + 2D光栅化原语 + 面向对象 Widget 控件树 (Button/Label/Progress/Slider/Panel) + SoftGPU 混合加速 |
| 实验性 | WiFi 驱动 (RTL8187L/RTL8812AU) | 🚧 | 驱动已实现，缺物理 USB 硬件 |
| 工程 | 主机单元测试 | ✅ | 362 个测试 (GoogleTest, ctest 发现，100% 通过) |
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
| **质量与安全** | `unit-tests` | 325 个 GoogleTest 单元与集成测试 (`ctest`) | 阻塞门禁 |
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

auroraOS 内置一套轻量级、可裁剪的**嵌入式 AI 运行时** (`ai/`)，代号 **February**，作为万物互联智能 AIOS 的端侧智能核心，面向智能手表、物联网终端及各类智能设备，负责把传感器信号与跨设备消息转化为可执行的意图 (Intent)。它由两套实现并存：遗留的 `ai/intent_engine.hpp`（传感器步数规则直接升降应用优先级），以及重新设计的模块化运行时 `ai/february/`（Phase 2.2 跨设备意图引擎）。February 的设计目标是**确定性、零堆分配、固定容量、可静态配置**，不使用任何大模型推理，全部路径可在资源受限目标 (Cortex-M0+, 8KB RAM) 上编译运行，让海量互联设备都能拥有本地智能。

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

## 📡 射频频谱感知 (Spectrum Sensing)

auroraOS 在 `drivers/rf/` 下提供一套 header-only、与硬件解耦的射频频谱感知框架，用于检测非法射频压制、干扰与异常信号（如无线干扰机、宽带噪声压制、扫频干扰）。框架分三层，遵循 AGENTS.md 的定点运算、零堆分配、`noexcept` 与单一职责约定，可同 `net/wireless/` 的无线安全审计能力互补，并与 `SecurityMonitor` 联动上报安全告警。

- **频谱传感器抽象 `spectrum_sensor.hpp`**：定义功率 Q8 定点类型（`PowerQ8 = int16_t`，实际 dBm = value/256，覆盖约 -128 ~ +127.99 dBm，含 `from_dbm` / `to_dbm` / `to_dbm_tenths` 换算），以及 `SpectrumBin` / `SpectrumSweep` 数据结构与纯虚接口 `ISpectrumSensor`（`init` / `sweep` / `set_freq_range` / `set_resolution_bw` / `power_up` / `power_down`）。同一抽象可对接 SDR 前端、专用频谱芯片或收发器 RSSI 通道。附 `MockSpectrumSensor` 可编程注入器（`fill` / `inject_cw` / `inject_band` / `add_cw`），供 host 测试与算法验证，不依赖真实射频硬件。
- **异常信号检测 `rf_analyzer.hpp`**：消费 `SpectrumSweep`，逐分箱维护噪声底估计（EMA 指数滑动平均 + 绝对偏差，无浮点无开方），检测四类异常——`AboveNoiseFloor`（超本地噪声底）、`AbsoluteHigh`（超绝对上限）、`SuddenBurst`（相邻采样瞬时跳变）、`WidebandRise`（大量分箱同时抬升的宽带压制）。输出策略为宽带压制优先输出全局告警、抑制 bin 级噪音；`analyze()` 在 `out=nullptr` 时仅推进基线，供干扰检测器复用。
- **干扰信号识别 `jamming_detector.hpp`**：组合持有 `RfAnalyzer` 引用以复用其噪声底与活动分箱判定，跨帧（固定环形历史缓冲）识别五类物理层干扰——`ContinuousWave`（单频点持续高功率）、`Narrowband`（局部窄带干扰）、`BroadbandNoise`（全频带均匀抬升）、`SweepingChirp`（峰值频率随时间线性移动）、`Pulsed`（高功率间歇出现）；并保留 `Spoofing` 类型供协议层协同确认。输出含中心频率、带宽估算、严重度与置信度的 `JammingAlert`，由调用方决定是否上报 `SecurityMonitor`，模块自身不耦合内核。

该框架当前为纯算法层（header-only，含 14 个 host 单元测试），真实射频前端驱动尚未接入 `CMakeLists.txt` 固件构建；接入真实 SDR/频谱芯片时只需实现 `ISpectrumSensor` 并在频谱守护任务中单线程推进 `RfAnalyzer` / `JammingDetector`。

---

## 🖼️ GUIX 图形框架 (GUIX Engine)

auroraOS 在 `experimental/guix/` 下提供了一套现代化、模块化、高能效的嵌入式多窗口与 UI 控件图形框架。该框架自底向上由 **窗口合成器 (Compositor)**、**多态窗口 (Window)**、**面向对象控件树 (Widget Tree)** 与 **标准控件族 (Controls)** 四层构成，并与底层 `SoftGpuDevice`（纯 C++ 软件光栅化 GPU）深度协同。

```
┌──────────────────────────────────────────────────────────┐
│                   GUIX Widget 控件体系                  │
│   [Button]      [Label]      [ProgressBar]     [Slider]  │
│          ▲           ▲              ▲               ▲     │
│          └───────────┴──────┬───────┴───────────────┘     │
│                       [Panel / Container]                 │
└─────────────────────────────┬────────────────────────────┘
                              │ 挂载至窗口根节点 (set_root_widget)
┌─────────────────────────────▼────────────────────────────┐
│                   GUIX Window (多态窗口)                  │
│  - 独立离屏表面 (Backing Store Surface)                   │
│  - 2D 光栅化原语 (点/线/圆/矩形/5x7 ASCII 字模)            │
│  - 局部脏矩形标记 (invalidate) & 局部坐标系变换            │
└─────────────────────────────┬────────────────────────────┘
                              │ 维护 Z-Order 双向链表
┌─────────────────────────────▼────────────────────────────┐
│                 GUIX Compositor (窗口合成器)              │
│  - 脏矩形差量合并与裁剪 (Damage Management)               │
│  - 硬件/软件混合渲染管线 (Opaque Blit / Alpha Blend)      │
│  - 屏幕背景与壁纸表面填充 (Background / Wallpaper)         │
│  - 顶层向下命中测试与输入事件路由 (Hit-testing & Dispatch)│
└─────────────────────────────┬────────────────────────────┘
                              │ 提交至 GPU
┌─────────────────────────────▼────────────────────────────┐
│            SoftGPU / 物理显示驱动 (Framebuffer)          │
└──────────────────────────────────────────────────────────┘
```

### 1. Compositor 窗口合成器 (`compositor.hpp/.cpp`)
- **Z-Order 堆叠链表**：维护窗口双向链表，支持 `raise_to_top`、`send_to_back` 与动态 Z 序重排。
- **脏矩形差量合并**：窗口移动、缩放、内容更新或控件点击时自动向合成器注册脏区域 (`add_damage`)，`composite()` 仅刷新受影响的屏幕子区域，避免全屏无效重绘。
- **壁纸与背景恢复**：在合成被损毁区域时，自动恢复背景单色或壁纸表面 (`set_wallpaper_surface`)。
- **事件分发中心**：从顶层窗口向底层进行几何命中测试 (`find_window_at`)，将指针移动、按下、抬起及键盘焦点事件无缝分发给前台窗口与焦点控件。

### 2. Window 多态窗口 (`window.hpp/.cpp`)
- **独立离屏显存**：每个 Window 持有独立的 `gpu::Surface` 离屏缓冲区，支持透明度 (`set_alpha`) 与动态扩缩容 (`resize`)。
- **全套 2D 光栅化原语**：内置 Bresenham 直线算法、中点画圆与实心圆、矩形填充与描边、5x7 ASCII 矢量点阵字体渲染（支持多倍缩放与透明背景绘制）。
- **控件宿主与双向分发**：支持通过 `set_root_widget()` 挂载控件树，自动在事件到达时转换屏幕坐标为窗口局部坐标，并触发控件树重绘与脏标记。

### 3. Widget 控件体系 (`widget.hpp/.cpp`)
- **树形层级管理**：内建侵入式树状节点（`parent`、`first_child`、`next_sibling`、`prev_sibling`），支持绝对窗口坐标与相对几何计算 (`get_window_bounds`)。
- **标准生命周期与渲染流**：统一的 `paint(Window*)` 虚函数与 `paint_tree(Window*)` 递归绘制流水线，配合 `invalidate(Window*)` 精准标记控件自身包围盒。
- **内置丰富控件族**：
  - **Button (`widget_button.hpp/.cpp`)**：支持正常态、按下态、禁用态色彩定制，文字自动居中，支持 `PointerDown/Up/Move` 手势判定并触发 `ButtonClickHandler` 点击回调。
  - **Label (`widget_label.hpp/.cpp`)**：支持纯文本与多行字符、前景色/背景色/透明背景切换、水平对齐方式（左对齐、居中、右对齐）与多倍字体缩放。
  - **ProgressBar (`widget_progress.hpp/.cpp`)**：支持任意数值范围（Min/Max/Value），自动边界钳位，底层轨道与填充条渲染，可选居中百分比数字显示。
  - **Slider (`widget_slider.hpp/.cpp`)**：可拖拽滑块控件，实时响应触控滑动，支持高亮激活轨道、滑块旋钮绘制与 `SliderValueChangedHandler` 数值变更回调。
  - **Panel (`widget_panel.hpp/.cpp`)**：通用容器控件，支持背景填充、边框绘制与子控件自动化递归布局。

---

## 🗺️ 开发路线图

基于当前功能状态，以下方向仍是进行中或尚未落地，可作为后续贡献重点：

- ✅ **SoftBus 密钥供应 (Secure Storage)**：`net/distributed_bus.hpp` 的密钥加载 `#error` 已移除，改为 `hal/secure_storage_hal.hpp` 抽象；miband8 从 customer OTP 读取每设备唯一密钥，其余板级走弱符号 fail-closed。
- 🚧 **BLE 协议栈完整化**：真实硬件驱动路径（HalBle → HCI → 安全模块）已打通并接入 miband8 构建；剩余工作为板级 UART 中断喂数 (`feed_rx_byte`) 与 NimBLE Host 桥接。
- ✅ **WiFi 安全审计 WirelessIDS 接入编译**：USB 驱动与监控任务 `.cpp` 已加入 `CMakeLists.txt SOURCES` 参与构建。
- ✅ **ST7789 显示驱动 (MiBand)**：完整初始化序列 + DMA 路径（WFI 等待替代忙等），复位/偏移/亮度/休眠与 PowerManager 联动。
- ✅ **微内核同步原语加固**：跨任务死锁检测 (WFG ABBA 检测)、立即优先级天花板协议 (IPCP)、静态无堆 POSIX 信号量池与定时等待唤醒。
- ✅ **进程级/任务级内核定时器**：零堆分配 `ProcessTimerManager`，相对/绝对/周期 (零漂移) 定时与 POSIX 信号/IPC/事件多通道通知。
- ✅ **GUIX 图形框架**：多窗口合成器 + 脏矩形差量裁剪 + 面向对象 Widget 树与标准控件族 (Button/Label/Progress/Slider/Panel)。
- 🚧 **WiFi 驱动 (RTL8187L / RTL8812AU)**：等待物理 USB 硬件接入。
- ❌ **触摸驱动真实硬件**：当前为 QEMU 仿真状态机。
- ✅ **AArch64 MMU + VAS**：实现完整 4 级 4KB 页表管理、虚实映射/解映射/权限修改/自动剪枝、QEMU virt 构建目标与 CI 测试流水线。
- ✅ **摄像头驱动子系统**：包含 `ICameraHal` 硬件抽象、OV2640 传感器驱动（SCCB 探测/缩放/特效）、MockCamera 测试驱动（SMPTE 8 彩条/动态小球）、乒乓双缓冲 DMA 与 `/dev/video0` VFS / IOCTL 交互接口。
- ❌ **SoftGPU**：仅抽象接口或占位源，无构建目标。

---

## 📜 发展时间线

auroraOS 于 2026 年 7 月 11 日从零起步，在约 5 周内完成了从内核骨架到安全能力体系的初步构建。以下时间线依据仓库 git 提交历史整理，按阶段划分里程碑（日期为对应阶段的首个提交）。

**内核命名日**：项目采用「AuroraOS = 完整操作系统，July = 微内核」的二分模型（见 `temp.md` 长期规划基线）。微内核定名为 **July Kernel**，职责限定为任务/线程管理、基础同步原语、时钟与定时、IPC、Capability、内存隔离与底层硬件抽象，明确不承载网络、VFS、UI、AI 等高层能力——这些均按「服务 → 子系统 API → 内核/Syscall → HAL → 硬件」的分层外置于内核之外。据此，本时间线所称「内核」即指 July Kernel。

### 2026-07-11 · 内核与硬件抽象奠基
项目以 capability 微内核为核心目标启动：搭建 `arch/`（ARM Cortex-M 与 RISC-V RV32）架构抽象、平台无关 `hal/` 硬件抽象层、`kernel/` 调度/内存/IPC/中断子系统、`syscall/` ABI、`boot/`/`bootloader/` 引导链。同期确定 QEMU `lm3s6965-qb` HIL 测试框架，`tests/` 目录以 GoogleTest 主机测试形式落地，为后续「host 侧验证内核算法」奠定工程基础。

### 2026-07-13 · 系统服务与外设驱动
引入驱动层 `drivers/`（显示帧缓冲、SSD1306/ST7789/OLED、输入、传感器、USB、存储、看门狗、电源）与服务层 `services/`（充电、低功耗电源管理、OTA 升级、系统时间）。内核安全机制成型：capability 能力系统、设备对象表 `DeviceRegistry`、权限/策略引擎，并补齐 `security_monitor` 与 `mutex`/`irq` 等同步原语。

### 2026-07-14 · 用户态应用与 AI 集成
`apps/` 用户态应用接入（看板手表应用、GUI 原型、Lua 脚本宿主）。`ai/` 模块引入 ONNX Runtime Micro 推理后端与 `ModelBundle` 模型封装，配套 `metrics/` 运行时指标与 `experimental/` 研究实现（如运行时解释器、轻量级调度器），形成「用户态 — 服务 — 内核」完整依赖分层。

### 2026-07-15 · 网络安全审计能力
`net/` 子系统扩展为安全审计中枢：无线入侵检测 `wireless_ids`（deauth 探测、信标伪造、握手重放）、BLE IDS、TCP 扫描器、Lua 网络协处理器。射频感知与安全监控打通，奠定「万物互联智能 AIOS」的联网与威胁检测底座。

### 2026-07-16 · 安全强化与零信任软总线
集中加固安全基座：`SecurityMonitor` 安全告警分级、OTA 安全校验与回滚、`secure_boot` 信任链校验、看门狗故障恢复。新增 `net/distributed_bus.hpp` SoftBus 分布式软总线（安全信道 + 设备发现 + 密钥供应），整体走向零信任分布式架构。

### 2026-08-14 至 08-15 · February AI 框架落地
`February` 分支在本轮集中实现并合并入主线（含 PR #4、#5 两次 Merge）。AI 子系统以 **February** 为代号完成三阶段演进：`FebruaryCore` / `persona` / `context_manager` / `event_bus` / `action_executor` / `intent_engine` / `compat_intent_engine` 等 Phase-1 核心头文件落地；Phase-1.5 补上 `log` / `cooldown` / `memory` / `intent_rules` 与 `SessionMemory`，`intent_engine` 改用规则表 + 冷却 + 记忆驱动；Phase-2 打通 `SoftBus` 软总线（`codec` / `transport` / `facade` / `OH adapter` / `PeerTable` / `close_peer`），将 `Planner` 接入 `FebruaryCore::on_intent`，并新增 `FebruaryCrit` 临界区钩子与 `SetPower` 等动作。同步修复 SoftBus 溢出、悬空 RX 接收槽、本地回显、虚假 TX 成功（Codex 评审），并补齐 February 可靠性/压力测试套件（环形溢出、编解码、对等节点、生命周期）。

### 2026-08-15 · 射频频谱感知与显示驱动重构
本轮收尾多项架构治理与能力扩展：新增 `drivers/rf/` 频谱感知框架（频谱传感器抽象 + 异常信号检测 + 五类物理层干扰识别，全定点零堆分配、14 个单元测试）；统一显示驱动形态（抽取 `SpiLcdDriverBase` 基类消除 OLED/ST7789 重复、St7789 改继承 `CharDevice`），修复 FontEngine 直写显存不触发脏标记导致文字不显示的隐患，并在 README 与功能状态表中沉淀健康算法优化与射频感知文档。

### 2026-08-18 · 微内核同步原语强化、死锁检测、进程级定时器与摄像头驱动体系
本轮集中治理并大幅加固了微内核核心同步机制、定时体系与外设驱动矩阵：
1. **死锁检测与天花板协议**：在 `Mutex` 中实现了跨任务等待图闭环检测（Deadlock Detection），遇 ABBA 循环死锁时主动中止加锁并置位 `EDEADLK`；落地立即优先级天花板协议（IPCP），支持持有锁期间即时提权与释放后的传递性优先级连锁恢复。
2. **POSIX 信号量静态内存池**：重构内核 `Semaphore` 与 `posix.cpp`，采用基于 `IrqGuard` 的无堆分配架构和静态内存池（`s_posix_sem_pool[16]`），消除动态 `new/delete`，补齐带超时唤醒的 `wait(timeout_ticks)` 与 `sem_trywait`。
3. **进程级内核定时器子系统**：新增 `kernel/core/process_timer.hpp/.cpp`，支持相对/绝对、单次/周期（零漂移重装）定时以及 POSIX 信号/IPC/事件三种异步通知机制，与硬件 SysTick 及 Tickless 低功耗深度集成，任务终止自动级联清理。
4. **摄像头驱动子系统**：实现 `hal/camera_hal.hpp` 底层 DVP/DMA 硬件抽象、`drivers/camera/ov2640_driver.hpp` 传感器驱动（SCCB 探测/双 Bank 切换/DSP 缩放/特效寄存器表）、`drivers/camera/mock_camera_driver.hpp` 仿真驱动（SMPTE 8 饱和彩条/动态弹跳小球），并打通 VFS `/dev/video0` 节点注册与标准 IOCTL 控制集。

### 2026-08-22 · RISC-V 硬件看门狗虚拟化修复与 GUIX 现代化图形框架全面落地
本轮重点修复了 RISC-V 目标启动与定时器中断异常，并全面推进了 GUIX 现代化多窗口与控件图形框架：
1. **RISC-V 虚拟化看门狗与时钟硬化**：排查并修复了在 QEMU RISC-V 平台定时器中断触发后出现的 `mcause: 0x5 (load access fault)` 空指针偏移 `0x1C` 崩溃；将 `SoftWdt` 驱动声明提升至文件静态作用域，确保 GNU 工具链将其虚表构造函数自动注入 `.init_array` 并在引导时确定性初始化；定时器重装载全面采用 64 位原子 `Arch::get_mtime()` 防止高位时间戳截断。
2. **GUIX 图形框架全栈实现**：
   - **Compositor (合成器)**：实现 Z 序双向链表管理 (`raise_to_top` / `send_to_back`)、局部脏矩形差量裁剪与合并、单色/壁纸表面背景恢复、自顶向下命中测试与输入事件路由、硬件/软件混合渲染管线（直拷与 Alpha 混合）。
   - **Window (多态窗口)**：支持独立离屏 Backing Store Surface、动态重设尺寸保留图像、局部重绘通知、全套 2D 光栅化绘图原语（Bresenham 直线、中点圆与实心圆、矩形、5x7 ASCII 字模）与控件树宿主。
   - **Widget 控件体系与标准控件族**：构建了支持树形层级、窗口坐标转换与事件分发的面向对象 `Widget` 基类，并完整实现了 `Button`（交互反馈与点击回调）、`Label`（对齐与字号缩放）、`ProgressBar`（范围钳位与百分比绘制）、`Slider`（滑块拖拽与数值变动回调）、`Panel`（容器与复合布局）标准控件族。
3. **测试工程扩充**：全自动化 GoogleTest 测试用例扩充至 **362 个**，100% 通过验证。

> 时间线精确到阶段首日；更早的小幅补丁（如 2026-07-12、07-17、07-18、07-27、08-14 的提交）多为对应阶段内的完善与缺陷修复，未单列。

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

**auroraOS** · 万物互联智能 AIOS —— 让每一台智能终端自主互联、协同感知、智能决策

<p>
  <a href="https://github.com/jencaoking/auroraOS">Repository</a> ·
  <a href="LICENSE">License</a> ·
  <a href="https://github.com/jencaoking/auroraOS/issues">Issues</a>
</p>

</div>
