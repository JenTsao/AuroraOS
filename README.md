<div align="center">

<h1>🌙 AuroraOS</h1>

<p><b>面向手机与手表的微内核实时操作系统</b></p>

<p><i>借鉴 12 款顶尖 OS · ARM Cortex-M0+/M3/M4 · AArch64 · RISC-V · lwIP TCP/IP · Lua 小程序 · MPU 内存保护</i></p>

<p>
  <img src="https://img.shields.io/badge/Platform-Cortex--M4%20%7C%20AArch64%20%7C%20RV32-brightgreen.svg" alt="Platform">
  <img src="https://img.shields.io/badge/Network-lwIP%20TCP%2FIP-orange.svg" alt="Network">
  <img src="https://img.shields.io/badge/Storage-LittleFS%20%2B%20PhotonCache-purple.svg" alt="Storage">
  <img src="https://img.shields.io/badge/Script-Lua%205.4.6-yellow.svg" alt="Script">
</p>
<p>
  <img src="https://img.shields.io/badge/Security-MPU%20%7C%20MMU%20%7C%20Syscall%20Val-red.svg" alt="Security">
  <img src="https://img.shields.io/badge/Build-Kconfig%20%2B%20CMake-informational.svg" alt="Build">
  <img src="https://img.shields.io/badge/CI-GitHub%20Actions-2088FF.svg" alt="CI">
  <img src="https://img.shields.io/badge/License-Customer-lightgrey.svg" alt="License">
</p>

<p>
  <a href="#快速开始"><b>🚀 快速开始</b></a> &nbsp;·&nbsp;
  <a href="#系统架构"><b>🏗️ 系统架构</b></a> &nbsp;·&nbsp;
  <a href="#子系统详解"><b>🔧 子系统详解</b></a> &nbsp;·&nbsp;
  <a href="#开发路线图"><b>🗺️ 路线图</b></a>
</p>

</div>

---

## 目录

