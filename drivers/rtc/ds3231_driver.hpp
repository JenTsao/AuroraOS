// =============================================================================
// drivers/rtc/ds3231_driver.hpp
//
// Maxim DS3231SN / DS3231M 温度补偿 RTC 主机接口驱动（I2C）。
//
// 选型理由（针对充电宝闹钟终端）：
//   - 温补晶振，±2ppm（−40~85°C），常温下约 ±1 分钟/年，闹钟场景完全够用
//   - 待机电流 100µA 级，CR2032 纽扣电池可维持数月的纯 RTC 供电
//   - 内建温度计，可直接读出 RTC die 温度——判断「芯片是否还健康」的直接证据
//   - I2C 接口，与 AuroraOS 现有 II2cHal 天然契合，无需新增 HAL
//
// 寄存器映射来源：Maxim DS3231/DS3231M datasheet, Figure 1 (Address Map) 与
// Table 1 (Timekeeping Registers)。本文件的地址与位域已逐条对照 datasheet
// 核实，修改前请重新核对，不要凭记忆改动。
//
//   地址空间 0x00..0x12（19 字节），多字节访问越过 0x12 后地址指针回卷到 0x00。
//
//   00h Seconds    bit7 EOS  | bit6-4 秒/2  | bit3-0 秒
//   01h Minutes    bit6-0 分钟
//   02h Hours      bit7 12/24(1=24h) | bit6 PM(仅12h) | bit5-4 十位 | bit3-0 个位
//   03h Day/Date   bit6 DY/DT | bit5 0 | bit4-0 星期(芯片不维护，用户可写)
//   04h Date       bit4-0 日 (1-31)
//   05h Date/Month bit6 Century | bit5 0 | bit4-0 月 (1-12)
//   06h Year       bit7-0 年份偏移 (相对 2000)
//   07h-0Ah Alarm1   0Bh-0Dh Alarm2（本驱动不建模，见下方已知限制）
//   0Eh Control    bit7 EOSC | bit6 BBSQW | bit5 CONV | bit4-3 RS2/RS1
//                  | bit2 INTCN | bit1 A2IE | bit0 A1IE
//   0Fh Status     bit7 OSF  | bit6-4 0 | bit3 EN32KHZ | bit2 BSY | bit1 A2F | bit0 A1F
//   10h Aging Offset
//   11h-12h Temperature  10 位二进制补码，0.25°C/LSB
//        11h: bit7 Sign | bit6-0 Data(V8..V2)
//        12h: bit7-6 Data(V1..V0) | bit5-0 0
//        datasheet 示例：00011001 01b = +25.25°C
//
// 已知限制（fail-safe 设计前提）：
//   1. 不实现 DS3231 硬件闹钟（07h-0Dh Alarm1/2）。闹钟调度统一走内核
//      ProcessTimerManager；硬件闹钟会与之形成两套并存的触发源，调度归属应由
//      上层架构决策，不在本驱动内隐式引入。
//   2. set_time() 把时间寄存器统一写成 24 小时制（02h bit7=1），故本设备不再有
//      AM/PM 歧义；读取路径仍完整处理 12 小时制，以兼容出厂即配置为 12h 或被
//      他人改过的芯片。
//   3. 时间寄存器写入为 07h/0Bh 起的闹钟区留出空间：只写 00h-06h 七个字节，
//      不触碰任何闹钟寄存器，因此 set_time() 对已有闹钟配置无破坏性。
//   4. 电池供电下晶振停振（OSF/EOS 置位）时，芯片会冻结秒寄存器。本驱动把该
//      状态上报为 RtcError::kTimeNotValid 而非返回陈旧时间，调用方须重新对时。
//
// 设计原则（遵循 AGENTS.md §27/§32，参照 drivers/sensor/bhy2_driver.hpp 模式）：
//   - 零动态内存分配：全部 constexpr 与栈上固定长度缓冲
//   - 经 auroraos::hal::II2cHal 抽象接口解耦硬件，主机测试注入 mock
//   - 无板级判断：I2C 地址由 configure() 注入，默认 0x68 可被板级覆盖
//   - 解析 fail-safe：BCD/位域越界一律返回错误并上报，绝不编造时间
//
// 并发语义（AGENTS.md §48）：
//   驱动内部不加锁——多字节时间读写本身是单次 I2C 事务，busy 层面已串行化，
//   因此不可能拼出撕裂的日历值。但 get_time() 是「时间突发 + 状态寄存器」两次
//   事务，若另一任务在其间调用 set_time()，本次 get_time 会返回略旧的时间值。
//   这对时钟读取是可接受的最终一致，不构成正确性问题；调用方若需要
//   「读时间 + 读到就立刻设闹钟」这类原子序列，须在应用层自行串行化。
// =============================================================================

