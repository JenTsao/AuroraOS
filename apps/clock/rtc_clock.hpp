// =============================================================================
// apps/clock/rtc_clock.hpp
//
// 墙上时间源抽象：RTC 硬件 / 外部对时 / 单调 tick 降级 三态。
//
// 解决的问题：
//   AuroraOS 内核时间源 `handle_get_time` 返回的是开机 tick 累计值，掉电归零。
//   而充电宝闹钟这类终端会频繁断电（拔掉充电线），若只有 tick 时间源，
//   每次上电都不知道「现在几点」，闹钟时刻无从谈起。
//
// 三种状态及其含义（trust() 查询）：
//
//   source=kRtc, trust=kTrusted
//       电池供电 RTC 正在提供可信墙上时间。掉电保时间。
//
//   source=kRtc, trust=kUntrusted
//       芯片在，但自报时间不可信（晶振停过：掉电过、纽扣电池耗尽、
//       或上次没来得及对时）。寄存器里留着的是过期值。
//       【必须重新对时】——此时把过期时间当现在用，会让闹钟「在开机后第
//       N 分钟就响」。这是电池供电设备最危险的失效模式。
//
//   source=kTick, trust=kDegraded
//       没有可用 RTC，纯粹靠上次对时建立的基准 + 单调 tick 推进。
//       设备还在走，但一重启就归零。仅适合作为过渡期降级，不适合长期使用。
//
//   source=kNone
//       尚未建立任何时间基准，now() 返回 kNotInitialized。
//
// 设计决策：不做自动降级
//   RTC 失效时本模块**只上报不兜底**。若在一次 I2C 偶发错误后偷偷切到 tick，
//   系统会在用户毫无察觉的情况下退化成「重启即失忆」。降级必须是显式决策，
//   由调用方在确认可接受后再调用 sync_from_external 建立新基准。
//   同理，kUntrusted 也不会自动退回 kDegraded。
//
// 零动态分配、单态固定内存，可静态分配在应用 BSS 中。
// =============================================================================

#ifndef AURORA_APP_CLOCK_RTC_CLOCK_HPP
#define AURORA_APP_CLOCK_RTC_CLOCK_HPP

#include <stdint.h>

#include "../../drivers/rtc/rtc_driver.hpp"
#include "alarm.hpp"

namespace auroraos {
namespace app {
namespace clock {

using auroraos::rtc::BrokenDownTime;
using auroraos::rtc::IRtcDriver;
using auroraos::rtc::RtcError;
using auroraos::rtc::validate_broken_down;
using auroraos::rtc::weekday_from_civil;

enum class ClockSource : uint8_t {
    kNone = 0, // 尚无时间基准
    kRtc,      // 电池供电 RTC
    kTick,     // 仅单调 tick（上次对时基准 + 流逝时间）
};

enum class ClockTrust : uint8_t {
    kTrusted = 0,   // 时间可信
    kUntrusted = 1, // RTC 在但自报时间过期，必须重新对时
    kDegraded = 2,  // tick 降级运行，重启即失忆
};

// ---------------------------------------------------------------------------
// RtcClock
//
// 线程安全说明：内部无锁。读时钟（now）与对时（sync_from_external）若要并发，
// 调用方须自行串行化。典型用法是单一 UI/应用任务独占本对象。
//
// tick_ms 由调用方注入而非本模块自行读取，理由有二：
//   1. 不引入任何定时器依赖，保持本模块在 host 侧完全可测
//   2. 由上层决定时间基准（SysTick / RTOS tick / 测试注入）
// ---------------------------------------------------------------------------
class RtcClock {
public:
    RtcClock() = default;

    // 注入 RTC 驱动。可为 nullptr，表示该板型没有 RTC（走纯 tick 降级）。
    // 必须在 begin() 之前调用。
    void attach_rtc(IRtcDriver* rtc) noexcept {
        rtc_ = rtc; // 非拥有指针，生命周期由板级/应用保证
        source_ = ClockSource::kNone;
        trust_ = ClockTrust::kUntrusted;
        tick_base_valid_ = false;
    }

    // 探测 RTC 并判定初始状态。
    //
    // 返回值含义：探测是否成功（器件是否在位）。时间是否可信请查 trust()。
    // - 器件在位且时间可读 → kRtc / kTrusted
    // - 器件在位但时间过期 → kRtc / kUntrusted（调用方应触发对时流程）
    // - 器件不在位        → kNone / kUntrusted（需外部对时建立基准）
    // - 总线错误          → kNone / kUntrusted
    RtcError begin() noexcept {
        source_ = ClockSource::kNone;
        trust_ = ClockTrust::kUntrusted;
        tick_base_valid_ = false;

        if (rtc_ == nullptr) {
            return RtcError::kNotPresent;
        }

        const RtcError probe = rtc_->init();
        if (probe != RtcError::kOk) {
            return probe;
        }

        source_ = ClockSource::kRtc;

        BrokenDownTime probe_time{};
        const RtcError read = rtc_->get_time(probe_time);
        if (read == RtcError::kOk) {
            trust_ = ClockTrust::kTrusted;
        } else if (read == RtcError::kTimeNotValid) {
            // 芯片在但晶振停过：保留 kRtc/kUntrusted，让上层知道必须对时，
            // 而不是把过期时间当成有效时间。
            trust_ = ClockTrust::kUntrusted;
        } else {
            // 总线/在位问题：此时不能声称有 RTC 时间可用
            source_ = ClockSource::kNone;
            trust_ = ClockTrust::kUntrusted;
        }
        return read;
    }

