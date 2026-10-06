// =============================================================================
// apps/clock/alarm.hpp
//
// 闹钟模型与触发时刻推算（纯逻辑，零内核依赖）。
//
// 定位（遵循 AGENTS.md §8 单一职责）：
//   本文件只回答一个问题——「下一个该响的闹钟是哪一个，还有多久」。
//   它不知道内核定时器、不知道文件系统、不知道 UI。真正的「什么时候唤醒」
//   由调度适配器决定（ProcessTimerManager / 任务 tick / RTOS 事件）。
//   正因为如此，本模块可以在 host 侧被完整测试，且能被任何调度机制复用。
//
// 依赖方向：apps → drivers（与应用层引用驱动的既有约定一致，见 apps/watch/）。
// 不反向依赖 kernel/、services/ 或 experimental/。
//
// 零动态分配：固定容量数组 + 栈上变量，可静态分配在应用 BSS 中。
// =============================================================================

#ifndef AURORA_APP_CLOCK_ALARM_HPP
#define AURORA_APP_CLOCK_ALARM_HPP

#include <stdint.h>
#include <stddef.h>

#include "../../drivers/rtc/rtc_driver.hpp"

namespace auroraos {
namespace app {
namespace clock {

using auroraos::rtc::BrokenDownTime;
using auroraos::rtc::civil_from_days;
using auroraos::rtc::days_from_civil;
using auroraos::rtc::days_in_month;
using auroraos::rtc::validate_broken_down;
using auroraos::rtc::weekday_from_civil;

// 闹钟槽位数。固定容量而非动态数组：8KB RAM 目标上无法接受堆分配，
// 且闹钟数量天然是用户可枚举的小集合。
constexpr size_t kMaxAlarms = 5;

// 贪睡时长上限（分钟）。防止误传超大值导致加法溢出。
constexpr uint16_t kMaxSnoozeMinutes = 24 * 60;

// ---------------------------------------------------------------------------
// 时间换算小工具
// ---------------------------------------------------------------------------
constexpr int32_t day_number(const BrokenDownTime& t) noexcept {
    return days_from_civil(static_cast<int32_t>(t.year), t.month, t.day);
}

constexpr uint32_t seconds_of_day(const BrokenDownTime& t) noexcept {
    return static_cast<uint32_t>(t.hour) * 3600u + static_cast<uint32_t>(t.minute) * 60u +
           static_cast<uint32_t>(t.second);
}

constexpr void refresh_weekday(BrokenDownTime& t) noexcept {
    t.weekday = weekday_from_civil(t.year, t.month, t.day);
}

// 在现有日期上加减天数，自动处理月末、年末与闰年。
// 不触碰时分秒字段。
inline void add_days(BrokenDownTime& t, int32_t days) noexcept {
    int32_t y = 0;
    uint8_t m = 0;
    uint8_t d = 0;
    civil_from_days(day_number(t) + days, y, m, d);
    t.year = static_cast<uint16_t>(y);
    t.month = m;
    t.day = d;
    refresh_weekday(t);
}

// 按「保留时分的墙钟语义」加减分钟，跨日时回填时分秒。
// 用于贪睡：贪睡是「9 分钟后的 07:09」，不是「7:09 这个时刻的第 N 天」。
inline void add_minutes(BrokenDownTime& t, uint16_t minutes) noexcept {
    uint32_t total = seconds_of_day(t) + static_cast<uint32_t>(minutes) * 60u;
    uint32_t extra_days = total / 86400u;
    total %= 86400u;
    t.hour = static_cast<uint8_t>(total / 3600u);
    t.minute = static_cast<uint8_t>((total % 3600u) / 60u);
    t.second = static_cast<uint8_t>(total % 60u);
    if (extra_days > 0) {
        add_days(t, static_cast<int32_t>(extra_days));
    }
    refresh_weekday(t);
}

// 全序比较：a < b 返回 true。用于挑出最近的一次触发。
constexpr bool time_before(const BrokenDownTime& a, const BrokenDownTime& b) noexcept {
    if (a.year != b.year) {
        return a.year < b.year;
    }
    if (a.month != b.month) {
        return a.month < b.month;
    }
    if (a.day != b.day) {
        return a.day < b.day;
    }
    return seconds_of_day(a) < seconds_of_day(b);
}

constexpr bool time_equal(const BrokenDownTime& a, const BrokenDownTime& b) noexcept {
    return a.year == b.year && a.month == b.month && a.day == b.day && seconds_of_day(a) == seconds_of_day(b);
}

// ---------------------------------------------------------------------------
// Alarm
// ---------------------------------------------------------------------------
enum class AlarmRepeat : uint8_t {
    kOnce = 0, // 响一次后自动关闭
    kDaily,    // 每天
    kWeekdays, // 按星期位掩码重复（仅工作日/仅周末等）
};

struct Alarm {
    bool enabled = false;
    uint8_t hour = 0;   // 0-23
    uint8_t minute = 0; // 0-59
    AlarmRepeat repeat = AlarmRepeat::kDaily;
    uint8_t weekday_mask = 0; // bit0=周一 .. bit6=周日；仅 kWeekdays 使用
};

// 输入合法性校验。
//
// 特别注意 kWeekdays + 空掩码：这种闹钟永远不可能触发。若放行，用户会看到
// 一个「已启用」的闹钟永远不响，是最典型的幽灵故障，故在入口就拒绝
// （AGENTS.md §22 显式错误处理）。
constexpr bool alarm_valid(const Alarm& a) noexcept {
    if (a.hour > 23 || a.minute > 59) {
        return false;
    }
    switch (a.repeat) {
    case AlarmRepeat::kOnce:
    case AlarmRepeat::kDaily:
        return true;
    case AlarmRepeat::kWeekdays:
        return (a.weekday_mask & 0x7Fu) != 0;
    default:
        return false; // 未知枚举值不得被静默接受
    }
}

constexpr bool weekday_selected(const Alarm& a, uint8_t iso_weekday) noexcept {
    // ISO: 1=周一 .. 7=周日；掩码 bit0=周一，因此左移一位对齐。
    if (iso_weekday < 1 || iso_weekday > 7) {
        return false;
    }
    return (a.weekday_mask & static_cast<uint8_t>(1u << (iso_weekday - 1u))) != 0;
}

// ---------------------------------------------------------------------------
// AlarmFire：一次待响事件
// ---------------------------------------------------------------------------
struct AlarmFire {
    int8_t index = -1;        // 命中的闹钟槽位；-1 表示无待响闹钟
    bool from_snooze = false; // true 表示来自贪睡而非闹钟本身
    uint32_t delay_s = 0;     // 距当前时刻的秒数
    BrokenDownTime at{};      // 绝对触发时刻（年月日时分秒有效，weekday 已回填）
    // 注意：from_snooze 为 true 时 index 无实际意义（置 0 仅为让调用方能把结果
    // 统一喂给 on_fired）。此时不要拿 index 去索引 alarm_at()，否则会读到一个
    // 与本次触发无关的闹钟。
};

// ---------------------------------------------------------------------------
// AlarmSchedule
//
// 持有闹钟集合与贪睡状态，提供纯计算的「下一个触发点」查询。
// 不做任何 I/O，调用方负责读取/保存。
// ---------------------------------------------------------------------------
class AlarmSchedule {
public:
    AlarmSchedule() = default;

