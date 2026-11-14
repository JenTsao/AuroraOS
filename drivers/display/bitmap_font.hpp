// =============================================================================
// drivers/display/bitmap_font.hpp
//
// AuroraOS 统一位图字体管线（支持 UTF-8 解码、1bpp/4bpp 字形、Flash 字库及 .afont 流式按需查表）
//
// 设计原则：
//  - 纯独立头实现：仅依赖 <stddef.h> 与 <stdint.h>，无动态内存分配、无异常、无 VFS 反向依赖
//  - 向下兼容 apps/watch/font_engine.hpp 原有的 GlyphInfo / FontDef 布局
//  - 支持从只读存储 / Flash / LittleFS（通过 IFontReader 抽象）二分查表并带定长零堆 LRU 缓存
//  - 所有二进制整数按显式小端字节序解析，避免 Cortex-M0+ / RV32 非对齐访存故障
// =============================================================================
#ifndef AURORA_BITMAP_FONT_HPP
#define AURORA_BITMAP_FONT_HPP

#include <stddef.h>
#include <stdint.h>

// =============================================================================
// 1. 像素格式与统一字形元数据
// =============================================================================

enum class FontPixelFormat : uint8_t {
    // 1bpp 连续位流（与 scripts/gen_font.py 及 font_data.cpp 一致）：
    // pixel_idx = row * width + col，字节内 MSB->LSB（0x80 >> (pixel_idx & 7)）
    MONO_1BPP_MSB = 0,
    // 4bpp 抗锯齿灰度连续半字节流（0=全透, 15=全不透明）：
    // pixel_idx = row * width + col，偶像素高半字节 (b >> 4)，奇像素低半字节 (b & 0x0F)
    GRAY_4BPP = 1,
};

struct GlyphInfo {
    uint8_t width{0};       // 字符像素宽度
    uint8_t height{0};      // 字符像素高度
    int8_t x_offset{0};     // X 轴绘制偏移
    int8_t y_offset{0};     // Y 轴绘制偏移
    uint32_t bitmap_idx{0}; // 在位图字节数组中的起始字节偏移
    uint8_t x_advance{0};   // 水平步进宽度（0 表示按 width + char_spacing 自动推算）
};

// 稀疏 Unicode 码点索引项（须按 codepoint 严格升序排列以支持二分查找）
struct UnicodeGlyphEntry {
    uint32_t codepoint{0};
    GlyphInfo glyph{};
};

struct FontDef {
    const uint8_t* bitmap_data{nullptr}; // Flash/RAM 中的字模字节数组
    const GlyphInfo* glyphs{nullptr};    // 连续 ASCII 码点表 ([start_char, end_char])
    uint8_t line_height{0};              // 行高（像素）
    char start_char{' '};                // ASCII 连续表起始字符
    char end_char{'~'};                  // ASCII 连续表结束字符
    FontPixelFormat format{FontPixelFormat::MONO_1BPP_MSB};
    uint8_t char_spacing{1};                          // 默认字间距（x_advance == 0 时追加）
    const UnicodeGlyphEntry* unicode_glyphs{nullptr}; // 稀疏 Unicode 升序映射表（汉字/全角符号等）
    uint32_t unicode_count{0};                        // unicode_glyphs 条目数
};

// 计算单个字形所需的紧凑位图字节数
constexpr uint32_t font_glyph_byte_size(uint8_t width, uint8_t height, FontPixelFormat format) noexcept {
    const uint32_t pixels = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
    if (pixels == 0u) {
        return 0u;
    }
    if (format == FontPixelFormat::GRAY_4BPP) {
        return (pixels + 1u) >> 1u;
    }
    return (pixels + 7u) >> 3u;
}

// 统一计算字形绘制后的光标步进宽度
constexpr uint8_t resolve_glyph_advance(const GlyphInfo& glyph, uint8_t char_spacing) noexcept {
    if (glyph.x_advance > 0u) {
        return glyph.x_advance;
    }
    const uint16_t adv = static_cast<uint16_t>(glyph.width) + static_cast<uint16_t>(char_spacing);
    return static_cast<uint8_t>(adv > 255u ? 255u : adv);
}

