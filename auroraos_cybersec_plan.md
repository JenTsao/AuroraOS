# 🛡️ AuroraOS → 网络安全随身系统 开发规划

## 一、现状分析

### AuroraOS 已有基础（核心实现 ~6,500 行，含头文件 ~23,500 行，含测试 ~59,700 行）

| 模块 | 完成度 | 对网络安全系统的价值 |
|------|--------|---------------------|
| O(1) 优先级抢占调度器 | ✅ 完整 | 实时响应安全事件 |
| MPU 内存保护 + 沙盒 | ✅ 完整 | 应用隔离，防止恶意代码扩散 |
| seL4 风格 Capability IPC | ✅ 完整 | 细粒度权限控制 |
| lwIP TCP/IP 全栈 + DHCP | ✅ 完整 | 网络数据包捕获与分析基础 |
| PacketTap 数据包捕获 | ✅ 完整 | `net/packet_capture.cpp` + `protocol_analyzer.hpp`，BPF 过滤器 + 真实时间戳 + Wireshark pcap 格式 `/dev/pcap0` |
| 分布式软总线 + HMAC-SHA256 鉴权 | ✅ 完整 | 安全设备间通信 |
| 安全启动 (Ed25519) + OTA | ✅ 完整 | 固件完整性验证 |
| 安全监控 + 看门狗 | ✅ 完整 | 异常行为检测 |
| 系统调用审计 AuditEngine | ✅ 完整 | 128 槽环形缓冲 + /proc/audit_log，已全量接入 SVC 与 POSIX |
| BLE 协议栈 + Security Mode 1 L3 | ✅ 已打通 | `net/ble/hal_ble_impl.cpp` + `hci/hci_uart_transport.cpp` 真实硬件驱动路径已接入（`CONFIG_BLE_ENABLED` 编译）|
| ELF 动态加载器 | ✅ 完整 | 动态加载安全工具模块 |
| Lua 脚本引擎 | ✅ 完整 | 快速编写安全检测脚本 |
| 帧缓冲 + 脏区域渲染 | ✅ 完整 | 便携设备屏幕显示 |
| 传感器框架 | ✅ 完整 | 环境感知（运动/位置） |
| LittleFS + PhotonCache | ✅ 完整 | 安全日志持久化存储 |
| RISC-V / ARM 多架构支持 | ✅ 完整 | 灵活硬件选型 |

### 核心优势
AuroraOS 已经具备**微内核隔离、实时调度、网络安全协议栈、安全启动链**四大基石，转型网络安全随身系统不需要从零开始，而是在现有架构上**叠加安全工具链和专用硬件驱动**。

---

## 二、目标定位

### 产品定义
> **AuroraSec** — 基于 AuroraOS 的微内核网络安全随身系统，运行于便携硬件（树莓派/国产RISC-V/手环形态），提供：
> - 网络扫描与漏洞探测
> - 无线网络安全审计（WiFi/BLE）
> - 入侵检测与实时告警
> - 安全日志采集与取证
> - 便携式 VPN/防火墙网关

### 硬件目标平台

| 平台 | 定位 | 优势 |
|------|------|------|
| 树莓派 Zero 2 W | 主力开发板（初期验证） | ARM64/Linux 生态兼容，WiFi+BT |
| 国产 RISC-V 开发板（如全志D1/Sipeed） | 自主可控路线 | 适配已有 RV32 移植 |
| 小米手环 8（现有 miband 分支） | 便携形态验证 | 随身佩戴、BLE 通信 |
| 定制 PCB（Ambiq Apollo4 + WiFi 模块） | 最终产品形态 | 超低功耗 + 便携 |

---

## 三、开发路线图（4 个阶段）

### Phase 5：网络安全内核增强（第1-3个月）

#### 5.1 网络数据包捕获引擎（✅ 已完善）

`net/packet_capture.cpp` + `net/protocol_analyzer.hpp` 已增强至工业级，编译进 lm3s 镜像。完善要点：

