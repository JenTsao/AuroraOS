// =============================================================================
// tests/unit/test_ds3231_driver.cpp
//
// Maxim DS3231 I2C RTC 主机接口驱动测试：
//   - 日历工具：闰年、月天数、ISO 星期、日期合法性、1/4°C 定点换算
//   - 探测：总线错误 / 器件无应答 / 未注入 HAL 三种失败语义
//   - 读时间：往返一致性、突发读原子性、12 小时制陷阱、世纪位
//   - 安全：EOS/OSF 停振、BCD 脏半字节、不存在日期必须拒绝上报而非编造
//   - 写时间：非法日期拦截、24h 模式落盘、不触碰闹钟区、停振标志清除
//   - 温度：10 位二进制补码符号扩展，负温不因整数除法算错
//
// Mock 采用「真实寄存器文件」模型而非固定返回值：驱动跑的是完整 BCD 编解码
// 路径，位域污染、停振标志等失效模式都能被真实注入。寄存器地址与位域严格对齐
// Maxim DS3231 datasheet Figure 1 / Table 1。
// =============================================================================
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../../drivers/rtc/ds3231_driver.hpp"

using auroraos::rtc::BrokenDownTime;
using auroraos::rtc::RtcError;
using auroraos::rtc::c_x4_to_tenths;
using auroraos::rtc::days_in_month;
using auroraos::rtc::is_leap_year;
using auroraos::rtc::validate_broken_down;
using auroraos::rtc::weekday_from_civil;

using Ds3231 = auroraos::rtc::ds3231::Ds3231Driver;
namespace ds3231 = auroraos::rtc::ds3231;

namespace {

// 真实 DS3231 寄存器空间 00h..12h。
constexpr size_t kRegCount = 19;

// =============================================================================
// MockDs3231I2cHal：19 字节寄存器文件 + 事务日志
// =============================================================================
class MockDs3231I2cHal : public auroraos::hal::II2cHal {
public:
    uint8_t regs[kRegCount];
    bool fail_all = false;          // 模拟总线上无应答（NACK）
    bool fail_read = false;         // 模拟读事务失败
    bool fail_write = false;        // 模拟写事务失败
    std::vector<size_t> read_lens;  // 每次 read_reg 的读取长度（断言突发原子性）
    std::vector<uint8_t> read_regs; // 每次 read_reg 的起始寄存器

    MockDs3231I2cHal() { reset(); }

    void reset() {
        memset(regs, 0, sizeof(regs));
        fail_all = false;
        fail_read = false;
        fail_write = false;
        read_lens.clear();
        read_regs.clear();
        // 上电默认：OSF=0、EN32KHZ=1（bit3=1）即 0x08，表示晶振健康在跑。
        // 注意 datasheet 的 POR 值是 0x88..0x8F（bit7 OSF=1，首次上电待同步），
        // 稳态健康值应为 0x08，否则 get_time 会正确地报 kTimeNotValid。
        regs[0x0F] = 0x08;
        set_temperature_c_x4(100);
    }

    // 按芯片实际布局写入一个合法时间（24 小时制）
    void seed_time(const BrokenDownTime& t) {
        regs[0x00] = ds3231::bin_to_bcd(t.second);
        regs[0x01] = ds3231::bin_to_bcd(t.minute);
        regs[0x02] = static_cast<uint8_t>(ds3231::kHoursMode24Mask | ds3231::bin_to_bcd(t.hour));
        regs[0x03] = 0; // 星期，芯片不维护
        regs[0x04] = ds3231::bin_to_bcd(t.day);
        const uint16_t off = static_cast<uint16_t>(t.year - 2000);
        regs[0x05] = static_cast<uint8_t>((off >= 100 ? ds3231::kMonthCenturyMask : 0x00) |
                                          ds3231::bin_to_bcd(t.month));
        regs[0x06] = ds3231::bin_to_bcd(static_cast<uint8_t>(off % 100));
    }

    // 10 位二进制补码温度：11h 提供 V9..V2，12h bit7-6 提供 V1..V0
    void set_temperature_c_x4(int16_t c_x4) {
        const uint16_t raw = static_cast<uint16_t>(c_x4 < 0 ? (c_x4 + 1024) : c_x4);
        regs[0x11] = static_cast<uint8_t>((raw >> 2) & 0xFFu);
        regs[0x12] = static_cast<uint8_t>((raw & 0x03u) << 6);
    }

