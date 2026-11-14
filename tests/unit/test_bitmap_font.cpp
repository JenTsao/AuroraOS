#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../drivers/display/bitmap_font.hpp"
#include "../../drivers/display/framebuffer.hpp"
#include "../../drivers/display/renderer2d.hpp"
#include "../../ui/font5x7_data.hpp"
#include "../../ui/ui_config.hpp"
#include "../../ui/vfs_font_reader.hpp"
#include "../../ui/widgets/text_view.hpp"
#include "../../vfs/vfs.hpp"

namespace {

void append_le16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void append_le32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

struct MockAfontGlyph {
    uint32_t codepoint;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t x_advance;
    std::vector<uint8_t> pixels;
};

std::vector<uint8_t> build_test_afont(FontPixelFormat fmt, uint8_t line_height, uint8_t spacing,
                                      std::vector<MockAfontGlyph> glyphs) {
    std::sort(glyphs.begin(), glyphs.end(), [](const MockAfontGlyph& a, const MockAfontGlyph& b) {
        return a.codepoint < b.codepoint;
    });

    const uint32_t count = static_cast<uint32_t>(glyphs.size());
    const uint32_t index_offset = AfontFormat::kHeaderBytes;
    const uint32_t bitmap_offset = index_offset + count * AfontFormat::kIndexEntryBytes;

    std::vector<uint8_t> out;
    append_le32(out, AfontFormat::kMagic);
    append_le16(out, AfontFormat::kVersion1);
    out.push_back(static_cast<uint8_t>(fmt));
    out.push_back(line_height);
    out.push_back(spacing);
    out.push_back(0u);
    out.push_back(0u);
    out.push_back(0u);
    append_le32(out, count);
    append_le32(out, index_offset);
    append_le32(out, bitmap_offset);

    std::vector<uint8_t> bitmap_blob;
    for (const auto& g : glyphs) {
        const uint32_t rel = static_cast<uint32_t>(bitmap_blob.size());
        const uint16_t stored = static_cast<uint16_t>(g.pixels.size());
        bitmap_blob.insert(bitmap_blob.end(), g.pixels.begin(), g.pixels.end());

        append_le32(out, g.codepoint);
        append_le32(out, rel);
        out.push_back(g.width);
        out.push_back(g.height);
        out.push_back(static_cast<uint8_t>(g.x_offset));
        out.push_back(static_cast<uint8_t>(g.y_offset));
        out.push_back(g.x_advance);
        out.push_back(0u);
        append_le16(out, stored);
    }

    out.insert(out.end(), bitmap_blob.begin(), bitmap_blob.end());
    return out;
}

class CountingReadOnlyVNode : public VNode {
public:
    explicit CountingReadOnlyVNode(std::vector<uint8_t> blob) : blob_(std::move(blob)) {}

    int read(char* buf, int len, int offset, void* /*priv*/ = nullptr) override {
        ++read_calls;
        if (!buf || len <= 0 || offset < 0 || static_cast<size_t>(offset) >= blob_.size()) {
            return 0;
        }
        const size_t avail = blob_.size() - static_cast<size_t>(offset);
        const size_t n = std::min(static_cast<size_t>(len), avail);
        std::memcpy(buf, blob_.data() + offset, n);
        bytes_read += n;
        return static_cast<int>(n);
    }

    int get_size(void* /*priv*/ = nullptr) const override {
        return static_cast<int>(blob_.size());
    }

    uint32_t read_calls{0u};
    size_t bytes_read{0u};

private:
    std::vector<uint8_t> blob_;
};

template <uint16_t W, uint16_t H> int count_non_bg_pixels(FrameBuffer<W, H>& fb, ColorRGB565 bg) {
    int count = 0;
    const ColorRGB565* raw = fb.get_raw_buffer();
    for (uint32_t i = 0u; i < static_cast<uint32_t>(W) * H; ++i) {
        if (raw[i] != bg) {
            ++count;
        }
    }
    return count;
}

} // namespace