**BPF 风格过滤器（已实现）：**
- L2 MAC 过滤：`mac_src` / `mac_dst`（独立启用，6 字节精确匹配）
- L3 IP 过滤：`ip_src` / `ip_dst`，含 CIDR 掩码（`ip_mask`，0=自动 /32）
- L3 协议过滤：`proto_bitmask`（32-bit 位图，bit 6=TCP, 17=UDP, 1=ICMP）
- L4 端口过滤：`port_src` / `port_dst`，范围匹配（`lo` ≤ port ≤ `hi`，支持环绕）
- L4 TCP flags 过滤：`tcp_flags_val`（SYN=0x02, ACK=0x10, FIN=0x01, RST=0x04, PSH=0x08）
- 复合判定模式：`filter_or` = true 时任一匹配即放行（OR），false 时全部匹配才放行（AND）
- 防御性解析：IHL < 5 畸形包拒绝、L4 头边界检查、非 IPv4 帧回退处理

**真实时间戳注入（已实现）：**
- 使用全局 `volatile uint32_t tick_count`（`boot/interrupts.cpp` SysTick_Handler 中每 ms 递增），不依赖 `TimerManager` 的单例初始化时序
- 转换：`ts_sec = tick_count / 1000`，`ts_usec = (tick_count % 1000) * 1000`，精度 1 ms

**ethernetif.cpp RX/TX 钩子（已接入）：**
- `low_level_input()` → `PacketCapture::instance().tap_rx_packet()`（第 53 行）
- `low_level_output()` → `PacketCapture::instance().tap_tx_packet()`（第 26 行）
- `ethernetif_init()` → `PacketCapture::instance().init()` 挂载 `/dev/pcap0`（第 106 行）

**VFS 字符设备（已实现）：**
- 挂载路径：`/dev/pcap0`，单一读取者（`open_file` 互斥）+ 只读（`write` 返回 -1）
- 输出格式：兼容 Wireshark pcap（little-endian magic 0xa1b2c3d4, version 2.4, LINKTYPE_ETHERNET=1）
- 读取协议：`read(offset=0)` → 全局文件头（24B），后续 `read` → 数据包记录（16B 头 + 帧数据）
- 环形缓冲 64 槽，满时覆盖最老包，追踪 `CaptureStats`（packets_captured/dropped/filtered/bytes/peak_ring_usage）
- ioctl 命令：`IOCTL_SET_FILTER`(1) / `IOCTL_ENABLE_PROMISC`(2) / `IOCTL_DISABLE_PROMISC`(3) / `IOCTL_GET_STATS`(4) / `IOCTL_RESET_STATS`(5) / `IOCTL_GET_FILTER`(6)

#### 5.2 防火墙/包过滤子系统（✅ 已实现并编译进镜像）

`net/firewall/` 目录原 5 个纯接口头文件已补齐 `.cpp` 实现，通过 `CMakeLists.txt` 编译进 lm3s 等目标。`firewall_engine.cpp` 的 `process_packet()` 已完整串起四层处理管线：

**处理管线（已落地）：**
- **流量整形/阈值防护**：`traffic_shaper_.process_packet()` 优先拦截（SYN/ICMP Flood 与端口扫描检测），超限丢包
- **规则匹配**：`rule_table` 按源/目的 IP、端口、协议、TCP 标志位、接口匹配，命中 `DROP`/`REJECT` 动作则经 `SecurityMonitor::report_firewall_anomaly("Rule Drop")` 上报
- **有状态检测**：`stateful_inspector_.process_tcp_packet()` 跟踪 TCP 连接状态机（SYN/SYN-ACK/ESTABLISHED），异常状态经 `SecurityMonitor::report_firewall_anomaly("Stateful Drop")` 上报
- **规则热加载**：Shell 命令 `fw add/delete/list/enable` 已提供

**模块构成（5 头 + 5 .cpp）：**
| 文件 | 职责 |
|------|------|
| `firewall_engine` | 总控 `process_packet()` 四层管线 + enable/is_enabled |
| `rule_parser` | 规则字符串解析 |
| `rule_table` | 规则存储与匹配 |
| `stateful_inspector` | TCP 状态机有状态检测 |
| `traffic_shaper` | 阈值防护与流量整形（抗 DDoS）|