#ifndef AURORA_RTC_DS3231_DRIVER_HPP
#define AURORA_RTC_DS3231_DRIVER_HPP

#include <stdint.h>

#include "rtc_driver.hpp"
#include "../../hal/i2c_hal.hpp"

namespace auroraos {
namespace rtc {
namespace ds3231 {

// ---------------------------------------------------------------------------
// 寄存器地址（见文件头 datasheet 映射）
// ---------------------------------------------------------------------------
constexpr uint8_t kRegSeconds = 0x00;
constexpr uint8_t kRegMinutes = 0x01;
constexpr uint8_t kRegHours = 0x02;
constexpr uint8_t kRegDayOfWeek = 0x03; // 芯片不维护，本驱动不解释
constexpr uint8_t kRegDate = 0x04;      // 日
constexpr uint8_t kRegMonth = 0x05;     // 月 + 世纪位
constexpr uint8_t kRegYear = 0x06;
constexpr uint8_t kRegControl = 0x0E;
constexpr uint8_t kRegStatus = 0x0F;
constexpr uint8_t kRegTempHi = 0x11;
constexpr uint8_t kRegTempLo = 0x12;

// 时间寄存器突发长度（00h..06h）。必须整块读：分次读会在两次事务之间跨过
// 秒/分/时进位点，拼出「23:59:60」这类不存在的时刻。
constexpr size_t kTimeRegLen = 7;

// 器件寄存器空间总长（00h..12h）。
constexpr size_t kRegSpaceLen = 19;

// 默认 7-bit 从机地址（datasheet: 地址字节 D0h，R/W 位为 0 时的 7-bit 形式）。
constexpr uint8_t kDefaultI2cAddr = 0x68;

// 00h Seconds 位域
constexpr uint8_t kSecondsEosMask = 0x80; // bit7: Oscillator Stopped

// 02h Hours 位域
constexpr uint8_t kHoursMode24Mask = 0x80; // bit7: 1=24 小时制
constexpr uint8_t kHoursPmMask = 0x40;     // bit6: PM 标志（仅 12 小时制有效）
constexpr uint8_t kHoursTenMask = 0x30;    // bit5-4
constexpr uint8_t kHoursOnesMask = 0x0F;   // bit3-0

// 04h / 05h 位域
constexpr uint8_t kDayOfMonthMask = 0xFF;   // 04h 是完整 BCD 字节：日 1-31 占满高半字节，
                                            // 不能用 0x1F 掩码（29 -> 0x29 & 0x1F = 0x09）
constexpr uint8_t kMonthFieldMask = 0x1F;   // 05h bit4-0: 月 1-12
constexpr uint8_t kMonthCenturyMask = 0x40; // 05h bit6

// 0Eh Control 位域
constexpr uint8_t kControlEoscMask = 0x80; // bit7: Enable Oscillator（1=切到 VBAT 时停振）

// 0Fh Status 位域
constexpr uint8_t kStatusOsfMask = 0x80;      // bit7: Oscillator Stop Flag
constexpr uint8_t kStatusWritableMask = 0x7F; // bit6-0: 读改写时必须保留

constexpr uint16_t kBaseYear = 2000; // 06h 年份寄存器的基准年

// 温度 10 位二进制补码的符号位与量化步长
constexpr uint16_t kTempSignBit = 0x200; // 10 位码的 bit9
constexpr int16_t kTempModulus = 1024;

// ---------------------------------------------------------------------------
// BCD 编解码
//
// BCD 半字节取值必须 <= 9。位域若被位翻转污染会得到 10-15，直接参与乘法会
// 算出越界的时间值，因此解码必须校验而非直接强转。
// ---------------------------------------------------------------------------
constexpr uint8_t bcd_to_bin(uint8_t bcd) noexcept {
    return static_cast<uint8_t>((bcd >> 4) * 10u + (bcd & 0x0Fu));
}

constexpr uint8_t bin_to_bcd(uint8_t bin) noexcept {
    return static_cast<uint8_t>(((bin / 10u) << 4) | (bin % 10u));
}

constexpr bool is_valid_bcd(uint8_t bcd) noexcept {
    return ((bcd >> 4) <= 9) && ((bcd & 0x0Fu) <= 9);
}

// ---------------------------------------------------------------------------
// Ds3231Driver
//
// 生命周期：configure(i2c[, addr]) → init() → 正常使用。
// 未 init() 成功时所有查询接口返回 kNotInitialized，避免用陈旧标志误判总线可用性。
//
// 所有权：i2c_ 是非拥有（borrowed）指针，由板级持有，驱动不负责其生命周期。
// 驱动实例不含动态成员，可静态分配在应用或板级 BSS 中。
// ---------------------------------------------------------------------------
class Ds3231Driver : public IRtcDriver {
public:
    Ds3231Driver() = default;

