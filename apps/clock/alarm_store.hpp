// =============================================================================
// apps/clock/alarm_store.hpp
//
// 闹钟配置的定长二进制持久化编解码（零堆、与文件系统解耦）。
//
// 为什么是定长二进制而不是 JSON：
//   1. 目标含 8KB RAM 的 Cortex-M0+，整份配置必须小到能塞进 LittleFS 单个块；
//      本格式固定 32 字节，JSON 光字段名就要几百字节。
//   2. 二进制布局无解析歧义，也不会出现「解析器接受了一个语法合法但语义
//      非法的配置」这种半吊子状态。
//   3. 字段宽度固定，未来加字段走 version 递增，旧数据按「版本不匹配即拒绝」
//      处理，不做猜测性迁移。
//
// 与文件系统解耦：本文件只负责「字节数组 <-> 闹钟集合」。调用方负责通过 VFS
// 读写 `/alarms.cfg`。这样本模块在 host 侧可直接测试，也让存储介质可替换
// （LittleFS / RamFS / OTA 双分区）。
//
// 失败语义（AGENTS.md §22）：任何校验不通过都返回 false 且**完全不修改**
// 目标对象。绝不允许出现「前 3 个闹钟写进去了，第 4 个发现损坏」这种半应用
// 状态——对配置而言那比整体拒绝更难排查。
// =============================================================================

#ifndef AURORA_APP_CLOCK_ALARM_STORE_HPP
#define AURORA_APP_CLOCK_ALARM_STORE_HPP

#include <stdint.h>
#include <stddef.h>

#include "alarm.hpp"