> 注：已复用 `SecurityMonitor` 做防火墙规则异常检测，与 5.5 节安全监控打通。
>
> 补充（最新状态）：防火墙已进一步拆分出用户空间服务层——`services/firewall/`（`firewall_service` / `firewall_client` / `firewall_audit`）通过 IPC 调用 `net/firewall/` 引擎，符合"服务移出内核"的架构方向（commit 1817d1d）。

#### 5.3 网络扫描引擎（✅ 已实现并编译进镜像）

`net/scanner/` 目录原 6 个纯接口头文件已全部补齐 `.cpp` 实现，并通过 `CMakeLists.txt` 编译进非 `qemu_rv32_virt` / `miband8` 目标（如 lm3s6965-qb）。实现要点：

**关键设计（已落地）：**
- **TaskNotify 零开销 IPC 并发**：`ScanEngine::init()` 创建最多 8 个 Worker 任务（默认 4），每 Worker 独立 1KB 栈、固定 `TaskPriority::Low`。主控通过 `TaskNotify::give(worker_id, job_id)` 分发作业，Worker 经 `TaskNotify::take(true)` 阻塞等待，无 CPU 轮询开销；执行完毕回推 `TaskNotify::give(controller_task_id, timestamp)`。
- **作业队列**：128 槽环形缓冲，Mutex 保护的 `dispatch_job_()` / `dequeue_job_()`；`ScanJobDesc` 携带 IP/端口/作业类型/job_id 完整上下文；作业经 `ScanEngine::register_handler(ScanJobType, IScanHandler*)` 分派到 `handlers/scan_handlers.cpp` 的 7 个策略 Handler（TCP/UDP/ACK 端口扫描、ARP 发现、ICMP Ping、服务检测、漏洞探测），策略可扩展（commit d1d55a1 重构为 Handler 架构）。
- **不阻塞系统**：Worker 固定在 5 级优先级的 Low（1）档，系统交互（Shell/Normal=2）不受影响。
- **ProcFS 实时查看**：`ScanResultNode` 继承 `ProcNode`，挂载到 `/proc/scan_results`，格式为 `IP\t端口\t状态\t服务\tCVE\t延迟`，环形缓冲 64 槽。
- **Lua 策略自定义**：`scan_lua_binding.cpp` 注册 `aurora.scan.*` 命名空间（14 个 API：set_timeout/set_retries、scan_tcp_port/scan_tcp_range/scan_udp_port、scan_hosts/ping_host、detect_service、probe_vuln、quick_scan、has_results/result_count/pop_result/clear_results）。在 `MiniProgramEngine` 中调用 `register_scan_lua_bindings(L)` 激活。

**模块构成（7 头 + 7 .cpp）：**
| 文件 | 职责 |
|------|------|
| `port_scanner` | TCP Connect / UDP / ACK 端口扫描，非阻塞可配超时 |
| `host_discovery` | ARP 扫描 + ICMP Ping 主机发现 |
| `service_detector` | 横幅抓取 + 22 条服务指纹匹配（OpenSSH/MySQL/Redis/Nginx/Apache…）|
| `vuln_probe` | 12 条 CVE 签名漏洞检测（Heartbleed/BlueKeep/Log4Shell/Spring4Shell…）|
| `scan_engine` | 总控引擎：TaskNotify IPC + Worker 池 + 作业队列 + ProcFS + Handler 注册表 |
| `handlers/scan_handlers` | 7 个策略 Handler 实现（TCP/UDP/ACK/ARP/ICMP/服务/漏洞）|
| `scan_lua_binding` | Lua 绑定（复用 `MiniProgramEngine`）|

> 注：`scan_engine::register_lua_bindings()` 已实现委托给 `scan_lua_binding.cpp` 的 `register_scan_lua_bindings()`。部分 Lua API（如 `scan_udp_port`/`ping_host`）当前为占位实现，需后续接真实 lwIP 探测路径。

#### 5.4 系统调用审计日志（✅ 已实现，未来扩展 Lua 规则）

已实现 `kernel/core/audit.hpp`（673 行）— 完整系统调用级审计引擎（注：内核目录重组后路径为 `kernel/core/`，原文档路径 `kernel/audit.hpp` 已废弃）：
- 128 槽环形缓冲区 + 规则引擎 + `/proc/audit_log` ProcFS 节点
- 所有 SVC 入口（`boot/interrupts.cpp`）均已接入 `AUDIT_HOOK_SVC`
- POSIX open/read/write/close 全部接入审计钩子
- 未来扩展：Lua 脚本自定义规则匹配异常行为

