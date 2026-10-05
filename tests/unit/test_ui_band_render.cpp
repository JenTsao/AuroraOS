// =============================================================================
// tests/unit/test_ui_band_render.cpp — 条带（band）渲染的像素级 golden 测试
//
// 覆盖「分带渲染只做了一半」这一缺陷的三条根因：
//   B-1  flush 必须接收屏幕绝对行号（framebuffer.hpp::flush 的 y_base）；
//   B-2  必须有分带循环（UiManager::render 逐带访问 + 裁剪到 damage ∩ band）；
//   B-3  整树重绘不再受 per-child 脏标记门控（否则第 2 条带起什么都画不出）。
//
// 测试策略：
//   1. ShadowPanel 模拟 ST7789 显存的「窗口 + 顺序写入」语义，于是「每条带被写到
//      屏幕的哪些绝对行」完全可观测 —— 这正是捕捉 B-1 的关键（若传相对行号，
//      所有条带都会落到屏幕顶部 0..34 行，oracle 比对立刻失败）。
//   2. 期望图像由**独立 oracle** 直接按场景语义算出（不使用 Renderer2D），
//      避免「参考实现与待测实现同源」导致测试失去判别力。
//   3. 场景只用纯色矩形，不依赖字模/位图，杜绝字体相关的 flaky。
//
// ⚠️ 本文件刻意**不**通过 #define 覆盖 AURORA_FB_CHUNK_HEIGHT：该宏会改变
//    UI::UIRenderer 的类型，进而改变 View::draw / View::render_all 的签名。
//    若在同一个测试可执行文件的不同 TU 里取不同值，会直接踩 ODR（vtable 同名
//    不同签名）。残带守卫因此改用 band_detail 的纯函数单测覆盖（见文件末尾）。
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../ui/ui_config.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/screen.hpp"
#include "../../ui/widgets/scroll_view.hpp"
#include "../../ui/widgets/scroll_view.hpp"

using namespace UI;

namespace {

constexpr uint16_t kW = static_cast<uint16_t>(DISPLAY_WIDTH);
constexpr uint16_t kH = static_cast<uint16_t>(DISPLAY_HEIGHT);
constexpr uint16_t kBandH = static_cast<uint16_t>(AURORA_UI_BAND_H);

// miband8 的板级约束（同一断言在 apps/watch/watch_app.cpp 里是编译期硬错误）。
static_assert(DISPLAY_HEIGHT % AURORA_UI_BAND_H == 0,
              "miband8: DISPLAY_HEIGHT must be divisible by AURORA_FB_CHUNK_HEIGHT");

// 文件内静态帧缓冲：test_lua_vm.cpp 已占用全局符号 g_fb，全部测试链接成单一
// 可执行文件，故置于匿名命名空间避免重名链接错误。
FrameBuffer<kW, kBandH> s_band_fb;

// ---------------------------------------------------------------------------
// 场景模型：Screen 默认整屏填黑，之后按加入顺序叠加若干纯色矩形
// （ViewGroup::draw 按 children_ 顺序绘制，后画的覆盖先画的 → 后者在上层）
// ---------------------------------------------------------------------------
struct RectSpec {
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint16_t color;
};

// 独立 oracle：不使用 Renderer2D，直接按场景语义算出 (x, y) 处的应有颜色。
uint16_t expected_pixel(const std::vector<RectSpec>& scene, int x, int y) {
    uint16_t c = 0x0000; // Screen::draw 的默认背景（纯黑）
    for (const RectSpec& s : scene) {
        if (x >= s.x && x < static_cast<int>(s.x) + s.w && y >= s.y && y < static_cast<int>(s.y) + s.h) {
            c = s.color;
        }
    }
    return c;
}

// 纯色矩形叶子：渲染结果完全确定，不依赖字体/位图。
class SolidView : public View {
public:
    explicit SolidView(const RectSpec& s) : View(s.x, s.y, s.w, s.h), color_(s.color) {}

    void draw(UIRenderer& r) override {
        r.fill_rect(x_, y_, width_, height_, color_);
    }

private:
    uint16_t color_;
};

// ---------------------------------------------------------------------------
// ShadowPanel：模拟 ST7789 显存的窗口写入语义（不含面板显示偏移）
//   - set_window 按整屏尺寸钳制（与 SpiLcdDriverBase::set_window 的钳制一致）
//   - write_patch 自窗口左上角逐像素顺序写入，行满换行、窗口写满即止
// ---------------------------------------------------------------------------
class ShadowPanel {
public:
    static constexpr uint16_t kSentinel = 0xFFFF;

