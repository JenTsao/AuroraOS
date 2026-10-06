// =============================================================================
// tests/unit/test_clock_app.cpp
//
// 智能闹钟应用层测试（apps/clock）：
//   - 日历换算：days_from_civil / civil_from_days 互逆性
//   - AlarmSchedule：日/一次性/星期重复的触发点推算、跨月跨年闰日、贪睡
//   - AlarmStore：定长二进制往返、损坏拒绝、失败不污染原状态
//   - RtcClock：三态时间源、tick 降级推进、以及「绝不静默降级」这条不变量
//
// 三者均为纯逻辑、零堆分配、无内核依赖，因此可在 host 侧完整验证。
// =============================================================================
#include <gtest/gtest.h>

#include <cstring>

#include "../../apps/clock/alarm.hpp"
#include "../../apps/clock/alarm_store.hpp"
#include "../../apps/clock/rtc_clock.hpp"
#include "../../drivers/rtc/rtc_driver.hpp"

using auroraos::app::clock::Alarm;
using auroraos::app::clock::AlarmFire;
using auroraos::app::clock::AlarmRepeat;
using auroraos::app::clock::AlarmSchedule;
using auroraos::app::clock::AlarmStore;
using auroraos::app::clock::ClockSource;
using auroraos::app::clock::ClockTrust;
using auroraos::app::clock::RtcClock;
using auroraos::app::clock::kMaxAlarms;
using auroraos::app::clock::kSerializedSize;

using auroraos::rtc::BrokenDownTime;
using auroraos::rtc::RtcError;
using auroraos::rtc::civil_from_days;
using auroraos::rtc::days_from_civil;

namespace {

BrokenDownTime make_time(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    BrokenDownTime t{};
    t.year = static_cast<uint16_t>(y);
    t.month = static_cast<uint8_t>(mo);
    t.day = static_cast<uint8_t>(d);
    t.hour = static_cast<uint8_t>(h);
    t.minute = static_cast<uint8_t>(mi);
    t.second = static_cast<uint8_t>(s);
    t.weekday = auroraos::rtc::weekday_from_civil(t.year, t.month, t.day);
    return t;
}

Alarm daily(uint8_t h, uint8_t m, bool on = true) {
    Alarm a;
    a.enabled = on;
    a.hour = h;
    a.minute = m;
    a.repeat = AlarmRepeat::kDaily;
    return a;
}

} // namespace

// ============================================================================
// 日历换算
// ============================================================================
TEST(ClockCalendar, DaysFromCivilMatchesEpoch) {
    EXPECT_EQ(0, days_from_civil(1970, 1, 1));
    EXPECT_EQ(1, days_from_civil(1970, 1, 2));
    EXPECT_LT(0, days_from_civil(2026, 10, 6));
}

TEST(ClockCalendar, CivilFromDaysIsInverseOfDaysFromCivil) {
    // 覆盖闰年边界、世纪年、月末年末
    const int dates[][3] = {
        {1970, 1, 1},  {1999, 12, 31}, {2000, 2, 29}, {2001, 2, 28}, {2024, 2, 29},
        {2026, 1, 31}, {2026, 10, 6},  {2100, 3, 1},  {2199, 12, 31},
    };
    for (const auto& d : dates) {
        SCOPED_TRACE(d[0]);
        int32_t y = 0;
        uint8_t m = 0;
        uint8_t day = 0;
        civil_from_days(days_from_civil(d[0], d[1], d[2]), y, m, day);
        EXPECT_EQ(d[0], y);
        EXPECT_EQ(d[1], m);
        EXPECT_EQ(d[2], day);
    }
}

TEST(ClockCalendar, ConsecutiveDaysAreConsecutive) {
    // 从 2024-02-27 连推 5 天，必须逐日递增并正确落到闰日
    BrokenDownTime t = make_time(2024, 2, 27);
    const int expected_day[] = {28, 29, 1, 2, 3};
    const int expected_month[] = {2, 2, 3, 3, 3};
    for (int i = 0; i < 5; ++i) {
        auroraos::app::clock::add_days(t, 1);
        EXPECT_EQ(expected_day[i], t.day);
        EXPECT_EQ(expected_month[i], t.month);
    }
    EXPECT_EQ(2024, t.year);
}