---

### Phase 6：无线安全审计（第3-6个月）

#### 6.1 WiFi 安全审计模块（✅ 已实现并接线编译）

`net/wireless/` 目录已实现 5 头文件（全 header-only 完整逻辑） + 4 `.cpp` 文件（USB 驱动 + 监控任务 + Lua 绑定），共 9 个文件。**当前 CMake 接线状态：`drivers/usb/usb_host.cpp` + `rtl8187l_monitor.cpp` + `rtl8812au_monitor.cpp` + `wifi_monitor_task.cpp` 已加入 `if(NOT BOARD STREQUAL "qemu_rv32_virt" AND NOT BOARD STREQUAL "miband8")` 网络块（CMakeLists.txt L141-155），`wireless_lua_binding.cpp` 在 `CONFIG_LUA_VM` 下追加（L157），已编译进 lm3s6965-qb 目标（排除 M0+ 核与 miband8 因 RAM 紧张）。**

**模块构成（9 文件）：**

| 文件 | 职责 |
|------|------|
| `wifi_monitor.hpp` | 802.11 帧结构体 + `WifiMonitorDevice` 抽象 + RTL8187L/RTL8812AU 驱动声明 + `ChannelHopper` |
| `beacon_analyzer.hpp` | 128 AP 表 + IE 遍历 + RSN IE WPA2/WPA3 识别 + Rogue AP 检测 |
| `handshake_capture.hpp` | 32 会话 + EAPOL 四次握手 + Nonce/MIC/PMKID + hashcat hc22000 格式 |
| `deauth_detector.hpp` | 64 BSSID 跟踪 + 滑动窗口 + 序列号跳跃 + 四级告警 + 防抖动 |
| `wireless_ids.hpp` | 256 事件环 + 64 告警队列 + 32 规则 + `/proc/wireless_ids` + `SecurityMonitor` 联动 |
| `rtl8187l_monitor.cpp` | USB 2.0 驱动：监控模式/RX 配置/信道/帧捕获/注入 |
| `rtl8812au_monitor.cpp` | USB 3.0 驱动：监控模式/双频信道/固件上传/TX 功率控制 |
| `wifi_monitor_task.cpp` | MPU 沙盒用户任务 + Capability IPC + 帧路由 + 信道跳变 |
| `wireless_lua_binding.cpp` | Lua `aurora.wireless.*` 11 API |

**USB 驱动框架：** `drivers/usb/` 已实现 4 文件（`usb_core.hpp` / `usb_host.hpp` / `usb_host.cpp` / `usb_wifi.hpp`），提供 LM3S6965 USB OTG HAL（枚举/Bulk/Control/寄存器读写），`usb_host.cpp` 已接线编译。

**MPU 沙盒隔离：** WiFi Monitor 任务以 `User` 特权创建 + `SandboxDescriptor` CRC32 + `CSpace` 仅授权 USB 寄存器区域。

> 上层分析模块 header-only 即完整可调——只要驱动 `capture_frame()` 喂入 `CapturedFrame`，整个无线审计栈就工作。

#### 6.2 BLE 安全测试框架（✅ Header-only 完整实现）

`net/ble/` 目录新增 4 个 header-only 安全模块，与现有 `BleManager`（`experimental/net/ble/ble_stack.cpp`）、`HalBle`、`BleSignatureVerifier` 集成，反向验证其他 BLE 设备安全性。BLE 真实硬件驱动路径已打通（commit 82266b0）：`net/ble/hal_ble_impl.cpp` + `hci/hci_uart_transport.cpp` 在 `CONFIG_BLE_ENABLED` 下编译进镜像。

**模块构成：**