// 从字形位图读取 (row, col) 处 4-bit 覆盖度 [0..15]
inline uint8_t sample_glyph_coverage4(const uint8_t* bitmap, uint8_t width, uint16_t row, uint16_t col,
                                      FontPixelFormat format) noexcept {
    if (!bitmap) {
        return 0u;
    }
    const uint32_t idx = static_cast<uint32_t>(row) * static_cast<uint32_t>(width) + static_cast<uint32_t>(col);
    if (format == FontPixelFormat::GRAY_4BPP) {
        const uint8_t packed = bitmap[idx >> 1u];
        return static_cast<uint8_t>((idx & 1u) ? (packed & 0x0Fu) : (packed >> 4u));
    }
    const uint8_t byte = bitmap[idx >> 3u];
    return (byte & (0x80u >> (idx & 7u))) ? 15u : 0u;
}

// =============================================================================
// 2. 严格 UTF-8 解码器 (RFC 3629)
// =============================================================================

// 从 UTF-8 字符串当前位置解码一个 Unicode 码点并前进游标。
// 遇到 NUL 结尾或 nullptr 返回 false；否则总消费至少 1 字节并返回 true（非法序列输出 U+FFFD）。
inline bool decode_utf8(const char*& cursor, uint32_t& out_codepoint) noexcept {
    if (!cursor) {
        return false;
    }
    const uint8_t b0 = static_cast<uint8_t>(*cursor);
    if (b0 == 0u) {
        return false;
    }
    if (b0 < 0x80u) {
        out_codepoint = b0;
        ++cursor;
        return true;
    }

    auto is_cont = [](uint8_t b) noexcept -> bool {
        return (b & 0xC0u) == 0x80u;
    };

    if ((b0 & 0xE0u) == 0xC0u) {
        if (b0 < 0xC2u) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint8_t b1 = static_cast<uint8_t>(cursor[1]);
        if (!is_cont(b1)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        out_codepoint = (static_cast<uint32_t>(b0 & 0x1Fu) << 6u) | static_cast<uint32_t>(b1 & 0x3Fu);
        cursor += 2;
        return true;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
        const uint8_t b1 = static_cast<uint8_t>(cursor[1]);
        if (!is_cont(b1)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint8_t b2 = static_cast<uint8_t>(cursor[2]);
        if (!is_cont(b2)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint32_t cp = (static_cast<uint32_t>(b0 & 0x0Fu) << 12u) | (static_cast<uint32_t>(b1 & 0x3Fu) << 6u) |
                            static_cast<uint32_t>(b2 & 0x3Fu);
        if (cp < 0x0800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        out_codepoint = cp;
        cursor += 3;
        return true;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
        if (b0 > 0xF4u) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint8_t b1 = static_cast<uint8_t>(cursor[1]);
        if (!is_cont(b1)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint8_t b2 = static_cast<uint8_t>(cursor[2]);
        if (!is_cont(b2)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint8_t b3 = static_cast<uint8_t>(cursor[3]);
        if (!is_cont(b3)) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        const uint32_t cp = (static_cast<uint32_t>(b0 & 0x07u) << 18u) | (static_cast<uint32_t>(b1 & 0x3Fu) << 12u) |
                            (static_cast<uint32_t>(b2 & 0x3Fu) << 6u) | static_cast<uint32_t>(b3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            out_codepoint = 0xFFFDu;
            ++cursor;
            return true;
        }
        out_codepoint = cp;
        cursor += 4;
        return true;
    }

    out_codepoint = 0xFFFDu;
    ++cursor;
    return true;
}

// =============================================================================
// 3. 统一字形视图与字库数据源抽象
// =============================================================================

struct FontGlyphView {
    GlyphInfo info{};
    const uint8_t* bitmap{nullptr}; // 非拥有指针，至少在下次 lookup_glyph 调用前有效
    uint32_t bitmap_bytes{0};       // bitmap 可安全读取的字节数
    FontPixelFormat format{FontPixelFormat::MONO_1BPP_MSB};
    uint8_t char_spacing{1};
};

class IFontSource {
public:
    virtual ~IFontSource() noexcept = default;
    [[nodiscard]] virtual uint8_t line_height() const noexcept = 0;
    [[nodiscard]] virtual uint8_t default_char_spacing() const noexcept = 0;
    [[nodiscard]] virtual bool lookup_glyph(uint32_t codepoint, FontGlyphView& out) noexcept = 0;
};

// 5x7 ASCII 回退字形的垂直居中与步进推算（与中文等大字号混排时自动等比放大）
inline void font_fallback_5x7_metrics(uint8_t target_line_h, uint8_t scale, uint8_t& out_char_scale, int16_t& out_y_off,
                                      int16_t& out_advance) noexcept {
    if (scale == 0u) {
        scale = 1u;
    }
    const uint8_t mult = (target_line_h >= 14u) ? static_cast<uint8_t>(target_line_h / 7u) : 1u;
    const uint16_t cs = static_cast<uint16_t>(scale) * static_cast<uint16_t>(mult);
    out_char_scale = static_cast<uint8_t>(cs > 255u ? 255u : cs);
    const int16_t eff_h = static_cast<int16_t>(7u * out_char_scale);
    const int16_t target_scaled = static_cast<int16_t>(target_line_h) * static_cast<int16_t>(scale);
    out_y_off = (target_scaled > eff_h) ? static_cast<int16_t>((target_scaled - eff_h) / 2) : 0;
    out_advance = static_cast<int16_t>(6u * out_char_scale);
}

// 缺失字形（豆腐块占位符）的统一度量
inline void font_missing_glyph_box(uint8_t line_h, uint8_t scale, uint16_t& out_w, uint16_t& out_h,
                                   int16_t& out_adv) noexcept {
    if (scale == 0u) {
        scale = 1u;
    }
    out_h = (line_h >= 6u) ? static_cast<uint16_t>(line_h - 2u) : 8u;
    out_w = (out_h >= 8u) ? static_cast<uint16_t>((out_h * 3u) / 4u) : out_h;
    out_adv = static_cast<int16_t>((out_w + 2u) * scale);
}

// 统一计算 UTF-8 字符串的总宽度（px），保证 TextView 对齐与 Renderer2D 光标完全一致
inline uint16_t measure_string_width_utf8(const char* str, IFontSource& font, uint8_t scale = 1u,
                                          bool use_5x7_fallback = true) noexcept {
    if (!str || !*str) {
        return 0u;
    }
    if (scale == 0u) {
        scale = 1u;
    }
    const uint8_t line_h = font.line_height();
    int32_t total_w = 0;
    const char* p = str;
    uint32_t cp = 0u;
    FontGlyphView gv{};
    while (decode_utf8(p, cp)) {
        if (font.lookup_glyph(cp, gv)) {
            total_w += static_cast<int32_t>(resolve_glyph_advance(gv.info, gv.char_spacing)) * scale;
        } else if (use_5x7_fallback && cp >= 0x20u && cp <= 0x7Eu) {
            uint8_t fb_scale = scale;
            int16_t y_off = 0;
            int16_t adv = 0;
            font_fallback_5x7_metrics(line_h, scale, fb_scale, y_off, adv);
            total_w += adv;
        } else {
            uint16_t bw = 0u;
            uint16_t bh = 0u;
            int16_t adv = 0;
            font_missing_glyph_box(line_h, scale, bw, bh, adv);
            total_w += adv;
        }
    }
    return (total_w > 65535) ? 65535u : static_cast<uint16_t>(total_w > 0 ? total_w : 0);
}

// 在 FontDef 中按 ASCII 直索引 + 稀疏 Unicode 二分查找解析字形
inline bool lookup_font_def(const FontDef& font, uint32_t codepoint, FontGlyphView& out,
                            size_t bitmap_capacity = 0u) noexcept {
    const GlyphInfo* found = nullptr;

    const uint8_t first = static_cast<uint8_t>(font.start_char);
    const uint8_t last = static_cast<uint8_t>(font.end_char);
    if (font.glyphs && last >= first && codepoint >= first && codepoint <= last) {
        found = &font.glyphs[codepoint - first];
    } else if (font.unicode_glyphs && font.unicode_count > 0u) {
        uint32_t lo = 0u;
        uint32_t hi = font.unicode_count;
        while (lo < hi) {
            const uint32_t mid = lo + ((hi - lo) >> 1u);
            const uint32_t mid_cp = font.unicode_glyphs[mid].codepoint;
            if (mid_cp < codepoint) {
                lo = mid + 1u;
            } else if (mid_cp > codepoint) {
                hi = mid;
            } else {
                found = &font.unicode_glyphs[mid].glyph;
                break;
            }
        }
    }

    if (!found) {
        return false;
    }

    const uint32_t req_bytes = font_glyph_byte_size(found->width, found->height, font.format);
    if (req_bytes > 0u) {
        if (!font.bitmap_data) {
            return false;
        }
        if (bitmap_capacity > 0u) {
            const uint64_t end = static_cast<uint64_t>(found->bitmap_idx) + static_cast<uint64_t>(req_bytes);
            if (end > static_cast<uint64_t>(bitmap_capacity)) {
                return false;
            }
        }
    }

    out.info = *found;
    out.bitmap = (req_bytes > 0u) ? (font.bitmap_data + found->bitmap_idx) : nullptr;
    out.bitmap_bytes = req_bytes;
    out.format = font.format;
    out.char_spacing = font.char_spacing;
    return true;
}

// 内存 / Flash XIP 字库封装
class MemoryFontSource final : public IFontSource {
public:
    explicit constexpr MemoryFontSource(const FontDef* font = nullptr, size_t bitmap_capacity = 0u) noexcept
        : font_(font), bitmap_capacity_(bitmap_capacity) {}

    void set_font(const FontDef* font, size_t bitmap_capacity = 0u) noexcept {
        font_ = font;
        bitmap_capacity_ = bitmap_capacity;
    }

    [[nodiscard]] const FontDef* font() const noexcept {
        return font_;
    }

    [[nodiscard]] uint8_t line_height() const noexcept override {
        return font_ ? font_->line_height : 0u;
    }

    [[nodiscard]] uint8_t default_char_spacing() const noexcept override {
        return font_ ? font_->char_spacing : 1u;
    }

    [[nodiscard]] bool lookup_glyph(uint32_t codepoint, FontGlyphView& out) noexcept override {
        if (!font_) {
            return false;
        }
        return lookup_font_def(*font_, codepoint, out, bitmap_capacity_);
    }

private:
    const FontDef* font_;
    size_t bitmap_capacity_;
};

// 组合字库回退（例如主字库为 LittleFS 中文字库，辅字库为内置 Flash ASCII 字库）
class FallbackFontSource final : public IFontSource {
public:
    constexpr FallbackFontSource(IFontSource* primary, IFontSource* secondary) noexcept
        : primary_(primary), secondary_(secondary) {}

    [[nodiscard]] uint8_t line_height() const noexcept override {
        const uint8_t p_h = primary_ ? primary_->line_height() : 0u;
        const uint8_t s_h = secondary_ ? secondary_->line_height() : 0u;
        return (p_h >= s_h) ? p_h : s_h;
    }

    [[nodiscard]] uint8_t default_char_spacing() const noexcept override {
        return primary_ ? primary_->default_char_spacing() : (secondary_ ? secondary_->default_char_spacing() : 1u);
    }

    [[nodiscard]] bool lookup_glyph(uint32_t codepoint, FontGlyphView& out) noexcept override {
        if (primary_ && primary_->lookup_glyph(codepoint, out)) {
            return true;
        }
        if (secondary_ && secondary_->lookup_glyph(codepoint, out)) {
            return true;
        }
        return false;
    }

private:
    IFontSource* primary_;
    IFontSource* secondary_;
};

// =============================================================================
// 4. 只读随机访问抽象 (Flash / LittleFS / RAM 统一底座)
// =============================================================================

class IFontReader {
public:
    virtual ~IFontReader() noexcept = default;
    [[nodiscard]] virtual uint32_t size() const noexcept = 0;
    // 从绝对字节偏移 offset 读取最多 len 字节到 out_buf，返回实际读取字节数
    [[nodiscard]] virtual size_t read_at(uint32_t offset, uint8_t* out_buf, size_t len) noexcept = 0;
};

// 直接指向 Flash XIP / 静态数组的零拷贝 Reader（测试或固定映射 Flash 时可直接喂给 StreamFontSource）
class MemoryFontReader final : public IFontReader {
public:
    constexpr MemoryFontReader(const uint8_t* data = nullptr, uint32_t size = 0u) noexcept
        : data_(data), size_(data ? size : 0u) {}

    void reset(const uint8_t* data, uint32_t size) noexcept {
        data_ = data;
        size_ = data ? size : 0u;
    }

    [[nodiscard]] uint32_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] size_t read_at(uint32_t offset, uint8_t* out_buf, size_t len) noexcept override {
        if (!data_ || !out_buf || offset >= size_ || len == 0u) {
            return 0u;
        }
        const uint32_t avail = size_ - offset;
        const size_t to_copy = (len < avail) ? len : static_cast<size_t>(avail);
        for (size_t i = 0u; i < to_copy; ++i) {
            out_buf[i] = data_[offset + i];
        }
        return to_copy;
    }

private:
    const uint8_t* data_;
    uint32_t size_;
};

// =============================================================================
// 5. .afont 二进制字库格式 ("AFNT" v1) 与零堆流式按需查表缓存
// =============================================================================

namespace detail {

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) | (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

} // namespace detail

struct AfontFormat {
    static constexpr uint32_t kMagic = 0x544E4641u; // "AFNT" 小端序
    static constexpr uint16_t kVersion1 = 1u;
    static constexpr uint32_t kHeaderBytes = 24u;
    static constexpr uint32_t kIndexEntryBytes = 16u;
};

template <size_t MaxGlyphBytes = 512u, size_t CacheSlots = 8u> class StreamFontSource final : public IFontSource {
    static_assert(MaxGlyphBytes >= 16u, "MaxGlyphBytes too small");
    static_assert(CacheSlots >= 1u, "CacheSlots must be >= 1");

public:
    StreamFontSource() noexcept = default;

    // 绑定 IFontReader 并校验 24B 文件头；不把索引表与位图全量载入 RAM
    [[nodiscard]] bool open(IFontReader* reader) noexcept {
        close();
        if (!reader) {
            return false;
        }
        const uint32_t total_size = reader->size();
        if (total_size < AfontFormat::kHeaderBytes) {
            return false;
        }

        uint8_t hdr[AfontFormat::kHeaderBytes]{};
        if (reader->read_at(0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
            return false;
        }

        const uint32_t magic = detail::read_le32(hdr + 0u);
        const uint16_t version = detail::read_le16(hdr + 4u);
        const uint8_t raw_fmt = hdr[6];
        const uint8_t line_h = hdr[7];
        const uint8_t spacing = hdr[8];
        const uint32_t count = detail::read_le32(hdr + 12u);
        const uint32_t idx_off = detail::read_le32(hdr + 16u);
        const uint32_t bmp_off = detail::read_le32(hdr + 20u);

        if (magic != AfontFormat::kMagic || version != AfontFormat::kVersion1) {
            return false;
        }
        if (raw_fmt > static_cast<uint8_t>(FontPixelFormat::GRAY_4BPP) || line_h == 0u) {
            return false;
        }
        if (idx_off < AfontFormat::kHeaderBytes || idx_off > total_size || bmp_off > total_size) {
            return false;
        }
        const uint64_t idx_bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(AfontFormat::kIndexEntryBytes);
        const uint64_t idx_end = static_cast<uint64_t>(idx_off) + idx_bytes;
        if (idx_end > static_cast<uint64_t>(bmp_off)) {
            return false;
        }

        reader_ = reader;
        reader_size_ = total_size;
        format_ = static_cast<FontPixelFormat>(raw_fmt);
        line_height_ = line_h;
        char_spacing_ = spacing;
        glyph_count_ = count;
        index_offset_ = idx_off;
        bitmap_offset_ = bmp_off;
        is_open_ = true;
        return true;
    }

    void close() noexcept {
        reader_ = nullptr;
        reader_size_ = 0u;
        glyph_count_ = 0u;
        index_offset_ = 0u;
        bitmap_offset_ = 0u;
        line_height_ = 0u;
        char_spacing_ = 1u;
        is_open_ = false;
        stamp_ = 0u;
        for (size_t i = 0u; i < CacheSlots; ++i) {
            cache_[i].valid = false;
            cache_[i].last_used = 0u;
            cache_[i].codepoint = 0u;
            cache_[i].bitmap_bytes = 0u;
        }
    }

    [[nodiscard]] bool is_open() const noexcept {
        return is_open_;
    }

    [[nodiscard]] uint32_t glyph_count() const noexcept {
        return glyph_count_;
    }

    [[nodiscard]] FontPixelFormat format() const noexcept {
        return format_;
    }

    [[nodiscard]] uint8_t line_height() const noexcept override {
        return line_height_;
    }

    [[nodiscard]] uint8_t default_char_spacing() const noexcept override {
        return char_spacing_;
    }

    [[nodiscard]] bool lookup_glyph(uint32_t codepoint, FontGlyphView& out) noexcept override {
        if (!is_open_ || !reader_ || glyph_count_ == 0u) {
            return false;
        }

        // 1. 先查静态定长 LRU 缓存，命中则零 I/O 返回
        for (size_t i = 0u; i < CacheSlots; ++i) {
            if (cache_[i].valid && cache_[i].codepoint == codepoint) {
                cache_[i].last_used = next_stamp();
                fill_view_from_slot(cache_[i], out);
                return true;
            }
        }

        // 2. 在文件索引区执行磁盘/Flash 二分查找，O(log N) 次 seek+read
        uint32_t lo = 0u;
        uint32_t hi = glyph_count_;
        uint8_t entry[AfontFormat::kIndexEntryBytes]{};
        bool found = false;

        while (lo < hi) {
            const uint32_t mid = lo + ((hi - lo) >> 1u);
            const uint32_t off = index_offset_ + mid * AfontFormat::kIndexEntryBytes;
            if (reader_->read_at(off, entry, sizeof(entry)) != sizeof(entry)) {
                return false;
            }
            const uint32_t mid_cp = detail::read_le32(entry + 0u);
            if (mid_cp < codepoint) {
                lo = mid + 1u;
            } else if (mid_cp > codepoint) {
                hi = mid;
            } else {
                found = true;
                break;
            }
        }

        if (!found) {
            return false;
        }

        const uint32_t rel_off = detail::read_le32(entry + 4u);
        const uint8_t gw = entry[8];
        const uint8_t gh = entry[9];
        const int8_t gx = static_cast<int8_t>(entry[10]);
        const int8_t gy = static_cast<int8_t>(entry[11]);
        const uint8_t gadv = entry[12];
        const uint16_t stored_bytes = detail::read_le16(entry + 14u);

        const uint32_t req_bytes = font_glyph_byte_size(gw, gh, format_);
        if (stored_bytes != req_bytes || stored_bytes > MaxGlyphBytes) {
            return false;
        }
        const uint64_t bmp_abs = static_cast<uint64_t>(bitmap_offset_) + static_cast<uint64_t>(rel_off);
        const uint64_t bmp_end = bmp_abs + static_cast<uint64_t>(stored_bytes);
        if (bmp_end > static_cast<uint64_t>(reader_size_)) {
            return false;
        }

        // 3. 选择空闲槽或最久未使用槽写入字模像素数据
        size_t victim = 0u;
        uint32_t min_stamp = cache_[0].last_used;
        for (size_t i = 0u; i < CacheSlots; ++i) {
            if (!cache_[i].valid) {
                victim = i;
                break;
            }
            if (cache_[i].last_used < min_stamp) {
                min_stamp = cache_[i].last_used;
                victim = i;
            }
        }

        CacheSlot& slot = cache_[victim];
        slot.valid = false; // 读失败时保持 fail-closed

        if (stored_bytes > 0u) {
            const size_t got = reader_->read_at(static_cast<uint32_t>(bmp_abs), slot.data, stored_bytes);
            if (got != stored_bytes) {
                return false;
            }
        }

        slot.codepoint = codepoint;
        slot.info.width = gw;
        slot.info.height = gh;
        slot.info.x_offset = gx;
        slot.info.y_offset = gy;
        slot.info.bitmap_idx = 0u;
        slot.info.x_advance = gadv;
        slot.bitmap_bytes = stored_bytes;
        slot.last_used = next_stamp();
        slot.valid = true;

        fill_view_from_slot(slot, out);
        return true;
    }

private:
    struct CacheSlot {
        bool valid{false};
        uint32_t last_used{0u};
        uint32_t codepoint{0u};
        GlyphInfo info{};
        uint16_t bitmap_bytes{0u};
        uint8_t data[MaxGlyphBytes]{};
    };

    uint32_t next_stamp() noexcept {
        if (stamp_ == 0xFFFFFFFFu) {
            for (size_t i = 0u; i < CacheSlots; ++i) {
                cache_[i].last_used = cache_[i].valid ? 1u : 0u;
            }
            stamp_ = 1u;
        }
        return ++stamp_;
    }

    void fill_view_from_slot(const CacheSlot& slot, FontGlyphView& out) const noexcept {
        out.info = slot.info;
        out.bitmap = (slot.bitmap_bytes > 0u) ? slot.data : nullptr;
        out.bitmap_bytes = slot.bitmap_bytes;
        out.format = format_;
        out.char_spacing = char_spacing_;
    }

    IFontReader* reader_{nullptr};
    uint32_t reader_size_{0u};
    FontPixelFormat format_{FontPixelFormat::MONO_1BPP_MSB};
    uint8_t line_height_{0u};
    uint8_t char_spacing_{1u};
    uint32_t glyph_count_{0u};
    uint32_t index_offset_{0u};
    uint32_t bitmap_offset_{0u};
    bool is_open_{false};
    uint32_t stamp_{0u};
    CacheSlot cache_[CacheSlots]{};
};

#endif // AURORA_BITMAP_FONT_HPP
