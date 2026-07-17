# 研究计划：auroraOS 项目分析

## 研究目标
对 auroraOS（一个面向智能手表/IoT 的微内核实时操作系统）进行全面的代码与架构分析，覆盖内核、启动/架构抽象、存储/网络、驱动/UI/应用、构建/测试，并核实其"借鉴 12 款 OS"的设计主张。结合已有 `research_report_memmanage_rootcause.md`（启动期 MemManage 故障根因）。

## 查询类型判定
**广度优先（Breadth-first）**：项目可自然拆分为多个独立子系统，需要"横向"并行调查每个子系统后再综合得出结论。

## 子任务拆分（并行子代理）
1. **内核核心**：scheduler（O(1) 优先级/帧感知）、KernelHeap（双重释放/realloc 修复）、Mutex PIP、Semaphore、MsgQueue SPSC、TaskNotify、Signal、Timer、WorkQueue、PowerManager、POSIX、Device 框架。
2. **启动/架构/MPU/系统调用/安全加固**：boot.S/interrupts.cpp、arch（cm4/cm4f/rv32/aarch64）、mpu.hpp、syscall.hpp（validate_user_ptr/IpcReplyDesc）、MemManage_Handler、libc.cpp。
3. **VFS/存储/网络/软总线**：VfsManager/VNode、RamFS/ProcFS/LittleFS/PhotonCache、distributed_softbus（HMAC-SHA256/正则白名单/LRU/DDoS 限流）、lwIP OSAL 适配、eth_driver 加固、BLE。
4. **驱动/UI/应用/Lua/ELF/AI**：display/framebuffer/oled/st7789、input/touch/gesture、sensor framework、NFC、ui complications、ScreenNavigator、shell/elf_loader/net_app/lua_ui_binding/watch、MiniProgramEngine、IntentEngine、ELF loader 安全。
5. **构建/测试/质量/度量/配置**：CMakeLists、Kconfig、scripts、tests（单元/集成/HIL/QEMU）、metrics、.github CI、构建健康度（注意 memmanage 报告提及 2 处编译错误）、休眠/未编译代码。
6. **Web/微信研究"借鉴来源"主张**：核实 BlueOS 帧感知调度/光子缓存、seL4 capability/微内核、HarmonyOS 分布式软总线、Zephyr Kconfig/传感器、FreeRTOS TaskNotify、QNX IPC、watchOS 生命周期、Garmin Connect IQ 等设计理念，评估 auroraOS 借鉴的准确性与差距。使用 web_search 与 wechat-article-search（务必带时间参数）。

## 微信文章检索策略
- 加载 `wechat-article-search` skill，检索关键词如「蓝河 BlueOS 帧感知调度」「分布式软总线 HarmonyOS」「seL4 微内核 capability」「Zephyr Kconfig 传感器框架」「watchOS 应用生命周期」等，时间范围不限但优先近 2-3 年。
- 将微信文章结论与 web_search 结论交叉比对，重点验证 README 中"借鉴 12 款 OS"的功能映射是否属实。

## 综合
回收 6 个子代理结果后，撰写结构化中文分析报告，包含：项目概览、各子系统分析、设计质量评估、安全加固评估、构建/测试健康度、与借鉴对象对比、结论与建议、参考来源。保存为 `research_report_auroraOS_analysis.md`（isArtifact:false，工作区根目录）。
