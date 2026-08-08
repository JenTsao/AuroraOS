# 🛡️ AuroraOS → 网络安全随身系统 开发规划

## 一、现状分析

### AuroraOS 已有基础（核心实现 ~6,500 行，含头文件 ~23,500 行，含测试 ~59,700 行）

| 模块 | 完成度 | 对网络安全系统的价值 |
|------|--------|---------------------|
| O(1) 优先级抢占调度器 | ✅ 完整 | 实时响应安全事件 |
| MPU 内存保护 + 沙盒 | ✅ 完整 | 应用隔离，防止恶意代码扩散 |
| seL4 风格 Capability IPC | ✅ 完整 | 细粒度权限控制 |
| lwIP TCP/IP 全栈 + DHCP | ✅ 完整 | 网络数据包捕获与分析基础 |
| PacketTap 数据包捕获 | 🚧 骨架可用 | `net/packet_capture.cpp` 已编译，时间戳硬编码 0（待完善）|
| 分布式软总线 + HMAC-SHA256 鉴权 | ✅ 完整 | 安全设备间通信 |
| 安全启动 (Ed25519) + OTA | ✅ 完整 | 固件完整性验证 |
| 安全监控 + 看门狗 | ✅ 完整 | 异常行为检测 |
| 系统调用审计 AuditEngine | ✅ 完整 | 128 槽环形缓冲 + /proc/audit_log，已全量接入 SVC 与 POSIX |
| BLE 协议栈 + Security Mode 1 L3 | ❌ 接口设计 | 仅头文件，无 .cpp 实现（待从 miband 分支迁移）|
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

#### 5.1 网络数据包捕获引擎（🚧 已有骨架 `net/packet_capture.cpp`，需完善）

现有 `net/packet_capture.cpp` 已实现基本捕获框架并被编译进 lm3s 镜像，但时间戳硬编码为 0（含开发者草稿注释），距工业级有距离。完善目标：
- 完善 BPF 风格过滤器
- 实现真实时间戳注入
- 在 `ethernetif.cpp` 的 RX 路径中入 `PacketTap` 钩子
- 数据输出到 VFS 的 `/dev/pcap0` 字符设备，兼容 Wireshark 格式

#### 5.2 防火墙/包过滤子系统（🚧 已有 .hpp 接口设计，需 .cpp 实现）

现有 `net/firewall/` 目录含 5 个头文件（firewall_engine.hpp、rule_parser.hpp、stateful_inspector.hpp、rule_table.hpp、traffic_shaper.hpp），均为纯接口声明、无 .cpp 实现。实现目标：
- 规则支持：源/目的 IP、端口、协议、TCP 标志位、接口
- 有状态检测：跟踪 TCP 连接状态机（SYN/SYN-ACK/ESTABLISHED）
- 阈值防护：SYN Flood、ICMP Flood、端口扫描检测
- 规则热加载：通过 Shell 命令 `fw add/delete/list/enable`
- 复用 `SecurityMonitor` 做防火墙规则异常检测

#### 5.3 网络扫描引擎（✅ 已实现并编译进镜像）

`net/scanner/` 目录原 6 个纯接口头文件已全部补齐 `.cpp` 实现，并通过 `CMakeLists.txt` 编译进非 `qemu_rv32_virt` 目标（如 lm3s6965-qb）。实现要点：

**关键设计（已落地）：**
- **TaskNotify 零开销 IPC 并发**：`ScanEngine::init()` 创建最多 8 个 Worker 任务（默认 4），每 Worker 独立 1KB 栈、固定 `TaskPriority::Low`。主控通过 `TaskNotify::give(worker_id, job_id)` 分发作业，Worker 经 `TaskNotify::take(true)` 阻塞等待，无 CPU 轮询开销；执行完毕回推 `TaskNotify::give(controller_task_id, timestamp)`。
- **作业队列**：128 槽环形缓冲，Mutex 保护的 `dispatch_job_()` / `dequeue_job_()`；`ScanJobDesc` 携带 IP/端口/作业类型/job_id 完整上下文；`execute_job_()` 按 `ScanJobType` 枚举分派到对应模块。
- **不阻塞系统**：Worker 固定在 5 级优先级的 Low（1）档，系统交互（Shell/Normal=2）不受影响。
- **ProcFS 实时查看**：`ScanResultNode` 继承 `ProcNode`，挂载到 `/proc/scan_results`，格式为 `IP\t端口\t状态\t服务\tCVE\t延迟`，环形缓冲 64 槽。
- **Lua 策略自定义**：`scan_lua_binding.cpp` 注册 `aurora.scan.*` 命名空间（14 个 API：set_timeout/set_retries、scan_tcp_port/scan_tcp_range/scan_udp_port、scan_hosts/ping_host、detect_service、probe_vuln、quick_scan、has_results/result_count/pop_result/clear_results）。在 `MiniProgramEngine` 中调用 `register_scan_lua_bindings(L)` 激活。

