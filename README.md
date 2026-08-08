<div align="center">

# AuroraOS

**面向智能手表与物联网终端的微内核实时操作系统**

ARM Cortex-M0+/M3/M4 · RISC-V RV32IMAC · lwIP TCP/IP · Lua 5.4.6 · MPU 内存保护

<p>
  <img src="https://img.shields.io/badge/Platform-Cortex--M0%2B%20%7C%20M3%20%7C%20M4%20%7C%20RV32-brightgreen.svg" alt="Platform">
  <img src="https://img.shields.io/badge/Network-lwIP%20TCP%2FIP-orange.svg" alt="Network">
  <img src="https://img.shields.io/badge/Storage-LittleFS%20%2B%20PhotonCache-purple.svg" alt="Storage">
  <img src="https://img.shields.io/badge/Script-Lua%205.4.6-yellow.svg" alt="Script">
  <img src="https://img.shields.io/badge/License-CAOSL%20v2.0-blue.svg" alt="License">
</p>

</div>

---

## 项目简介

auroraOS 是一个面向智能手表与物联网终端的微内核实时操作系统。在精简的代码体积内实现了优先级抢占调度、完整 TCP/IP 网络协议栈、MPU 内存隔离、Lua 小程序引擎、帧感知渲染、分布式软总线等特性。

| 指标 | 说明 |
|------|------|
| 目标架构 | ARM Cortex-M0+/M3/M4 (Thumb-2)、RISC-V 32 (RV32IMAC) |
| 支持板级 | TI LM3S6965-QB (Cortex-M3, QEMU)、QEMU RV32 Virt、ST Nucleo-L031K6 (Cortex-M0+)、小米手环 8 (Apollo3 M4F, 内核未启动) |
| 构建系统 | CMake + Kconfig (Linux 内核风格可裁剪配置) |
| CI/CD | GitHub Actions (9 jobs: 4 目标固件构建 + HIL 冒烟 + 单元测试 + ASAN/UBSAN + 覆盖率 + clang-tidy + cppcheck + Release) |
| 开发语言 | C++ (内核) + C (驱动/lwIP/Lua) + ARM/RISC-V Assembly (启动/异常向量) |
| 第三方依赖 | lwIP 2.x · Lua 5.4.6 · LittleFS (git submodule) · ed25519 |

### 设计参考

参考了以下操作系统的公开文档与源码，具体实现均为独立编写：

- **NuttX** — POSIX 兼容层、ProcFS 设计
- **FreeRTOS** — TaskNotify、tickless 思路
- **Zephyr** — Kconfig、传感器框架接口
- **seL4** — Capability 模型、IPC 端点
- **Linux** — VFS 设计
- **HarmonyOS** — 分布式软总线
- **vivo BlueOS** — 帧感知调度、光子缓存、超级渲染树
- **watchOS** — 表盘 Complication、应用生命周期

---

## 目录结构