// -----------------------------------------------------------------------------
// 1. RFC 3629 UTF-8 解析与畸形边界防护
// -----------------------------------------------------------------------------

TEST(BitmapFontTest, DecodeUtf8ValidMultiByteAndBoundaries) {
    const char* p = "A\xC2\xB0\xE5\xBF\x83\xE7\x8E\x87\xF0\x9F\x98\x80"; // "A°心率😀"
    std::vector<uint32_t> cps;
    uint32_t cp = 0u;
    while (decode_utf8(p, cp)) {
        cps.push_back(cp);
    }
    ASSERT_EQ(cps.size(), 5u);
    EXPECT_EQ(cps[0], static_cast<uint32_t>('A'));
    EXPECT_EQ(cps[1], 0x00B0u);  // '°'
    EXPECT_EQ(cps[2], 0x5FC3u);  // '心'
    EXPECT_EQ(cps[3], 0x7387u);  // '率'
    EXPECT_EQ(cps[4], 0x1F600u); // '😀'
    EXPECT_FALSE(decode_utf8(p, cp));
}

TEST(BitmapFontTest, DecodeUtf8MalformedNeverOverreadsOrStalls) {
    // 非法序列：截断 3 字节、孤立续字节、过长编码、UTF-16 代理项、> U+10FFFF
    const char* inputs[] = {
        "\xE5\xBF",         // NUL 截断的 3 字节头
        "\x80Z",            // 孤立续字节 + 'Z'
        "\xC0\xAF",         // 过长编码 '/'
        "\xED\xA0\x80",     // U+D800 UTF-16 代理项
        "\xF4\x90\x80\x80", // U+110000 越界
    };

    for (const char* s : inputs) {
        const char* p = s;
        uint32_t cp = 0u;
        ASSERT_TRUE(decode_utf8(p, cp));
        EXPECT_EQ(cp, 0xFFFDu);
        EXPECT_GT(p, s); // 单调前进，不死循环
    }

    const char* stray_then_ascii = "\x80K";
    uint32_t cp = 0u;
    ASSERT_TRUE(decode_utf8(stray_then_ascii, cp));
    EXPECT_EQ(cp, 0xFFFDu);
    ASSERT_TRUE(decode_utf8(stray_then_ascii, cp));
    EXPECT_EQ(cp, static_cast<uint32_t>('K'));
    EXPECT_FALSE(decode_utf8(stray_then_ascii, cp));

    const char* null_p = nullptr;
    EXPECT_FALSE(decode_utf8(null_p, cp));
}

// -----------------------------------------------------------------------------
// 2. 4bpp 抗锯齿混合 + 稀疏 Unicode Flash FontDef + 5x7 ASCII 回退
// -----------------------------------------------------------------------------

