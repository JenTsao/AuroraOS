// =============================================================================
// drivers/rtc/rtc_driver.hpp
//
// 电池供电 RTC（Real-Time Clock）芯片驱动的设备 API 抽象层。
//
// 定位（遵循 AGENTS.md §27 驱动分层）：
//     Application → Device API（本文件）→ Driver interface → HAL → Hardware
// 本文件只定义「一个墙上时钟」需要的能力与错误语义，不含任何具体芯片寄存器
// 知识。DS3231 的 BCD 编码、12/24 小时制、世纪位等细节全部封装在
// drivers/rtc/ds3231_driver.hpp 内部，不向上泄漏。
//
// 为什么先建接口再写实现（AGENTS.md §9）：
// 充电宝/闹钟类终端的 RTC 选型随功耗预算变化——DS3231 待机电流 100µA 级，
// 低功耗场景会换 PCF8563 或超低功耗的 RV-3028。把日历时间换算与合法性校验收
// 进本文件后，后续每种芯片的实现只剩「寄存器映射 + BCD 编解码」，不会再复制
// 一份闰年/天数表。
//
// 设计约束（遵循 AGENTS.md §11/§12/§22）：
//   - 零动态内存分配：全部 constexpr / 固定容量
//   - noexcept：驱动只调用 II2cHal，不分配、不阻塞，不存在异常路径
//   - 显式错误码而非 bool：见下方 RtcError 的语义说明
// =============================================================================

#ifndef AURORA_RTC_RTC_DRIVER_HPP
#define AURORA_RTC_RTC_DRIVER_HPP

#include <stdint.h>

namespace auroraos {
namespace rtc {

// -----------------------------------------------------------------------------
// RtcError
//
// 【为什么不沿用仓库惯用的 bool 返回值】
// bool 会把「总线上没有这个芯片」和「芯片在，但晶振停了、时间是垃圾」压成同一个
// false。对闹钟来说这两种失败的上层处置完全相反：
//   - kNotPresent    → 降级到 tick 计时 / 提示用户接上备用电源，继续走
//   - kTimeNotValid  → 必须丢弃当前时间并从可信源（BLE 手机对时）重新同步
//                      若误走 tick 分支，设备会用开机以来的累计时间当作墙上时间，
//                      表现为「闹钟在开机后第 N 分钟就响」这类难查的故障。
// 因此时间源接口用显式错误码，调用方被迫区分（AGENTS.md §22）。
// -----------------------------------------------------------------------------
enum class RtcError : uint8_t {
    kOk = 0,          // 成功
    kNotInitialized,  // init() 未成功就调用了其他接口
    kNotPresent,      // 探测失败：总线上无应答（地址不对 / 未焊接 / 掉电）
    kBusError,        // I2C 事务失败：HAL 层面报错
    kTimeNotValid,    // 芯片在线但自报时间不可信（OSF/EOS 置位，见各驱动）
    kInvalidArgument, // 调用方传入的日历时间非法（2 月 30 日等）
    kUnsupported,     // 器件在位但不支持该能力（如无温度计的 RTC 读温度）
};

// -----------------------------------------------------------------------------
// BrokenDownTime
//
// 墙上时间的「分解形式」。刻意不携带时区与夏令时：MCU 无可信时区数据库，
// 跨时区语义应由上层（SoftBus / 手机 App）处理，驱动只负责纯粹的日历换算。
// -----------------------------------------------------------------------------
struct BrokenDownTime {
    uint16_t year;   // 完整年份，如 2026（非「年份偏移」）
    uint8_t month;   // 1-12
    uint8_t day;     // 1-31
    uint8_t hour;    // 0-23（24 小时制，驱动内部统一，AM/PM 不外泄）
    uint8_t minute;  // 0-59
    uint8_t second;  // 0-59
    uint8_t weekday; // 1=周一 ... 7=周日（ISO-8601）。由日期计算，不读芯片
};

// -----------------------------------------------------------------------------
// 日历工具（纯函数，无状态，驱动与上层共用）
//
// 放在本文件而非某个具体驱动里，是为了避免每加一种 RTC 就复制一份闰年表
// （AGENTS.md §40 反复制粘贴）。
// -----------------------------------------------------------------------------

// 闰年判定：公历规则，能被 400 整除，或被 4 整除且不被 100 整除。
constexpr bool is_leap_year(uint16_t year) noexcept {
    return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

// 某年某月的天数。month 越界返回 0，由调用方的 validate_broken_down 拦截。
constexpr uint8_t days_in_month(uint16_t year, uint8_t month) noexcept {
    switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        return 31;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    case 2:
        return is_leap_year(year) ? 29 : 28;
    default:
        return 0;
    }
}

// 日历 <-> 连续天数换算（Howard Hinnant 的 days_from_civil / civil_from_days，公开领域）。
//
// 二者互为逆运算，是「加一天」「求两个日期相差几天」「闹钟触发时刻推算」等
// 运算的公共底座。放在本文件而非某个上层模块里，是为了避免每加一处日期推算
// 就复制一份（AGENTS.md §40）。
//
// 约定：day 0 = 1970-01-01（Unix 纪元）。int32_t 可覆盖 ±5.8 百万年，
// 对本项目的 2000-2199 年份区间绰绰有余。
constexpr int32_t days_from_civil(int32_t y, uint8_t m, uint8_t d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);               // [0, 399]
    const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u; // [0, 365]
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;           // [0, 146096]
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// days_from_civil 的逆运算。z 越界到公历范围之外时结果无意义，调用方需自行
// 约束（本项目所有调用点都由 validate_broken_down 先行把关）。
constexpr void civil_from_days(int32_t z, int32_t& y, uint8_t& m, uint8_t& d) noexcept {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);                   // [0, 146096]
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; // [0, 399]
    const int32_t yy = static_cast<int32_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); // [0, 365]
    const uint32_t mp = (5u * doy + 2u) / 153u;                      // [0, 11]
    d = static_cast<uint8_t>(doy - (153u * mp + 2u) / 5u + 1u);
    // mp<10 对应 3..12 月，mp>=10 对应 1..2 月（减 9 后进位到下一年，由下面 +1 修正）。
    // 这里用 int 运算而非无符号回绕，避免读代码时需要额外推导。
    const int32_t mm = static_cast<int32_t>(mp) + ((mp < 10u) ? 3 : -9);
    m = static_cast<uint8_t>(mm);
    y = yy + ((mm <= 2) ? 1 : 0);
}

