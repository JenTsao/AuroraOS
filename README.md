<div align="center">

# AuroraOS

**面向智能手表与物联网终端的能力型安全微内核实时操作系统**

ARM Cortex-M0+/M3/M4F · RISC-V RV32IMAC · ARMv8-A AArch64(探索) · lwIP TCP/IP · Lua 5.4.6 · MPU/PMP 内存保护 · NimBLE BLE

<p>
  <img src="https://img.shields.io/badge/Platform-Cortex--M0%2B%20%7C%20M3%20%7C%20M4F%20%7C%20RV32-brightgreen.svg" alt="Platform">
  <img src="https://img.shields.io/badge/Network-lwIP%20TCP%2FIP-orange.svg" alt="Network">
  <img src="https://img.shields.io/badge/Storage-LittleFS%20%2B%20PhotonCache-purple.svg" alt="Storage">
  <img src="https://img.shields.io/badge/Script-Lua%205.4.6-yellow.svg" alt="Script">
  <img src="https://img.shields.io/badge/License-CAOSL%20v2.0-blue.svg" alt="License">
</p>

</div>

> **文档导航（以本文与 `ARCHITECTURE.md` 为准）**
> - `README.md`（本文）— 现状速览与构建指南（**截至 2026-09**）。
> - `ARCHITECTURE.md` — 内核架构规范（调度 / IPC / 能力 / 内存），**权威参考**。
> - `AGENTS.md` — AI 编码代理工程契约（依赖方向、禁止 stable→experimental 等）。
> - `DOCS/KNOWN_ISSUES.md` — 已知缺陷与 CI 陷阱（**MiBand8 端口当前损坏**）。
> - `auroraos_cybersec_plan.md` — **历史规划**（Phase 5–8 AuroraSec 愿景），部分已落地、部分已演进。

---

## 项目简介

auroraOS 是一个从零自研的**能力型（capability-based）安全微内核 RTOS**，内核代号 **July Kernel**。在精简代码体积内实现了 32 级优先级抢占调度、完整 TCP/IP 协议栈、MPU/PMP 内存隔离、seL4 风格能力空间（CSpace）与 IPC、Lua 小程序引擎、帧感知渲染、分布式软总线，并正在叠加网络安全（防火墙 / 扫描 / 抓包 / 无线审计 / HIDS）与板载 AI 意图引擎等子系统。

| 指标 | 说明 |
|------|------|
| 内核模型 | 能力型微内核（Capability-Based Microkernel） |
| 目标架构 | ARM Cortex-M0+ / M3 / M4F、RISC-V RV32IMAC、ARMv8-A AArch64（探索） |
| 支持板级 | TI LM3S6965-QB (M3, QEMU) ✅ · QEMU RV32 Virt ✅ · ST Nucleo-L031K6 (M0+) ✅ · 小米手环8 (Apollo3 M4F) ❌(CI 损坏) · QEMU AArch64 Virt 🚧(MMU 孵化中) |
| 构建系统 | CMake + Kconfig（Linux 内核风格可裁剪配置） |
| 开发语言 | C++（内核 / freestanding） + C（驱动 / lwIP / Lua / NimBLE） + ARM / RISC-V 汇编 |
| 第三方依赖 | lwIP 2.x · Lua 5.4.6 · LittleFS · ed25519 · NimBLE（Apache MyNewt，3rdparty/nimble_port）· GoogleTest（主机测试） |

### 设计参考

参考了以下操作系统的公开文档与源码，具体实现均为独立编写：

- **NuttX** — POSIX 兼容层、ProcFS 设计
- **FreeRTOS** — TaskNotify、tickless 思路
- **Zephyr** — Kconfig、传感器框架接口
- **seL4** — Capability 模型、IPC 端点
- **Linux** — VFS 设计
- **HarmonyOS** — 分布式软总线
- **vivo BlueOS** — 帧感知调度、光子缓存
- **watchOS** — 表盘 Complication、应用生命周期

---

## 架构分层