TEST(BitmapFontTest, Gray4BppGlyphBlendingAndMixedChineseAsciiFallback) {
    // 4bpp 字形：
    //  '心' (U+5FC3): 4x2，包含 0 / 8 / 15 / 4 等半字节覆盖度
    //    row0: [15, 8, 0, 15] -> 0xF8, 0x0F
    //    row1: [0, 15, 8, 4]  -> 0x0F, 0x84
    //  '率' (U+7387): 2x2 全亮 -> [15,15, 15,15] -> 0xFF, 0xFF
    static const uint8_t kZhBitmap[] = {
        0xF8u, 0x0Fu, 0x0Fu, 0x84u, // U+5FC3 (4 bytes)
        0xFFu, 0xFFu,               // U+7387 (2 bytes)
    };
    static const UnicodeGlyphEntry kZhEntries[] = {
        {0x5FC3u, {4u, 2u, 0, 1, 0u, 6u}},
        {0x7387u, {2u, 2u, 1, 1, 4u, 4u}},
    };
    const FontDef zh_font = {
        kZhBitmap, nullptr,    14u, static_cast<char>(0), static_cast<char>(0), FontPixelFormat::GRAY_4BPP,
        1u,        kZhEntries, 2u,
    };

    FrameBuffer<48, 24> fb;
    Renderer2D<48, 24> renderer(fb);
    const ColorRGB565 kBg = 0x2104u;
    const ColorRGB565 kFg = 0xFFFFu;
    renderer.clear(kBg);

    const char* mixed = "心率:7";
    const uint16_t measured = renderer.measure_string_utf8(mixed, zh_font, 1u, font5x7_data);
    const int16_t end_x = renderer.draw_string_utf8(2, 4, mixed, 1u, kFg, kBg, zh_font, true, font5x7_data);
    EXPECT_EQ(end_x - 2, measured);

    const ColorRGB565* raw = fb.get_raw_buffer();
    // '心' 在 (x=2, y=4+1=5)
    EXPECT_EQ(raw[5 * 48 + 2], kFg); // cov=15 -> 前景色
    const ColorRGB565 blended_mid = raw[5 * 48 + 3];
    EXPECT_NE(blended_mid, kBg); // cov=8 -> 半透明混色
    EXPECT_NE(blended_mid, kFg);
    EXPECT_EQ(raw[5 * 48 + 4], kBg); // cov=0 -> 透明保持底色
    EXPECT_EQ(raw[5 * 48 + 5], kFg);

    // 验证后面的回退 5x7 数字 '7' 也有实际像素写入（x = 2 + 6 + 4 + 12 = 24 起）
    int digit_pixels = 0;
    for (int y = 4; y < 18; ++y) {
        for (int x = 20; x < 36; ++x) {
            if (raw[y * 48 + x] != kBg) {
                ++digit_pixels;
            }
        }
    }
    EXPECT_GT(digit_pixels, 10);
}

// -----------------------------------------------------------------------------
// 3. .afont 流式按需二分查找 + 零堆 LRU 缓存 + VfsFontReader 集成
// -----------------------------------------------------------------------------

TEST(BitmapFontTest, StreamFontSourceVfsOnDemandLookupAndLruCache) {
    // 构造包含 64 个码点的 .afont 4bpp 字库（含 '中' U+4E2D, '文' U+6587, '你' U+4F60, '好' U+597D）
    std::vector<MockAfontGlyph> glyphs;
    for (uint32_t i = 0u; i < 60u; ++i) {
        glyphs.push_back(
            {0x8000u + i * 4u, 4u, 4u, 0, 0, 6u, {0xAAu, 0x55u, 0xAAu, 0x55u, 0xAAu, 0x55u, 0xAAu, 0x55u}});
    }
    glyphs.push_back({0x4E2Du, 2u, 2u, 0, 0, 4u, {0xF0u, 0x0Fu}}); // '中'
    glyphs.push_back({0x6587u, 2u, 2u, 0, 0, 4u, {0x0Fu, 0xF0u}}); // '文'
    glyphs.push_back({0x4F60u, 2u, 2u, 0, 0, 4u, {0xFFu, 0xFFu}}); // '你'
    glyphs.push_back({0x597Du, 2u, 2u, 0, 0, 4u, {0xF8u, 0x8Fu}}); // '好'

    const std::vector<uint8_t> afont_blob = build_test_afont(FontPixelFormat::GRAY_4BPP, 16u, 1u, glyphs);
    CountingReadOnlyVNode vnode(afont_blob);

    VfsManager::instance().init();
    ASSERT_TRUE(VfsManager::instance().mount("/lfs/zh_test.afont", &vnode));

    UI::VfsFontReader reader;
    ASSERT_TRUE(reader.open("/lfs/zh_test.afont"));
    EXPECT_EQ(reader.size(), afont_blob.size());

    StreamFontSource<64u, 2u> stream_font;
    ASSERT_TRUE(stream_font.open(&reader));
    EXPECT_EQ(stream_font.glyph_count(), 64u);
    EXPECT_EQ(stream_font.line_height(), 16u);
    EXPECT_EQ(stream_font.format(), FontPixelFormat::GRAY_4BPP);

    // 仅打开文件应只读取 24B 文件头，绝不整库加载
    EXPECT_LT(vnode.bytes_read, 64u);

    const uint32_t calls_before_lookup = vnode.read_calls;
    FontGlyphView gv{};
    ASSERT_TRUE(stream_font.lookup_glyph(0x4E2Du, gv)); // '中'
    EXPECT_EQ(gv.info.width, 2u);
    EXPECT_EQ(gv.bitmap_bytes, 2u);
    EXPECT_EQ(gv.bitmap[0], 0xF0u);
    EXPECT_EQ(gv.bitmap[1], 0x0Fu);
    const uint32_t calls_after_first = vnode.read_calls;
    // O(log2(64)) ~ 6 次索引 read + 1 次位图 read，远小于线性扫描 64 条
    EXPECT_LE(calls_after_first - calls_before_lookup, 8u);

    // 再次读取同一字形应 100% 命中 LRU 缓存（零 I/O）
    ASSERT_TRUE(stream_font.lookup_glyph(0x4E2Du, gv));
    EXPECT_EQ(vnode.read_calls, calls_after_first);

    // 读取 '文' 后缓存满 (CacheSlots=2)，两者均应在缓存内
    ASSERT_TRUE(stream_font.lookup_glyph(0x6587u, gv));
    EXPECT_EQ(gv.bitmap[0], 0x0Fu);
    const uint32_t calls_after_second = vnode.read_calls;
    ASSERT_TRUE(stream_font.lookup_glyph(0x4E2Du, gv));
    ASSERT_TRUE(stream_font.lookup_glyph(0x6587u, gv));
    EXPECT_EQ(vnode.read_calls, calls_after_second);

    // 触发 LRU 淘汰：刚刷新了 '文'，再读 '你' 会淘汰更早的 '中'，而 '文' 仍在缓存
    ASSERT_TRUE(stream_font.lookup_glyph(0x4F60u, gv));
    const uint32_t calls_after_third = vnode.read_calls;
    ASSERT_TRUE(stream_font.lookup_glyph(0x6587u, gv));
    EXPECT_EQ(vnode.read_calls, calls_after_third);
}