    // 注入 I2C 通道与从机地址。必须在 init() 之前调用。
    void configure(auroraos::hal::II2cHal* i2c, uint8_t i2c_addr = kDefaultI2cAddr) noexcept {
        i2c_ = i2c; // 非拥有指针
        addr_ = i2c_addr;
        initialized_ = false;
    }

    // 探测器件在位。
    //
    // 判据：状态寄存器(0Fh)可读且不是全 0/全 1。总线上未焊接的器件会让 I2C
    // NACK（由 HAL 返回 false）；地址悬空/上拉缺失则会读回全 0x00 或全 0xFF。
    // 两者都判为「不在位」，避免把总线噪声当成有效状态上报。
    RtcError init() noexcept override {
        initialized_ = false;
        if (i2c_ == nullptr) {
            return RtcError::kNotPresent;
        }

        uint8_t status = 0;
        if (!i2c_->read_reg(addr_, kRegStatus, &status, 1)) {
            return RtcError::kBusError;
        }
        if (status == 0x00 || status == 0xFF) {
            return RtcError::kNotPresent;
        }

        initialized_ = true;
        return RtcError::kOk;
    }

    bool initialized() const noexcept override {
        return initialized_;
    }

    RtcError get_time(BrokenDownTime& out) const noexcept override {
        if (!initialized_) {
            return RtcError::kNotInitialized;
        }

        uint8_t buf[kTimeRegLen];
        // 单次突发读，保证 00h..06h 在同一总线事务内取回。II2cHal::read_reg
        // 契约要求实现方以单次锁覆盖「写寄存器地址 + 读回数据」全程。
        if (!i2c_->read_reg(addr_, kRegSeconds, buf, kTimeRegLen)) {
            return RtcError::kBusError;
        }

        // 状态寄存器只读一次：OSF 不在时间突发里，但每次 get_time 都必须复查——
        // 晶振可能在上次成功读取之后才停摆。
        // 总线代价可忽略：1 字节 @400kHz 约 50µs，按 1Hz 轮询计算占空比 <0.01%。
        uint8_t status = 0;
        if (!i2c_->read_reg(addr_, kRegStatus, &status, 1)) {
            return RtcError::kBusError;
        }
        if (status == 0x00 || status == 0xFF) {
            return RtcError::kNotPresent;
        }

        // 晶振停振时寄存器内容是陈旧值，必须拒绝上报。
        // EOS：00h bit7，取自上面已读到的突发数据。
        if ((buf[kRegSeconds] & kSecondsEosMask) != 0) {
            return RtcError::kTimeNotValid;
        }
        // OSF：0Fh bit7，掉电或纽扣电池耗尽后置位。
        if ((status & kStatusOsfMask) != 0) {
            return RtcError::kTimeNotValid;
        }

        // 逐字段 BCD 校验：先屏蔽标志位再判半字节，任一字段越界即数据损坏。
        if (!is_valid_bcd(static_cast<uint8_t>(buf[kRegSeconds] & 0x7Fu)) ||
            !is_valid_bcd(static_cast<uint8_t>(buf[kRegMinutes] & 0x7Fu)) ||
            !is_valid_bcd(static_cast<uint8_t>(decode_hour_raw(buf[kRegHours]))) ||
            !is_valid_bcd(static_cast<uint8_t>(buf[kRegDate] & kDayOfMonthMask)) ||
            !is_valid_bcd(static_cast<uint8_t>(buf[kRegMonth] & kMonthFieldMask)) || !is_valid_bcd(buf[kRegYear])) {
            return RtcError::kTimeNotValid;
        }

        const uint8_t second = bcd_to_bin(static_cast<uint8_t>(buf[kRegSeconds] & 0x7Fu));
        const uint8_t minute = bcd_to_bin(static_cast<uint8_t>(buf[kRegMinutes] & 0x7Fu));
        const uint8_t hour = decode_hour(buf[kRegHours]);
        const uint8_t day = bcd_to_bin(static_cast<uint8_t>(buf[kRegDate] & kDayOfMonthMask));
        const uint8_t month = bcd_to_bin(static_cast<uint8_t>(buf[kRegMonth] & kMonthFieldMask));
        const uint8_t year_off = bcd_to_bin(buf[kRegYear]);
        const bool century = (buf[kRegMonth] & kMonthCenturyMask) != 0;

        BrokenDownTime decoded{};
        decoded.year = static_cast<uint16_t>(kBaseYear + year_off + (century ? 100u : 0u));
        decoded.month = month;
        decoded.day = day;
        decoded.hour = hour;
        decoded.minute = minute;
        decoded.second = second;
        // 03h 的星期字段芯片不维护且用户可写，不作为可信来源，一律由日期推算。
        decoded.weekday = weekday_from_civil(decoded.year, decoded.month, decoded.day);

        // 芯片可能存着被位翻转污染的「看似合法」组合，这里做一次完整日历校验。
        if (!validate_broken_down(decoded)) {
            return RtcError::kTimeNotValid;
        }

        out = decoded;
        return RtcError::kOk;
    }