    // 写入一个槽位。非法闹钟（含永不触发的空星期掩码）被拒绝且不改变原状态。
    bool set(uint8_t index, const Alarm& a) noexcept {
        if (index >= kMaxAlarms || !alarm_valid(a)) {
            return false;
        }
        alarms_[index] = a;
        alarms_[index].weekday_mask = static_cast<uint8_t>(a.weekday_mask & 0x7Fu);
        return true;
    }

    bool enable(uint8_t index, bool on) noexcept {
        if (index >= kMaxAlarms) {
            return false;
        }
        alarms_[index].enabled = on;
        return true;
    }

    const Alarm& alarm_at(uint8_t index) const noexcept {
        static const Alarm kEmpty{};
        return index < kMaxAlarms ? alarms_[index] : kEmpty;
    }

    size_t enabled_count() const noexcept {
        size_t n = 0;
        for (size_t i = 0; i < kMaxAlarms; ++i) {
            if (alarms_[i].enabled) {
                ++n;
            }
        }
        return n;
    }

    // 响铃后回调。传入 next_after() 返回的那一次触发，而不是裸槽位号——
    // 只有知道是「哪一次」被消费了，才能防止同一秒内被重复返回。
    //
    // - 贪睡触发：清除贪睡状态，不影响闹钟本身
    // - 一次性闹钟：自动关闭
    // - 重复闹钟：保持启用，并把该次触发记为已消费
    void on_fired(const AlarmFire& fire) noexcept {
        if (fire.index < 0) {
            return;
        }
        if (fire.from_snooze) {
            clear_snooze();
            return;
        }
        const uint8_t index = static_cast<uint8_t>(fire.index);
        if (index >= kMaxAlarms) {
            return;
        }
        last_fired_at_ = fire.at;
        has_fired_ = true;
        if (alarms_[index].repeat == AlarmRepeat::kOnce) {
            alarms_[index].enabled = false;
        }
    }

    // 设置贪睡触发。minutes 为 0 视为取消贪睡（避免「贪睡 0 分钟」产生
    // 一个立即触发的死循环）。重复调用会覆盖上一次未响的贪睡。
    void snooze(const BrokenDownTime& now, uint16_t minutes) noexcept {
        if (minutes == 0 || minutes > kMaxSnoozeMinutes) {
            clear_snooze();
            return;
        }
        snooze_at_ = now;
        add_minutes(snooze_at_, minutes);
        snooze_pending_ = true;
    }