    void set_eos() { regs[0x00] |= ds3231::kSecondsEosMask; }
    void set_osf() { regs[0x0F] |= ds3231::kStatusOsfMask; }

    bool write(uint8_t, const uint8_t*, size_t len) override {
        if (fail_all || fail_write) {
            return false;
        }
        return len <= kRegCount;
    }

    bool write_reg(uint8_t, uint8_t reg, const uint8_t* data, size_t len) override {
        if (fail_all || fail_write) {
            return false;
        }
        if (static_cast<size_t>(reg) + len > kRegCount || data == nullptr) {
            return false;
        }
        memcpy(regs + reg, data, len);
        return true;
    }

    bool read(uint8_t, uint8_t*, size_t len) override {
        if (fail_all || fail_read) {
            return false;
        }
        return len <= kRegCount;
    }

    bool read_reg(uint8_t, uint8_t reg, uint8_t* data, size_t len) override {
        read_regs.push_back(reg);
        read_lens.push_back(len);
        if (fail_all || fail_read) {
            return false;
        }
        if (static_cast<size_t>(reg) + len > kRegCount || data == nullptr) {
            return false;
        }
        memcpy(data, regs + reg, len);
        return true;
    }
};

BrokenDownTime make_time(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    BrokenDownTime t{};
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    t.second = s;
    t.weekday = weekday_from_civil(y, mo, d);
    return t;
}

} // namespace

// =============================================================================
// 日历工具
// =============================================================================
TEST(RtcCalendar, LeapYearRules) {
    EXPECT_TRUE(is_leap_year(2000));  // 能被 400 整除
    EXPECT_TRUE(is_leap_year(2024));
    EXPECT_FALSE(is_leap_year(1900)); // 能被 100 整除但不能被 400 整除
    EXPECT_FALSE(is_leap_year(2026));
    EXPECT_FALSE(is_leap_year(2100));
}

TEST(RtcCalendar, DaysInMonthHandlesLeapFebruary) {
    EXPECT_EQ(31, days_in_month(2026, 1));
    EXPECT_EQ(30, days_in_month(2026, 4));
    EXPECT_EQ(28, days_in_month(2026, 2));
    EXPECT_EQ(29, days_in_month(2024, 2)); // 闰年
    EXPECT_EQ(29, days_in_month(2000, 2));
    EXPECT_EQ(28, days_in_month(1900, 2));
    EXPECT_EQ(0, days_in_month(2026, 0)); // 越界
    EXPECT_EQ(0, days_in_month(2026, 13));
}

TEST(RtcCalendar, WeekdayMatchesKnownAnchors) {
    // 1970-01-01 = 周四
    EXPECT_EQ(4, weekday_from_civil(1970, 1, 1));
    // 2000-02-29 = 周二
    EXPECT_EQ(2, weekday_from_civil(2000, 2, 29));
    // 2024-01-01 = 周一
    EXPECT_EQ(1, weekday_from_civil(2024, 1, 1));
    // 2026-01-01 = 周四
    EXPECT_EQ(4, weekday_from_civil(2026, 1, 1));
    // 2026-10-06 = 周二
    EXPECT_EQ(2, weekday_from_civil(2026, 10, 6));
    // 跨年连续性：2026-12-31 的次日 2027-01-01 必须前进一步
    const uint8_t a = weekday_from_civil(2026, 12, 31);
    const uint8_t b = weekday_from_civil(2027, 1, 1);
    EXPECT_EQ((a % 7) + 1, b);
    // 闰日连续性：2024-02-29 是周四，次日 2024-03-01 是周五
    EXPECT_EQ(4, weekday_from_civil(2024, 2, 29));
    EXPECT_EQ(5, weekday_from_civil(2024, 3, 1));
}