// ============================================================================
// AlarmSchedule
// ============================================================================
TEST(AlarmSchedule, EmptyScheduleHasNoNextFire) {
    AlarmSchedule sched;
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 12, 0, 0));
    EXPECT_EQ(-1, f.index);
}

TEST(AlarmSchedule, DisabledAlarmIsIgnored) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0, false)));
    EXPECT_EQ(0U, sched.enabled_count());
    EXPECT_EQ(-1, sched.next_after(make_time(2026, 10, 6, 12, 0, 0)).index);
}

TEST(AlarmSchedule, DailyAlarmLaterTodayFiresToday) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(23, 30)));
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 12, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(23, f.at.hour);
    EXPECT_EQ(30, f.at.minute);
    EXPECT_EQ(6, f.at.day);
    EXPECT_EQ(11u * 3600u + 30u * 60u, f.delay_s);
}

TEST(AlarmSchedule, DailyAlarmAlreadyPassedRollsToTomorrow) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0)));
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 12, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(7, f.at.day);
    EXPECT_EQ(7, f.at.hour);
    // 次日 07:00 距 12:00 共 19 小时
    EXPECT_EQ(19u * 3600u, f.delay_s);
}

TEST(AlarmSchedule, AlarmExactlyAtNowStillFires) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0)));
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    EXPECT_EQ(0, f.index);
    EXPECT_EQ(0u, f.delay_s);
}

TEST(AlarmSchedule, OneShotDisablesItselfAfterFiring) {
    AlarmSchedule sched;
    Alarm a = daily(7, 0);
    a.repeat = AlarmRepeat::kOnce;
    ASSERT_TRUE(sched.set(0, a));

    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 6, 0, 0));
    ASSERT_EQ(0, f.index);
    sched.on_fired(f);
    EXPECT_EQ(0U, sched.enabled_count());
    EXPECT_EQ(-1, sched.next_after(make_time(2026, 10, 7, 6, 0, 0)).index);
}

TEST(AlarmSchedule, DailyAlarmStaysEnabledButDoesNotRefireSameSecond) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0)));
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    ASSERT_EQ(0, f.index);
    sched.on_fired(f);
    EXPECT_EQ(1U, sched.enabled_count());

    // 关键回归：调度器可能在同一秒内「查询→响铃→再查询」。若不记录已响过
    // 的那一次，这里会再次返回今天 07:00（delay 0），形成零延迟死循环。
    const AlarmFire again = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    ASSERT_EQ(0, again.index);
    EXPECT_EQ(7, again.at.day); // 必须已推进到明天
    EXPECT_GT(again.delay_s, 0u);
}

TEST(AlarmSchedule, WeekdayAlarmPicksNextMatchingDay) {
    AlarmSchedule sched;
    Alarm a;
    a.enabled = true;
    a.hour = 8;
    a.minute = 15;
    a.repeat = AlarmRepeat::kWeekdays;
    a.weekday_mask = 0x01; // 仅周一
    ASSERT_TRUE(sched.set(0, a));

    // 2026-10-06 是周二
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 9, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(1, f.at.weekday); // 下周一
    EXPECT_EQ(12, f.at.day);
    EXPECT_EQ(8, f.at.hour);
}

TEST(AlarmSchedule, WeekdayAlarmSameDayBeforeTimeStillFiresToday) {
    AlarmSchedule sched;
    Alarm a;
    a.enabled = true;
    a.hour = 8;
    a.minute = 15;
    a.repeat = AlarmRepeat::kWeekdays;
    a.weekday_mask = 0x02; // 仅周二
    ASSERT_TRUE(sched.set(0, a));

    // 2026-10-06 周二 06:00，闹钟 08:15 → 今天
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 6, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(6, f.at.day);
    EXPECT_EQ(2, f.at.weekday);
}