    ShadowPanel() : gram_(static_cast<size_t>(kW) * kH, kSentinel) {}

    void reset() {
        gram_.assign(static_cast<size_t>(kW) * kH, kSentinel);
        max_row_ = -1;
        windows_ = 0;
        last_win_y1_ = 0;
    }

    void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        if (x0 >= kW) x0 = static_cast<uint16_t>(kW - 1);
        if (y0 >= kH) y0 = static_cast<uint16_t>(kH - 1);
        if (x1 >= kW) x1 = static_cast<uint16_t>(kW - 1);
        if (y1 >= kH) y1 = static_cast<uint16_t>(kH - 1);
        win_x0_ = x0;
        win_y0_ = y0;
        win_x1_ = x1;
        win_y1_ = y1;
        cur_x_ = x0;
        cur_y_ = y0;
        last_win_y1_ = y1;
        ++windows_;
    }

    void write_patch(const uint16_t* px, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            if (cur_x_ > win_x1_) {
                cur_x_ = win_x0_;
                ++cur_y_;
            }
            if (cur_y_ > win_y1_)
                return; // 窗口已写满，越界部分丢弃
            gram_[static_cast<size_t>(cur_y_) * kW + cur_x_] = px[i];
            if (cur_y_ > max_row_)
                max_row_ = cur_y_;
            ++cur_x_;
        }
    }

    uint16_t at(int x, int y) const {
        return gram_[static_cast<size_t>(y) * kW + x];
    }

    int32_t max_row() const {
        return max_row_;
    }

    int windows() const {
        return windows_;
    }

    uint16_t last_win_y1() const {
        return last_win_y1_;
    }

private:
    std::vector<uint16_t> gram_;
    uint16_t win_x0_ = 0, win_y0_ = 0, win_x1_ = 0, win_y1_ = 0;
    uint16_t cur_x_ = 0, cur_y_ = 0;
    uint16_t last_win_y1_ = 0;
    int32_t max_row_ = -1;
    int windows_ = 0;
};

// 条带落屏接收器：begin_band 返回共享渲染器，end_band 走真实的
// FrameBuffer::flush(driver, y_base) → ShadowPanel。
class TestBandSink : public IBandSink {
public:
    UIRenderer* renderer = nullptr;
    FrameBuffer<kW, kBandH>* fb = nullptr;
    ShadowPanel panel;

    int bands = 0;
    int64_t first_top = -1;
    int64_t last_top = -1;
    uint16_t last_rows = 0;

    UIRenderer& begin_band(int16_t logical_top, uint16_t rows) noexcept override {
        if (bands == 0)
            first_top = logical_top;
        ++bands;
        last_top = logical_top;
        last_rows = rows;
        return *renderer;
    }

    void end_band(int16_t logical_top, uint16_t /*rows*/) noexcept override {
        fb->flush(panel, static_cast<uint16_t>(logical_top));
    }
};

// 计数根：只统计 draw() 被调用的次数，不做任何像素操作。
class CountingRoot : public ViewGroup {
public:
    int draws = 0;

    CountingRoot() : ViewGroup(0, 0, kW, kH) {}

    void draw(UIRenderer& r) override {
        ++draws;
        ViewGroup::draw(r);
    }
};

// 什么都不画的叶子：仅用于触发子控件级 invalidate()。
class NoopLeaf : public View {
public:
    NoopLeaf(int16_t x, int16_t y, uint16_t w, uint16_t h) : View(x, y, w, h) {}

    void draw(UIRenderer&) override {}
};

struct Mismatch {
    int x = -1;
    int y = -1;
    uint16_t got = 0;
    uint16_t want = 0;
    uint32_t count = 0;

    bool none() const {
        return count == 0;
    }
};

Mismatch diff_rect(const ShadowPanel& panel, const std::vector<RectSpec>& scene, int x0, int y0, int x1, int y1) {
    Mismatch m;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const uint16_t want = expected_pixel(scene, x, y);
            const uint16_t got = panel.at(x, y);
            if (got != want) {
                if (m.count == 0) {
                    m.x = x;
                    m.y = y;
                    m.got = got;
                    m.want = want;
                }
                ++m.count;
            }
        }
    }
    return m;
}