TEST(RtcCalendar, ValidateRejectsImpossibleDates) {
    EXPECT_TRUE(validate_broken_down(make_time(2026, 10, 6, 23, 59, 59)));
    EXPECT_TRUE(validate_broken_down(make_time(2024, 2, 29, 0, 0, 0))); // 闰日合法

    EXPECT_FALSE(validate_broken_down(make_time(2026, 2, 30, 0, 0, 0))); // 平年 2 月无 30 日
    EXPECT_FALSE(validate_broken_down(make_time(2026, 4, 31, 0, 0, 0))); // 4 月无 31 日
    EXPECT_FALSE(validate_broken_down(make_time(2026, 13, 1, 0, 0, 0))); // 月越界
    EXPECT_FALSE(validate_broken_down(make_time(2026, 0, 1, 0, 0, 0)));  // 月越界
    EXPECT_FALSE(validate_broken_down(make_time(2026, 10, 0, 0, 0, 0))); // 日越界
    EXPECT_FALSE(validate_broken_down(make_time(2026, 10, 32, 0, 0, 0)));
    EXPECT_FALSE(validate_broken_down(make_time(2026, 10, 6, 24, 0, 0))); // 时越界
    EXPECT_FALSE(validate_broken_down(make_time(2026, 10, 6, 0, 60, 0))); // 分越界
    EXPECT_FALSE(validate_broken_down(make_time(2026, 10, 6, 0, 0, 60))); // 秒越界
    EXPECT_FALSE(validate_broken_down(make_time(1999, 1, 1, 0, 0, 0)));   // 年份下界
    EXPECT_FALSE(validate_broken_down(make_time(2200, 1, 1, 0, 0, 0)));   // 年份上界
}

TEST(RtcCalendar, QuarterDegreeToTenthsRoundsNegativeCorrectly) {
    EXPECT_EQ(250, c_x4_to_tenths(100));   // 25.0°C
    EXPECT_EQ(0, c_x4_to_tenths(0));
    EXPECT_EQ(850, c_x4_to_tenths(340));    // 85.0°C
    EXPECT_EQ(-400, c_x4_to_tenths(-160)); // -40.0°C
    // 负温度下整数除法若向零截断会把 -0.25°C 算成 0 而非 -3
    EXPECT_EQ(-3, c_x4_to_tenths(-1));
    EXPECT_EQ(-8, c_x4_to_tenths(-3));
    EXPECT_EQ(3, c_x4_to_tenths(1));
}

TEST(RtcCalendar, BcdRoundTrip) {
    for (uint8_t v = 0; v < 100; ++v) {
        EXPECT_EQ(v, ds3231::bcd_to_bin(ds3231::bin_to_bcd(v)));
    }
}

TEST(RtcCalendar, BcdRejectsDirtyHalfNibbles) {
    // 半字节取值 > 9 说明位域被污染，不能当作合法 BCD 参与乘法
    EXPECT_FALSE(ds3231::is_valid_bcd(0x1A));
    EXPECT_FALSE(ds3231::is_valid_bcd(0xA1));
    EXPECT_FALSE(ds3231::is_valid_bcd(0xFF));
    EXPECT_TRUE(ds3231::is_valid_bcd(0x59));
}

// =============================================================================
// 探测
// =============================================================================
TEST(Ds3231Probe, SucceedsWhenDevicePresent) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    EXPECT_FALSE(drv.initialized());
    EXPECT_EQ(RtcError::kOk, drv.init());
    EXPECT_TRUE(drv.initialized());
    EXPECT_STREQ("ds3231", drv.name());
}

TEST(Ds3231Probe, ReportsBusErrorWhenNack) {
    MockDs3231I2cHal mock;
    mock.fail_all = true; // 总线无应答
    Ds3231 drv;
    drv.configure(&mock);
    EXPECT_EQ(RtcError::kBusError, drv.init());
    EXPECT_FALSE(drv.initialized());
}

TEST(Ds3231Probe, ReportsNotPresentWhenStatusRegisterReadsAllZeroOrAllOnes) {
    // 上拉缺失 / 地址悬空时 I2C 会读回全 0x00 或全 0xFF，不能当成有效器件
    for (uint8_t poison : {0x00, 0xFF}) {
        MockDs3231I2cHal mock;
        mock.regs[0x0F] = poison;
        Ds3231 drv;
        drv.configure(&mock);
        EXPECT_EQ(RtcError::kNotPresent, drv.init());
        EXPECT_FALSE(drv.initialized());
    }
}

TEST(Ds3231Probe, FailsClosedWithoutInjectedHal) {
    Ds3231 drv; // 未调用 configure()
    EXPECT_EQ(RtcError::kNotPresent, drv.init());
    EXPECT_FALSE(drv.initialized());
}