| 文件 | 职责 |
|------|------|
| `ble_scanner.hpp` | BLE 4.x 广告包 AD 结构解析（37 种 AD Type 常量）+ 64 设备指纹库（MAC/类型/RSSI 历史/公司 ID/安全标志）+ 设备分类（Phone/Watch/Beacon/Tracker 等 11 类）+ 弱安全计数 |
| `gatt_auditor.hpp` | 8 设备 + 32 特征/设备跟踪 + 特征权限矩阵审计（OpenWrite/WeakAuth/MissingEncryption）+ 9 条已知漏洞服务 UUID（ANCS/HID/ImmediateAlert 等 + CVE 说明）+ 4 级严重度 + `SecurityMonitor` 集成点 |
| `ble_mitm.hpp` | 16 会话跟踪 + 10 种 MITM 告警类型（配对降级/连接参数异常/RSSI 悖论/断连风暴/地址欺骗/监管超时/跳频异常/安全级别降级）+ 置信度 0-100 + 检测规则含滑动窗口阈值 |
| `ble_ids.hpp` | 256 事件环 + 64 告警队列 + 32 可配置规则 + 7 条预置规则（ScanStorm＞30/10s→Alert、ConnectFlood＞5/5s→Report+Block、KnownVulnerableSvc→Report、PairingDowngrade→Report、UnauthorizedWrite＞3/10s→Report 等）+ `/proc/ble_ids` ProcFS 节点 |

**复用现有代码：**
- `BleManager`（`experimental/net/ble/ble_stack.cpp`）：Security Mode 1 Level 3 + HCI 事件队列 + 连接状态机——BleIds 通过 `feed_connection_event`/`feed_signature_failure` 挂钩
- `BleSignatureVerifier`（`net/ble/`）：Ed25519 签名验证 + Nonce 防重放——UnauthorizedWrite IDS 规则联动
- `SecurityMonitor::report_firewall_anomaly()`：ble_ids.hpp 第 445 行预留集成（注释 + reason 字符串构建就绪）
- `GattAuditor` 的 `map_security_level()` 静态方法直接映射 BLE Security Mode/Level（Mode 1 Level 1-4, Mode 2 Level 1-2）

#### 6.3 射频频谱感知（可选）
```
新增 drivers/rf/ 目录：
├── spectrum_sensor.hpp    — 频谱传感器抽象
├── rf_analyzer.hpp        — 异常信号检测
└── jamming_detector.hpp   — 干扰信号识别
```

---

### Phase 7：入侵检测与响应（第6-9个月）

#### 7.1 嵌入式 NIDS（网络入侵检测系统）
```
新增 security/ids/ 目录：
├── ids_engine.hpp         — IDS 核心引擎
├── signature_db.hpp       — 攻击特征库（Snort 规则子集）
├── anomaly_detector.hpp   — 基于统计的异常检测
├── traffic_analyzer.hpp   — 流量行为分析
└── alert_manager.hpp      — 告警管理（分级/去重/聚合）
```

**检测能力：**
- 特征检测：端口扫描、SYN Flood、ARP 欺骗、DNS 隧道
- 异常检测：基于流量基线的离群点检测
- 协议异常：畸形包、分片攻击、TCP 标志位异常
- 告警输出：串口/屏幕/网络推送三重通道

**性能目标：**
- 参考 AnCyR 方案，在 Cortex-M4 上开销 < 5%
- 规则匹配使用 Aho-Corasick 多模式算法
- 热路径无动态内存分配（复用 `MemoryPool`）

#### 7.2 主机入侵检测（HIDS）
```
新增 security/hids/ 目录：
├── file_integrity.hpp     — 文件完整性监控（哈希校验）
├── process_monitor.hpp    — 任务行为监控
├── privilege_auditor.hpp  — 权限提升检测
└── rootkit_scanner.hpp    — 内核级 Rootkit 扫描
```

**复用 AuroraOS 特性：**
- `CSpace` 能力空间 → 监控权限异常
- `MPU` 内存保护 → 检测非法内存访问
- `ProcFS` → 暴露 HIDS 状态
- `SecurityMonitor` → 集成 HIDS 告警

#### 7.3 自动响应系统
```
新增 security/response/ 目录：
├── response_engine.hpp    — 响应策略引擎
├── auto_block.hpp         — 自动封禁（动态防火墙规则）
├── quarantine.hpp         — 隔离受感染任务/设备
└── forensic_snapshot.hpp  — 取证快照（内存+流量）
```

---

### Phase 8：便携形态与产品化（第9-12个月）