TEST(AlarmSchedule, WeekdayAlarmWithTodayPassedRollsToNextWeek) {
    AlarmSchedule sched;
    Alarm a;
    a.enabled = true;
    a.hour = 8;
    a.repeat = AlarmRepeat::kWeekdays;
    a.weekday_mask = 0x02; // 仅周二
    ASSERT_TRUE(sched.set(0, a));

    // 2026-10-06 周二 09:00，闹钟周二 08:00 → 下周二 08:00，即 7 天减 1 小时
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 9, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(2, f.at.weekday);
    EXPECT_EQ(13, f.at.day);
    EXPECT_EQ(7u * 86400u - 3600u, f.delay_s);
}

TEST(AlarmSchedule, PicksEarliestAcrossMultipleAlarms) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(23, 0)));
    ASSERT_TRUE(sched.set(1, daily(6, 30)));
    ASSERT_TRUE(sched.set(2, daily(12, 0)));

    // 05:00 —— 三个都还没到，最早的应是 06:30
    const AlarmFire early = sched.next_after(make_time(2026, 10, 6, 5, 0, 0));
    EXPECT_EQ(1, early.index);
    EXPECT_EQ(6, early.at.hour);
    EXPECT_EQ(30, early.at.minute);

    // 07:00 —— 06:30 已过（已推到明天），今天最早的是 12:00
    const AlarmFire later = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    EXPECT_EQ(2, later.index);
    EXPECT_EQ(12, later.at.hour);
    EXPECT_EQ(6, later.at.day);
}

TEST(AlarmSchedule, RollsOverYearBoundary) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0)));
    const AlarmFire f = sched.next_after(make_time(2026, 12, 31, 12, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(2027, f.at.year);
    EXPECT_EQ(1, f.at.month);
    EXPECT_EQ(1, f.at.day);
}

TEST(AlarmSchedule, RollsOverMonthBoundaryShorterMonth) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 0)));
    const AlarmFire f = sched.next_after(make_time(2026, 1, 31, 12, 0, 0));
    ASSERT_EQ(0, f.index);
    EXPECT_EQ(2, f.at.month);
    EXPECT_EQ(1, f.at.day);
}

TEST(AlarmSchedule, SetRejectsInvalidInput) {
    AlarmSchedule sched;
    Alarm a = daily(7, 0);

    a.hour = 24;
    EXPECT_FALSE(sched.set(0, a));
    a.hour = 7;
    a.minute = 60;
    EXPECT_FALSE(sched.set(0, a));
    a.minute = 0;
    EXPECT_FALSE(sched.set(kMaxAlarms, a)); // 槽位越界
    EXPECT_EQ(0U, sched.enabled_count());
}

TEST(AlarmSchedule, RejectsWeekdayAlarmThatCanNeverFire) {
    // 空星期掩码的「已启用」闹钟会永远不响——幽灵故障，入口必须拒绝
    AlarmSchedule sched;
    Alarm a;
    a.enabled = true;
    a.hour = 7;
    a.repeat = AlarmRepeat::kWeekdays;
    a.weekday_mask = 0x00;
    EXPECT_FALSE(sched.set(0, a));
    EXPECT_EQ(0U, sched.enabled_count());
}

TEST(AlarmSchedule, WeekdayMaskIsClampedToSevenBits) {
    AlarmSchedule sched;
    Alarm a;
    a.enabled = true;
    a.hour = 7;
    a.repeat = AlarmRepeat::kWeekdays;
    a.weekday_mask = 0xFF; // 含非法高位
    ASSERT_TRUE(sched.set(0, a));
    EXPECT_EQ(0x7F, sched.alarm_at(0).weekday_mask);
}

// ---------------------------------------------------------------------------
// 贪睡
// ---------------------------------------------------------------------------
TEST(AlarmSnooze, SchedulesNineMinutesLater) {
    AlarmSchedule sched;
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    EXPECT_TRUE(f.from_snooze);
    EXPECT_EQ(7, f.at.hour);
    EXPECT_EQ(9, f.at.minute);
    EXPECT_EQ(9u * 60u, f.delay_s);
}

TEST(AlarmSnooze, ZeroMinutesCancelsSnooze) {
    AlarmSchedule sched;
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    ASSERT_TRUE(sched.snooze_pending());
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 0);
    EXPECT_FALSE(sched.snooze_pending());
    EXPECT_EQ(-1, sched.next_after(make_time(2026, 10, 6, 7, 0, 0)).index);
}