TEST(Ds3231Probe, AllQueriesRejectedBeforeInit) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    // 未 init 就使用必须 fail-closed，而不是用陈旧状态标志读总线
    BrokenDownTime t{};
    EXPECT_EQ(RtcError::kNotInitialized, drv.get_time(t));
    EXPECT_EQ(RtcError::kNotInitialized, drv.set_time(t));
    int16_t temp = 0;
    EXPECT_EQ(RtcError::kNotInitialized, drv.get_temperature_c_x4(temp));
    EXPECT_FALSE(drv.oscillator_running());
}

// =============================================================================
// 读写时间往返
// =============================================================================
TEST(Ds3231Time, RoundTripsRepresentativeTimes) {
    const BrokenDownTime cases[] = {
        make_time(2026, 10, 6, 0, 0, 0),     // 零点
        make_time(2026, 10, 6, 23, 59, 59),  // 一天最后一秒
        make_time(2024, 2, 29, 12, 0, 0),    // 闰日
        make_time(2000, 1, 1, 0, 30, 0),     // 基准年起点
        make_time(2099, 12, 31, 18, 45, 30), // 年份寄存器上界
        make_time(2100, 1, 1, 6, 15, 0),     // 跨世纪位
    };

    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    for (const auto& expected : cases) {
        SCOPED_TRACE(expected.year);
        ASSERT_EQ(RtcError::kOk, drv.set_time(expected));
        BrokenDownTime got{};
        ASSERT_EQ(RtcError::kOk, drv.get_time(got));
        EXPECT_EQ(expected.year, got.year);
        EXPECT_EQ(expected.month, got.month);
        EXPECT_EQ(expected.day, got.day);
        EXPECT_EQ(expected.hour, got.hour);
        EXPECT_EQ(expected.minute, got.minute);
        EXPECT_EQ(expected.second, got.second);
        EXPECT_EQ(expected.weekday, got.weekday);
    }
}

TEST(Ds3231Time, ReadsTimeRegistersInSingleBurst) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 8, 30, 0));
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.read_lens.clear();
    mock.read_regs.clear();
    BrokenDownTime got{};
    ASSERT_EQ(RtcError::kOk, drv.get_time(got));

    // 00h..06h 必须整块读：分次读会在事务之间跨过秒进位点，
    // 拼出 23:59:60 这类不存在的时刻。
    ASSERT_GE(mock.read_lens.size(), 1U);
    EXPECT_EQ(ds3231::kTimeRegLen, mock.read_lens[0]);
    EXPECT_EQ(ds3231::kRegSeconds, mock.read_regs[0]);
}

TEST(Ds3231Time, DecodesTwelveHourModeWithoutAmPmOffByOne) {
    // 12h 位域：[7]=12/24(0=12h) [6]=PM [5:4]=十位 [3:0]=个位
    // 经典陷阱：12 AM 是 0 点，12 PM 是 12 点。若对 PM 直接加 12，
    // 12 PM 会被算成 24（越界）而 12 AM 算成 0 恰好蒙对，错误只在 12 PM 暴露。
    struct Case {
        uint8_t reg;
        uint8_t expected_hour;
        const char* what;
    };
    const Case cases[] = {
        {0x12, 0, "12 AM -> 0"},  {0x52, 12, "12 PM -> 12"}, {0x01, 1, "1 AM -> 1"},
        {0x41, 13, "1 PM -> 13"}, {0x03, 3, "3 AM -> 3"},   {0x43, 15, "3 PM -> 15"},
        {0x09, 9, "9 AM -> 9"},   {0x49, 21, "9 PM -> 21"}, {0x11, 11, "11 AM -> 11"},
        {0x51, 23, "11 PM -> 23"},
    };

    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        mock.reset();
        mock.seed_time(make_time(2026, 10, 6, 0, 0, 0));
        mock.regs[0x02] = c.reg; // 覆盖为 12 小时制原始值
        BrokenDownTime got{};
        ASSERT_EQ(RtcError::kOk, drv.get_time(got));
        EXPECT_EQ(c.expected_hour, got.hour);
    }
}