#### 8.1 便携硬件平台
```
目标硬件规格：
- SoC：全志 D1 (RISC-V 64bit) 或 Ambiq Apollo4 (Cortex-M4F)
- 无线：WiFi 6 (AX200) + BLE 5.2 + Zigbee (可选)
- 屏幕：1.5" AMOLED (现有 ST7789 驱动复用)
- 电池：2000mAh (目标续航 8h 持续监控)
- 外壳：3D 打印便携盒子（手环/钥匙扣形态）
```

#### 8.2 安全 GUI 控制台
```
扩展现有 UI 框架：
├── gui/dashboard.hpp       — 安全态势仪表盘
├── gui/alert_view.hpp      — 实时告警视图
├── gui/network_map.hpp     — 网络拓扑图
├── gui/scan_results.hpp    — 扫描结果可视化
└── gui/settings.hpp        — 系统配置界面
```

**复用现有代码：**
- `Renderer2D` 2D 绘图引擎 → 绘制图表
- `ScreenNavigator` 页面栈 → 多页面导航
- `Complication 引擎` → 数据驱动刷新
- `Lua UI 绑定` → Lua 脚本自定义仪表盘

#### 8.3 安全日志与取证
```
扩展 vfs/：
├── forensic_store.hpp      — 取证数据加密存储
├── log_rotation.hpp       — 日志轮转与压缩
├── chain_of_custody.hpp    — 证据链完整性（Merkle Tree）
└── export_tool.hpp         — 导出 PCAP/JSON/PDF 报告
```

#### 8.4 安全运维接口
```
扩展 apps/shell.cpp：
- 新增命令：scan / sniff / fw / ids / vuln / report
- Lua REPL 模式：交互式安全脚本编写
- 远程管理：SSH-over-BLE（利用现有 BLE 栈）
```

---

## 四、技术架构（目标态）

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 安全应用层 (security/apps/)                                            │
│ Lua 安全脚本 │ 扫描器 │ 渗透工具 │ 取证工具 │ 报告生成器               │
├─────────────────── SVC 系统调用边界 ────────────────────────────────────┤
│ 安全内核扩展 (security/)                                               │
│ IDS 引擎 │ 防火墙 │ HIDS │ 响应引擎 │ 审计日志 │ 取证存储             │
├─────────────────────────────────────────────────────────────────────────┤
│ AuroraOS 内核 (kernel/) — 已有 ✅                                      │
│ 调度器 │ MPU │ IPC │ CSpace │ SecurityMonitor │ Watchdog               │
├─────────────────────────────────────────────────────────────────────────┤
│ 网络子系统 (net/) — 已有 + 扩展                                        │
│ lwIP │ PacketTap │ WiFi Audit │ BLE Audit │ Scanner │ Firewall          │
├─────────────────────────────────────────────────────────────────────────┤
│ 驱动层 (drivers/) — 扩展无线驱动                                       │
│ USB WiFi │ SPI OLED │ I2C Sensor │ BLE │ NFC │ 以太网                  │
├─────────────────────────────────────────────────────────────────────────┤
│ 硬件层                                                                 │
│ RISC-V D1 / Cortex-M4F / 树莓派 Zero 2 W                              │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 五、立即可启动的任务清单

### 第 1 周：环境搭建与网络抓包验证（✅ 已完成）
1. ✅ 系统已在 LM3S6965 QEMU 上运行
2. ✅ lwIP raw API 通过 `PacketCapture` VNode 拦截所有 RX/TX 包
3. ✅ `ethernetif.cpp` 中 `PacketTap` 钩子已接入（`tap_rx_packet` / `tap_tx_packet`）

### 第 2-3 周：防火墙 MVP（✅ 已完成）
4. ✅ 实现 `firewall_engine.hpp` 基础规则匹配（IP+Port 过滤）
5. ✅ 添加 Shell 命令 `fw add "drop src 192.168.1.100"`
6. ✅ 编写 Lua 脚本调用防火墙 API

### 第 4-6 周：端口扫描器（✅ 已完成）
> 实现落在 `net/scanner/`，而非 `security/scanner/`（`security/` 为后续独立安全子系统规划目录）。详见 5.3 节。