    RtcError set_time(const BrokenDownTime& t) noexcept override {
        if (!initialized_) {
            return RtcError::kNotInitialized;
        }
        // 先校验再产生 I2C 事务：DS3231 会静默接受 2 月 30 日并进位成 3 月 2 日，
        // 错误一旦被上层读回就很难定位。
        if (!validate_broken_down(t)) {
            return RtcError::kInvalidArgument;
        }

        const uint16_t year_off = static_cast<uint16_t>(t.year - kBaseYear);

        uint8_t buf[kTimeRegLen];
        buf[kRegSeconds] = bin_to_bcd(t.second);
        buf[kRegMinutes] = bin_to_bcd(t.minute);
        // 统一写 24 小时制：bit7=1。24h 模式下 bit6 是掩码位，置 0。
        buf[kRegHours] = kHoursMode24Mask | bin_to_bcd(t.hour);
        // 03h 星期字段芯片不维护且驱动不使用，留 0。
        buf[kRegDayOfWeek] = 0;
        buf[kRegDate] = bin_to_bcd(t.day);
        // 05h bit6 世纪位：06h 只有 00-99，用世纪位承载第三个世纪。
        buf[kRegMonth] = static_cast<uint8_t>((year_off >= 100 ? kMonthCenturyMask : 0x00) | bin_to_bcd(t.month));
        buf[kRegYear] = bin_to_bcd(static_cast<uint8_t>(year_off % 100));

        // 只写 00h..06h，不触碰 07h 起的闹钟区，对已有闹钟配置无破坏性。
        if (!i2c_->write_reg(addr_, kRegSeconds, buf, kTimeRegLen)) {
            return RtcError::kBusError;
        }

        clear_oscillator_stop_flags();
        return RtcError::kOk;
    }

    bool oscillator_running() const noexcept override {
        if (!initialized_) {
            return false;
        }
        uint8_t status = 0;
        if (!i2c_->read_reg(addr_, kRegStatus, &status, 1)) {
            return false;
        }
        if (status == 0x00 || status == 0xFF) {
            return false;
        }
        return (status & kStatusOsfMask) == 0;
    }

    // 10 位有符号温度，0.25°C 分辨率，量程 -40~85°C。
    RtcError get_temperature_c_x4(int16_t& out) const noexcept override {
        if (!initialized_) {
            return RtcError::kNotInitialized;
        }

        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!i2c_->read_reg(addr_, kRegTempHi, &hi, 1)) {
            return RtcError::kBusError;
        }
        if (!i2c_->read_reg(addr_, kRegTempLo, &lo, 1)) {
            return RtcError::kBusError;
        }
        // 不对 0xFF/0xFF 做「器件不在」判定：11h=0xFF 且 12h=0xFF 解出的 10 位码
        // 是 0x3FF = -1，即 -0.25°C，是合法读数，据此误报会把真实温度说成掉线。
        // 器件在位由 init() 负责判定；真掉线时 read_reg 会 NACK，落到 kBusError。