TEST(Ds3231Time, IgnoresPmMaskBitInTwentyFourHourMode) {
    // 24h 模式下 bit6 是掩码位，置 1 不应把 13 点读成 25 点
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    mock.seed_time(make_time(2026, 10, 6, 13, 0, 0));
    mock.regs[0x02] |= ds3231::kHoursPmMask;

    BrokenDownTime got{};
    ASSERT_EQ(RtcError::kOk, drv.get_time(got));
    EXPECT_EQ(13, got.hour);
}

TEST(Ds3231Time, IgnoresStaleDayOfWeekRegister) {
    // 03h 星期字段芯片不维护且用户可写，写错也不会被纠正——必须由日期推算
    MockDs3231I2cHal mock;
    const BrokenDownTime t = make_time(2026, 10, 6, 10, 0, 0);
    mock.seed_time(t);
    mock.regs[0x03] = 0x77; // 故意写入非法星期
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    BrokenDownTime got{};
    ASSERT_EQ(RtcError::kOk, drv.get_time(got));
    EXPECT_EQ(t.weekday, got.weekday);
}

// =============================================================================
// 失效安全：停振 / 数据损坏必须拒绝上报，不得编造时间
// =============================================================================
TEST(Ds3231Safety, RejectsStaleTimeWhenOscillatorStopped) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    mock.set_eos();
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    BrokenDownTime got{};
    // 寄存器里仍有看似合法的时间，但晶振已停 —— 断电场景下必须让上层知道
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));
}

TEST(Ds3231Safety, RejectsTimeWhenOsfSet) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    mock.set_osf();
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));
    EXPECT_FALSE(drv.oscillator_running());
}

TEST(Ds3231Safety, AcceptsTimeAfterOscillatorFlagsCleared) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    mock.set_osf();
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    EXPECT_FALSE(drv.oscillator_running());

    // 对时是清除停振标志的时机，此后时间重新可信
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 9, 15, 0)));
    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kOk, drv.get_time(got));
    EXPECT_EQ(9, got.hour);
}

TEST(Ds3231Safety, RejectsCorruptBcdHalfNibble) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    // 分钟字段低半字节被污染成 0xA（非法 BCD）。
    // 注意不能写 0xA5：0xA5 & 0x7F = 0x25 是合法的「25 分钟」，掩码会把脏位吃掉。
    mock.reset();
    mock.seed_time(make_time(2026, 10, 6, 7, 30, 0));
    mock.regs[0x01] = 0x5A;
    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));

    // 小时字段脏半字节（24h 模式）
    mock.reset();
    mock.seed_time(make_time(2026, 10, 6, 7, 30, 0));
    mock.regs[0x02] = 0x9B;
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));

    // 日字段脏半字节（04h）
    mock.reset();
    mock.seed_time(make_time(2026, 10, 6, 7, 30, 0));
    mock.regs[0x04] = 0x1B;
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));
}

TEST(Ds3231Safety, RejectsNonexistentCalendarDatesStoredOnChip) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    // 平年 2 月 30 日：BCD 全部合法，但日历上不存在，必须在完整校验处拦下
    mock.reset();
    mock.regs[0x00] = ds3231::bin_to_bcd(0);
    mock.regs[0x01] = ds3231::bin_to_bcd(0);
    mock.regs[0x02] = ds3231::kHoursMode24Mask;
    mock.regs[0x04] = ds3231::bin_to_bcd(30); // day  = 30
    mock.regs[0x05] = ds3231::bin_to_bcd(2);  // month = 2
    mock.regs[0x06] = ds3231::bin_to_bcd(26); // 2026 非闰年

    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));
}

TEST(Ds3231Safety, RejectsMonthZero) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    mock.reset();
    mock.regs[0x02] = ds3231::kHoursMode24Mask;
    mock.regs[0x04] = ds3231::bin_to_bcd(1);
    mock.regs[0x05] = 0x00; // month = 0
    mock.regs[0x06] = ds3231::bin_to_bcd(26);

    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kTimeNotValid, drv.get_time(got));
}

TEST(Ds3231Safety, PropagatesBusErrorFromRead) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.fail_read = true;
    BrokenDownTime got{};
    EXPECT_EQ(RtcError::kBusError, drv.get_time(got));
    EXPECT_FALSE(drv.oscillator_running());
}