TEST(BitmapFontTest, StreamFontSourceHandlesMultiChunkLargeGlyphOverVfs) {
    // 验证 > 512 B（跨越 VfsManager 分块读取阈值）的较大 4bpp 字形可无损读出
    std::vector<uint8_t> big_pixels(720u);
    for (size_t i = 0u; i < big_pixels.size(); ++i) {
        big_pixels[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    const std::vector<uint8_t> blob =
        build_test_afont(FontPixelFormat::GRAY_4BPP, 40u, 2u, {{0x798Fu, 40u, 36u, 0, 0, 42u, big_pixels}});

    CountingReadOnlyVNode vnode(blob);
    VfsManager::instance().init();
    ASSERT_TRUE(VfsManager::instance().mount("/lfs/big.afont", &vnode));

    UI::VfsFontReader reader;
    ASSERT_TRUE(reader.open("/lfs/big.afont"));

    StreamFontSource<1024u, 2u> font;
    ASSERT_TRUE(font.open(&reader));

    FontGlyphView gv{};
    ASSERT_TRUE(font.lookup_glyph(0x798Fu, gv));
    ASSERT_EQ(gv.bitmap_bytes, 720u);
    EXPECT_EQ(std::memcmp(gv.bitmap, big_pixels.data(), 720u), 0);
}

TEST(BitmapFontTest, StreamFontSourceRejectsCorruptedOrOversizedFonts) {
    // 1. Bad magic
    std::vector<uint8_t> bad =
        build_test_afont(FontPixelFormat::GRAY_4BPP, 16u, 1u, {{'A', 2u, 2u, 0, 0, 4u, {0xFFu, 0xFFu}}});
    bad[0] = 'X';
    MemoryFontReader mr(bad.data(), static_cast<uint32_t>(bad.size()));
    StreamFontSource<64u, 2u> font;
    EXPECT_FALSE(font.open(&mr));

    // 2. Oversized glyph (> MaxGlyphBytes=16) must fail closed on lookup
    const std::vector<uint8_t> large = build_test_afont(
        FontPixelFormat::GRAY_4BPP, 16u, 1u, {{0x4E00u, 8u, 8u, 0, 0, 10u, std::vector<uint8_t>(32u, 0xFFu)}});
    mr.reset(large.data(), static_cast<uint32_t>(large.size()));
    StreamFontSource<16u, 2u> small_cache_font;
    ASSERT_TRUE(small_cache_font.open(&mr));
    FontGlyphView gv{};
    EXPECT_FALSE(small_cache_font.lookup_glyph(0x4E00u, gv));

    // 3. Truncated bitmap payload at EOF
    std::vector<uint8_t> truncated = build_test_afont(FontPixelFormat::GRAY_4BPP, 16u, 1u,
                                                      {{0x4E00u, 4u, 4u, 0, 0, 6u, std::vector<uint8_t>(8u, 0xFFu)}});
    truncated.pop_back();
    mr.reset(truncated.data(), static_cast<uint32_t>(truncated.size()));
    ASSERT_TRUE(font.open(&mr));
    EXPECT_FALSE(font.lookup_glyph(0x4E00u, gv));
}

// -----------------------------------------------------------------------------
// 4. TextView UTF-8 居中/右对齐 与 分带渲染一致性测试
// -----------------------------------------------------------------------------

TEST(BitmapFontTest, TextViewUtf8WidthAlignmentAndBandRender) {
    // ASCII '7' (4bpp) + 汉字 '心' (U+5FC3), '率' (U+7387)
    static const uint8_t kMixedBmp[] = {
        0xFFu, 0xFFu, 0xFFu, 0xFFu, // U+5FC3 (4x2 -> 4 bytes)
        0xFFu, 0xFFu, 0xFFu, 0xFFu, // U+7387 (4x2 -> 4 bytes)
    };
    static const UnicodeGlyphEntry kEntries[] = {
        {0x5FC3u, {4u, 2u, 0, 1, 0u, 6u}},
        {0x7387u, {4u, 2u, 0, 1, 4u, 6u}},
    };
    const FontDef font = {
        kMixedBmp, nullptr,  16u, static_cast<char>(0), static_cast<char>(0), FontPixelFormat::GRAY_4BPP,
        1u,        kEntries, 2u,
    };

    const char* utf8_text = "心率:7";
    // '心'(6) + '率'(6) + ':'(5x2+2=12) + '7'(12) = 36 px
    MemoryFontSource src(&font);
    EXPECT_EQ(measure_string_width_utf8(utf8_text, src, 1u, true), 36u);

    // 验证 TextView 右对齐在宽度 100 时应落在 x = 20 + (100 - 36) = 84 起
    UI::TextView tv(20, 34, 100, 16, utf8_text, 0xFFFFu, 0x0000u, 1u, UI::TextAlign::RIGHT);
    tv.set_font_source(&src);

    // 验证跨条带渲染 (band0: y=0..34, band1: y=35..69) 与整屏参考渲染完全等价
    constexpr uint16_t kBandH = AURORA_UI_BAND_H;
    static_assert(kBandH >= 35u, "Expected band height >= 35");

    FrameBuffer<DISPLAY_WIDTH, 70> full_fb;
    Renderer2D<DISPLAY_WIDTH, 70> full_r(full_fb);
    full_r.clear(0x1082u);
    full_r.draw_string_utf8(84, 34, utf8_text, 1u, 0xFFFFu, 0x0000u, src, true, font5x7_data);

    FrameBuffer<DISPLAY_WIDTH, kBandH> band0_fb;
    UI::UIRenderer band0_r(band0_fb);
    band0_r.clear(0x1082u);
    band0_r.set_band_origin(0);
    tv.draw(band0_r);

    FrameBuffer<DISPLAY_WIDTH, kBandH> band1_fb;
    UI::UIRenderer band1_r(band1_fb);
    band1_r.clear(0x1082u);
    band1_r.set_band_origin(kBandH);
    tv.draw(band1_r);

    for (uint16_t y = 30u; y < 52u; ++y) {
        const ColorRGB565* ref_row = full_fb.get_raw_buffer() + y * DISPLAY_WIDTH;
        const ColorRGB565* band_row = (y < kBandH) ? (band0_fb.get_raw_buffer() + y * DISPLAY_WIDTH)
                                                   : (band1_fb.get_raw_buffer() + (y - kBandH) * DISPLAY_WIDTH);
        EXPECT_EQ(std::memcmp(ref_row + 80, band_row + 80, 40u * sizeof(ColorRGB565)), 0)
            << "Mismatch at logical row " << y;
    }
    EXPECT_GT(count_non_bg_pixels(band1_fb, 0x1082u), 10);
}

TEST(BitmapFontTest, VNodeFontReaderFallbackSourceAndOpaque1BppScale2) {
    // 1. Primary 4bpp Chinese glyph in VNodeFontReader ('表' U+8868)
    const std::vector<uint8_t> primary_blob =
        build_test_afont(FontPixelFormat::GRAY_4BPP, 12u, 1u, {{0x8868u, 2u, 2u, 0, 0, 4u, {0xF8u, 0x4Fu}}});
    CountingReadOnlyVNode primary_vnode(primary_blob);
    UI::VNodeFontReader vnode_reader(&primary_vnode);
    StreamFontSource<64u, 2u> primary_font;
    ASSERT_TRUE(primary_font.open(&vnode_reader));

    // 2. Secondary 1bpp ASCII FontDef in Flash ('A' = 0x41)
    //    3x2 1bpp: bits [1,1,1, 1,0,1] -> 11110100 = 0xF4
    static const uint8_t kAscii1BppBmp[] = {0xF4u};
    static const GlyphInfo kAsciiGlyphs[] = {
        {3u, 2u, 0, 1, 0u, 4u},
    };
    const FontDef secondary_def = {
        kAscii1BppBmp, kAsciiGlyphs, 10u, 'A', 'A', FontPixelFormat::MONO_1BPP_MSB, 1u, nullptr, 0u,
    };
    MemoryFontSource secondary_font(&secondary_def, sizeof(kAscii1BppBmp));
    FallbackFontSource chain(&primary_font, &secondary_font);
    EXPECT_EQ(chain.line_height(), 12u);

    FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H> fb;
    UI::UIRenderer renderer(fb);
    const ColorRGB565 kBg = 0x001Fu;
    const ColorRGB565 kFg = 0xFFFFu;
    renderer.clear(0x0000u);

    // 组合字库渲染 "A表"，透明关闭 (bg = kBg) 且 scale = 2
    UI::TextView tv(2, 2, "A表", kFg, kBg, 2u, UI::TextAlign::LEFT);
    tv.set_font_source(&chain);
    tv.set_transparent_background(false);
    tv.draw(renderer);

    // 'A' 应在 (2, 2 + 1*2 = 4)，其中 (col=1, row=1) 是 0 位 -> 应绘制不透明底色 kBg
    const ColorRGB565* raw = fb.get_raw_buffer();
    EXPECT_EQ(raw[4 * DISPLAY_WIDTH + 2], kFg); // col=0,row=0 -> 1
    EXPECT_EQ(raw[6 * DISPLAY_WIDTH + 4], kBg); // col=1,row=1 (scale=2 -> x=2+2=4, y=4+2=6) -> 0 (opaque kBg)

    // '表' 起点 x = 2 + 4*2 = 10, y = 2；(col=1,row=0) cov=8 -> 与 kBg 半透明混色
    const ColorRGB565 mid = raw[2 * DISPLAY_WIDTH + 12];
    EXPECT_NE(mid, kBg);
    EXPECT_NE(mid, kFg);
    EXPECT_GT(primary_vnode.read_calls, 1u);
}