uint32_t count_sentinel(const ShadowPanel& panel) {
    uint32_t n = 0;
    for (uint16_t y = 0; y < kH; ++y) {
        for (uint16_t x = 0; x < kW; ++x) {
            if (panel.at(x, y) == ShadowPanel::kSentinel)
                ++n;
        }
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// 公共夹具：每个用例前后把 UiManager 的全局绑定清干净，避免跨用例污染
// （UiManager 是进程级单例，测试链接成单一可执行文件）。
// ---------------------------------------------------------------------------
class BandRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        UiManager::instance().set_band_sink(nullptr);
        UiManager::instance().set_renderer(nullptr);
        UiManager::instance().set_root_view(nullptr);
    }

    void TearDown() override {
        UiManager& ui = UiManager::instance();
        ui.set_band_sink(nullptr);
        ui.set_renderer(nullptr);
        ViewGroup* root = ui.get_root_view();
        ui.set_root_view(nullptr); // 先解绑，再释放（根拥有其子控件）
        delete root;
    }
};

// =============================================================================
// 1. 分带等价性（整屏损伤）：14 条带的合成结果必须逐像素等于独立 oracle
// =============================================================================
TEST_F(BandRenderTest, FullDamageBandedOutputMatchesOracle) {
    const std::vector<RectSpec> scene = {
        {10, 10, 40, 40, 0xF800},  // 第 0 条带
        {100, 300, 50, 30, 0x07E0}, // 第 8 条带
        {0, 455, 192, 35, 0x001F},  // 最后一条带，整条铺满
    };

    TestBandSink sink;
    UI::UIRenderer r(s_band_fb);
    sink.renderer = &r;
    sink.fb = &s_band_fb;

    UiManager& ui = UiManager::instance();
    ui.set_renderer(&r);
    ui.set_band_sink(&sink);

    Screen* root = new Screen();
    for (const RectSpec& s : scene) {
        root->add_child(new SolidView(s));
    }
    ui.set_root_view(root);
    (void)root->take_damage(); // 排空建树期（push / add_child）累积的基线脏区
    root->invalidate();        // 整屏脏

    sink.panel.reset();
    ui.render();

    // 490 / 35 = 14 条带；第一条从 0 起，最后一条从 455 起、恰好 35 行。
    EXPECT_EQ(sink.bands, 14);
    EXPECT_EQ(sink.first_top, 0);
    EXPECT_EQ(sink.last_top, 455);
    EXPECT_EQ(sink.last_rows, kBandH);
    // 每条带一次 set_window。
    EXPECT_EQ(sink.panel.windows(), 14);
    // B-1 的直接断言：最后一行 y=489 必须被写到，且窗口绝不越过面板底边。
    EXPECT_EQ(sink.panel.max_row(), static_cast<int32_t>(kH) - 1);
    EXPECT_EQ(sink.panel.last_win_y1(), static_cast<uint16_t>(kH - 1));

    const Mismatch m = diff_rect(sink.panel, scene, 0, 0, kW - 1, kH - 1);
    EXPECT_TRUE(m.none()) << "首个不匹配 (" << m.x << "," << m.y << ") got=0x" << std::hex << m.got << " want=0x"
                          << m.want << std::dec << "，共 " << m.count << " / " << (kW * kH) << " 处";

    // 条带原点必须复位，避免跨帧污染。
    EXPECT_EQ(r.get_band_origin(), 0u);
}

// =============================================================================
// 2. 局部损伤：只访问与之相交的条带，且损伤区外一个像素都不推送
// =============================================================================
TEST_F(BandRenderTest, PartialDamageTouchesOnlyIntersectingBands) {
    const std::vector<RectSpec> scene = {
        {10, 10, 40, 40, 0xF800},
        {100, 300, 50, 30, 0x07E0}, // 目标：y 300..329 横跨两条带
        {0, 455, 192, 35, 0x001F},
    };

    TestBandSink sink;
    UI::UIRenderer r(s_band_fb);
    sink.renderer = &r;
    sink.fb = &s_band_fb;

    UiManager& ui = UiManager::instance();
    ui.set_renderer(&r);
    ui.set_band_sink(&sink);

    Screen* root = new Screen();
    SolidView* hot = nullptr;
    for (const RectSpec& s : scene) {
        SolidView* v = new SolidView(s);
        if (s.color == 0x07E0)
            hot = v;
        root->add_child(v);
    }
    ASSERT_NE(hot, nullptr);

    ui.set_root_view(root);
    (void)root->take_damage();

    hot->invalidate(); // 只有中间那块脏

    sink.panel.reset();
    ui.render();

    // y 300..329 跨越 band#8（top=280）与 band#9（top=315）→ 恰好两条带。
    EXPECT_EQ(sink.bands, 2);
    EXPECT_EQ(sink.first_top, 280);
    EXPECT_EQ(sink.last_top, 315);
    EXPECT_EQ(sink.last_rows, kBandH);

    const Mismatch m = diff_rect(sink.panel, scene, 100, 300, 149, 329);
    EXPECT_TRUE(m.none()) << "损伤区内首个不匹配 (" << m.x << "," << m.y << ") got=0x" << std::hex << m.got
                          << " want=0x" << m.want << std::dec << "，共 " << m.count << " 处";

    // 损伤区（50x30 = 1500 px）之外必须是哨兵 → 证明没有超量推送。
    const uint32_t written = static_cast<uint32_t>(kW) * kH - count_sentinel(sink.panel);
    EXPECT_EQ(written, 50u * 30u) << "实际推送像素数应与损伤面积一致";
}