TEST(Ds3231Safety, PropagatesBusErrorFromWrite) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    mock.fail_write = true;
    EXPECT_EQ(RtcError::kBusError, drv.set_time(make_time(2026, 10, 6, 7, 0, 0)));
}

// =============================================================================
// 写时间
// =============================================================================
TEST(Ds3231Write, RejectsInvalidDateWithoutTouchingBus) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    uint8_t before[kRegCount] = {0};
    memcpy(before, mock.regs, kRegCount);
    mock.read_lens.clear();

    EXPECT_EQ(RtcError::kInvalidArgument, drv.set_time(make_time(2026, 2, 30, 0, 0, 0)));
    EXPECT_EQ(RtcError::kInvalidArgument, drv.set_time(make_time(2026, 13, 1, 0, 0, 0)));
    EXPECT_EQ(RtcError::kInvalidArgument, drv.set_time(make_time(2026, 1, 1, 25, 0, 0)));

    // 非法输入不得产生任何 I2C 事务：DS3231 会静默接受 2 月 30 日并进位
    EXPECT_TRUE(mock.read_lens.empty());
    EXPECT_EQ(0, memcmp(before, mock.regs, kRegCount));
}

TEST(Ds3231Write, WritesTwentyFourHourModeAlways) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 13, 45, 30)));

    EXPECT_NE(0, mock.regs[0x02] & ds3231::kHoursMode24Mask);
    EXPECT_EQ(0, mock.regs[0x02] & ds3231::kHoursPmMask);
    EXPECT_EQ(13, ds3231::bcd_to_bin(mock.regs[0x02] & 0x3F));
    EXPECT_EQ(45, ds3231::bcd_to_bin(mock.regs[0x01]));
    EXPECT_EQ(30, ds3231::bcd_to_bin(mock.regs[0x00]));
    EXPECT_EQ(6, ds3231::bcd_to_bin(mock.regs[0x04]));  // 日
    EXPECT_EQ(10, ds3231::bcd_to_bin(mock.regs[0x05] & 0x1F)); // 月
    EXPECT_EQ(26, ds3231::bcd_to_bin(mock.regs[0x06]));         // 年
}

TEST(Ds3231Write, DoesNotTouchAlarmRegisters) {
    // 07h..0Dh 是 Alarm1/Alarm2 区，set_time 只写 00h..06h，
    // 不得破坏外部已配置的硬件闹钟
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    for (uint8_t r = 0x07; r <= 0x0D; ++r) {
        mock.regs[r] = static_cast<uint8_t>(0xA0 + r);
    }
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 13, 45, 30)));
    for (uint8_t r = 0x07; r <= 0x0D; ++r) {
        EXPECT_EQ(static_cast<uint8_t>(0xA0 + r), mock.regs[r]);
    }
}

TEST(Ds3231Write, SetsCenturyBitAcrossTwoHundredYears) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2099, 6, 15, 8, 0, 0)));
    EXPECT_EQ(0, mock.regs[0x05] & ds3231::kMonthCenturyMask);
    EXPECT_EQ(99, ds3231::bcd_to_bin(mock.regs[0x06]));

    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2100, 6, 15, 8, 0, 0)));
    EXPECT_NE(0, mock.regs[0x05] & ds3231::kMonthCenturyMask);
    EXPECT_EQ(0, ds3231::bcd_to_bin(mock.regs[0x06]));
}

TEST(Ds3231Write, ClearsEosOsfAndEnablesOscillatorOnBatteryBackup) {
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 7, 0, 0));
    mock.set_eos();
    mock.set_osf();
    mock.regs[0x0E] = 0x80; // EOSC=1：切到 VBAT 时停振

    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 9, 0, 0)));

    EXPECT_EQ(0, mock.regs[0x00] & ds3231::kSecondsEosMask);
    EXPECT_EQ(0, mock.regs[0x0F] & ds3231::kStatusOsfMask);
    // EOSC 必须清零：否则拔掉充电线后 RTC 停振，闹钟会在下次上电时失效
    EXPECT_EQ(0, mock.regs[0x0E] & ds3231::kControlEoscMask);
}

TEST(Ds3231Write, PreservesStatusRegisterLowBitsWhenClearingOsf) {
    // 清 OSF 是读-改-写：EN32KHZ/BSY/A2F/A1F 必须保留，不能连带清掉
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.set_osf();
    mock.regs[0x0F] = static_cast<uint8_t>(mock.regs[0x0F] | 0x0B); // EN32KHZ|A2F|A1F
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 9, 0, 0)));
    EXPECT_EQ(0x0B, mock.regs[0x0F]);
}