**模块构成（6 头 + 6 .cpp）：**
| 文件 | 职责 |
|------|------|
| `port_scanner` | TCP Connect / UDP / ACK 端口扫描，非阻塞可配超时 |
| `host_discovery` | ARP 扫描 + ICMP Ping 主机发现 |
| `service_detector` | 横幅抓取 + 22 条服务指纹匹配（OpenSSH/MySQL/Redis/Nginx/Apache…）|
| `vuln_probe` | 12 条 CVE 签名漏洞检测（Heartbleed/BlueKeep/Log4Shell/Spring4Shell…）|
| `scan_engine` | 总控引擎：TaskNotify IPC + Worker 池 + 作业队列 + ProcFS |
| `scan_lua_binding` | Lua 绑定（复用 `MiniProgramEngine`）|

> 注：`scan_engine::register_lua_bindings()` 已实现委托给 `scan_lua_binding.cpp` 的 `register_scan_lua_bindings()`。部分 Lua API（如 `scan_udp_port`/`ping_host`）当前为占位实现，需后续接真实 lwIP 探测路径。

#### 5.4 系统调用审计日志（✅ 已实现，未来扩展 Lua 规则）

已实现 `kernel/audit.hpp`（589 行）— 完整系统调用级审计引擎：
- 128 槽环形缓冲区 + 规则引擎 + `/proc/audit_log` ProcFS 节点
- 所有 SVC 入口（`boot/interrupts.cpp`）均已接入 `AUDIT_HOOK_SVC`
- POSIX open/read/write/close 全部接入审计钩子
- 未来扩展：Lua 脚本自定义规则匹配异常行为

---

### Phase 6：无线安全审计（第3-6个月）

#### 6.1 WiFi 安全审计模块
```
新增 net/wireless/ 目录：
├── wifi_monitor.hpp       — 监控模式驱动框架
├── beacon_analyzer.hpp    — 信标帧分析（隐藏SSID/弱加密检测）
├── handshake_capture.hpp  — WPA/WPA2 四次握手捕获
├── deauth_detector.hpp    — 解除认证攻击检测
└── wireless_ids.hpp       — 无线入侵检测规则引擎
```

**硬件适配：**
- 优先适配支持 monitor mode 的 USB WiFi 模块（RTL8812AU/8187L）
- 通过 USB 驱动框架（需新增 `drivers/usb/`）接入
- 利用 MPU 沙盒隔离 WiFi 驱动，防止驱动漏洞影响内核

#### 6.2 BLE 安全测试框架
```
扩展 net/ble/（现有 miband 分支代码）：
├── ble_scanner.hpp        — BLE 设备发现与指纹
├── gatt_auditor.hpp      — GATT 服务安全审计
├── ble_mitm.hpp          — BLE 中间人攻击检测/演示
└── ble_ids.hpp            — BLE 异常行为检测
```

**复用现有代码：**
- `BleManager` 已支持 Security Mode 1 Level 3
- 可反向验证其他 BLE 设备的安全性
- 利用 `SecurityMonitor` 监控 BLE 连接异常

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

### 第 1 周：环境搭建与网络抓包验证
1. `git clone --recursive` 拉取代码，在 LM3S6965 QEMU 上跑通现有系统
2. 验证 lwIP 的 raw API 能否拦截所有入站/出站包
3. 在 `ethernetif.cpp` 中插入第一个 `PacketTap` 钩子，打印所有以太网帧

### 第 2-3 周：防火墙 MVP
4. 实现 `firewall_engine.hpp` 基础规则匹配（IP+Port 过滤）
5. 添加 Shell 命令 `fw add "drop src 192.168.1.100"`
6. 编写 Lua 脚本调用防火墙 API

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
│   ├── packet_capture.hpp   # 新增：数据包捕获
│   ├── protocol_analyzer.hpp# 新增：协议分析
│   └── ble/                 # 扩展：BLE 安全
├── drivers/
│   ├── usb/                 # 新增：USB 主机驱动
│   └── rf/                  # 新增：射频驱动
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

**建议起点**：先从 `PacketTap` 数据包捕获入手，这是所有网络安全功能的"眼睛"，也是验证 lwIP 可扩展性的最佳切入点。