TEST(AlarmSnooze, OvernightSnoozeCarriesToNextDay) {
    AlarmSchedule sched;
    sched.snooze(make_time(2026, 10, 6, 23, 50, 0), 20);
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 23, 50, 0));
    ASSERT_EQ(7, f.at.day);
    EXPECT_EQ(0, f.at.hour);
    EXPECT_EQ(10, f.at.minute);
}

TEST(AlarmSnooze, WinsWhenEarlierThanAlarm) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(12, 0)));
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    EXPECT_TRUE(f.from_snooze);
    EXPECT_EQ(9, f.at.minute);
}

TEST(AlarmSnooze, LosesWhenLaterThanAlarm) {
    AlarmSchedule sched;
    ASSERT_TRUE(sched.set(0, daily(7, 5)));
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    const AlarmFire f = sched.next_after(make_time(2026, 10, 6, 7, 0, 0));
    EXPECT_FALSE(f.from_snooze);
    EXPECT_EQ(0, f.index);
    EXPECT_EQ(5, f.at.minute);
}

TEST(AlarmSnooze, IgnoredOnceSnoozeTimeHasPassed) {
    AlarmSchedule sched;
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    // 已经过了贪睡时刻
    EXPECT_EQ(-1, sched.next_after(make_time(2026, 10, 6, 7, 10, 0)).index);
}

TEST(AlarmSnooze, OverlongRequestCancelsRatherThanOverflows) {
    AlarmSchedule sched;
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 9);
    sched.snooze(make_time(2026, 10, 6, 7, 0, 0), 65535);
    EXPECT_FALSE(sched.snooze_pending());
}

// ============================================================================
// AlarmStore
// ============================================================================
TEST(AlarmStore, SerializedSizeIsFixed) {
    EXPECT_EQ(32u, kSerializedSize);
}

TEST(AlarmStore, RoundTripsAllAlarmKinds) {
    AlarmSchedule src;
    Alarm d = daily(7, 30);
    ASSERT_TRUE(src.set(0, d));

    Alarm once = daily(22, 15);
    once.repeat = AlarmRepeat::kOnce;
    ASSERT_TRUE(src.set(1, once));

    Alarm wd;
    wd.enabled = true;
    wd.hour = 9;
    wd.minute = 5;
    wd.repeat = AlarmRepeat::kWeekdays;
    wd.weekday_mask = 0x15; // 周一/三/五
    ASSERT_TRUE(src.set(2, wd));

    uint8_t buf[kSerializedSize];
    size_t written = 0;
    ASSERT_TRUE(AlarmStore::serialize(src, buf, sizeof(buf), written));
    EXPECT_EQ(kSerializedSize, written);

    AlarmSchedule dst;
    ASSERT_TRUE(AlarmStore::deserialize(buf, written, dst));
    EXPECT_EQ(3U, dst.enabled_count());
    for (uint8_t i = 0; i < 3; ++i) {
        EXPECT_EQ(src.alarm_at(i).enabled, dst.alarm_at(i).enabled) << "alarm " << int(i);
        EXPECT_EQ(src.alarm_at(i).hour, dst.alarm_at(i).hour) << "alarm " << int(i);
        EXPECT_EQ(src.alarm_at(i).minute, dst.alarm_at(i).minute) << "alarm " << int(i);
        EXPECT_EQ(src.alarm_at(i).repeat, dst.alarm_at(i).repeat) << "alarm " << int(i);
        EXPECT_EQ(src.alarm_at(i).weekday_mask, dst.alarm_at(i).weekday_mask) << "alarm " << int(i);
    }
}

TEST(AlarmStore, EmptyScheduleRoundTrips) {
    AlarmSchedule src;
    uint8_t buf[kSerializedSize];
    size_t written = 0;
    ASSERT_TRUE(AlarmStore::serialize(src, buf, sizeof(buf), written));
    AlarmSchedule dst;
    ASSERT_TRUE(AlarmStore::deserialize(buf, written, dst));
    EXPECT_EQ(0U, dst.enabled_count());
}

