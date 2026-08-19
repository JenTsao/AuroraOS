/**
 * @file softbus_codec.hpp
 * @brief Binary Intent frame for SoftBus (zero-heap, little-endian)
 *
 * Wire layout (v1), max ~96 bytes:
 *   magic     u16  = 0xFB02
 *   version   u8   = 1
 *   flags     u8   = 0
 *   type      u16  IntentType
 *   conf      u32  confidence_x1000
 *   source    u32
 *   param0    i32
 *   param1    i32
 *   text_len  u8
 *   text      text_len bytes (ASCII / UTF-8 prefix)
 *   ts_ms     u32  timestamp
 *   peer      u32  peer_id (sender view)
 *
 * No dynamic allocation. Pack fails if buffer too small.
 */
#ifndef AURORA_FEBRUARY_SOFTBUS_CODEC_HPP
#define AURORA_FEBRUARY_SOFTBUS_CODEC_HPP

#include "types.hpp"
#include "string_util.hpp"
#include <cstddef>
#include <cstdint>

namespace aurora {
namespace february {

constexpr uint16_t kSoftBusMagic   = 0xFB02;
constexpr uint8_t  kSoftBusVersion = 1;
constexpr unsigned kSoftBusMaxText = 64;
/** Upper bound of one frame (header + full text). */
constexpr unsigned kSoftBusFrameMax = 4 + 2 + 4 + 4 + 4 + 4 + 1 + kSoftBusMaxText + 4 + 4;

struct SoftBusFrameHeader {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;
};

namespace detail {

inline void put_u16(uint8_t*& p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p += 2;
}
inline void put_u32(uint8_t*& p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
    p += 4;
}
inline void put_i32(uint8_t*& p, int32_t v) {
    put_u32(p, static_cast<uint32_t>(v));
}
inline uint16_t get_u16(const uint8_t*& p) {
    uint16_t v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2;
    return v;
}
inline uint32_t get_u32(const uint8_t*& p) {
    uint32_t v = static_cast<uint32_t>(p[0]) |
                 (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) |
                 (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    return v;
}
inline int32_t get_i32(const uint8_t*& p) {
    return static_cast<int32_t>(get_u32(p));
}

}  // namespace detail

/**
 * Pack Intent into out[]. Returns bytes written, or 0 on failure.
 */
inline unsigned softbus_pack_intent(const Intent& in, uint32_t peer_id,
                                    uint32_t timestamp_ms,
                                    uint8_t* out, unsigned out_cap) {
    if (!out || out_cap < 4 + 2 + 4 + 4 + 4 + 4 + 1 + 4 + 4) {
        return 0;
    }
    unsigned text_len = 0;
    while (in.text[text_len] && text_len < kSoftBusMaxText) {
        ++text_len;
    }
    const unsigned need = 4 + 2 + 4 + 4 + 4 + 4 + 1 + text_len + 4 + 4;
    if (out_cap < need) {
        return 0;
    }

    uint8_t* p = out;
    detail::put_u16(p, kSoftBusMagic);
    *p++ = kSoftBusVersion;
    *p++ = 0;  // flags
    detail::put_u16(p, static_cast<uint16_t>(in.type));
    detail::put_u32(p, in.confidence_x1000);
    detail::put_u32(p, in.source_id);
    detail::put_i32(p, in.param0);
    detail::put_i32(p, in.param1);
    *p++ = static_cast<uint8_t>(text_len);
    for (unsigned i = 0; i < text_len; ++i) {
        *p++ = static_cast<uint8_t>(in.text[i]);
    }
    detail::put_u32(p, timestamp_ms);
    detail::put_u32(p, peer_id);
    return static_cast<unsigned>(p - out);
}

/**
 * Unpack frame into Intent + meta. Returns true on success.
 */
inline bool softbus_unpack_intent(const uint8_t* data, unsigned len,
                                  Intent& out_intent,
                                  uint32_t& out_peer_id,
                                  uint32_t& out_timestamp_ms) {
    out_intent.clear();
    out_peer_id = 0;
    out_timestamp_ms = 0;
    if (!data || len < 4 + 2 + 4 + 4 + 4 + 4 + 1 + 4 + 4) {
        return false;
    }
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    const uint16_t magic = detail::get_u16(p);
    if (magic != kSoftBusMagic) {
        return false;
    }
    const uint8_t version = *p++;
    const uint8_t flags = *p++;
    (void)flags;
    if (version != kSoftBusVersion) {
        return false;
    }
    if (p + 2 + 4 + 4 + 4 + 4 + 1 > end) {
        return false;
    }
    out_intent.type = static_cast<IntentType>(detail::get_u16(p));
    out_intent.confidence_x1000 = detail::get_u32(p);
    out_intent.source_id = detail::get_u32(p);
    out_intent.param0 = detail::get_i32(p);
    out_intent.param1 = detail::get_i32(p);
    const unsigned text_len = *p++;
    if (text_len >= kSoftBusMaxText || p + text_len + 4 + 4 > end) {
        return false;
    }
    for (unsigned i = 0; i < text_len; ++i) {
        out_intent.text[i] = static_cast<char>(p[i]);
    }
    out_intent.text[text_len] = '\0';
    p += text_len;
    out_timestamp_ms = detail::get_u32(p);
    out_peer_id = detail::get_u32(p);
    return out_intent.valid() || out_intent.type == IntentType::None;
}

}  // namespace february
}  // namespace aurora

#endif  // AURORA_FEBRUARY_SOFTBUS_CODEC_HPP