```
应用层 (apps/)          表盘/watch · Shell · Lua 小程序 · ELF 加载器 · 意图引擎入口
   ↓  SVC / ECALL 系统调用边界
服务层 (services/)      firewall / net / power / sensor / vfs 的微内核服务任务（*_service + *_ipc）
   ↓
运行时 (runtime/)       app_base / app_sandbox / app_manifest / aurora_runtime（应用沙箱）
   ↓
内核核心 (kernel/)      scheduler(32级) · IPC(Endpoint+PIP) · CSpace · mm(TLSF) · MPU/PMP · 审计 · 安全监控
   ↓
架构抽象 (arch/) → 板级 (boards/) → 驱动 (drivers/) → HAL (hal/) → 硬件
```

> 说明：`experimental/` 不属于稳定架构，稳定内核**禁止**依赖 experimental 代码（见 `AGENTS.md`）。GUIX 合成器、自研 BLE 栈、相机 / NFC 等位于 `experimental/`。

---

## 目录结构（实际）

```
apps/          应用层（Shell, Lua 引擎, ELF 加载, watch 表盘, m0plus_main, kernel.cpp）
services/      服务层（firewall / net / power / sensor / vfs 的 *_service.cpp + *_ipc.hpp）
runtime/       应用运行时（app_base, app_sandbox, app_manifest, aurora_runtime, lua_heap）
kernel/        内核核心（core/  mm/  scheduler/  task/  interrupt/）
arch/          架构抽象层（arm/cortex-m{0+,3,4,4f} · riscv/rv32imac · arm/cortex-a/aarch64 + MMU）
boards/        板级支持包（ti/lm3s6965-qb, st/nucleo-l031k6, xiaomi/miband8, qemu/{rv32_virt,aarch64_virt}）
boot/          启动与硬件抽象（Reset_Handler, PendSV, SVC, SysTick）
bootloader/    安全启动（Ed25519 验签 + OTA 双分区）
vfs/           虚拟文件系统（VNode, RamFS, ProcFS, LittleFS, PhotonCache, SoftBus）
net/           网络子系统（lwIP 适配, 防火墙, 扫描器, 抓包, 软总线, BLE 安全, 无线审计）
drivers/       驱动层（display, input, sensor, usb, storage, watchdog, power, camera, gpu, rf）
ui/            稳定 UI 框架（ScreenNavigator, UiManager, View, Complication, widgets）
ai/            意图引擎（ai/february：february_core, intent_engine, world_model, planner, persona…）
metrics/       性能度量（DWT 周期计数器, 延迟记录器）
hal/           HAL 抽象（secure_storage 等）
utils/         工具（HMAC-SHA256, JSON 解析）
experimental/  实验性代码（GUIX 合成器, 自研 BLE 栈, 相机, NFC, 通知中心）
security/      安全子系统监控任务（hids / response 的 monitor_task）
config/        构建配置（Kconfig, 链接脚本, 分区表, toolchain）
scripts/       构建脚本（Kconfig 生成, QEMU 启动, HIL, 固件打包）
tests/         测试（GoogleTest 单元 / 集成 / 压力；tests/build/_deps 含 googletest）
3rdparty/      第三方依赖（lwIP, Lua, LittleFS, ed25519, nimble_port）
```

---

## 功能状态（2026-09）