TEST(AlarmStore, DisabledAlarmsPreserveTheirTime) {
    AlarmSchedule src;
    Alarm a = daily(6, 15, false);
    ASSERT_TRUE(src.set(0, a));
    uint8_t buf[kSerializedSize];
    size_t written = 0;
    ASSERT_TRUE(AlarmStore::serialize(src, buf, sizeof(buf), written));

    AlarmSchedule dst;
    ASSERT_TRUE(AlarmStore::deserialize(buf, written, dst));
    EXPECT_FALSE(dst.alarm_at(0).enabled);
    EXPECT_EQ(6, dst.alarm_at(0).hour); // 关掉的闹钟不该被抹成 0:00
    EXPECT_EQ(15, dst.alarm_at(0).minute);
}

TEST(AlarmStore, RejectsBufferTooSmallOrNull) {
    AlarmSchedule src;
    uint8_t buf[kSerializedSize] = {0};
    size_t written = 0;
    uint8_t small[8] = {0};
    EXPECT_FALSE(AlarmStore::serialize(src, small, sizeof(small), written));
    EXPECT_FALSE(AlarmStore::serialize(src, nullptr, kSerializedSize, written));
    EXPECT_FALSE(AlarmStore::deserialize(nullptr, kSerializedSize, src));
    EXPECT_FALSE(AlarmStore::deserialize(buf, kSerializedSize - 1, src));
}

namespace {

// 造一份含一个启用闹钟的合法镜像，供损坏注入测试使用
void make_valid_image(uint8_t (&buf)[kSerializedSize]) {
    AlarmSchedule s;
    s.set(0, daily(7, 30));
    size_t w = 0;
    AlarmStore::serialize(s, buf, kSerializedSize, w);
}

// 断言：反序列化失败且目标对象保持调用前的样子（失败不污染）
template <typename Mutate>
void expect_rejected_without_corrupting(Mutate mutate) {
    uint8_t buf[kSerializedSize];
    make_valid_image(buf);
    mutate(buf);

    AlarmSchedule dst;
    Alarm existing = daily(23, 45);
    ASSERT_TRUE(dst.set(0, existing));
    const size_t before = dst.enabled_count();

    EXPECT_FALSE(AlarmStore::deserialize(buf, kSerializedSize, dst));
    EXPECT_EQ(before, dst.enabled_count());
    EXPECT_EQ(23, dst.alarm_at(0).hour);
    EXPECT_EQ(45, dst.alarm_at(0).minute);
}

} // namespace

TEST(AlarmStore, RejectsBadMagic) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) { buf[0] = 'X'; });
}

TEST(AlarmStore, RejectsUnknownVersion) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) { buf[4] = 99; });
}

TEST(AlarmStore, RejectsBadChecksum) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) { buf[28] ^= 0xFF; });
}

TEST(AlarmStore, RejectsOutOfRangeHour) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) {
        buf[8 + 1] = 30; // hour=30，越界
        // 重算校验和，确保是「语义非法」而非「校验和失败」被拒
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < 28; ++i) {
            h ^= buf[i];
            h *= 16777619u;
        }
        buf[28] = static_cast<uint8_t>(h & 0xFF);
        buf[29] = static_cast<uint8_t>((h >> 8) & 0xFF);
        buf[30] = static_cast<uint8_t>((h >> 16) & 0xFF);
        buf[31] = static_cast<uint8_t>((h >> 24) & 0xFF);
    });
}

TEST(AlarmStore, RejectsReservedBytesSet) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) {
        buf[6] = 1; // reserved 必须为 0
    });
}

TEST(AlarmStore, RejectsUnknownFlagBits) {
    expect_rejected_without_corrupting([](uint8_t (&buf)[kSerializedSize]) {
        buf[8] |= 0x80; // weekday_mask 之上还有 bit7 未定义
    });
}

// ============================================================================
// RtcClock
// ============================================================================
namespace {

class MockRtcDriver : public auroraos::rtc::IRtcDriver {
public:
    bool probe_ok = true;
    bool init_done = false;
    RtcError get_result = RtcError::kOk;
    RtcError set_result = RtcError::kOk;
    BrokenDownTime time{};
    int set_calls = 0;