    void clear_snooze() noexcept {
        snooze_pending_ = false;
    }

    bool snooze_pending() const noexcept {
        return snooze_pending_;
    }

    // -------------------------------------------------------------------------
    // next_after：核心查询。纯计算，不修改任何状态。
    //
    // 判定语义：候选时刻 >= now 即视为可触发。这样在恰好 07:00:00 轮询时
    // 闹钟会响一次；下一次轮询（07:00:01）候选时刻已早于 now，自动推进到明天，
    // 不会重复触发。
    //
    // 前提：调用方必须以足够细的粒度轮询（或按时唤醒）。若从休眠中恢复时已经
    // 越过触发点 5 秒，则该次轮询看到的候选时刻早于 now，本轮不会补响——
    // 这是刻意取舍：闹钟类产品宁可漏响一次也不要在一觉醒来后突然炸响。
    // -------------------------------------------------------------------------
    AlarmFire next_after(const BrokenDownTime& now) const noexcept {
        AlarmFire best{};

        for (size_t i = 0; i < kMaxAlarms; ++i) {
            const Alarm& a = alarms_[i];
            if (!a.enabled) {
                continue;
            }
            BrokenDownTime candidate{};
            if (!next_occurrence_of(a, now, last_fired_at_, has_fired_, candidate)) {
                continue;
            }
            if (best.index < 0 || time_before(candidate, best.at)) {
                best.index = static_cast<int8_t>(i);
                best.from_snooze = false;
                best.at = candidate;
            }
        }

        // 贪睡参与竞争，取更早的那个。
        if (snooze_pending_ && !time_before(snooze_at_, now)) {
            if (best.index < 0 || time_before(snooze_at_, best.at)) {
                best.index = 0;
                best.from_snooze = true;
                best.at = snooze_at_;
            }
        }

        if (best.index < 0) {
            return best;
        }

        // 现在不为负（调用方传入合法时间），故差值用无符号计算是安全的。
        const int32_t day_diff = day_number(best.at) - day_number(now);
        const int32_t sec_diff =
            static_cast<int32_t>(seconds_of_day(best.at)) - static_cast<int32_t>(seconds_of_day(now));
        int32_t total = day_diff * 86400 + sec_diff;
        if (total < 0) {
            total = 0; // 时钟回拨：宁可立刻响，也不要算出巨大的负延迟
        }
        best.delay_s = static_cast<uint32_t>(total);
        return best;
    }

    // 清空全部闹钟与贪睡状态。持久化加载失败时由调用方决定是否回退到此状态。
    void clear() noexcept {
        for (size_t i = 0; i < kMaxAlarms; ++i) {
            alarms_[i] = Alarm{};
        }
        clear_snooze();
        has_fired_ = false;
    }

private:
    // 求 a 在 now 之后（含等于）的最近一次**尚未被消费**的触发时刻。
    // false 表示不存在。
    static bool next_occurrence_of(const Alarm& a, const BrokenDownTime& now, const BrokenDownTime& last_fired,
                                   bool has_fired, BrokenDownTime& out) noexcept {
        BrokenDownTime cand = now;
        cand.hour = a.hour;
        cand.minute = a.minute;
        cand.second = 0;

        // 该次触发刚被消费掉：即使候选时刻 == now 也必须跳过，否则调度器
        // 在同一秒内「查询→响铃→再查询」会拿到同一个结果，形成零延迟死循环。
        const bool candidate_consumed = has_fired && time_equal(cand, last_fired);

        if (a.repeat == AlarmRepeat::kWeekdays) {
            // 试 today..today+7 共 8 个候选：今天这次已过且刚响过时，同星期的
            // 下一次正好落在 offset 7，只扫 0..6 会漏掉它（会误判成「永不触发」）。
            // 掩码非空由 alarm_valid 保证，故 today+1..today+7 中必有一天命中。
            for (uint8_t offset = 0; offset <= 7; ++offset) {
                BrokenDownTime probe = cand;
                if (offset > 0) {
                    add_days(probe, offset);
                }
                if (!weekday_selected(a, probe.weekday)) {
                    continue;
                }
                if (has_fired && time_equal(probe, last_fired)) {
                    continue; // 该次已响过
                }
                if (!time_before(probe, now)) { // probe >= now
                    out = probe;
                    return true;
                }
            }
            return false; // 掩码为空才会走到这里
        }

        // kOnce / kDaily：今天已过、或今天这一场刚响过，则推到明天
        if (time_before(cand, now) || candidate_consumed) {
            add_days(cand, 1);
        }
        out = cand;
        return true;
    }

    Alarm alarms_[kMaxAlarms];
    BrokenDownTime snooze_at_{};
    bool snooze_pending_ = false;
    BrokenDownTime last_fired_at_{};
    bool has_fired_ = false;
};

} // namespace clock
} // namespace app
} // namespace auroraos

#endif // AURORA_APP_CLOCK_ALARM_HPP