    // 读取当前时间，并顺带刷新可信度状态。
    //
    // 非 const 是有意的：晶振可能在 begin() 之后才停摆，若 now() 不更新
    // trust_，needs_resync() 会在最需要它的时候撒谎。
    //
    // kRtc 路径：直接问驱动，并补齐星期（芯片的星期字段不可信，见 ds3231 驱动）。
    // kTick 路径：基准时间 + (tick_ms - tick_base_ms) 秒。星期随之推算。
    //
    // 返回值即 rtc::RtcError。kTimeNotValid / kBusError / kNotPresent /
    // kNotInitialized 都需要调用方处置（见文件头「不做自动降级」）。
    RtcError now(uint32_t tick_ms, BrokenDownTime& out) noexcept {
        if (source_ == ClockSource::kRtc) {
            if (rtc_ == nullptr) {
                return RtcError::kNotPresent;
            }
            const RtcError r = rtc_->get_time(out);
            if (r == RtcError::kOk) {
                out.weekday = weekday_from_civil(out.year, out.month, out.day);
                trust_ = ClockTrust::kTrusted;
            } else if (r == RtcError::kTimeNotValid) {
                trust_ = ClockTrust::kUntrusted;
            }
            return r;
        }

        if (source_ == ClockSource::kTick) {
            if (!tick_base_valid_) {
                return RtcError::kNotInitialized;
            }
            // 无符号减法在 tick 计数回绕（约 49.7 天）时仍得到正确流逝量，
            // 只要真实流逝时间小于 2^32 毫秒。
            const uint32_t elapsed_ms = tick_ms - tick_base_ms_;
            out = tick_base_time_;
            add_seconds(out, elapsed_ms / 1000u);
            refresh_weekday(out);
            return RtcError::kOk;
        }

        return RtcError::kNotInitialized;
    }

    // 外部对时（BLE 手机下发、或用户手动拨钟）。
    //
    // 行为：
    //   1. 校验时间合法性，非法直接拒绝且不改变任何状态
    //   2. 若有可用 RTC，尝试写回芯片，使断电后仍能保持
    //   3. 无论芯片写入是否成功，都建立 tick 基准，作为本次会话的时间来源
    //
    // 返回 true 表示「本次会话有了可用时间基准」。注意：芯片写入失败但基准
    // 建立成功时仍返回 true——此时 source=kTick/trust=kDegraded，设备能走，
    // 但重启后会失忆。调用方若需要断电保时间，应检查 source()==kRtc。
    bool sync_from_external(const BrokenDownTime& t, uint32_t tick_ms) noexcept {
        if (!validate_broken_down(t)) {
            return false;
        }

        BrokenDownTime base = t;
        refresh_weekday(base);

        bool rtc_ok = false;
        if (source_ == ClockSource::kRtc && rtc_ != nullptr && rtc_->initialized()) {
            rtc_ok = (rtc_->set_time(base) == RtcError::kOk);
        }

        tick_base_time_ = base;
        tick_base_ms_ = tick_ms;
        tick_base_valid_ = true;

        if (rtc_ok) {
            source_ = ClockSource::kRtc;
            trust_ = ClockTrust::kTrusted;
        } else {
            source_ = ClockSource::kTick;
            trust_ = ClockTrust::kDegraded;
        }
        return true;
    }

    ClockSource source() const noexcept {
        return source_;
    }

    ClockTrust trust() const noexcept {
        return trust_;
    }

    // 是否需要上层触发对时流程。
    //
    // 供 UI 决定「请连接手机校时」或「时间未知，请手动设置」的唯一判据。
    bool needs_resync() const noexcept {
        return source_ == ClockSource::kNone || trust_ != ClockTrust::kTrusted;
    }

private:
    // 在墙钟语义下加若干秒，跨日时进位到时分秒。
    static void add_seconds(BrokenDownTime& t, uint32_t seconds) noexcept {
        uint32_t total = seconds_of_day(t) + seconds;
        uint32_t extra_days = total / 86400u;
        total %= 86400u;
        t.hour = static_cast<uint8_t>(total / 3600u);
        t.minute = static_cast<uint8_t>((total % 3600u) / 60u);
        t.second = static_cast<uint8_t>(total % 60u);
        if (extra_days > 0) {
            add_days(t, static_cast<int32_t>(extra_days));
        }
    }

    IRtcDriver* rtc_ = nullptr; // 非拥有指针
    ClockSource source_ = ClockSource::kNone;
    ClockTrust trust_ = ClockTrust::kUntrusted;
    BrokenDownTime tick_base_time_{};
    uint32_t tick_base_ms_ = 0;
    bool tick_base_valid_ = false;
};

} // namespace clock
} // namespace app
} // namespace auroraos

#endif // AURORA_APP_CLOCK_RTC_CLOCK_HPP