    RtcError init() noexcept override {
        init_done = probe_ok;
        return probe_ok ? RtcError::kOk : RtcError::kBusError;
    }
    bool initialized() const noexcept override {
        return init_done;
    }
    RtcError get_time(BrokenDownTime& out) const noexcept override {
        if (get_result != RtcError::kOk) {
            return get_result;
        }
        out = time;
        return RtcError::kOk;
    }
    RtcError set_time(const BrokenDownTime& t) noexcept override {
        ++set_calls;
        if (set_result != RtcError::kOk) {
            return set_result;
        }
        time = t;
        // 对时成功意味着晶振重新跑起来了，后续读时间应当可信
        get_result = RtcError::kOk;
        return RtcError::kOk;
    }
    bool oscillator_running() const noexcept override {
        return get_result == RtcError::kOk;
    }
    RtcError get_temperature_c_x4(int16_t& out) const noexcept override {
        out = 0;
        return RtcError::kOk;
    }
    const char* name() const noexcept override {
        return "mock_rtc";
    }
};

} // namespace

TEST(RtcClock, NoRtcAttachedStaysUninitialized) {
    RtcClock clk;
    clk.attach_rtc(nullptr);
    EXPECT_EQ(RtcError::kNotPresent, clk.begin());
    EXPECT_EQ(ClockSource::kNone, clk.source());
    EXPECT_TRUE(clk.needs_resync());

    BrokenDownTime out{};
    EXPECT_EQ(RtcError::kNotInitialized, clk.now(0, out));
}

TEST(RtcClock, HealthyRtcBecomesTrusted) {
    MockRtcDriver rtc;
    rtc.time = make_time(2026, 10, 6, 8, 30, 15);
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();

    EXPECT_EQ(ClockSource::kRtc, clk.source());
    EXPECT_EQ(ClockTrust::kTrusted, clk.trust());
    EXPECT_FALSE(clk.needs_resync());

    BrokenDownTime out{};
    ASSERT_EQ(RtcError::kOk, clk.now(12345, out));
    EXPECT_EQ(8, out.hour);
    EXPECT_EQ(30, out.minute);
    EXPECT_EQ(2, out.weekday); // 2026-10-06 周二
}

TEST(RtcClock, RtcWithStaleTimeRequiresResync) {
    MockRtcDriver rtc;
    rtc.time = make_time(2020, 1, 1, 0, 0, 0);
    rtc.get_result = RtcError::kTimeNotValid;
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();

    EXPECT_EQ(ClockSource::kRtc, clk.source());
    EXPECT_EQ(ClockTrust::kUntrusted, clk.trust());
    EXPECT_TRUE(clk.needs_resync());

    BrokenDownTime out{};
    EXPECT_EQ(RtcError::kTimeNotValid, clk.now(0, out));
}

TEST(RtcClock, NoSilentDegradeWhenRtcFailsAfterBegin) {
    // 核心不变量：begin() 时 RTC 健康，之后晶振停摆——必须如实上报，
    // 绝不能偷偷退回 tick 让系统「看起来一切正常」直至重启失忆。
    MockRtcDriver rtc;
    rtc.time = make_time(2026, 10, 6, 8, 0, 0);
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();
    ASSERT_EQ(ClockTrust::kTrusted, clk.trust());

    rtc.get_result = RtcError::kTimeNotValid; // 运行中掉电
    BrokenDownTime out{};
    EXPECT_EQ(RtcError::kTimeNotValid, clk.now(0, out));
    EXPECT_EQ(ClockTrust::kUntrusted, clk.trust());
    EXPECT_TRUE(clk.needs_resync());
    EXPECT_EQ(ClockSource::kRtc, clk.source()); // 没有被降级成 kTick
}

TEST(RtcClock, SyncFromExternalWritesBackToRtc) {
    MockRtcDriver rtc;
    rtc.time = make_time(2020, 1, 1, 0, 0, 0);
    rtc.get_result = RtcError::kTimeNotValid;
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();
    ASSERT_TRUE(clk.needs_resync());

    const BrokenDownTime phone = make_time(2026, 10, 6, 21, 15, 30);
    EXPECT_TRUE(clk.sync_from_external(phone, 1000));
    EXPECT_EQ(1, rtc.set_calls);
    EXPECT_EQ(ClockSource::kRtc, clk.source());
    EXPECT_EQ(ClockTrust::kTrusted, clk.trust());
    EXPECT_FALSE(clk.needs_resync());

    BrokenDownTime out{};
    ASSERT_EQ(RtcError::kOk, clk.now(2000, out));
    EXPECT_EQ(21, out.hour);
    EXPECT_EQ(15, out.minute);
}

