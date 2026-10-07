#ifndef AURORA_WATCH_FEBRUARY_GLUE_HPP
#define AURORA_WATCH_FEBRUARY_GLUE_HPP

// ============================================================
// apps/watch/february_glue.hpp — February 运行时 ↔ MiBand 8 固件胶水层
// ============================================================
//
// 定位
// --------------------------------
// 接线之前的状态：February 是一套 5272 行、1636 行 host 单测覆盖的 header-only
// AI 运行时，但全仓库没有一个固件 TU 引用它 —— `FebruaryCore::instance()`
// 零调用，ai/february/Kconfig 甚至没被顶层 Kconfig source（本次一并修）。
//
// 真正在跑的「AI 引擎」是 `ai/intent_engine.hpp`：112 行硬编码阈值表，宿主
// apps/kernel.cpp，而 **miband8 根本不编 apps/kernel.cpp**。所以这次在
// miband8 上接线 February 是**纯增量**，与遗留引擎零冲突、不需要互斥开关、
// 也不存在删掉就无法回退的风险 —— 符合「保守接线」的前提。
//
// 分层
// --------------------------------
// 本文件只做三件事，且刻意不碰硬件：
//   1. 启动 FebruaryService（含内核临界区绑定）
//   2. 把「本周期采到的原始信号」翻译成 February 的 feed_* 调用
//   3. 推进一轮 run_once(now_ms)
//
// 硬件采样（SensorManager / ChargingManager / BLE）在 miband_kernel.hpp 的
// sensor_ble_daemon_task 里完成，本文件只接收已取好的值 —— 因此它可以在
// 主机单测里被完整覆盖，不需要任何 HAL stub。
//
// 为什么自己不做活动分类
// --------------------------------
// 遗留 `IntentEngine::infer_activity()` 是 5 类硬编码阈值；February 的
// context_manager.hpp 从步数增量、加速度模长与静默时长自行推导 8 类
// ActivityState（见 context_manager.hpp:42-44 / 113-114 / 177-178）。所以
// 本文件**只喂原始信号**（步数 / 心率 / 电量 / 时间 / BLE），
// 分类交给 February —— 这正是迁移到它的意义所在。
//
// SoftBus 现状
// --------------------------------
// February 的 SoftBus 是自包含的（只依赖自身头文件，不引 net/softbus），
// 通过 SoftBusTransportOps 回调与平台对接。MiBand 8 侧**本阶段不绑定
// transport**，因此 February 可以完成完整的本地推理，但意图发不出设备。
// 这是刻意的：先把推理链路跑通、量到体积，再决定要不要接跨设备。
// ============================================================

#include <stdint.h>

// Kconfig → config.hpp 宏翻译（必须早于任何 February 头文件）
#include "../../ai/february/kconfig_glue.hpp"
#include "../../ai/february/service.hpp"
#include "../../ai/february/february_core.hpp"

namespace aurora {
namespace watch {

// ============================================================
// FebSensorSample — 一个采样周期内的原始信号
//
// 全部用「本周期快照」而非累计量：February 的所有 feed_* 都会自己维护
// 差分与时间窗，重复喂累计步数会让它自己算 delta 时得到 0。
// ============================================================
struct FebSensorSample {
    uint32_t steps{0};        // 累计步数（SensorManager 原始计数）
    uint8_t battery_pct{100}; // 电量百分比
    uint16_t heart_rate{0};   // 心率 bpm；0 = 本周期无心率数据
    uint8_t hour{0};          // 0-23
    uint8_t minute{0};        // 0-59
    uint8_t weekday{0};       // 0-6
    bool ble_connected{false};
    int16_t ble_rssi_dbm{0};
};

// ============================================================
// 生命周期
// ============================================================

// 启动 FebruaryService。幂等：重复调用只会在第一次真正 start()。
// 返回 false 表示 February 在本构建中被 Kconfig 关掉（CONFIG_FEBRUARY=n），
// 此时其余接口全部退化为 no-op，调用方无需加 #ifdef。
bool february_boot() noexcept;

// 供测试与固件注入的临界区绑定入口（默认不绑 = no-op）。
// 固件侧传 Arch::disable_interrupts/enable_interrupts。
void february_bind_critical_section(void (*enter)(void*), void (*exit)(void*)) noexcept;

// 清空「值是否变化」的去重缓存。
//
// 存在的理由不只是为了让单测好写：这是驱动层的标准故障恢复入口。若传感器
// 重新初始化、或调用方把 `g_last` 喂成了陈旧值（例如换了一块表），缓存里的
// 旧值会让「值其实已变」的信号被误判成「没变」而永久跳过，February 会静默
// 地停留在错误的世界状态里。宿主在传感器重初始化后应显式调一次。
//
// 只清缓存，不碰 FebruaryCore —— 记忆/事件是否重置由调用方决定。
void february_reset_dedup_cache() noexcept;

// ============================================================
// 每周期调用
// ============================================================

// 喂一个采样周期。内部按「值是否变化」决定喂不喂，避免把 0 心率当成
// 「心率掉到 0」喂给 February（那会被判成异常状态）。
void february_feed(const FebSensorSample& s) noexcept;

// 推进一轮推理，返回本轮处理的事件数。未 boot 或未启用时返回 0。
uint32_t february_tick(uint32_t now_ms) noexcept;

// 诊断读数（ProcFS / shell 观测用）。
uint32_t february_total_intents() noexcept;
bool february_is_running() noexcept;

} // namespace watch
} // namespace aurora

#endif // AURORA_WATCH_FEBRUARY_GLUE_HPP