```
auroraOS/
├── apps/                 # 应用层 (Shell, Lua 引擎, ELF 加载器, 网络应用)
├── kernel/               # 内核核心 (调度器, 内存, 同步原语, IPC, CSpace, MPU, 信号, 定时器)
├── boot/                 # 启动与硬件抽象 (Reset_Handler, PendSV, SVC, SysTick)
├── bootloader/           # 安全启动 (Ed25519 验签 + OTA 双分区)
├── vfs/                  # 虚拟文件系统 (VNode, RamFS, ProcFS, LittleFS, PhotonCache)
├── net/                  # 网络子系统 (防火墙, 包捕获, 扫描器, BLE 安全, 软总线, 无线安全审计)
├── drivers/              # 驱动层 (显示, 输入, 传感器, USB, 存储, 看门狗, 电源)
├── ui/                   # UI 框架 (ScreenNavigator, View, Complication, 基础控件)
├── arch/                 # 架构抽象层 (ARM Cortex-M0+/M3/M4/M4F/M4F, ARMv8-A AArch64 探索, RISC-V RV32)
├── boards/               # 板级支持包 (LM3S6965, Nucleo-L031K6, MiBand 8, QEMU RV32)
├── adapter/net/          # lwIP OSAL 适配层 (Mutex/Sem/Mbox/Thread 映射, 以太网接口)
├── syscall/              # SVC/ECALL 系统调用定义
├── metrics/              # 性能度量 (DWT 周期计数器, 延迟记录器, 功耗分析)
├── utils/                # 工具 (HMAC-SHA256, JSON 解析器)
├── ai/                   # 意图引擎 (传感器规则决策)
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

---

## 功能状态

| 子系统 | 功能 | 状态 | 说明 |
|--------|------|------|------|
| 内核调度 | O(1) 优先级抢占调度器 (5 级: Idle/Low/Normal/High/Realtime) | ✅ | 就绪位图 + 侵入式双向链表，O(1) 入队/出队/最高优先级检索 |
| 内核调度 | 帧感知调度 FrameSchedulerV2 | ✅ | 30fps 帧内/帧间窗口分级，`volatile bool` 实现，不依赖 `<atomic>` |
| 同步原语 | Mutex (优先级继承 PIP) | ✅ | 传递性优先级继承、递归加锁、超时机制、RAII UniqueLock |
| 同步原语 | Semaphore / MessageQueue SPSC / TaskNotify / Signal | ✅ | 计数信号量 ISR 安全；无锁 SPSC 环形队列；32 位零开销通知；POSIX signal/kill/raise |
| 内存管理 | KernelHeap (First-Fit + Split + Lazy Coalesce) | ✅ | 线程安全 (IrqGuard RAII)，8 字节对齐，魔数校验，OOM 懒合并 |
| 内存管理 | MemoryPool (O(1) 固定块分配器) | ✅ | 空闲链表，边界检查，双重释放检测 |
| 内存保护 | MPU (Cortex-M4, PMSAv7) | ✅ | 8 区域配置，Flash 只读 + RAM 特权态 + 用户栈沙盒，PendSV 动态切换 |
| 内存保护 | MPU (Apollo3 M4F) | ❌ | `mpu_configure_region` 未提供实现 |
| 内存保护 | AArch64 MMU + VAS | ❌ | 仅抽象接口 (`kernel/vasp.hpp`)，无 CMake 构建目标 |
| 存储 | VFS (VNode 多态) + RamFS + ProcFS | ✅ | open/read/write/close/lseek/ioctl 完整接口，路径遍历防护 |
| 存储 | LittleFS + PhotonCache (LRU 页缓存) | ✅ | 掉电安全日志式文件系统，8 槽 LRU 缓存，脏页延迟写，3 次重试 |
| 存储 | SoftBus (UART RPC 总线) | ✅ | 有 `.cpp` 实现，非 M0+ 目标编译时包含，带凭证验证 |
| 网络 | lwIP 2.x 全栈 (IPv4/TCP/UDP/ICMP/ARP/DHCP) | ✅ | Socket + Netconn 双 API，`LWIP_TCPIP_CORE_LOCKING` 核心锁 |
| 网络 | 以太网驱动 (StellarisEth, LM3S6965) | ✅ | 独立 RX/TX 互斥锁，4 字节对齐 memcpy，异常包 FIFO 排空 |
| 网络 | 防火墙 FirewallEngine | ✅ | 规则匹配 (IP+Port) + 流量整形 (抗 DDoS) + Shell 命令 + Lua 绑定 |
| 网络 | 数据包捕获 PacketCapture | ✅ | `/dev/pcap0` 字符设备，BPF 风格过滤器，Wireshark .pcap 格式 |
| 网络 | 网络扫描 NetworkScanner | ✅ | 端口/主机/服务/漏洞 4 模块，TaskNotify Worker 池，Lua `aurora.scan.*` 绑定 |
| 网络 | 分布式软总线 DistributedSoftBus | ✅ | HMAC-SHA256 挑战应答 + 防重放 + 能力白名单 + LRU 路由表 + DDoS 限速 |
| 网络 | BLE 协议栈 (基础) | 🚧 | `experimental/net/ble/ble_stack.cpp` 完整 (连接状态机+HCI+GATT+Ed25519)；`net/ble/` 4 个 header-only 安全模块 |
| 网络 | WiFi 安全审计 WirelessIDS | 🚧 | 5 模块 header-only 完整；驱动 .cpp 已实现，但 **未加入 CMakeLists.txt SOURCES**，不参与编译 |
| IPC/安全 | IPC (seL4 风格 Endpoint) + 类型化消息 | ✅ | Endpoint::call/receive/reply，IpcMessage<T> 模板，编译期类型安全 |
| IPC/安全 | 能力空间 CSpace (lookup/delete/derive/mint/revoke/grant) | ✅ | 16 槽位，权限降级检测，全局撤销 |
| IPC/安全 | 安全监控 SecurityMonitor | ✅ | 心跳监考 + 看门狗联动 + 堆压力检测 + 栈溢出计数 |
| IPC/安全 | 看门狗管理 WatchdogManager | ✅ | 80% idle 阈值喂狗，弱符号透明接入调度循环 |
| IPC/安全 | 系统调用审计 AuditEngine | ✅ | 128 槽环形缓冲 + 规则引擎 + `/proc/audit_log` |
| IPC/安全 | 安全启动 (Ed25519 + OTA) | ✅ | Ed25519 验签 + A/B 双分区断电安全，生产构建 `#error` 强制真实密钥 |
| 显示 | 帧缓冲 + 脏区域渲染 | ✅ | set_pixel/fill_rect 自动标记脏矩形，flush 只刷新变动区域 |
| 显示 | OLED 驱动 (Mock) | ✅ | SPI 接口框架 + 窗口化局部更新协议，无真实 SPI/DMA |
| 显示 | ST7789 驱动 (MiBand) | 🚧 | 半实现，DMA 忙等 + 注释 |
| 显示 | Renderer2D 2D 引擎 | ✅ | 完整实现 |
| 输入 | InputEvent / TouchDriver / GestureRecognizer | ✅ | 统一事件抽象，触摸驱动，7 种手势识别 (Tap/双按/长按/上下左右滑) |
| 输入 | 触摸驱动 (真实硬件) | ❌ | QEMU 仿真状态机，非真实硬件 |
| 电源 | 5 级功耗管理 (RUN→IDLE→LIGHT_SLEEP→DEEP_SLEEP→SHUTDOWN) | ✅ | 设计完整，含 Tickless 模式、抬腕唤醒、BLE 状态绑定 |
| 电源 | 充电管理 | ✅ | 电池状态机 (DISCHARGING/PRE_CHARGE/FAST_CHARGE/CHARGE_DONE/FAULT) |
| 传感器 | 传感器框架 (Zephyr 风格) | ✅ | SensorDriver 抽象，HeartRateSensor (模拟 75 BPM)，Accelerometer |
| 传感器 | 健康算法 (PPG 滤波 + 计步 + 活动识别) | ✅ | 滑动窗口 + IIR 低通滤波器，活动状态识别 (静止/行走/跑步/睡眠) |
| UI | 页面栈导航 ScreenNavigator | ✅ | Push/Pop/Replace，平移与渐变转场动画，页面生命周期 |
| UI | 表盘 Complication 引擎 | ✅ | 数据驱动 UI，预定义心率和计步回调（步数硬编码 1234） |
| UI | 基础控件 (button, text_view, arc_progress) | ✅ | 3 种基础控件 |
| 运行时 | Lua 5.4.6 小程序引擎 | ✅ | 自定义 KernelHeap 分配器，Lua ↔ UI 绑定，传感器 API 暴露 |
| 运行时 | ELF 动态加载器 | ✅ | ARM Thumb ELF 加载，地址回绕校验，W^X 保护，MPU 沙盒 |
| 运行时 | 应用生命周期 ACB | ✅ | FOREGROUND/BACKGROUND/SUSPENDED 状态机，动态优先级调整 |
| 运行时 | 意图引擎 IntentEngine | ✅ | 基于传感器步数规则决策，自动提升/降级应用优先级 |
| 移植 | Cortex-M3 (LM3S6965, QEMU) | ✅ | 主 HIL 平台，完整可运行 |
| 移植 | RISC-V RV32 (QEMU) | ✅ | 独立异常向量，完整可运行 |
| 移植 | Cortex-M0+ (Nucleo-L031K6) | ✅ | 裸板适配，64KB Flash / 8KB RAM 限制，最大任务数 4 |
| 移植 | Cortex-M4F (MiBand 8) | 🚧 | `kernel_init` 被注释，无法进入调度器 |
| 移植 | AArch64 (ARMv8-A) | ❌ | 仅探索代码，无 CMake 构建目标 |
| 实验性 | 通知中心 NotificationCenter | ✅ | 优先级堆队列 + BLE 协议解析 + Overlay 横幅/全屏绘制 |
| 实验性 | NFC 卡模拟 | 🚧 | 控制器抽象，有 .cpp 实现 |
| 实验性 | 摄像头 | ❌ | 仅抽象接口，占位 |
| 实验性 | SoftGPU | ❌ | 源存在，无 CMake 目标 |
| 实验性 | GUIX 图形框架 | 🚧 | 合成器 + 窗口，部分实现 |
| 实验性 | WiFi 驱动 (RTL8187L/RTL8812AU) | 🚧 | 驱动已实现，缺物理 USB 硬件 |
| 工程 | 主机单元测试 | ✅ | 32 通过 / 3 失败 (含 12 个死测试) |
| 工程 | CI/CD (GitHub Actions) | ✅ | 9 jobs：4 目标固件构建 + HIL 冒烟 + 194 单元测试 + ASAN+UBSAN + 覆盖率 + clang-tidy + cppcheck + Release |
| 工程 | 性能度量 Metrics (DWT) | 🚧 | LatencyRecorder 无 ProcFS 输出，parse_metrics 占位 |