        // 11h 提供 V9..V2（bit7 为 V9 即符号位），12h bit7-6 提供 V1..V0。
        const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 2) | ((lo & 0xC0u) >> 6));
        int16_t value = static_cast<int16_t>(raw & 0x03FFu);
        // 10 位二进制补码符号扩展。用显式减法而非有符号右移，避免依赖实现
        // 定义的行为（AGENTS.md §49 要求检查编译/ABI 假设）。
        if ((raw & kTempSignBit) != 0) {
            value = static_cast<int16_t>(value - kTempModulus);
        }

        out = value;
        return RtcError::kOk;
    }

    const char* name() const noexcept override {
        return "ds3231";
    }

    // 清除停振标志并确保晶振在电池供电下继续运行。
    //
    // 仅在 set_time() 成功后调用：
    //   - OSF 需在时钟边沿 1 秒内清除，紧跟对时写入是最可靠时机
    //   - EOSC=1 会在切到 VBAT 时停振，对电池供电终端必须显式清零，
    //     否则拔掉充电宝的充电线后 RTC 就停了
    void clear_oscillator_stop_flags() noexcept {
        // 00h bit7 EOS：读-改-写清零，不影响秒计数本身。
        uint8_t seconds = 0;
        if (i2c_->read_reg(addr_, kRegSeconds, &seconds, 1)) {
            const uint8_t next = static_cast<uint8_t>(seconds & static_cast<uint8_t>(~kSecondsEosMask));
            i2c_->write_reg(addr_, kRegSeconds, &next, 1);
        }

        // 0Eh bit7 EOSC：读-改-写清零，保留 BBSQW/CONV/RS/INTCN/A*IE 配置。
        uint8_t control = 0;
        if (i2c_->read_reg(addr_, kRegControl, &control, 1)) {
            const uint8_t next = static_cast<uint8_t>(control & static_cast<uint8_t>(~kControlEoscMask));
            i2c_->write_reg(addr_, kRegControl, &next, 1);
        }

        // 0Fh bit7 OSF：读-改-写清零，保留 bit6-0（EN32KHZ/BSY/A2F/A1F）。
        uint8_t status = 0;
        if (i2c_->read_reg(addr_, kRegStatus, &status, 1)) {
            const uint8_t next = static_cast<uint8_t>(status & kStatusWritableMask);
            i2c_->write_reg(addr_, kRegStatus, &next, 1);
        }
    }

private:
    // 取出 02h 中真正参与 BCD 校验的位域，用于 get_time 的脏半字节检测。
    // 两种模式下手/个位都落在 bit5-0：24h 模式下 bit6 是掩码位，12h 模式下
    // bit6 是 PM 标志位，都不参与 BCD 运算。
    static uint8_t decode_hour_raw(uint8_t raw) noexcept {
        return static_cast<uint8_t>(raw & 0x3Fu);
    }

    // 小时寄存器解码，完整处理 12/24 小时制。
    static uint8_t decode_hour(uint8_t raw) noexcept {
        if ((raw & kHoursMode24Mask) != 0) {
            return bcd_to_bin(static_cast<uint8_t>(raw & 0x3Fu));
        }
        // 12h: bit5-4 十位, bit3-0 个位, bit6 为 PM。
        const uint8_t hour12 = static_cast<uint8_t>((((raw & kHoursTenMask) >> 4) * 10u) + (raw & kHoursOnesMask));
        // 12 小时制的 12 AM 必须映射到 0 点、12 PM 映射到 12 点。若直接对 PM
        // 加 12，12 PM 会被算成 24（越界）而 12 AM 算成 0 恰好蒙对——错误只在
        // 12 PM 暴露，是本驱动最易踩的编码陷阱，故显式用 %12 归一。
        return static_cast<uint8_t>((hour12 % 12u) + (((raw & kHoursPmMask) != 0) ? 12u : 0u));
    }

    auroraos::hal::II2cHal* i2c_ = nullptr; // 非拥有指针，生命周期由板级保证
    uint8_t addr_ = kDefaultI2cAddr;
    bool initialized_ = false;
};

} // namespace ds3231
} // namespace rtc
} // namespace auroraos

#endif // AURORA_RTC_DS3231_DRIVER_HPP