namespace auroraos {
namespace app {
namespace clock {

// ---------------------------------------------------------------------------
// 存储格式
//
//   偏移  长度  字段
//   0     4    magic = 'A''L''R''M'
//   4     1    version = 1
//   5     1    alarm_count（写死为 kMaxAlarms，供未来扩容时校验）
//   6     1    reserved（必须为 0）
//   7     1    reserved（必须为 0）
//   8     20   5 个闹钟 × 4 字节：
//                [0] flags  bit0=enabled  bit1-2=repeat  bit3-7=reserved(0)
//                [1] hour
//                [2] minute
//                [3] weekday_mask（bit0-6 = 周一..周日，bit7 reserved(0)）
//   28    4    FNV-1a 32，覆盖 [0, 28)，小端存放
//   ────────────
//   合计  32
//
// 注意 weekday_mask 必须独占一整字节：7 天需要 7 个位，若与 enabled/repeat
// 挤在同一个字节里只能放下 5 位，高 2 天会被静默截断成「永远不响」的闹钟。
//
// 多字节字段（FNV）一律用显式移位读写，不依赖主机字节序（AGENTS.md §49）。
// ---------------------------------------------------------------------------
constexpr size_t kStoreMagicLen = 4;
constexpr uint8_t kStoreVersion = 1;
constexpr size_t kStoreHeaderLen = 8;
constexpr size_t kAlarmRecordLen = 4;
constexpr size_t kStoreBodyLen = kStoreHeaderLen + kMaxAlarms * kAlarmRecordLen; // 28
constexpr size_t kStoreHashLen = 4;
constexpr size_t kSerializedSize = kStoreBodyLen + kStoreHashLen; // 32

constexpr uint8_t kFlagEnabled = 0x01;
constexpr uint8_t kFlagRepeatMask = 0x06;                            // bit1-2
constexpr uint8_t kFlagDefinedMask = kFlagEnabled | kFlagRepeatMask; // 已定义位，其余必须为 0
constexpr uint8_t kWeekdayMaskAllowed = 0x7F;                        // bit0-6（bit7 保留）
constexpr uint8_t kReservedMustBeZero = 0x00;

// 存储层允许的 repeat 枚举范围（与 AlarmRepeat 的底层值一致）
constexpr uint8_t kRepeatMax = 2;

namespace detail {

// FNV-1a 32 位。算法与 security/hids/file_integrity.hpp 保持一致以便团队
// 习惯统一，但此处独立实现：app 层不应反向依赖安全子系统的类成员
// （AGENTS.md §42 依赖方向）。定长 32 字节的数据块用 FNV 足够，不引入 CRC 库。
constexpr uint32_t fnv1a32(const uint8_t* data, size_t len) noexcept {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint32_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

inline void store_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint32_t load_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace detail

// ---------------------------------------------------------------------------
// AlarmStore
// ---------------------------------------------------------------------------
class AlarmStore {
public:
    // 把 sched 序列化进调用方提供的缓冲区。
    // cap 不足 kSerializedSize 时返回 false 且不写入任何内容。
    static bool serialize(const AlarmSchedule& sched, uint8_t* buf, size_t cap, size_t& written) noexcept {
        if (buf == nullptr || cap < kSerializedSize) {
            return false;
        }

        buf[0] = 'A';
        buf[1] = 'L';
        buf[2] = 'R';
        buf[3] = 'M';
        buf[4] = kStoreVersion;
        buf[5] = static_cast<uint8_t>(kMaxAlarms);
        buf[6] = kReservedMustBeZero;
        buf[7] = kReservedMustBeZero;

        for (size_t i = 0; i < kMaxAlarms; ++i) {
            const Alarm& a = sched.alarm_at(static_cast<uint8_t>(i));
            uint8_t* rec = buf + kStoreHeaderLen + i * kAlarmRecordLen;
            uint8_t flags = 0;
            if (a.enabled) {
                flags |= kFlagEnabled;
            }
            flags |= static_cast<uint8_t>((static_cast<uint8_t>(a.repeat) << 1) & kFlagRepeatMask);
            rec[0] = flags;
            rec[1] = a.hour;
            rec[2] = a.minute;
            rec[3] = static_cast<uint8_t>(a.weekday_mask & kWeekdayMaskAllowed);
        }

        detail::store_u32_le(buf + kStoreBodyLen, detail::fnv1a32(buf, kStoreBodyLen));
        written = kSerializedSize;
        return true;
    }

    // 从缓冲区恢复。校验失败时 out 完全不被修改。
    static bool deserialize(const uint8_t* buf, size_t len, AlarmSchedule& out) noexcept {
        if (buf == nullptr || len != kSerializedSize) {
            return false;
        }
        if (buf[0] != 'A' || buf[1] != 'L' || buf[2] != 'R' || buf[3] != 'M') {
            return false;
        }
        if (buf[4] != kStoreVersion) {
            return false; // 格式演进时不猜测性迁移
        }
        if (buf[5] != static_cast<uint8_t>(kMaxAlarms)) {
            return false;
        }
        if (buf[6] != kReservedMustBeZero || buf[7] != kReservedMustBeZero) {
            return false;
        }
        if (detail::load_u32_le(buf + kStoreBodyLen) != detail::fnv1a32(buf, kStoreBodyLen)) {
            return false; // 校验和不符：位翻转或写入被截断
        }

        // 先解到临时对象，全部通过后才提交，避免半应用状态。
        AlarmSchedule staged;
        for (size_t i = 0; i < kMaxAlarms; ++i) {
            const uint8_t* rec = buf + kStoreHeaderLen + i * kAlarmRecordLen;
            const uint8_t flags = rec[0];

            // 保留位必须为 0：出现未知位说明写入端与本读取端的格式已分叉，
            // 继续解释会静默产生错误的闹钟。
            if ((flags & static_cast<uint8_t>(~kFlagDefinedMask)) != 0) {
                return false;
            }
            if ((rec[3] & static_cast<uint8_t>(~kWeekdayMaskAllowed)) != 0) {
                return false;
            }

            Alarm a;
            a.enabled = (flags & kFlagEnabled) != 0;
            a.repeat = static_cast<AlarmRepeat>((flags & kFlagRepeatMask) >> 1);
            a.weekday_mask = rec[3];
            a.hour = rec[1];
            a.minute = rec[2];

            // 复用模型层的合法性校验：时/分越界、repeat 越界、以及
            // kWeekdays + 空掩码（永不触发的幽灵闹钟）都在这里被拒。
            if (!alarm_valid(a)) {
                return false;
            }
            if (!staged.set(static_cast<uint8_t>(i), a)) {
                return false;
            }
        }

        out = staged;
        return true;
    }
};

} // namespace clock
} // namespace app
} // namespace auroraos

#endif // AURORA_APP_CLOCK_ALARM_STORE_HPP