---

## 快速开始

### 环境准备

- `arm-none-eabi-gcc` / `arm-none-eabi-g++` (ARM GNU Toolchain)
- `gcc-riscv64-unknown-elf` (RISC-V GNU Toolchain, 可选)
- `CMake` >= 3.20
- `Make` / `Ninja`
- `QEMU` (qemu-system-arm + qemu-system-misc)
- `Python 3` + `kconfiglib`

```bash
pip install kconfiglib
```

### 构建并运行 (LM3S6965, QEMU)

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

### 构建其他目标

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

### 构建并运行主机单元测试

```bash
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j
ctest --test-dir build_tests --output-on-failure
```

---

## 核心架构

```
┌────────────────────────────────────────────────────────────────┐
│                    应用层 (apps/)                               │
│  Shell  │ Lua MiniProgram  │ IntentEngine  │ AppLifecycle      │
│  ELF Loader  │ DistributedSoftBus  │ WatchFace Complications  │
├────────────────  SVC/ECALL 系统调用边界 ───────────────────────┤
│                    内核核心 (kernel/)                           │
│  Scheduler (5级抢占) │ FrameScheduler (30fps帧感知)            │
│  MutexPI │ Semaphore │ MessageQueue │ TaskNotify │ Signal      │
│  KernelHeap │ MemoryPool │ Timer │ WorkQueue │ PowerManager    │
│  IPC (Endpoint) │ CSpace (seL4 风格) │ MPU │ AuditEngine      │
│  SecurityMonitor │ WatchdogManager │ POSIX 兼容层               │
├───────────────────────────────────────────────────────────────┤
│                  文件系统 (vfs/)                                │
│  VfsManager (VNode多态)  │ RamFS  │ ProcFS  │ LittleFS + Cache│
├───────────────────────────────────────────────────────────────┤
│                  网络协议栈                                     │
│  lwIP 2.x (TCP/UDP/ICMP/ARP/DHCP)  │ OSAL 适配层              │
│  FirewallEngine │ PacketCapture │ NetworkScanner               │
│  DistributedSoftBus │ BLE Security Framework                   │
├───────────────────────────────────────────────────────────────┤
│                   驱动层 (drivers/)                             │
│  display/ (帧缓冲+脏区域+OLED+ST7789)  │ input/ (触摸+手势)    │
│  sensor/  │ storage/  │ usb/  │ watchdog/  │ power/            │
├───────────────────────────────────────────────────────────────┤
│                  架构抽象层 (arch/)                             │
│  Arch:: namespace (disable_irq/enable_irq/wfi/trigger_switch)  │
│  arch_impl.hpp (cm0plus/cm3/cm4/cm4f/rv32)                    │
│  start_first_task()  │ systick_init()  │ init_thread_stack()   │
├───────────────────────────────────────────────────────────────┤
│                  板级支持 (boards/)                             │
│  ti/lm3s6965-qb  │ st/nucleo-l031k6  │ qemu/rv32_virt         │
│  xiaomi/miband8 (miband 分支)                                 │
└───────────────────────────────────────────────────────────────┘
```

---

## 许可证

本项目采用 **CAOSL v2.0 (JENCAO Custom Advanced Open Source License v2.0)** 开源许可证。详见 [LICENSE](LICENSE) 文件。

这是一个自定义开源协议，包含强 Copyleft、专利保护、道德使用限制和商业双授权条款。

第三方依赖保留各自许可证：
- lwIP: BSD-3-Clause
- LittleFS: BSD-3-Clause
- Lua 5.4.6: MIT
- ed25519: MIT

---

<div align="center">
  <p><i>auroraOS · 从学习演示到多分支 Lua 化智能手表 RTOS 平台</i></p>
  <p><i>Repository: https://github.com/jencaoking/auroraOS</i></p>
</div>