| 子系统 | 功能 | 状态 | 说明 |
|--------|------|------|------|
| 内核调度 | 32 级 O(1) 优先级位图抢占调度 | ✅ | ready_bitmask + CLZ，侵入式双向链表；旧版 README 误写为「5 级」 |
| 内核调度 | 帧感知调度 FrameSchedulerV2 | ✅ | 30fps 帧内 / 帧间窗口分级，volatile 实现 |
| 同步原语 | Mutex(PIP) / Semaphore / SPSC / TaskNotify / Signal | ✅ | 传递性优先级继承、递归锁、超时、RAII |
| 内存管理 | KernelHeap — **TLSF**（双级隔离适配，384 bins） | ✅ | O(1) 分配 / 释放，魔数校验；旧版 README 误写为 First-Fit |
| 内存管理 | MemoryPool（固定块 O(1)） | ✅ | 空闲链表、边界检查、双重释放检测 |
| 内存保护 | MPU（Cortex-M0+/M3/M4, PMSAv7） | ✅ | SRD 硬件栈哨兵，PendSV 动态切换 |
| 内存保护 | AArch64 MMU + VAS | 🚧 | arch/arm/cortex-a/mmu + 单元测试；无完整固件目标 |
| IPC/安全 | Endpoint IPC（seL4 风格）+ 类型化消息 + PIP | ✅ | call/receive/reply，badge 鉴权，优先级队列 |
| IPC/安全 | CSpace 能力空间（16 槽） | ✅ | lookup/derive/mint/revoke/grant，权限降级检测 |
| IPC/安全 | 审计引擎 AuditEngine | ✅ | 128 槽环形缓冲 + 规则引擎 + /proc/audit_log |
| IPC/安全 | 安全监控 / 看门狗 / 安全启动(Ed25519+OTA) | ✅ | 心跳监考、堆压力、80% idle 喂狗；生产构建强制真实密钥 |
| 存储 | VFS + RamFS + ProcFS + LittleFS + PhotonCache | ✅ | open/read/write/close/lseek/ioctl；LRU 页缓存 |
| 网络 | lwIP 2.x 全栈 + 双 API | ✅ | Socket / Netconn；LWIP_TCPIP_CORE_LOCKING 核心锁 |
| 网络 | 防火墙 FirewallEngine + 流量整形 + 有状态检测 | ✅ | rule_table/stateful_inspector/traffic_shaper + services/firewall/* |
| 网络 | 网络扫描 ScanEngine | ✅ | port/host/service/vuln；TaskNotify Worker 池；Lua 绑定 |
| 网络 | 数据包捕获 PacketCapture (/dev/pcap0) | ✅ | BPF 过滤器 + Wireshark pcap 格式 |
| 网络 | 分布式软总线 DistributedSoftBus | ✅ | HMAC-SHA256 挑战应答 + 防重放 + LRU 路由 |
| 网络 | BLE — NimBLE 移植 + 自研 HAL | ✅ | 3rdparty/nimble_port；net/ble/hal_ble_impl + HCI UART 传输 |
| 网络 | 无线安全审计 WirelessIDS | 🚧 | 分析模块 header-only 完整；rtl8187l/rtl8812au 驱动已编译，需真实 USB WiFi 硬件 |
| 网络 | BLE 安全框架（scanner/gatt_auditor/mitm/ids） | 🚧 | header-only 设计稿，待接真实 BLE 流量 |
| 显示 | 帧缓冲 + 脏区域 + Renderer2D + 软 GPU | ✅ | soft_gpu.cpp 已实现并编译；OLED/ST7789 多为抽象 / 仿真驱动 |
| 输入 | InputEvent / Touch / Gesture(7 种) | 🚧 | 头文件 + GT316 驱动；真实硬件触摸待验证 |
| 电源 | 5 级功耗 + 充电管理 | ✅ | RUN→IDLE→LIGHT→DEEP→SHUTDOWN；services/power |
| 传感器 | 传感器框架 + 健康算法 | ✅ | SensorDriver 抽象；PPG / 计步 / 活动识别 |
| UI | 稳定 UI 框架（ScreenNavigator/Complication/widgets） | ✅ | 页面栈 + 转场 + 生命周期；Lua UI 绑定 |
| UI | GUIX 合成器（experimental） | 🚧 | 合成器 + 窗口 + 控件，部分实现 |
| 运行时 | 应用沙箱 runtime/（manifest + sandbox） | ✅ | 与能力模型对齐；lua_heap 隔离 |
| AI | 意图引擎 ai/february | 🚧 | february_core/intent_engine/world_model/planner/persona… 框架（header-only） |
| 安全 | HIDS / 自动响应 监控任务 | 🚧 | security/hids、security/response 已按板条件编译 |
| 移植 | LM3S6965 (QEMU) | ✅ | 主 HIL 平台，构建产物 auroraOS.elf/.bin 存在 |
| 移植 | RV32 (QEMU) | ✅ | 独立异常向量，可构建 |
| 移植 | Cortex-M0+ (Nucleo-L031K6) | ✅ | 64KB Flash / 8KB RAM，最大任务数 4 |
| 移植 | Cortex-M4F (MiBand8) | ❌ | **CI 损坏**：64KB BSS 超限 + kernel_init 未调用，无法进入调度器（见 KNOWN_ISSUES） |
| 移植 | AArch64 (QEMU virt) | 🚧 | MMU 单元测试通过；固件目标孵化中 |
| 工程 | 主机单元测试 | ✅ | ~57 个 test_*.cpp（unit / integration / stress），GoogleTest |
| 工程 | CI/CD | ✅ | 多目标固件构建 + 单测 + ASAN/UBSAN + 覆盖率 + clang-tidy + cppcheck |

---

## 构建与运行

环境：`arm-none-eabi-gcc/g++`、`gcc-riscv64-unknown-elf`（可选）、CMake ≥ 3.20、Make/Ninja、QEMU、`Python3 + kconfiglib`。

```bash
pip install kconfiglib
git clone --recursive https://github.com/jencaoking/auroraOS.git
cd auroraOS
python scripts/genconfig.py

# LM3S6965 (QEMU)
mkdir build_lm3s && cd build_lm3s
cmake -DBOARD=lm3s6965-qb ..
cmake --build . -j$(nproc)        # 注意：用 cmake --build，勿用裸 make（见 KNOWN_ISSUES §4a）
qemu-system-arm -M lm3s6965evb -nographic -kernel auroraOS.elf
```

启动后进入 `aurora>` 终端，支持 `help`、`ps`、`free`、`cat`、`ifconfig`、`ping`、`udpsend` 等命令。

其他目标：

| 目标 | BOARD | toolchain |
|------|-------|-----------|
| RISC-V RV32 | `qemu_rv32_virt` | config/toolchain_rv32.cmake |
| Cortex-M0+ | `nucleo_l031k6` | config/toolchain.cmake |
| MiBand8 (Apollo3) | `miband8` | config/toolchain_miband.cmake（⚠️ 当前损坏） |
| AArch64 | `qemu_aarch64_virt` | 对应 arch cmake |

> **注意**：`config/autoconf.{h,cmake}` 与 `.config` 被 git 跟踪且默认针对 LM3S；切换板子前需删除这些陈旧的 Kconfig 产物，否则会继承错误 flag（KNOWN_ISSUES §4b）。

主机单元测试：

```bash
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j
ctest --test-dir build_tests --output-on-failure
```

---

## 验证与质量

- **测试**：`tests/unit`（内核 / IPC / CSpace / MPU / 内存 / 网络 / 安全 / MMU / UI / Lua / GPU）、`tests/integration`（系统启动）、`tests/stress`（OOM 压力）共约 57 个 GoogleTest 用例。
- **静态 / 动态分析**：CI 含 ASAN+UBSAN、clang-tidy、cppcheck、覆盖率。
- **已知问题**：见 `DOCS/KNOWN_ISSUES.md`（MiBand8 BSS 溢出、NimBLE 上游 `file(GLOB)` 陷阱、stale Kconfig、生成器不匹配等）。

---

## 合规提示

项目包含双用途（dual-use）安全能力（网络扫描、漏洞探测、无线握手捕获、隐蔽身份 `net/stealth_identity.hpp` 等）。`DOCS/compliance_baseline.md` 记录合规基线；使用须限定于**自有网络的授权安全审计**。

---

## 文档与现实的差异

本文档力求反映代码现状。历史文档请注意：

- `README.md`（本文）— 现状速览（优先参考）
- `ARCHITECTURE.md` — 内核架构规范（权威）
- `AGENTS.md` — 工程契约（AI 代理必读）
- `auroraos_cybersec_plan.md` — **历史规划**，部分已落地、部分已演进（如 wireless 已接入 CMake、新增 services/runtime/security/ai 层）

---

## 许可证

CAOSL v2.0（JENCAO Custom Advanced Open Source License v2.0）：强 Copyleft + 专利保护 + 道德使用限制 + 商业双授权。第三方依赖保留各自许可证（lwIP/LittleFS BSD-3、Lua/MIT、ed25519 MIT、NimBLE Apache-2.0）。

<div align="center">
  <p><i>auroraOS · 能力型安全微内核 RTOS（July Kernel）</i></p>
  <p><i>Repository: https://github.com/jencaoking/auroraOS</i></p>
</div>