// =============================================================================
// 3. 帧门控 + 端到端重绘（历史缺陷「子控件更新后永不重绘」的验收）
// =============================================================================
TEST_F(BandRenderTest, FrameGatingAndEndToEndRedraw) {
    TestBandSink sink;
    UI::UIRenderer r(s_band_fb);
    sink.renderer = &r;
    sink.fb = &s_band_fb;

    UiManager& ui = UiManager::instance();
    ui.set_renderer(&r);
    ui.set_band_sink(&sink);

    CountingRoot* root = new CountingRoot();
    NoopLeaf* leaf = new NoopLeaf(0, 0, 20, 20);
    root->add_child(leaf);
    ui.set_root_view(root);

    // ① 首帧：整屏脏 → 14 条带；每条带整树重绘一次（render_all 是逐带调用）
    ui.render();
    EXPECT_EQ(sink.bands, 14);
    EXPECT_EQ(root->draws, 14);

    // ② 无变更：damage 为空 → 不空转（0 draw + 0 flush）
    ui.render();
    EXPECT_EQ(sink.bands, 14);
    EXPECT_EQ(root->draws, 14);

    // ③ 子控件级变更必须驱动重绘：损伤只有 30x30，落在第 0 条带
    //    → 恰好增加 1 条带、1 次整树重绘。
    //    旧门控（is_dirty()）下本断言必 FAIL：子控件 invalidate() 只把世界矩形
    //    累积到根的 damage_，不再置根的 is_dirty_，于是渲染被永久 skip。
    leaf->set_size(30, 30);
    ui.render();
    EXPECT_EQ(sink.bands, 15); // 端到端「真的重画了」
    EXPECT_EQ(root->draws, 15); // 且只重绘相交的那一条带

    // ④ damage 已消费 → 不重复重绘
    ui.render();
    EXPECT_EQ(sink.bands, 15);
    EXPECT_EQ(root->draws, 15);
}

// =============================================================================
// 4. 残带守卫（纯函数单测）
//
// 面板高未必是条带高的整数倍。分带循环里的 rows = min(band_h, panel_h - top)
// 必须让最后一条带「刚好抵达面板底行」——不缺（漏画底行）也不溢（越界写显存）。
// 这里直接对 UiManager 分带循环实际调用的纯函数做穷举，覆盖：
//   490/35（miband8 主配置，整除）、490/30（旧配置，残带 10 行）、
//   490/64、128/35（nucleo）、16/35 与 16/16（lm3s6965，条带高被裁到面板高）。
// =============================================================================
TEST(BandGeometry, RemainderGuardCoversPanelExactly) {
    struct Case {
        int16_t panel_h;
        int16_t band_h;
        int bands;
        int16_t last_top;
        uint16_t last_rows;
    };

    const Case cases[] = {
        {490, 35, 14, 455, 35}, // miband8 主配置：14 条带，无残带
        {490, 30, 17, 480, 10}, // 旧配置 30：16*30=480，残带 10 行
        {490, 64, 8, 448, 42},  // 非整除
        {128, 35, 4, 105, 23},  // nucleo：128 不被 35 整除
        {128, 1, 128, 127, 1},
        {16, 16, 1, 0, 16},   // lm3s6965 自适应后：单带 = 整屏
        {16, 35, 1, 0, 16},   // 防御：条带高大于面板高时仍只覆盖 16 行
        {1, 35, 1, 0, 1},
    };

    for (const Case& c : cases) {
        int bands = 0;
        int16_t last_top = -1;
        uint16_t last_rows = 0;
        const int16_t last_row = static_cast<int16_t>(c.panel_h - 1);

        for (int16_t top = band_detail::first_top_for(0, c.band_h); top <= last_row;
             top = static_cast<int16_t>(top + c.band_h)) {
            const uint16_t rows = band_detail::rows_at(top, c.panel_h, c.band_h);
            if (rows == 0)
                break;
            ++bands;
            last_top = top;
            last_rows = rows;
        }

        EXPECT_EQ(bands, c.bands) << "panel_h=" << c.panel_h << " band_h=" << c.band_h;
        EXPECT_EQ(last_top, c.last_top) << "panel_h=" << c.panel_h << " band_h=" << c.band_h;
        EXPECT_EQ(last_rows, c.last_rows) << "panel_h=" << c.panel_h << " band_h=" << c.band_h;
        // 覆盖性：最后一条带必须正好抵达面板底行。
        EXPECT_EQ(static_cast<int16_t>(last_top + static_cast<int16_t>(last_rows) - 1), last_row)
            << "panel_h=" << c.panel_h << " band_h=" << c.band_h;
    }
}