// 星期计算。
//
// 不读芯片的星期寄存器：DS3231 的 Day 寄存器字段是用户可写位，芯片自身
// 不维护，写错也不会被纠正。从日期算出来才是唯一可信来源。
//
// 返回 ISO-8601 星期：1=周一 ... 7=周日。
constexpr uint8_t weekday_from_civil(uint16_t y, uint8_t m, uint8_t d) noexcept {
    const int32_t days = days_from_civil(static_cast<int32_t>(y), m, d);
    // day 0 = 1970-01-01 = 周四，故 +3 偏移后得到 0=周一..6=周日，再 +1 转 ISO-8601。
    // 若用 +4 得到的是 0=周日 基准下的编号，再加 1 会把周四算成 5，整体差一天。
    int32_t dow = (days + 3) % 7;
    if (dow < 0) { // C++ 余数向零截断，负数天数需归一
        dow += 7;
    }
    return static_cast<uint8_t>(dow + 1);
}

// 日历时间合法性校验。驱动在写芯片之前必须过这一关：
// DS3231 会对非法输入静默接受并产生不可预测的进位（例如 2 月 30 日会写成
// 3 月 2 日），这种错误一旦落盘到闹钟配置就极难排查。
constexpr bool validate_broken_down(const BrokenDownTime& t) noexcept {
    if (t.year < 2000 || t.year > 2199) {
        return false;
    }
    if (t.month < 1 || t.month > 12) {
        return false;
    }
    if (t.day < 1 || t.day > days_in_month(t.year, t.month)) {
        return false;
    }
    if (t.hour > 23 || t.minute > 59 || t.second > 59) {
        return false;
    }
    return true;
}

// 1/4°C 定点温度转 10°C 定点（-400 表示 -40.0°C）。
// 单独提供而不是让调用方直接做 raw*10/4：负温度下整数除法向零截断会算错
// （-1/4°C 会变成 0 而非 -3），这里统一按四舍五入处理。
constexpr int16_t c_x4_to_tenths(int16_t c_x4) noexcept {
    if (c_x4 >= 0) {
        return static_cast<int16_t>((c_x4 * 10 + 2) / 4);
    }
    return static_cast<int16_t>(-((-c_x4 * 10 + 2) / 4));
}

// -----------------------------------------------------------------------------
// IRtcDriver
//
// 电池供电 RTC 芯片的驱动接口。实现者负责：寄存器映射、BCD 编解码、晶振健康
// 标志上报。调用方只需知道「现在几点」和「这个时间可不可信」。
// -----------------------------------------------------------------------------
class IRtcDriver {
public:
    virtual ~IRtcDriver() = default;

    // 探测器件并完成最小初始化。失败返回 kNotPresent 或 kBusError。
    virtual RtcError init() noexcept = 0;

    // init() 是否已成功。未初始化时调用其他接口返回 kNotInitialized。
    virtual bool initialized() const noexcept = 0;

    // 读取当前墙上时间。
    //
    // 关键语义：晶振停振（掉电 / 备用电池耗尽）时，芯片寄存器里仍是上次写入的
    // 旧值，看上去「有值」实则早已过期。此时实现必须返回 kTimeNotValid 而
    // 不是把陈旧值交给上层——这正是断电场景下充电宝闹钟最危险的失效模式。
    virtual RtcError get_time(BrokenDownTime& out) const noexcept = 0;

    // 写入墙上时间。传入非法日期返回 kInvalidArgument，不产生 I2C 事务。
    // 写入成功后实现应清除晶振停振标志。
    virtual RtcError set_time(const BrokenDownTime& t) noexcept = 0;

    // 读取晶振健康状态。true 表示计时可信。
    // 该接口不消耗时间读数开销，可用于「低电告警时顺带查一次」这类低频场景。
    virtual bool oscillator_running() const noexcept = 0;

    // 读取芯片温度（1/4°C 定点）。无温度计的器件返回 kUnsupported。
    virtual RtcError get_temperature_c_x4(int16_t& out) const noexcept = 0;

    // 器件型号标识，用于日志与 /proc 展示。
    virtual const char* name() const noexcept = 0;
};

} // namespace rtc
} // namespace auroraos

#endif // AURORA_RTC_RTC_DRIVER_HPP