TEST(Ds3231Write, PreservesControlRegisterConfiguration) {
    // 读-改-写不得清掉 INTCN / A1IE / CONV 等已配置的功能位
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.regs[0x0E] = 0x85; // EOSC | INTCN | A2IE | A1IE
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 9, 0, 0)));
    EXPECT_EQ(0x05, mock.regs[0x0E]);
}

TEST(Ds3231Write, DaysOfWeekBitsLeftZero) {
    // 03h 星期字段芯片不维护且驱动不使用，不应写入假星期
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    ASSERT_EQ(RtcError::kOk, drv.set_time(make_time(2026, 10, 6, 9, 0, 0)));
    EXPECT_EQ(0, mock.regs[0x03]);
}

// =============================================================================
// 温度
// =============================================================================
TEST(Ds3231Temperature, DecodesSignedTenBitValue) {
    struct Case {
        int16_t c_x4;
        const char* what;
    };
    const Case cases[] = {
        {0, "0°C"},        {100, "25.0°C"},  {101, "+25.25°C (datasheet 示例)"},
        {340, "85.0°C 上界"}, {-1, "-0.25°C"}, {-41, "-10.25°C"},
        {-160, "-40.0°C 下界"},
    };

    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        mock.reset();
        mock.set_temperature_c_x4(c.c_x4);
        int16_t got = 12345;
        ASSERT_EQ(RtcError::kOk, drv.get_temperature_c_x4(got));
        EXPECT_EQ(c.c_x4, got);
    }
}

TEST(Ds3231Temperature, MatchesDatasheetEncodingExample) {
    // datasheet: "00011001 01b = +25.25°C"
    // 11h = 0b00011001 = 0x19, 12h bit7-6 = 0b01
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.regs[0x11] = 0x19;
    mock.regs[0x12] = 0x40;
    int16_t got = 0;
    ASSERT_EQ(RtcError::kOk, drv.get_temperature_c_x4(got));
    EXPECT_EQ(101, got);          // 101 * 0.25 = 25.25°C
    EXPECT_EQ(253, c_x4_to_tenths(got)); // 25.3°C
}

TEST(Ds3231Temperature, DecodesWhenDontCareBitsAreSetOnNegativeReading) {
    // 回归：12h 的 bit5-0 是 datasheet 标注的 don't care，写 1 属正常。
    // 11h=0xFF / 12h=0xFF 解出的 10 位码是 0x3FF = -0.25°C，是合法读数。
    // 早期实现用 "两寄存器都读到 0xFF 即判定器件不在" 做防悬空检查，会把
    // 这个真实温度误报成 kNotPresent——此处钉死该行为。
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    mock.regs[0x11] = 0xFF;
    mock.regs[0x12] = 0xFF;
    int16_t got = 0;
    ASSERT_EQ(RtcError::kOk, drv.get_temperature_c_x4(got));
    EXPECT_EQ(-1, got);
    EXPECT_EQ(-3, c_x4_to_tenths(got)); // -0.3°C
}

TEST(Ds3231Temperature, PropagatesBusError) {
    MockDs3231I2cHal mock;
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());
    mock.fail_read = true;
    int16_t got = 0;
    EXPECT_EQ(RtcError::kBusError, drv.get_temperature_c_x4(got));
}

TEST(Ds3231Temperature, DoesNotDisturbTimeRegisters) {
    // 读温度只能碰 11h/12h，不得污染时间寄存器
    MockDs3231I2cHal mock;
    mock.seed_time(make_time(2026, 10, 6, 11, 22, 33));
    Ds3231 drv;
    drv.configure(&mock);
    ASSERT_EQ(RtcError::kOk, drv.init());

    uint8_t before[ds3231::kTimeRegLen] = {0};
    memcpy(before, mock.regs, ds3231::kTimeRegLen);
    int16_t temp = 0;
    ASSERT_EQ(RtcError::kOk, drv.get_temperature_c_x4(temp));
    EXPECT_EQ(0, memcmp(before, mock.regs, ds3231::kTimeRegLen));
}