TEST(RtcClock, SyncRejectsInvalidTimeWithoutChangingState) {
    MockRtcDriver rtc;
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();
    const ClockSource before = clk.source();

    EXPECT_FALSE(clk.sync_from_external(make_time(2026, 2, 30, 0, 0, 0), 0));
    EXPECT_FALSE(clk.sync_from_external(make_time(2026, 10, 6, 25, 0, 0), 0));
    EXPECT_EQ(before, clk.source());
    EXPECT_EQ(0, rtc.set_calls);
}

TEST(RtcClock, FallsBackToTickWhenNoRtc) {
    RtcClock clk;
    clk.attach_rtc(nullptr);
    clk.begin();
    ASSERT_TRUE(clk.sync_from_external(make_time(2026, 10, 6, 7, 0, 0), 0));

    EXPECT_EQ(ClockSource::kTick, clk.source());
    EXPECT_EQ(ClockTrust::kDegraded, clk.trust());

    BrokenDownTime out{};
    ASSERT_EQ(RtcError::kOk, clk.now(90 * 1000u, out)); // 90 秒后
    EXPECT_EQ(7, out.hour);
    EXPECT_EQ(1, out.minute);
    EXPECT_EQ(30, out.second);
}

TEST(RtcClock, TickFallbackCarriesAcrossDayBoundary) {
    RtcClock clk;
    clk.attach_rtc(nullptr);
    clk.begin();
    ASSERT_TRUE(clk.sync_from_external(make_time(2026, 10, 6, 23, 59, 30), 0));

    BrokenDownTime out{};
    ASSERT_EQ(RtcError::kOk, clk.now(60 * 1000u, out)); // 1 分钟后
    EXPECT_EQ(7, out.day);
    EXPECT_EQ(0, out.hour);
    EXPECT_EQ(0, out.minute);
    EXPECT_EQ(30, out.second);
}

TEST(RtcClock, TickFallbackCarriesAcrossMonthBoundary) {
    RtcClock clk;
    clk.attach_rtc(nullptr);
    clk.begin();
    ASSERT_TRUE(clk.sync_from_external(make_time(2026, 1, 31, 23, 0, 0), 0));

    BrokenDownTime out{};
    ASSERT_EQ(RtcError::kOk, clk.now(2u * 3600u * 1000u, out)); // 2 小时后
    EXPECT_EQ(2, out.month);
    EXPECT_EQ(1, out.day);
    EXPECT_EQ(1, out.hour);
}

TEST(RtcClock, TickFallbackSurvivesCounterWrapAround) {
    // uint32 毫秒计数约 49.7 天回绕一次，回绕前后流逝量必须仍然正确
    RtcClock clk;
    clk.attach_rtc(nullptr);
    clk.begin();
    const uint32_t base = 0xFFFFF000u;
    ASSERT_TRUE(clk.sync_from_external(make_time(2026, 10, 6, 12, 0, 0), base));

    BrokenDownTime out{};
    // 跨越 2^32 边界：base + 5000ms 在无符号回绕后仍应算出 5 秒
    ASSERT_EQ(RtcError::kOk, clk.now(base + 5000u, out));
    EXPECT_EQ(12, out.hour);
    EXPECT_EQ(0, out.minute);
    EXPECT_EQ(5, out.second);
}

TEST(RtcClock, SyncFallsBackToTickWhenRtcWriteFails) {
    MockRtcDriver rtc;
    rtc.set_result = RtcError::kBusError;
    RtcClock clk;
    clk.attach_rtc(&rtc);
    clk.begin();

    EXPECT_TRUE(clk.sync_from_external(make_time(2026, 10, 6, 7, 0, 0), 0));
    // 本次会话可用，但降级：重启后会失忆
    EXPECT_EQ(ClockSource::kTick, clk.source());
    EXPECT_EQ(ClockTrust::kDegraded, clk.trust());
    EXPECT_TRUE(clk.needs_resync());
}