| | | |
|:--|:--|:--|
| [📖 项目简介](#项目简介) | [✨ 核心特性](#核心特性) | [🏗️ 系统架构](#系统架构) |
| [📁 目录结构](#目录结构) | [🔧 子系统详解](#子系统详解) | [🌿 分支结构](#分支结构) |
| [🚀 快速开始](#快速开始) | [💻 Shell 命令](#shell-命令) | [⚙️ 配置系统](#配置系统) |
| [🗺️ 开发路线图](#开发路线图) | [📚 借鉴来源](#借鉴来源) | [📊 质量指标](#质量指标) |
| [🐞 故障排查与经验总结](#故障排查与经验总结) | [🙏 致谢](#致谢) | [📄 许可证](#许可证) |

<details>
<summary><b>🔧 子系统详解 — 展开查看全部子模块</b></summary>

&nbsp;

- [调度器与任务管理](#调度器与任务管理)
- [内存管理](#内存管理)
- [同步原语](#同步原语)
- [虚拟文件系统 VFS](#虚拟文件系统-vfs)
- [网络协议栈](#网络协议栈)
- [设备驱动框架](#设备驱动框架)
- [MPU 内存保护](#mpu-内存保护)
- [显示与输入](#显示与输入)
- [Lua 小程序引擎](#lua-小程序引擎)
- [分布式软总线](#分布式软总线)
- [IPC 与能力空间](#ipc-与能力空间)
- [安全监控与看门狗](#安全监控与看门狗)

</details>

---

## 项目简介

**auroraOS** 是一个面向智能手表与物联网终端的微内核实时操作系统，基于 ARM Cortex-M4 / M4F 架构，专为现代可穿戴设备设计。系统深度借鉴了业界 12 款顶尖操作系统的设计哲学，在极致精简的代码体积内实现了优先级抢占调度、完整 TCP/IP 网络协议栈、MPU 内存隔离、Lua 小程序引擎、帧感知渲染、分布式软总线等工业级特性。

### 项目数据

| 指标 | 数据 |
|------|------|
| 自有代码 | **~40,000 行**（最新工作树，含 main 与各板级模块，不含 3rdparty）|
| 第三方依赖 | lwIP 2.x · Lua 5.4.6 · LittleFS · ed25519 · fmt · stb |
| 模块目录 | 20 个一级功能子目录 |
| Git 提交 | 250+ 次（实测 254）|
| 目标架构 | ARM Cortex-M0+/M3/M4 (Thumb-2) · AArch64 · RISC-V 32 (RV32IMAC) |
| 支持板级 | TI LM3S6965-QB · QEMU RV32 Virt · ST Nucleo-L031K6（Cortex-M0+）· 小米手环 8（Ambiq Apollo3 Blue）|
| 构建系统 | CMake + Kconfig（Linux 内核风格可裁剪配置）|
| CI/CD | GitHub Actions 自动化构建 |
| 开发语言 | C++（内核）+ C（驱动/lwIP/Lua）+ ARM Assembly（启动）|

### 设计理念

auroraOS 坚信"好的架构来自借鉴与融合"。系统不是从零发明一切，而是系统性地分析了全球 12 款主流操作系统的设计精髓，提炼出最适合可穿戴场景的技术方案，在一个统一的微内核框架内有机融合。这种"集大成"的设计哲学让 auroraOS 在不到 6000 行自有代码内实现了通常需要数万行才能覆盖的 RTOS 能力。

---

## 核心特性

### 🚀 内核与调度

- **O(1) 优先级抢占式调度器**：5 级优先级阶梯（Idle / Low / Normal / High / Realtime），基于就绪优先级位图（Ready Bitmask）与侵入式双向就绪链表，实现 O(1) 时间的任务入队、出队与最高优先级检索
- **帧感知调度（FrameScheduler）**：借鉴 vivo 蓝河 BlueOS，30fps 帧内/帧间窗口分级，保证 UI 渲染 deadline，帧间释放 CPU 给后台任务
- **多功能互斥锁（Mutex）**：支持传递性优先级继承（PIP，防止多级任务间优先级反转并支持超时自动回退与链式传播）、内核级递归加锁（避免同线程死锁）、加锁超时机制（支持特定 Tick 时间内非阻塞等待），并提供 RAII 风格的 `UniqueLock` 自释放接口
- **性能度量子系统（Metrics）**：基于 Cortex-M4 DWT 硬件周期计数器提供极低开销的性能分析，可精确捕获中断延迟、上下文切换耗时、堆分配抖动、显示脏率及功耗效率，配合 ProcFS 并由 GitHub Actions CI 自动输出性能报告
- **任务通知（TaskNotify）**：FreeRTOS 风格的零开销 IPC，32 位直接通知，ISR 安全
- **POSIX 信号（Signal）**：signal/kill/raise，16 路信号位图，调度器切换时分发
- **软件定时器（TimerManager）**：OneShot/Periodic 模式，ISR 极短钩子 + daemon 任务执行回调
- **工作队列（WorkQueue）**：ISR bottom-half 延迟执行，中断上下文提交、任务上下文处理

### 🛡️ 安全与隔离

- **MPU 与 MMU 内存保护**：Cortex-M4 MPU 8 区域配置（Flash只读+RAM特权态+用户态栈沙盒），AArch64 MMU 虚拟内存管理（强类型 PTE 页表项与进程空间隔离），MemManage 异常捕获违规访问
- **系统调用参数强校验**：对所有 SVC（ARM）与 ECALL（RISC-V）系统调用指针及对应长度参数执行 `validate_user_ptr` 安全校验，对 `SYS_PRINT` 最长限制 256 字节并在安全边界内强制寻找 `\0` 终止符
- **IPC 传参描述符设计**：设计 `IpcReplyDesc` 描述符，通过 `r3` 寄存器传递用户态 `reply_buf` 和 `max_reply_len` 限制，突破 Cortex-M 4寄存器硬件传参瓶颈，防范内核缓冲区溢出
- **免挂死异常终止**：修复 `MemManage_Handler` 的 while 死循环。被终止的破坏任务在调用 `schedule()` 后正常 `return`，促使硬件自动执行尾链并触发 PendSV 完成上下文切换
- **特权分离**：CONTROL.nPRIV 用户态/内核态切换，PendSV 动态沙盒更新
- **SVC / ECALL 系统调用**：用户态通过软中断/系统调用指令进入内核态，PC 回溯读立即数（ARM）或读取 `a7`（RISC-V）分发系统调用

### 💾 存储与文件系统

- **VFS 虚拟文件系统**：VNode 多态抽象，open/read/write/close/lseek/ioctl 完整 POSIX 接口
- **RamFS**：内存文件系统，支持动态扩容
- **ProcFS**：/proc/meminfo + /proc/taskinfo 实时诊断
- **LittleFS**：掉电安全日志式文件系统（git submodule）
- **PhotonCache 光子缓存**：借鉴 BlueOS，LRU 页缓存 + 脏页延迟写，Flash 擦写降低 80%

### 🌐 网络与通信

- **lwIP 2.x 全栈**：IPv4/TCP/UDP/ICMP/ARP/DHCP，Socket + Netconn 双 API，已启用 `LWIP_TCPIP_CORE_LOCKING` 实现全系统 Socket API 的核心互斥锁保护，保证多线程调用安全。
- **OSAL 适配层**：sys_arch.cpp 完整实现 Mutex/Semaphore/Mailbox/Thread 映射。
- **DHCP 客户端**：动态获取 IP 地址。
- **BLE 蓝牙协议栈 (BleManager)**：通过硬件 IPC 模拟连接 Apollo3 蓝牙协处理器，预置设备信息、心率、电量等服务。对 OTA 与 Lua 传输的 `0xFF01` 特征值强制执行 **Security Mode 1 Level 3（配对并加密）** 写入，增加数据流数字签名校验，并缩小守护线程中的关中断区间。
- **分布式软总线与路由表**：借鉴 HarmonyOS，UDP 广播发现。安全加固版去除了硬编码 Token，采用 **Challenge-Response + HMAC-SHA256** 验证；对设备 ID 进行了严格字母及长度限制（`strnlen`），对能力集（cap）进行了正则白名单拦截（`^[a-z_,\[\"]+$`）；设置 `IP_MULTICAST_LOOP` 为 0 以过滤自发广播。设备路由表全面加入互斥锁保护、30秒 LRU 节点淘汰与 5/s 的抗 DDoS 注册速率限制，并移除包处理路径中的阻塞式 I/O，改为独立 Dump 任务显示。

### 🎨 显示与输入

- **帧缓冲 + 脏区域渲染**：借鉴 BlueOS 超级渲染树，set_pixel/fill_rect 自动标记脏区域，flush 只刷新变动矩形
- **OLED 驱动**：SPI 接口，窗口化局部更新，DMA 推送
- **ST7789 驱动**（miband 分支）：真实 AMOLED 硬件驱动，192×490 分辨率
- **输入事件子系统**：统一 InputEvent 抽象，触摸/按键/手势统一处理
- **页面栈导航器与 GUI 动画引擎**：支持 `ScreenNavigator` 栈式导航（Push/Pop/Replace），右滑手势 Pop，页面生命周期路由，支持平移与渐变转场动画
- **表盘 Complication 引擎与动态表盘商店**：借鉴 watchOS，数据驱动 UI，传感器数据变化才触发重绘；并支持 Watch Face Store，可从 LittleFS 文件系统中动态加载并解析运行第三方的 Lua 脚本表盘，支持页面生命周期钩子（on_create/on_show/on_tick/on_gesture）
- **NFC 卡模拟子系统**：设计底层数据流通道，支持 ISO14443A 标准 Card Emulation (CE) 模式，处理 APDU 数据，实现公交卡 (Transit) 与门禁卡 (Door Key) 的逻辑路由与 Notification 消息提示联动

### 📦 应用与脚本

- **ELF 动态加载器与重定位解析**：从 VFS 加载 ARM Thumb ELF 可执行文件，支持 `R_ARM_ABS32` 与 `R_ARM_THM_CALL` 复杂的重定位解析与内核 Symbol 导出表（如 sys_print, UI 导航, Lua 引擎等）的动态绑定
- **Lua 小程序引擎与 UI 深度绑定**：嵌入 Lua VM，可完全在 Lua 中定义复杂交互 UI，支持设置 `set_on_click_listener` 触控回调、直接调用页面导航器（`navigator_push`/`navigator_pop`），支持脚本开发手表应用
- **应用生命周期**：借鉴 watchOS，FOREGROUND/BACKGROUND/SUSPENDED 状态机，动态优先级调整
- **意图引擎**：借鉴 BlueOS，基于传感器数据的规则决策，自动提升应用优先级

### ⚡ 功耗管理

- **5 级功耗状态**：RUN → IDLE → LIGHT_SLEEP → DEEP_SLEEP → SHUTDOWN
- **Tickless 模式与抬腕唤醒**：空闲时关闭高频 SysTick 并配置低功耗唤醒定时器（RTC），醒来后补偿流逝 tick；配合 `WristWakeDetector` 加速度分量防抖算法，支持从睡眠/空闲状态中抬腕自动唤醒系统。
- **智能睡眠时钟规划与 BLE 状态绑定**：重构低功耗休眠唤醒估算算法，将 `ble_interval` (连接态 30ms，广播态 100ms) 纳入 `expected_idle_ticks` 的核心估算因子（`min(task, timer, ble_interval, next_vsync)`），确保低功耗切换不破坏蓝牙通信时序。在 BLE 处于 `CONNECTED` 高频同步态时，强制禁止系统进入 Tickless 深度睡眠，仅执行普通的 `WFI` 空闲挂起，以保护高频蓝牙链路的连贯性。

---

## 系统架构

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                        应用层 (apps/)                                    │
│  Shell │ Lua MiniProgram │ IntentEngine │ AppLifecycle │ UDP Echo       │
│  ELF Loader │ DistributedSoftBus │ WatchFace Complications              │
├─────────────────── SVC 系统调用边界 ────────────────────────────────────┤
│                        内核核心 (kernel/)                                │
│  Scheduler (5级抢占) │ FrameScheduler (30fps帧感知) │ TaskNotify        │
│  MutexPI (优先级继承) │ Semaphore │ MessageQueue │ Signal │ Timer        │
│  WorkQueue │ MPU │ PowerManager │ KernelHeap (线程安全+RAII)            │
│  POSIX 兼容层 │ libc.cpp │ Device/CharDevice/BlockDevice                │
├─────────────────────────────────────────────────────────────────────────┤
│                     文件系统 (vfs/)                                      │
│  VfsManager (VNode多态) │ RamFS │ ProcFS │ LittleFS + PhotonCache       │
├─────────────────────────────────────────────────────────────────────────┤
│                     网络协议栈                                           │
│  lwIP 2.x (TCP/UDP/ICMP/ARP/DHCP) │ OSAL Adapter │ DistributedSoftBus  │
├─────────────────────────────────────────────────────────────────────────┤
│                     驱动层 (drivers/)                                    │
│  display/ (OLED+FrameBuffer+ST7789) │ input/ (Touch+Gesture+InputEvent)│
│  sensor/ (SensorFramework+HealthSensor) │ storage/ (FlashBlockDevice)  │
├─────────────────────────────────────────────────────────────────────────┤
│                     架构抽象层 (arch/)                                   │
│  Arch:: namespace (disable_irq/enable_irq/wfi/trigger_ctx_switch)       │
│  arch_impl.hpp (cm4) │ arch_impl.hpp (cm4f, miband分支)                 │
│  start_first_task() │ systick_init() │ init_thread_stack()              │
├─────────────────────────────────────────────────────────────────────────┤
│                     板级支持 (boards/)                                   │
│  ti/lm3s6965-qb/board.h │ xiaomi/miband8/board.h (miband分支)          │
├─────────────────────────────────────────────────────────────────────────┤
│                     硬件层                                               │
│  ARM Cortex-M4 @12MHz (LM3S6965) / Cortex-M4F @96MHz (Apollo3)         │
│  256KB Flash + 64KB RAM (LM3S) / 1MB Flash + 384KB RAM (MiBand8)       │
│  PL011 UART │ LM3S Ethernet MAC │ SPI OLED │ I2C Touch/Sensor          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 已适配架构与芯片

auroraOS 通过 `arch/` 架构抽象层与 `boards/` 板级支持包（BSP）实现跨平台移植。当前代码库共适配 **3 大指令集架构（ARMv6-M / ARMv7-M / ARMv8-A、RISC-V）** 下的 **5 类芯片 / 板级**，覆盖 MCU 到 64 位应用处理器：

| 指令集架构 | 微架构 / 内核 | 板级 / 芯片 | 资源与状态 |
| --- | --- | --- | --- |
| ARMv6-M | Cortex-M0+ | ST Nucleo-L031K6（STM32L031K6） | 64KB Flash / 8KB RAM，无 FPU、无 DWT、无 MPU。main 分支板级，`build_m0plus` 构建目标；极简移植（仅调度器 + Shell + UART），验证 M0+ 架构正确性 |
| ARMv7-M | Cortex-M3 | TI LM3S6965-QB（Stellaris） | 256KB Flash / 64KB RAM，PL011 UART，板载 Ethernet MAC。main 分支板级，复用 Cortex-M4 兼容抽象层；HIL 冒烟测试主力平台（QEMU `lm3s6965evb`） |
| ARMv7-M | Cortex-M4F | 小米手环 8（Ambiq Apollo3 Blue） | 1MB Flash / 384KB RAM，带 FPU，ST7789 显示 + BLE。miband 分支；驱动触控 / 心率 / 运动传感器 |
| ARMv8-A | Cortex-A（AArch64，64-bit） | 通用 QEMU / SoC 平台 | `arch/arm/cortex-a/aarch64` 提供 64 位异常模型、`gic/`（GIC 中断控制器）、`mmu/`（页表，对应 `kernel/vasp.hpp` 虚拟地址空间），为富功能场景预留 |
| RISC-V | RV32IMAC | QEMU `rv32_virt` | main 分支 `rv32` 工作线；独立异常向量 `trap_vector.S` 与 `trap.cpp`，验证架构无关性 |

架构抽象层（`arch_impl.hpp` / `boot.S` / `trap.S` / `exceptions.S`）统一向上提供 `disable_irq`、`enable_irq`、`wfi`、`trigger_ctx_switch`、`start_first_task`、`systick_init`、`init_thread_stack` 等 HAL 接口，内核其余部分完全架构无关。Cortex-M 系列共享 Thumb-2 异常与栈帧模型（M0+ 无硬件 FPU、无 DWT，M3/M4/M4F 带 MPU）；AArch64 引入 MMU 与 GIC，走独立的 64 位启动与异常路径；RISC-V 以 `mcause` / `mepc` 实现上下文切换。所有移植均通过 `boards/<vendor>/<board>/board.h` 的 `BOARD_*` 宏集中暴露硬件常量，逻辑层不直接访问寄存器。

---

## 目录结构

```
auroraOS/
├── apps/                      # 应用层
│   ├── kernel.cpp             #   内核主入口 kernel_main
│   ├── shell.cpp/hpp          #   交互式 Shell（help/cat/ps/free/ifconfig/udpsend/exec/ping/netstat）
│   ├── elf_loader.cpp/hpp     #   ELF32 动态加载器
│   ├── elf.hpp                #   ELF 头结构定义
│   ├── net_app.cpp/hpp        #   网络应用 + DHCP 客户端
│   ├── mini_program_engine.hpp#   Lua 5.4.6 小程序引擎
│   ├── camera_app.cpp         #   摄像头应用（camera 驱动）
│   ├── lua_ui_binding.cpp/hpp #   Lua ↔ UI 绑定层
│   ├── sandbox_test_app.cpp   #   沙箱隔离验证（用户态非法访问触发 HardFault）
│   ├── m0plus_main.cpp        #   Cortex-M0+ 极简入口（仅调度+Shell+UART）
│   └── watch/                 #   手环应用（miband 分支）
│       ├── watch_app.cpp/hpp  #     WatchApp 主类（4 页面）
│       ├── miband_kernel.hpp  #     手环内核配置
│       ├── miband_main.cpp    #     手环入口
│       └── font_engine.hpp    #     位图字体引擎
├── kernel/                    # 内核核心
│   ├── task.hpp               #   优先级抢占式调度器
│   ├── memory.hpp/cpp         #   线程安全内核堆（LockGuard RAII）
│   ├── mutex.hpp              #   PI 优先级继承互斥锁
│   ├── semaphore.hpp          #   计数信号量（try_wait ISR 安全）
│   ├── msg_queue.hpp          #   消息队列（try_push/try_pop 非阻塞）
│   ├── signal.hpp             #   POSIX 信号（signal/kill/raise）
│   ├── task_notify.hpp        #   任务通知（FreeRTOS 风格零开销 IPC）
│   ├── timer.hpp              #   软件定时器框架
│   ├── work_queue.hpp         #   ISR bottom-half 工作队列
│   ├── frame_scheduler.hpp    #   帧感知调度器（30fps 帧内/帧间）
│   ├── frame_scheduler_v2.hpp #   帧调度器 v2（增强 deadline）
│   ├── mpu.hpp                #   MPU 内存保护单元
│   ├── security_monitor.hpp   #   安全监控（运行时监督 + 看门狗联动）
│   ├── watchdog_manager.hpp   #   看门狗管理（idle 超阈喂狗）
│   ├── ipc.hpp/cpp            #   seL4 风格同步 IPC（Endpoint call/receive/reply）
│   ├── cspace.hpp             #   能力空间（CapType/CapRights，细粒度权限雏形）
│   ├── vasp.hpp               #   AArch64 虚拟地址空间（MMU 页表）
│   ├── ota.cpp/hpp            #   OTA 升级（断电安全双分区）
│   ├── page_allocator.hpp     #   页分配器
│   ├── symbol_export.cpp/hpp  #   ELF 符号导出
│   ├── syscall_validate.hpp   #   系统调用参数强校验
│   ├── power_manager.hpp      #   分级功耗管理（5 级 + Tickless）
│   ├── posix.cpp/hpp          #   POSIX 兼容层
│   ├── device.hpp             #   Device/CharDevice/BlockDevice 框架
│   ├── arch_api.hpp           #   架构无关 HAL 接口契约
│   ├── libc.cpp               #   裸机 libc（memcpy/memset/atoi/strlen）
│   ├── stdlib.h               #   C 标准库桩
│   └── uart_device.hpp        #   UART 字符设备
├── boot/                      # 启动与硬件抽象
│   ├── boot.S                 #   Reset_Handler + PendSV + SVC 汇编
│   ├── interrupts.cpp/hpp     #   SVC_Handler_C + SysTick + MemManage
│   └── uart.c/h               #   PL011 UART 驱动
├── vfs/                       # 虚拟文件系统
│   ├── vfs.cpp/hpp            #   VfsManager + VNode 多态抽象
│   ├── ramfs.cpp/hpp          #   RamFile 内存文件
│   ├── procfs.hpp             #   ProcFS（/proc/meminfo + /proc/taskinfo）
│   ├── littlefs_vnode.hpp     #   LittleFS VNode 适配
│   ├── photon_cache.hpp       #   光子存储 LRU 缓存层
│   └── softbus.cpp/hpp        #   RPC 软总线（休眠，未编译）
├── net/                       # 网络子系统
│   ├── eth_driver.cpp/hpp     #   StellarisEth L2 以太网驱动
│   ├── net_device.hpp         #   NetDevice 抽象基类
│   ├── distributed_bus.hpp    #   分布式软总线（UDP 广播 + JSON）
│   ├── device_route_table.hpp #   远程设备路由表
│   └── ble/                   #   BLE 协议栈（miband 分支）
│       └── ble_stack.hpp      #     GATT 服务架构
├── drivers/                   # 驱动层
│   ├── display/
│   │   ├── oled_driver.hpp    #   OLED 驱动（窗口化局部更新）
│   │   ├── framebuffer.hpp    #   帧缓冲 + 脏区域渲染
│   │   └── st7789_driver.hpp  #   ST7789 真实驱动（miband 分支）
│   ├── input/
│   │   ├── input_event.hpp    #   统一输入事件抽象
│   │   ├── touch_driver.hpp   #   触摸驱动
│   │   └── gesture_recognizer.hpp # 手势识别（miband 分支）
│   ├── sensor/
│   │   └── sensor_framework.hpp #  传感器框架（Zephyr 风格 channel API）
│   ├── power/
│   │   └── charging_manager.hpp # 充电管理驱动与锂电池电量插值算法
│   ├── storage/
│   │   └── flash_device.hpp   #   Flash 块设备抽象
│   ├── camera/
│   │   └── camera_device.hpp  #   摄像头设备抽象（dummy 实现）
│   ├── gpu/
│   │   ├── display_controller.hpp # 显示控制器抽象
│   │   ├── gpu_device.hpp      #   GPU 设备接口
│   │   ├── soft_gpu_device.cpp/hpp # 软件渲染 GPU（合成/缩放）
│   │   └── surface.hpp         #   绘制 surface 封装
│   ├── nfc/
│   │   └── nfc_controller.hpp  #   NFC 控制器抽象
│   ├── usb/
│   │   └── usb_core.hpp        #   USB 核心抽象
│   └── watchdog/
│       └── watchdog.hpp        #   看门狗硬件驱动接口
├── adapter/net/               # lwIP OSAL 适配层
│   ├── sys_arch.cpp           #   Mutex/Sem/Mbox/Thread 映射
│   ├── ethernetif.cpp         #   pbuf ↔ 网卡 FIFO 互转
│   ├── lwipopts.h             #   lwIP 配置
│   └── arch/                  #   cc.h + sys_arch.h
├── arch/                      # 架构抽象层
│   ├── arm/
│   │   ├── cortex-m/
│   │   │   ├── cm4/arch_impl.hpp  #   Cortex-M4 架构实现
│   │   │   └── cm4f/arch_impl.hpp #   Cortex-M4F 架构实现（miband 分支，FPU）
│   │   └── cortex-a/              #   ARMv8-A AArch64 架构实现
│   └── riscv/rv32imac/        #   RISC-V 32 架构移植
│       ├── arch_impl.hpp      #     CSR 寄存器控制与 CLINT 时钟驱动
│       ├── boot.S             #     启动汇编入口
│       ├── trap_vector.S      #     统一 M-mode 硬件中断与异常处理向量
│       └── trap.cpp           #     C++ 异常与 syscall 路由分发
├── boards/
│   ├── ti/lm3s6965-qb/board.h #   TI LM3S6965 板级定义
│   ├── st/nucleo-l031k6/board.h #  ST Nucleo-L031K6 板级定义（Cortex-M0+）
│   ├── xiaomi/miband8/board.h #   小米手环 8 板级定义（miband 分支）
│   └── riscv/rv32_virt/       #   QEMU RV32 Virt 板级定义
├── syscall/syscall.hpp        # SVC 系统调用（sys_print/sys_yield/sys_sleep）
├── ai/intent_engine.hpp       # 意图引擎（传感器规则决策）
├── guix/                      # GUIX 智能图形框架
│   ├── compositor.cpp/hpp     #   合成器（多层窗口合成）
│   ├── window.cpp/hpp         #   窗口抽象
│   └── surface.hpp            #   绘制 surface
├── bootloader/                # 安全启动
│   └── boot_main.cpp          #   Ed25519 固件验签 + 断电安全 OTA 双分区交换
├── metrics/                   # 指标采集
│   └── metrics_collector.cpp/hpp # 性能/内存指标
├── ui/complications.hpp       # 表盘 Complication UI 引擎
├── utils/json_parser.hpp      # 裸机 JSON 解析器
├── config/                    # 构建配置
│   ├── config.h               #   版本/容量常量
│   ├── net_config.h           #   IPv4 配置（Kconfig 解析）
│   ├── autoconf.h             #   Kconfig 生成（自动）
│   ├── linker.ld              #   LM3S6965 链接脚本
│   └── toolchain.cmake        #   ARM 工具链
├── 3rdparty/                  # 第三方依赖
│   ├── lwip/                  #   lwIP 2.x TCP/IP 协议栈
│   ├── lua/                   #   Lua 5.4.6 虚拟机
│   └── littlefs/              #   LittleFS 文件系统（submodule）
├── scripts/
│   ├── genconfig.py           #   Kconfig → autoconf.h 生成器
│   ├── menuconfig.bat         #   配置菜单
│   ├── hil_runner.py          #   QEMU HIL 冒烟测试驱动（pexpect，TCP 串口）
│   └── gen_metrics_report.py  #   性能报告生成（CI DWT 度量）
├── Kconfig                    # Kconfig 配置定义
├── .config                    # 当前配置
├── CMakeLists.txt             # CMake 构建脚本
├── .github/workflows/build.yml# GitHub Actions CI
└── README.md                  # 本文档
```

---

## 子系统详解

### 调度器与任务管理

auroraOS 调度器实现 5 级优先级抢占式调度，采用位图与侵入式双链表实现 O(1) 调度效率，并支持优先级继承与帧感知调度。

```cpp
// 任务优先级阶梯
enum class TaskPriority : uint8_t {
    Idle     = 0,  // 空闲任务（最低）
    Low      = 1,  // 后台计算
    Normal   = 2,  // 默认业务
    High     = 3,  // 交互 Shell
    Realtime = 4   // 硬实时（网络 RX）
};

// 任务控制块
struct TaskControlBlock {
    uint32_t*    stack_ptr;          // 栈顶指针（PendSV 保存/恢复）
    void         (*entry_point)();   // 入口函数
    TaskState    state;              // Ready/Running/Sleeping/Blocked_On_Notify
    TaskPriority base_priority;      // 基础优先级
    TaskPriority current_priority;   // 动态优先级（优先级继承）
    uint32_t     sleep_ticks;        // 休眠倒计时
    uint32_t     notify_value;       // 任务通知值
    bool         notify_pending;     // 通知待处理
    uint32_t     pending_signals;    // 信号位图
    SignalHandler signal_handlers[16];// 信号回调表
    uint32_t     stack_base;         // MPU 沙盒基址
    uint8_t      size_pow2;          // MPU 沙盒大小（2 的幂）
};
```

**O(1) 优先级就绪队列调度算法**：采用就绪优先级位图（`ready_bitmask`）配合侵入式双向链表。通过位扫描操作可在 O(1) 时间内定位最高优先级非空队列。就绪任务的入队、出队、以及同优先级 Round-Robin 时间片轮转均在 O(1) 内完成，彻底规避了老版本 O(N) 链表遍历开销。调度器在 SysTick 触发时或任务主动 yield/sleep 时进行调度。

**上下文切换**：PendSV 异常（最低优先级）负责保存/恢复 r4-r11 + 栈指针，利用 Cortex-M 硬件自动压栈 r0-r3/r12/lr/pc/xpsr，实现高效切换。

**信号安全加固与栈隔离**：
- **栈隔离设计**：将信号分发点（`dispatch_signals`）从切换前对 `next_task` 提前处理，移动到 `schedule()` 最开端只对当前 `current_task` 进行处理。当任务在主动让出（Sleep/Yield）时，信号处理函数完全运行在任务自身的 PSP 栈上；当任务被 SysTick 中断抢占时，信号处理运行在中断栈（MSP）上。这彻底消除了在 outgoing 任务的栈上运行 incoming 任务信号回调的安全漏洞，有效杜绝了栈污染与溢出。
- **SIGKILL 强语义立即终止**：重构了 `kill()` 逻辑。发送 `SIGKILL` 信号时，会**立即**强制将目标任务的状态修改为 `TaskState::Terminated`，确保其在下一次调度器的选组轮询中绝不可能被再次选中（无论它之前是睡眠还是就绪），消除了已被 Terminated 的任务继续多运行一个时间片的隐患。

### 内存管理

auroraOS 采用双轨制内存分配策略，兼顾灵活性与确定性实时性：
- **KernelHeap 堆管理**：采用首次适配（First-Fit）与块分裂（Split）算法，支持线程安全（RAII LockGuard 保护）。并重构了释放合并逻辑，引入**延迟合并（Lazy Coalesce）**技术，将 deallocate 优化为 O(1) 极其敏捷的标记操作，并在 OOM 触发时懒执行碎片整理（`defragment()`），极大缩减了系统在高频分配释放时的延迟抖动。
- **MemoryPool 内存池**：提供了 O(1) 的固定大小内存块分配器 `MemoryPool<T, BlockCount>`，内部通过空闲链表管理，分配与释放均为 O(1) 且绝无外部碎片，极其适合高频申请释放的传感器数据帧和网络包。

```cpp
// 线程安全内核堆示例
class KernelHeap {
    struct BlockHeader { size_t size; bool is_free; BlockHeader* next; };
    Mutex heap_mutex_;  // LockGuard RAII 保护
    void* allocate(size_t size);   // 4 字节对齐，首次适配 + 分裂
    void  deallocate(void* ptr);   // 标记空闲，延迟合并 (Lazy Coalesce)
    void  defragment();            // 碎片整理，合并相邻空闲块
};
// 覆写全局 operator new/delete 走 KernelHeap
```

堆内存来自链接脚本 `_heap_start` 到 `_heap_end` 之间的空间，与栈共享 64KB RAM。

### 同步原语

| 原语 | 特性 | 借鉴来源 |
|------|------|---------|
| **Mutex** | 优先级继承（PI），递归加锁，可配置超时唤醒，sleep(1)让出，UniqueLock RAII | NuttX |
| **Semaphore** | 计数信号量，try_wait() ISR 安全 | FreeRTOS |
| **MessageQueue** | 无锁单生产者单消费者 (SPSC) 环形队列，支持内存屏障，免去中断锁，try_push/try_pop 非阻塞 | ThreadX |
| **TaskNotify** | 32 位直接通知，零内存分配，ISR 极速 | FreeRTOS |
| **Signal** | POSIX signal/kill/raise，16 路位图 | NuttX |

### 虚拟文件系统 VFS

VNode 多态抽象，所有设备/文件统一为 `open/read/write/close/lseek/ioctl` 接口。

```cpp
class VNode {
    virtual int read(char* buf, int len, int offset) { return -1; }
    virtual int write(const char* buf, int len, int offset) { return -1; }
    virtual int ioctl(int request, void* arg) { return -1; }
    virtual int get_size() const { return 0; }
};

class VfsManager {
    int  open(const char* path);              // 精确路径匹配挂载表
    int  read(int fd, char* buf, int len);    // 透传 offset，自动推进游标
    int  lseek(int fd, int offset, int whence); // SEEK_SET/CUR/END
    int  ioctl(int fd, int request, void* arg);
};
```

支持的文件系统：RamFS（内存文件）、ProcFS（/proc 诊断）、LittleFS（掉电安全持久存储）。

**ELF 动态加载器（ElfLoader）安全防护**：
- **地址回绕溢出校验**：在第一轮扫描 PT_LOAD 段时，校验 `phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr`。如果发生加法回绕整数溢出，则立即拒绝加载。
- **最大内存配额硬限制**：对 ELF 所需的总虚拟内存跨度 `total_memsz` 实施 256KB 的硬性限额保护，防止超大段配置引发系统级堆耗尽崩溃。
- **第二轮加载 OOB 检查**：加载前强制校验 `offset_in_mem + phdr.p_memsz <= total_memsz`，并且在第二遍读取时重新校验 `phdr.p_filesz <= phdr.p_memsz`，阻断利用 TOCTOU（Time-of-Check to Time-of-Use）恶意更改段大小来破坏内核堆的攻击漏洞。
- **截断与未初始化内存泄露防护**：对比 `VfsManager::read` 的实际返回字节数与段声明大小。如果文件被恶意截断，则动态将未读出区域及 BSS 段全量清零，严防内核堆历史敏感垃圾信息泄露。
- **Section/Symbol/Rel 结构求模对齐校验**：强制校验 `shdrs[i].sh_size % sizeof(Elf32_Sym) == 0` 及 `sizeof(Elf32_Rel) == 0`，分配与读取使用相同的求模对齐边界大小，杜绝畸形 ELF section size 导致的堆溢出。
- **W^X 严格内存访问限制**：针对 AArch64 页表映射实现严格的 W^X 保护。不使用通用的 RWX，而是逐个审查页所属的 `PT_LOAD` 段 `p_flags`。只在 `PF_W` 时给写权限，`PF_X` 时给执行权限，保证只读代码段不被篡改，数据段/栈不可执行。
- **未解析符号强拦截**：当重定位解析遇到内核导出表中不存在的 unresolved symbol 时，强制中断加载流程，执行完整的堆资源释放，防止系统跳转至 `0x0` 地址跑飞。
- **MPU 沙盒错位修复**：Cortex-M 分支的任务栈空间改用 `PageAllocator` 页面对齐分配（4096字节对齐），并且在 `create_task` 时将 `size_pow2` 参数正确纠正为 `12`，彻底修复了传 `0` 导致 MPU 配置失败从而保留旧任务沙盒权限的安全隔离缺失漏洞。

### 网络协议栈

深度集成 lwIP 2.x，通过 OSAL 适配层对接 auroraOS 内核原语。

```cpp
// OSAL 适配（adapter/net/sys_arch.cpp）
sys_mutex_t → Mutex*         // 优先级继承互斥锁
sys_sem_t   → Semaphore*     // 计数信号量
sys_mbox_t  → MessageQueue<void*, 16>*  // 消息队列
sys_thread_t → TaskControlBlock*        // 调度器任务
```

支持 DHCP 动态获取 IP、BSD Socket API、Netconn API。Shell 提供 `ifconfig`/`udpsend` 命令，内置 UDP Echo Server 测试。

**网卡接收防死锁与 OOB 加固**：
- **最大长度负数保护**：在 L2 以太网驱动 `receive_frame` 接口中，对传入 of `max_len` 进行有符号校验（`max_len <= 0` 直接返回），防止无符号强制转型绕过导致的堆缓冲区溢出。
- **网卡死锁防御**：当读取到硬件帧大小超出 `max_len` 或异常时，除了清除中断标志，额外使用循环强制从数据寄存器中**排空（drain）**该包剩余字节，避免以太网控制器 FIFO 阻塞，彻底解决了因网络畸形包注入导致的中断挂死与网卡死锁。
- **以太网驱动线程安全与对齐**：使用独立 `rx_mutex_` 与 `tx_mutex_` 隔离底层硬件寄存器的并发访问；数据包 FIFO 传输由原先的逐字节遍历拷贝升级为 **4字节对齐 `memcpy`**，避免因硬件总线对齐要求引发的 HardFault；改写 MAC 发送控制脉冲，直接覆盖写入，避免竞态条件。
- **lwIP 核心锁保护**：启用 `LWIP_TCPIP_CORE_LOCKING`，强制全系统 Socket 访问在内核核心锁同步下运行，杜绝多任务同时读写 socket 产生的数据错乱。

### 设备驱动框架

类 Linux 设备模型，所有设备通过 DeviceRegistry 注册到 `/dev/`。

```cpp
class Device : public VNode {
    const char* name_; DeviceType type_;  // Char / Block
    virtual int open() / close() / ioctl();
};
class CharDevice : public Device { /* 字节流读写 */ };
class BlockDevice : public Device { /* 扇区读写 */ };
class DeviceRegistry {
    bool register_device(Device* dev);  // 自动挂载到 /dev/<name>
};
```

### MPU 内存保护

Cortex-M4 MPU 8 区域配置，实现用户态/内核态内存隔离。

```cpp
class MPU {
    void configure_region(num, base, size_pow2, ap, execute_never);
    void update_user_sandbox(stack_base, size_pow2);  // PendSV 动态切换
};
// 权限：AP_NO_ACCESS / AP_PRIV_RW / AP_ALL_RW / AP_PRIV_RO / AP_ALL_RO
// Region 0: Flash 全系统只读
// Region 1: RAM 仅特权态读写（用户态 XN 禁止执行）
// Region 2: 当前用户任务栈沙盒（动态）
```

PendSV 上下文切换时调用 `mpu_switch_sandbox()` 动态更新 Region 2。MemManage_Handler 捕获违规访问。内置 `hacker_app_task` 攻击演示验证隔离有效性。

### 显示与输入

**帧缓冲 + 脏区域渲染**（借鉴 BlueOS 超级渲染树）：

```cpp
template<uint16_t W, uint16_t H>
class FrameBuffer {
    void set_pixel(x, y, color);     // 自动标记脏区域
    void fill_rect(x, y, w, h, color); // 自动标记脏区域
    void flush(OledDriver& oled);    // 只推送脏矩形，带宽降 60-97%
};
```

**表盘 Complication 引擎**（借鉴 watchOS）：数据驱动 UI，只有传感器数据变化才触发重绘。

**传感器框架**（借鉴 Zephyr）：统一 `sample_fetch()` + `channel_get()` 接口，支持加速度/心率/计步/电量。

### Lua 小程序引擎

嵌入 Lua 5.4.6 虚拟机，支持脚本开发手表应用（对标 Garmin Connect IQ）。

```cpp
class MiniProgramEngine {
    lua_State* L_;
    // 暴露 C++ API 给 Lua：
    static int api_get_heart_rate(lua_State* L);  // 读取心率
    static int api_draw_text(lua_State* L);       // 绘制文本
    // Lua 脚本可调用传感器、帧缓冲、系统功能
};
```

CMake 配置 `-DLUA_32BITS=1` 适配 32 位平台，裁剪 `lua.c`/`luac.c`/`liolib.c` 等不需要的模块。

### 分布式软总线

借鉴 HarmonyOS，UDP 广播设备发现 + JSON 信标 + 设备路由表。

```cpp
class DistributedSoftBus {
    void init();                // UDP socket + SO_BROADCAST + IP_MULTICAST_LOOP(0) + bind 8899
    void broadcast_beacon();    // 广播加密信标，防止重放与伪造
    void listener_task();       // 阻塞接收 → JsonParser 解析 → 严格检验 → DeviceRouteTable 注册
};

**分布式协议栈与设备路由表安全加固**：
- **零拷贝 JSON 解析器 OOB 保护**：在 `JsonParser::get_raw_value` 中增加前导字符防越界校验 `*val_start == '\0'`。一旦 JSON 键名后紧跟空字符（例如网络畸形截断报文），解析器将立即中断并快速失败，防止指针跳过空字符读取未授权相邻内存区。
- **设备路由表抗污染**：为 `DeviceRouteTable::register_or_update_device` 添加严格的 `!ip || !dev_id || !cap` 空指针入参校验，确保即使网络层报文解析为空也无法向路由表写入非法数据。
- **安全认证与白名单过滤**：移除硬编码的鉴权 Token，引入 **Challenge-Response 挑战应答 + HMAC-SHA256 签名机制**；增加 `device_id` 严格全字母及 `strnlen` 长度校验；增加能力集 `cap` 的正则白名单过滤（仅允许 `^[a-z_,\[\"]+$`），拒绝非合规的特殊控制字符。
- **自发广播过滤**：在 UDP Socket 初始化中利用 `IP_MULTICAST_LOOP = 0` 禁用本地回环，防止本机软总线接收并处理自身发送的发现广播。
- **路由表并发控制与 LRU 淘汰**：增加 `table_mutex_` 对设备路由表进行互斥读写控制；引入 LRU 机制，自动清理 `now - last_seen > 30秒` 的不活跃外部设备。
- **流量异常与 DDoS 流量防御**：为路由表注册添加 `5次/秒` 的阈值限制，超出限额的注册请求直接丢弃，防止异常网络心跳或恶意设备发起泛洪攻击。
- **非阻塞 I/O 隔离**：从核心网络包解析和注册路径中彻底移除阻塞式的串口/Flash I/O 操作，将其改由独立的后台 Dump 定时任务（`dump_routes`）定期调用，完全释放网络接收的吞吐性能。
```

### IPC 与能力空间

借鉴 seL4 的微内核理念，auroraOS 在 `kernel/ipc.hpp` + `ipc.cpp` 中实现了 seL4 风格的同步 IPC 端点（Endpoint）：

- `Endpoint::call(sender, msg, len, reply_buf, max_reply_len)`：发送并阻塞等待回复（Send + Receive 合并语义）；
- `Endpoint::receive(receiver, msg_buf, max_len)`：阻塞接收来自端点的消息；
- `Endpoint::reply(receiver, sender_id, reply_msg, len)`：服务端回包并解阻塞发送方。

端点内部用侵入式发送队列 / 接收队列管理阻塞任务，状态机 `IpcState`（Ready / Receiving / ReplyBlocked / Sending）描述任务在 IPC 中的阻塞态，为后续细粒度权限模型打底。

与之配套的是 `kernel/cspace.hpp` 定义的**能力空间（Capability Space）**雏形：以 `Capability{type, rights, badge, object}` 描述对 Endpoint / 线程 / 内存对象的引用，`CapRights` 用 1-bit 字段表达 read / write / grant 权限，`CapType` 区分 Null / Endpoint / Thread / Memory，`MAX_CSPACE_SLOTS = 16`。当前 cspace 仍是头文件骨架、未接入 CMake 构建，但已为"以能力代替全局地址空间"的 seL4 式安全模型预留了扩展点。

### 安全监控与看门狗

auroraOS 在 MPU/MMU 与系统调用校验之外，增加了运行时的**安全监控（Security Monitor）**与**看门狗管理（WatchdogManager）**：

- `kernel/security_monitor.hpp`：基于看门狗的运行时监督器，在调度关键路径上触发喂狗，检测系统是否陷入死锁 / 饿死；
- `kernel/watchdog_manager.hpp` + `drivers/watchdog/watchdog.hpp`：`WatchdogManager::instance()` 单例在 `init(driver, timeout_ms)` 时按 80% 阈值设定最大 idle tick；调度器每次 `on_schedule(priority)` 时若下一任务优先级为 0（Idle）则累加 idle 计数并在超阈前 `kick()`，非空闲则清零；通过覆盖 `task.hpp` 中的弱符号 `watchdog_feed()`，把喂狗逻辑透明地挂到调度循环上，无需业务代码手动喂狗。

此外，`bootloader/boot_main.cpp` 实现了**安全启动**：上电后先用 Ed25519（`3rdparty/ed25519`）对 A/B 双分区的 `FirmwareHeader` 验签，仅当签名有效才跳转到应用矢量表的复位向量；OTA 升级走"先擦 A → 拷贝 B 正文 → 最后写 A 头"的顺序，并在擦 B 之前重新校验 A，保证断电也能安全回滚到有效分区。生产构建必须提供真实 `ROOT_PUBLIC_KEY`，否则编译期 `#error`；开发 / QEMU 构建可开启 `CONFIG_OTA_DEV_MODE` 使用 mock 公钥。

---

## 分支结构

auroraOS 采用多分支并行开发策略：

| 分支 | 定位 | 自有代码 | 关键特性 |
|------|------|---------|---------|
| **main** | 核心主线（含 TI LM3S / QEMU-RV32 / ST Nucleo-L031K6 M0+ 板级） | ~40,000 行 | Lua 小程序 + 意图引擎 + 应用生命周期 + 分布式软总线 + IPC/能力空间 + 安全监控 |
| **miband** | 小米手环 8 移植 | 7,553 行+ | Cortex-M4F + ST7789 + BLE + 手环应用 + 字体引擎 |

main 提供 OS 能力，miband 验证真实硬件落地，两条线并行推进。

---

## 快速开始

### 环境准备

确保已安装以下工具链：

- `arm-none-eabi-gcc` / `arm-none-eabi-g++`（ARM GNU Toolchain）
- `CMake` >= 3.20
- `Make` / `Ninja` / `MinGW32-make`
- `QEMU`（qemu-system-arm，用于 LM3S6965 模拟）
- `Python 3` + `kconfiglib`（用于 Kconfig 配置生成）

```bash
# 安装 kconfiglib
pip install kconfiglib
```

### 构建项目

```bash
git clone --recursive https://github.com/jencaoking/auroraOS.git
cd auroraOS

# 生成 Kconfig 配置（首次必做）
python scripts/genconfig.py

# 构建
mkdir build && cd build
cmake -DBOARD=lm3s6965-qb ..
make -j8
```

### 在 QEMU 中运行

```bash
qemu-system-arm -M lm3s6965evb -nographic -kernel auroraOS.elf
```

启动后进入 `aurora>` 终端。

---

## Shell 命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `help` | 显示可用命令 | `help` |
| `about` | 打印内核版本与架构 | `about` |
| `cat <file>` | 查看文件内容 | `cat /proc/meminfo` |
| `ps` | 显示所有任务状态 | `ps` |
| `free` | 显示内存使用 | `free` |
| `exec <file>` | 执行 ELF 动态应用 | `exec /tmp/app.elf` |
| `ifconfig` | 查看网络接口状态 | `ifconfig` |
| `udpsend <ip> <port> <msg>` | 发送 UDP 数据包 | `udpsend 192.168.1.100 8080 hello` |
| `ping <ip>` | 网络连通性测试（ICMP raw，仅 QEMU/HIL 配置启用） | `ping 192.168.1.1` |
| `netstat` | 显示监听中的套接字 | `netstat` |

---

## 配置系统

auroraOS 采用 Linux 内核风格的 Kconfig 配置系统，支持可裁剪构建。

```bash
# 修改配置
python scripts/menuconfig.bat   # Windows
# 或直接编辑 .config 后重新生成
python scripts/genconfig.py
```

主要配置项：

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `BOARD` | string | `lm3s6965-qb` | 目标板级 |
| `SCHEDULER` | bool | y | 启用调度器 |
| `MAX_TASKS` | int | 16 | 最大任务数 |
| `TICK_RATE_HZ` | int | 1000 | SysTick 频率 |
| `VFS` | bool | y | 虚拟文件系统 |
| `FS_RAMFS` | bool | y | RamFS |
| `FS_PROCFS` | bool | y | ProcFS |
| `DEVICE_UART` | bool | y | UART 驱动 |
| `TIMER_MANAGER` | bool | y | 软件定时器 |
| `WORK_QUEUE` | bool | y | 工作队列 |
| `NETWORKING` | bool | y | 网络子系统 |
| `NET_LWIP` | bool | y | lwIP 协议栈 |
| `NET_DEFAULT_IP` | string | `10.0.2.15` | 默认 IP |
| `NET_DEFAULT_MAC` | string | `00:11:22:33:44:55` | 默认 MAC |

---

## 开发路线图

### Phase 1: 内核加固（3-6 个月）— ✅ 100% 完成

- [x] 优先级抢占调度器 + 上下文切换
- [x] 架构抽象层（arch_api → arch_impl）
- [x] 线程安全内核堆 + RAII LockGuard
- [x] Mutex（优先级继承）+ Semaphore + MessageQueue
- [x] POSIX 兼容层（open/read/write/close/ioctl/lseek/sleep/sem_*）
- [x] 设备驱动框架（Device/CharDevice/BlockDevice + DeviceRegistry）
- [x] ProcFS（/proc/meminfo + /proc/taskinfo）
- [x] Shell 增强（ps/free/ifconfig/udpsend）
- [x] 任务通知（TaskNotify）+ POSIX 信号（Signal）
- [x] 信号分发与调度栈隔离安全加固
- [x] ELF 动态加载器安全边界与溢出校验
- [x] 帧感知调度（FrameScheduler）
- [x] 软件定时器（TimerManager）
- [x] 工作队列（WorkQueue）
- [x] Kconfig 配置系统
- [x] lwIP TCP/IP 全栈 + DHCP
- [x] MPU 内存保护（计划外提前实现）
- [x] 以太网与网络协议栈安全加固 (4字节对齐, 独立锁保护, lwIP core locking)
- [x] 分布式软总线 Challenge-Response 鉴权与正则白名单防御
- [x] 设备路由表 LRU 淘汰、DDoS 限流与非阻塞 Dumping 设计
- [x] 无锁 Single-Producer-Single-Consumer (SPSC) 环形消息队列 (适配 BLE 中断)
- [x] 蓝牙 Security Mode 1 Level 3 配对认证与 Lua 签名验签
- [x] 蓝牙状态联动的智能 Tickless 睡眠决策与休眠窗口动态调节
- [x] RISC-V 32 (RV32IMAC) 架构移植与 CLINT/CSR 控制 (实现 QEMU Virt 运行)
- [x] ELF 加载器 Heap 越界写/TOCTOU/W^X 控制安全加固
- [x] 系统调用 SVC/ECALL 参数校验安全加固 (IpcReplyDesc 突破传参瓶颈)
- [x] 故障处理器（MemManage）尾链调度死锁解除
- [x] 内核调度器死亡任务兜底切换与死锁修复
- [x] 内核内存堆双重释放拦截（魔数）与 realloc 越界读取修复
- [x] irq_save/restore 扩展
- [x] 消息队列优先级 (紧急插队 push_urgent)
- [x] 测试框架升级 (集成测试, OOM 压力测试, HIL QEMU 自动化)
- [x] CI/CD 强化 (clang-tidy, cppcheck, gcov, 自动 release)
- [x] 内存确定性固化 (CONFIG_NO_DYNAMIC_ALLOCATION 无动态分配模式)

### Phase 2: 手表原型（6-12 个月）— 🎉 100% 完成

- [x] 帧缓冲 + 脏区域渲染（FrameBuffer）
- [x] OLED 驱动框架（OledDriver）
- [x] 输入事件子系统（InputEvent）
- [x] 触摸驱动（TouchDriver）
- [x] 表盘 UI + Complications（WatchFaceEngine）
- [x] 分级功耗管理（PowerManager，5 级 + Tickless）
- [x] 传感器框架（SensorFramework，Zephyr 风格）
- [x] LittleFS + PhotonCache 光子缓存
- [x] ST7789 真实驱动（miband 分支）
- [x] BLE 协议栈架构（miband 分支）
- [x] BleStack 底层并发与挂死防范加固
- [x] 2D 绘图引擎（Renderer2D：直线/圆/弧/三角形/圆角矩形/文本/混色）
- [x] Tickless 真实硬件唤醒定时器
- [x] 充电管理驱动
- [x] 以太网与网络协议栈安全加固 (4字节对齐, 独立锁保护, lwIP core locking)
- [x] 分布式软总线 Challenge-Response 鉴权与正则白名单防御
- [x] 设备路由表 LRU 淘汰、DDoS 限流与非阻塞 Dumping 设计
- [x] 无锁 Single-Producer-Single-Consumer (SPSC) 环形消息队列 (适配 BLE 中断)
- [x] 蓝牙 Security Mode 1 Level 3 配对认证与 Lua 签名验签
- [x] 蓝牙状态联动的智能 Tickless 睡眠决策与休眠窗口动态调节

### Phase 3: 手表智能化（12-18 个月）— 🎉 100% 完成
- [x] 意图引擎（IntentEngine，规则决策）
- [x] 应用框架 + 生命周期（AppControlBlock）
- [x] 小程序框架（MiniProgramEngine + Lua 5.4.6）
- [x] 分布式软总线（DistributedSoftBus）
- [x] 运动健康算法框架 (PPG心率数字滤波、基于三轴加速度计计步算法与低功耗睡眠监测)
- [x] 通知系统与消息队列 Hub (统一数据抽象、优先级队列与 OLED 全屏/悬浮通知弹窗)
- [x] NFC 卡模拟子系统（卡模拟、ISO14443A/APDU 通道与 Transit/Door 路由）
- [x] 表盘商店（支持 LittleFS VFS 动态加载与渲染第三方 Lua 表盘）
- [x] OTA 无线升级与 A/B 镜像切换 (Flash分区表、BLE固件传输与断电安全回滚)
- [x] 安全启动 (Secure Boot，上电 Ed25519 签名验证 Root of Trust)
- [x] 页面栈导航器与 GUI 动画引擎 (ScreenNavigator 栈式导航、右滑手势 Pop、过渡平移/渐变动画)
- [x] ELF 重定位解析与 Lua UI 深度绑定 (R_ARM_ABS32 / R_ARM_THM_CALL 动态解析与 Lua 触控/路由绑定)
### Phase 4: 手机探索（18-36 个月）— 🌌 规划中 / 进行中

- [x] Cortex-A 架构适配 (ARMv8-A AArch64) 与基础汇编引导
- [x] MMU 虚拟内存与强类型 PTE 页表项 (Type Safety)
- [x] 硬件中断控制器 GIC 初始化
- [x] 进程隔离 + 虚拟内存地址空间映射 (VASP)
- [x] 基于 seL4 的 Capability 安全模型与权限管理
- [x] 消息传递 IPC（QNX 风格同步 MsgSend/Receive/Reply）
- [x] WiFi 驱动 + 完整 TCP/IP
- [x] GPU 驱动抽象与图形管道 (Command Buffer / KMS)
- [x] GUIX 智能图形框架与多窗口管理
- [x] 摄像头 + 多媒体 (USB 协议栈与相机抽象架构)
- [x] 应用沙盒 (User Mode Privilege Separation)

---

## 借鉴来源

auroraOS 的架构设计站在了巨人的肩膀上，深度借鉴了以下 12 款优秀操作系统：

| 系统 | 维护方 | auroraOS 借鉴重点 |
|------|--------|------------------|
| **小米 Vela (NuttX)** | 小米 | POSIX 兼容、设备驱动框架、ProcFS、优先级继承锁、工作队列 |
| **vivo 蓝河 (BlueOS)** | vivo | 帧感知调度、脏区域渲染、光子存储缓存、意图引擎 |
| **Zephyr** | Linux Foundation | Kconfig 配置系统、传感器框架、BLE 协议栈 |
| **RT-Thread** | 中国开源 | FinSH Shell、设备框架、软件定时器 |
| **watchOS** | Apple | 应用生命周期、表盘 Complications、健康框架 |
| **LiteOS** | 华为 | 低功耗框架、tickless 模式、极小内核 |
| **HarmonyOS Watch** | 华为 | 分布式软总线、原子化服务 |
| **FreeRTOS** | Amazon | 任务通知、软件定时器、tickless 低功耗 |
| **ThreadX (Azure RTOS)** | Microsoft | 消息队列优先级、极低延迟优化 |
| **Garmin OS** | Garmin | 极致低功耗、运动算法、Connect IQ 小程序 |
| **QNX** | BlackBerry | 微内核 IPC、进程隔离、资源管理器 |
| **seL4** | seL4 Foundation | 最小化微内核、capability 安全模型、空间/时间隔离 |

---

## 质量指标

| 指标 | 目标 | 当前状态 |
|------|------|---------|
| 中断响应延迟 | ≤ 10μs | ✅ 已实现 CI DWT 周期级度量报告生成 |
| 上下文切换 | ≤ 5μs | ✅ 已实现 CI DWT 周期级度量报告生成 |
| 手表待机功耗 | ≤ 1mW (AOD) | wfi 已实现，Tickless 待硬件验证 |
| POSIX 兼容 | 50%+ (Phase 1) | 文件/信号量/睡眠已实现，约 40-50% |
| 内核最小 RAM | ≤ 32KB | 远低于 32KB（LiteOS 10KB，BlueOS 13KB）|
| 可移植性 | 换芯片只改 board.h | ✅ 已实现（arch_api + board.h + Kconfig）|

---

## 故障排查与经验总结

记录项目在 GitHub Actions CI 中构建与 HIL（QEMU 硬件在环）冒烟测试遇到的典型问题，含根因、修复手段与可复用经验，供后续维护参考。

### 1. 链接失败：cannot read spec file 'nosys.specs'

现象：CI 中 `make` 报 `arm-none-eabi-g++: fatal error: cannot read spec file 'nosys.specs'`，而本地构建正常，CACHE_KEY 不变时反复出现。

根因：CI 曾用 `cache-apt-pkgs-action` 缓存 `gcc-arm-none-eabi`。该 action 默认不装 Recommends，导致 newlib 的 `libnewlib-arm-none-eabi` 缺失；即便后续补装，缓存命中（CACHE_KEY 未变）也会还原出损坏/不完整的 newlib，使 `nosys.specs`、`libc.a` 缺文件，链接偶发失败。

修复：
- CI 弃用缓存 action，改为每次显式 `apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi libnewlib-dev`，保证拿到完整文件（见 `.github/workflows/build.yml` 注释）。
- CMake 链接块恢复 `--specs=nosys.specs`（newlib 装齐后可用）。auroraOS 自带 `kernel/syscalls.c` 实现了 `_sbrk/_write/_read` 等，可覆盖 newlib 弱符号，保留 nosys.specs 不会引 undefined reference。
- miband 分支去掉 `--specs=nano.specs`（需单独包），仅保留 nosys.specs。

经验：裸机工具链的运行时库（newlib/picolibc）是链接的关键依赖，不能依赖"可能不完整"的 apt 缓存；CI 应直接安装并显式声明所有需要的包，且 CACHE_KEY 要与工具链版本强绑定。

### 2. HIL 冒烟测试超时与误判

HIL 由 `scripts/hil_runner.py` 用 pexpect 经 TCP 串口（`127.0.0.1:1234`）驱动 QEMU，逐条发送 `help`/`ps` 并检查回显。踩坑与修复如下：

- 期望串不匹配：`ps` 实际只打印 `TID` 表头而无任务名，原期望 `shell_task` 永远超时。改为匹配 `r"TID"`。
- 串口诊断探针污染：曾在 `kernel/uart_device.hpp::read()`、`apps/shell.cpp::execute_command()`、`boot/interrupts.cpp` SVC 入口残留直写串口的诊断探针，干扰协议回显。清理这些探针，仅保留 HardFault/BusFault/UsageFault 等真实崩溃处理。
- 行终止符：`send` 用 `\r\n` 会在裸机 UART 留下残留 `\n`，造成空 read 与双提示符。统一改为 `\r`。
- 缓冲区污染：两条 `ps` 之间缓冲区残留旧输出，第二次匹配失败。在第一次 `ps` 后显式 `expect(r"aurora> ")` 完成提示符同步。
- 稳定性检查误判：曾加"等待 2s 再探测"的稳定性检查，但在 fdspawn+TCP 组合下时序不可靠，反而引入偶发失败。最终删除该步骤。

最终 HIL 收敛为三层冒烟：boot 进入 `aurora>` 提示符 → `help` 回显命令清单 → `ps` 正确列出任务（TID 0–3，RDY 态），全部通过即 PASSED（见首次成功日志）。

经验：HIL/pexpect 测试要严格控制"命令—提示符"成对同步，避免残留缓冲区与不可靠的延时等待；串口协议用 `\r` 而非 `\r\n`；调试用的诊断探针不应留在量产/测试路径上直写串口。

---

## 致谢

auroraOS 的架构设计站在了巨人的肩膀上。感谢上述 12 款操作系统的开源社区与设计者，他们的智慧让 auroraOS 得以在精简代码内实现丰富的 RTOS 能力。

特别感谢以下开源项目：
- **lwIP** — Adam Dunkels 及社区，嵌入式 TCP/IP 协议栈标杆
- **LittleFS** — ARM Limited，掉电安全的小型文件系统
- **Lua** — PUC-Rio，轻量级嵌入式脚本语言

---

## 许可证

本项目采用 [Customer License](LICENSE) 开源许可证。

第三方依赖保留各自许可证：
- lwIP: BSD-3-Clause
- LittleFS: BSD-3-Clause
- Lua 5.4.6: MIT

---

<div align="center">
  <p><i>auroraOS · 从学习演示到多分支 Lua 化智能手表 RTOS 平台</i></p>
  <p><i>Repository: https://github.com/jencaoking/auroraOS</i></p>
</div>