// =============================================================================
// 5. 滚动容器内的子控件变更：脏区必须落在「视口所见的屏幕行」上
//
// 缺陷根因：子控件坐标是 ScrollView 的**内容坐标**，而 damage 冒泡用的是
// **世界（屏幕）坐标**，两者相差一个 (scroll_x, scroll_y)。世界矩形若不减去祖先
// 滚动偏移，滚动后变更就会把脏区登记到屏外的内容坐标上，分带循环据此选带
// → 真正改变的那一屏行永不被重绘（画面卡住，直到一次整屏脏才恢复）。
//
// 场景（内容坐标 → scroll_y=200 → 屏幕坐标）：
//   hot    y 300..319 → 屏幕 100..119   ← 本例把它移到 310 → 屏幕 110..129
//   anchor y 250..279 → 屏幕  50..79    ← 不动，且不在脏区内，应完全不被推送
// =============================================================================
TEST_F(BandRenderTest, ScrolledChildDamageLandsOnViewportRows) {
    TestBandSink sink;
    UI::UIRenderer r(s_band_fb);
    sink.renderer = &r;
    sink.fb = &s_band_fb;

    UiManager& ui = UiManager::instance();
    ui.set_renderer(&r);
    ui.set_band_sink(&sink);

    Screen* root = new Screen();
    ScrollView* sv = new ScrollView(0, 0, kW, 200);
    sv->set_scrollbar_visible(false); // 像素只由场景决定，杜绝滚动条干扰
    sv->set_content_size(kW, 400);
    SolidView* hot = new SolidView({10, 300, 40, 20, 0xF800});
    SolidView* anchor = new SolidView({100, 250, 30, 30, 0x07E0});
    sv->add_child(anchor);
    sv->add_child(hot);
    root->add_child(sv);

    sv->scroll_to(0, 200); // max_scroll = 400 - 200 = 200（已到下限）

    // ① 纯几何：世界矩形必须已扣掉滚动偏移
    EXPECT_EQ(hot->world_bounds().y, 100);
    EXPECT_EQ(anchor->world_bounds().y, 50);

    // ② 基线整屏帧：滚动后可见内容应与 oracle 一致
    ui.set_root_view(root);
    (void)root->take_damage(); // 排空建树/滚动期累积的基线脏区
    root->invalidate();
    ui.render();
    const std::vector<RectSpec> baseline = {
        {100, 50, 30, 30, 0x07E0}, // anchor
        {10, 100, 40, 20, 0xF800}, // hot
    };
    const Mismatch m0 = diff_rect(sink.panel, baseline, 0, 0, kW - 1, kH - 1);
    EXPECT_TRUE(m0.none()) << "基线帧首个不匹配 (" << m0.x << "," << m0.y << ")";

    // ③ 增量帧：移动滚动容器内的子控件，脏区必须落在可见行
    sink.panel.reset();
    sink.bands = 0;
    hot->set_position(10, 310); // 屏幕 110..129；旧位置 100..109 需被擦除
    ui.render();

    // union(旧,新) = y 100..129 → band#2(top=70, 覆盖 100..104) 与 band#3(top=105)。
    EXPECT_EQ(sink.bands, 2);
    EXPECT_EQ(sink.first_top, 70);
    EXPECT_EQ(sink.last_top, 105);

    // hot 只占 x 10..49，其余列本就未被推送（保持哨兵），故比对限定该列区间。
    const std::vector<RectSpec> after = {
        {10, 110, 40, 20, 0xF800}, // hot 移动后；100..109 应为黑（旧位置已擦除）
    };
    const Mismatch m1 = diff_rect(sink.panel, after, 10, 100, 49, 129);
    EXPECT_TRUE(m1.none()) << "增量帧首个不匹配 (" << m1.x << "," << m1.y << ") got=0x" << std::hex << m1.got
                           << " want=0x" << m1.want << std::dec << "，共 " << m1.count << " 处";

    // anchor 不在脏区 → 一条像素都不该被推到它所在的行（证明没有退化成整屏刷）
    EXPECT_EQ(sink.panel.at(100, 50), ShadowPanel::kSentinel);
}