7. ✅ 实现 `port_scanner.cpp` TCP Connect / UDP / ACK 扫描（头文件 `port_scanner.hpp` 原仅接口）
8. ✅ 利用 `TaskNotify` 实现并发扫描（Worker 池 + 128 槽作业队列；当前为 Connect 扫描，`SYN` 半开扫描待 lwIP raw socket 支持）
9. ✅ 扫描结果输出到 `/proc/scan_results`（ProcFS 节点 `ScanResultNode`）

### 第 7-8 周：IDS 特征引擎
10. 移植 Snort 社区规则子集（前 50 条高危规则）
11. 实现 Aho-Corasick 多模式匹配算法
12. 集成 `SecurityMonitor` 实现告警联动

---

## 六、代码组织建议

### 新增目录结构
```
auroraOS/
├── security/                # 新增：安全子系统根目录
│   ├── ids/                 # 入侵检测引擎
│   ├── firewall/            # 防火墙/包过滤
│   ├── hids/                # 主机入侵检测
│   ├── response/            # 自动响应
│   ├── scanner/             # 网络扫描
│   ├── wireless/            # 无线安全审计
│   ├── forensic/            # 取证与日志
│   └── audit/               # 系统审计
├── net/
│   ├── packet_capture.cpp/hpp   # ✅ 已实现（/dev/pcap0 + BPF 过滤器 + Wireshark pcap）
│   ├── protocol_analyzer.hpp    # ✅ 已实现（BPF 风格协议分析引擎）
│   ├── firewall/                # ✅ 已实现（5 头 + 5 .cpp）+ services/firewall/ 用户空间服务
│   ├── scanner/                 # ✅ 已实现（6 模块 + handlers/ Handler 架构）
│   ├── wireless/                # ✅ 已实现（9 文件，已接线编译进 lm3s6965-qb）
│   └── ble/                     # ✅ BLE 安全测试框架（4 个 header-only 模块）+ 真实硬件驱动路径
├── drivers/
│   ├── usb/                 # ✅ 已实现：LM3S6965 USB OTG HAL（usb_core/usb_host/usb_wifi）
│   └── rf/                  # 新增：射频驱动（未开始）
└── gui/
    ├── dashboard.hpp         # 新增：安全仪表盘
    └── alert_view.hpp        # 新增：告警视图
```

### 分支策略
```
main                    ← 稳定内核（不破坏现有功能）
├── security/core       ← 防火墙 + IDS + 审计（第一步）
├── security/wireless   ← WiFi/BLE 审计（第二步）
├── security/gui        ← 安全控制台 UI（第三步）
└── security/hardware   ← 新硬件适配（持续）
```

---

## 七、风险与应对

| 风险 | 影响 | 应对策略 |
|------|------|---------|
| 硬件资源不足（RAM/Flash） | 安全工具链体积大 | 模块化裁剪 + Lua 脚本化 + 外部存储 |
| WiFi 模块 monitor mode 驱动 | 无线审计核心依赖 | 优先选 RTL8812AU（Linux 驱动成熟），封装为独立任务 |
| 实时性与安全处理冲突 | 扫描/IDS 影响系统响应 | 利用 5 级优先级调度，安全任务设为 Low/Normal |
| 功耗限制 | 便携设备续航短 | 复用现有 Tickless + 分级功耗，空闲时进入 DEEP_SLEEP |
| 法律合规 | 渗透测试工具的法律风险 | 内置使用协议确认 + 仅允许审计自有网络 |

---

## 八、总结

AuroraOS 已经是一个**架构完整、安全基础扎实**的微内核 RTOS，转型网络安全随身系统的核心工作是：

1. **网络层增强**（PacketTap + 防火墙 + 扫描器）— 3 个月
2. **无线安全审计**（WiFi/BLE）— 3 个月
3. **入侵检测与响应**（NIDS/HIDS）— 3 个月
4. **产品化**（硬件 + GUI + 取证）— 3 个月

**最大优势**：已有的 MPU 沙盒、Capability IPC、安全启动、Lua 引擎，恰好是安全工具平台的理想底座——工具之间天然隔离，不会互相污染，也不会拖垮内核。

**建议起点**：PacketTap 数据包捕获已完善，下一步可从防火墙增强或 IDS 特征引擎开始，这两个是网络安全防御的"大脑"。
