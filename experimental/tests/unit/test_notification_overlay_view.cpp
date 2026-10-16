// test_notification_overlay_view.cpp — 通知浮层固件侧实现单元测试
//
// 覆盖 experimental/apps/notification_center.hpp 之外、本次新增的两块：
//   1. apps/watch/notification_overlay_view.hpp
//      —— INotificationOverlay 端口的 UI::View 实现（状态机 + 手势 + 像素）
//   2. ui/overlay_root_view.hpp
//      —— 浮层根容器的绘制顺序、手势路由与「非拥有」析构契约
//
// 之所以能在主机上跑：ui_config.hpp 在 AURORA_HOST_TEST 下用宏给出
// DISPLAY_WIDTH/HEIGHT 而不引 board.h（tests/stubs/board.h 亦给同值），
// FrameBuffer 默认构造不碰硬件驱动（驱动只在 flush() 才用到），因此可以
// 真开一块 192x35 显存跑 Renderer2D 断言像素，而不是只 mock 接口。
//
// 注意：NotificationOverlayView::handle_gesture 内部调用
// NotificationCenter 单例的 dismiss_current()，故每个用例都必须
// set_overlay(本对象) + clear()，避免跨用例污染全局单例。

#include <gtest/gtest.h>

#include <memory>

#ifndef AURORA_HOST_TEST
#define AURORA_HOST_TEST
#endif

#include "../../../ui/overlay_root_view.hpp"
#include "../../../apps/watch/notification_overlay_view.hpp"

using aurora::Notification;
using aurora::NotificationCategory;
using aurora::NotificationCenter;
using aurora::NotificationOverlayView;
using aurora::NotificationPriority;

namespace {

Notification make(NotificationPriority p, NotificationCategory cat, const char* title, const char* body) {
    Notification n{};
    n.id = 1;
    n.priority = p;
    n.category = cat;
    for (int i = 0; i < Notification::kTitleMaxLen && title && title[i]; ++i) {
        n.title[i] = title[i];
    }
    for (int i = 0; i < Notification::kBodyMaxLen && body && body[i]; ++i) {
        n.body[i] = body[i];
    }
    return n;
}

using Fb = FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H>;

} // namespace

// =============================================================
// 状态机：可见性 / 模式 / 几何
// =============================================================
class OverlayViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        overlay_ = std::make_unique<NotificationOverlayView>();
        NotificationCenter::instance().set_overlay(overlay_.get());
    }

    void TearDown() override {
        NotificationCenter::instance().set_overlay(nullptr);
        NotificationCenter::instance().clear();
        overlay_.reset();
    }

    std::unique_ptr<NotificationOverlayView> overlay_;
};

TEST_F(OverlayViewTest, StartsHiddenAndGone) {
    EXPECT_FALSE(overlay_->is_visible());
    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::hidden);
    // 初始即 GONE：浮层不该在首帧白白占一次遍历，也不该命中手势
    EXPECT_EQ(overlay_->get_visibility(), UI::Visibility::GONE);
}

TEST_F(OverlayViewTest, NormalNotificationShowsBannerWithBannerHeight) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::message, "Alice", "hi"));

    EXPECT_TRUE(overlay_->is_visible());
    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::banner);
    EXPECT_EQ(overlay_->get_height(), NotificationOverlayView::kBannerHeight);
    EXPECT_EQ(overlay_->get_visibility(), UI::Visibility::VISIBLE);
    EXPECT_STREQ(overlay_->current().title, "Alice");
}

TEST_F(OverlayViewTest, CriticalPriorityEscalatesToFullscreen) {
    overlay_->show(make(NotificationPriority::critical, NotificationCategory::app, "SOS", "help"));

    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::fullscreen);
    EXPECT_EQ(overlay_->get_height(), static_cast<uint16_t>(DISPLAY_HEIGHT));
    EXPECT_TRUE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, CallCategoryEscalatesToFullscreenEvenAtLowPriority) {
    // 枚举约定：category == call 一律全屏，与 priority 无关
    overlay_->show(make(NotificationPriority::low, NotificationCategory::call, "Mom", "incoming call"));

    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::fullscreen);
}

// =============================================================
// banner 超时
// =============================================================
TEST_F(OverlayViewTest, BannerAutoHidesAfterDuration) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::message, "t", "b"));

    // 差 1ms 不收
    overlay_->tick(NotificationOverlayView::kBannerDurationMs - 1);
    EXPECT_TRUE(overlay_->is_visible());

    overlay_->tick(1);
    EXPECT_FALSE(overlay_->is_visible());
    EXPECT_EQ(overlay_->get_visibility(), UI::Visibility::GONE);
}

TEST_F(OverlayViewTest, FullscreenDoesNotAutoHide) {
    overlay_->show(make(NotificationPriority::critical, NotificationCategory::app, "SOS", "help"));

    // 全屏弹窗必须等用户手势，tick 再多次也不能自己消失
    for (int i = 0; i < 100; ++i) {
        overlay_->tick(1000);
    }
    EXPECT_TRUE(overlay_->is_visible());
    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::fullscreen);
}

TEST_F(OverlayViewTest, BannerTimerResetsOnEachShow) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "a", "1"));
    overlay_->tick(2000);
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "b", "2"));

    // 新通知应重新计满 3s，而不是接着上一条的剩余时间
    overlay_->tick(2000);
    EXPECT_TRUE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, DismissWithEmptyQueueLeavesNothingVisible) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "a", "1"));
    NotificationCenter::instance().dismiss_current();

    EXPECT_FALSE(overlay_->is_visible());
    EXPECT_EQ(overlay_->get_mode(), NotificationOverlayView::DisplayMode::hidden);
}

// =============================================================
// 手势：按呈现模式区分权限
// =============================================================
// 背景：watch_app.cpp 的 handle_key_event() 把侧键短按映射成 SWIPE_RIGHT
// （语义「返回上一页」），因此 banner 绝不能吞掉 SWIPE_RIGHT，否则用户在
// 看表时收到消息、按侧键想返回，结果只是关掉一条 3 秒后自己就消失的提示。
// fullscreen 是真模态，才拦右滑。
// =============================================================
TEST_F(OverlayViewTest, FullscreenConsumesSwipeRightAndPopsNextQueued) {
    overlay_->show(make(NotificationPriority::critical, NotificationCategory::app, "first", "1"));
    NotificationCenter::instance().post(make(NotificationPriority::critical, NotificationCategory::app, "second", "2"));
    ASSERT_EQ(NotificationCenter::instance().pending_count(), 1);

    GestureEvent swipe = {GestureType::SWIPE_RIGHT, 96, 40, 40, 0, 300, false};
    EXPECT_TRUE(overlay_->handle_gesture(swipe));

    // 走的是 NotificationCenter 而非直接 hide()：队列里的条目必须顶上
    EXPECT_TRUE(overlay_->is_visible());
    EXPECT_STREQ(overlay_->current().title, "second");
    EXPECT_EQ(NotificationCenter::instance().pending_count(), 0);
}

TEST_F(OverlayViewTest, BannerDoesNotConsumeSwipeRightSoSideKeyStillNavigates) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "a", "1"));

    GestureEvent swipe = {GestureType::SWIPE_RIGHT, 96, 40, 40, 0, 300, false};
    // 放行 → 事件继续下沉到 ScreenNavigator，页面照常返回
    EXPECT_FALSE(overlay_->handle_gesture(swipe));
    EXPECT_TRUE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, TapInsideBannerDismisses) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "a", "1"));

    // 横幅占 (0,0,192,80)，点在其内部
    GestureEvent tap = {GestureType::TAP, 96, 40, 0, 0, 80, false};
    EXPECT_TRUE(overlay_->handle_gesture(tap));
    EXPECT_FALSE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, TapBelowBannerIsNotConsumed) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::app, "a", "1"));

    // 点在横幅之外的页面区域：必须透传给页面控件，不能被 toast 吞掉
    GestureEvent tap_below = {GestureType::TAP, 96, 300, 0, 0, 80, false};
    EXPECT_FALSE(overlay_->handle_gesture(tap_below));
    EXPECT_TRUE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, OtherGesturesAreNotConsumed) {
    overlay_->show(make(NotificationPriority::critical, NotificationCategory::app, "SOS", "1"));

    // 即便在全屏模态下，左滑/上滑/长按也不该被浮层吃掉（保留页面级能力）
    GestureEvent left = {GestureType::SWIPE_LEFT, 96, 40, -40, 0, 300, false};
    GestureEvent up = {GestureType::SWIPE_UP, 96, 40, 0, -40, 300, false};
    GestureEvent long_press = {GestureType::LONG_PRESS, 96, 40, 0, 0, 800, false};

    EXPECT_FALSE(overlay_->handle_gesture(left));
    EXPECT_FALSE(overlay_->handle_gesture(up));
    EXPECT_FALSE(overlay_->handle_gesture(long_press));
    EXPECT_TRUE(overlay_->is_visible());
}

TEST_F(OverlayViewTest, GesturesIgnoredWhileHidden) {
    GestureEvent swipe = {GestureType::SWIPE_RIGHT, 96, 40, 40, 0, 300, false};
    GestureEvent tap = {GestureType::TAP, 96, 40, 0, 0, 80, false};
    EXPECT_FALSE(overlay_->handle_gesture(swipe));
    EXPECT_FALSE(overlay_->handle_gesture(tap));
}

// =============================================================
// 像素：真实 Renderer2D 跑一遍 draw
// =============================================================
//
// 关键前提：Fb 是 **条带** 高度（AURORA_UI_BAND_H = 35），而视图坐标系覆盖
// 整屏 490 行 —— 这正是 miband8 的渲染模型。Renderer2D 把「屏幕第 y 行」
// 映射到「条带第 y - band_origin 行」，所以每个断言都必须先 set_band_origin
// 再取像素，且索引不能越过条带高度（越界读会拿到垃圾值甚至被 ASAN 拦下）。
// =============================================================
TEST_F(OverlayViewTest, DrawWritesBannerBackgroundAndCategoryTagAcrossBands) {
    overlay_->show(make(NotificationPriority::normal, NotificationCategory::message, "Msg", "body text"));
    ASSERT_EQ(overlay_->get_height(), NotificationOverlayView::kBannerHeight); // 80 > 单条带 35

    // ── 条带 0：承载横幅的逻辑第 0..34 行 ──────────────────────
    {
        auto fb = std::make_unique<Fb>();
        UI::UIRenderer r(*fb);
        r.set_band_origin(0);
        overlay_->draw(r);

        const ColorRGB565* buf = fb->get_raw_buffer();
        // 横幅内部（x=100 避开左侧色条与 6px 圆角，y=20 落在本条带内）
        EXPECT_EQ(buf[20 * DISPLAY_WIDTH + 100], NotificationOverlayView::kBgBanner);
        // 左侧 4px 分类色条：message → 0x07E0
        EXPECT_EQ(buf[20 * DISPLAY_WIDTH + 1], static_cast<ColorRGB565>(0x07E0));
    }

    // ── 条带 1：承载横幅的逻辑第 35..69 行 ─────────────────────
    // 固件里 UiManager::render() 的分带循环会对每条带各调一次 draw()，
    // 横幅跨条带时两段都必须被画出来，否则 490 行面板上横幅只有顶上一条带可见。
    {
        auto fb = std::make_unique<Fb>();
        UI::UIRenderer r(*fb);
        r.set_band_origin(AURORA_UI_BAND_H); // 原点 = 35
        overlay_->draw(r);

        const ColorRGB565* buf = fb->get_raw_buffer();
        // 逻辑 y=55 → 条带缓冲第 55-35 = 20 行
        EXPECT_EQ(buf[20 * DISPLAY_WIDTH + 100], NotificationOverlayView::kBgBanner);
    }
}

TEST_F(OverlayViewTest, DrawIsNoOpWhileHidden) {
    auto fb = std::make_unique<Fb>();
    UI::UIRenderer r(*fb);
    overlay_->draw(r);

    const ColorRGB565* buf = fb->get_raw_buffer();
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], 0x0000) << "hidden overlay must not paint, index " << i;
    }
}

// =============================================================
// OverlayRootView
// =============================================================
namespace {

// 记录自身被绘制进日志的叶子节点，用来验证 add 顺序即绘制顺序（Z 序）。
class LoggingLeaf : public UI::View {
public:
    LoggingLeaf(int id, int* log, int* count) : UI::View(0, 0, 10, 10), id_(id), log_(log), count_(count) {}

    void draw(UI::UIRenderer&) override {
        if (log_ && count_) {
            log_[(*count_)++] = id_;
        }
    }

private:
    int id_;
    int* log_;
    int* count_;
};

} // namespace

class OverlayRootViewTest : public ::testing::Test {};

TEST_F(OverlayRootViewTest, DrawsChildrenInAddOrderSoOverlaySitsOnTop) {
    int log[4] = {-1, -1, -1, -1};
    int count = 0;
    LoggingLeaf page(1, log, &count);    // 相当于 ScreenNavigator
    LoggingLeaf overlay(2, log, &count); // 相当于通知浮层

    UI::OverlayRootView root;
    root.add_child(&page);
    root.add_child(&overlay);
    ASSERT_EQ(root.get_child_count(), 2);

    auto fb = std::make_unique<Fb>();
    UI::UIRenderer r(*fb);
    root.render_all(r);

    ASSERT_EQ(count, 2);
    // 后 add 的先不成立 —— add 顺序即绘制顺序，后者才能盖住前者
    EXPECT_EQ(log[0], 1);
    EXPECT_EQ(log[1], 2);
}

TEST_F(OverlayRootViewTest, SkipsInvisibleChildren) {
    int log[4] = {-1, -1, -1, -1};
    int count = 0;
    LoggingLeaf visible(1, log, &count);
    LoggingLeaf hidden(2, log, &count);
    hidden.set_visibility(UI::Visibility::GONE);

    UI::OverlayRootView root;
    root.add_child(&visible);
    root.add_child(&hidden);

    auto fb = std::make_unique<Fb>();
    UI::UIRenderer r(*fb);
    root.render_all(r);

    EXPECT_EQ(count, 1);
    EXPECT_EQ(log[0], 1);
}

TEST_F(OverlayRootViewTest, DestructorDoesNotDeleteChildren) {
    int log[4] = {-1, -1, -1, -1};
    int count = 0;
    LoggingLeaf leaf(1, log, &count);

    {
        UI::OverlayRootView root;
        root.add_child(&leaf);
        ASSERT_EQ(root.get_child_count(), 1);
    }

    // 根容器析构后子节点必须仍然存活：~ViewGroup 会 delete children_，
    // OverlayRootView 的「非拥有」契约要求它在析构里先摘链。
    // 若契约被破坏，这里是 use-after-free（CI 的 ASAN job 会直接报）。
    EXPECT_EQ(leaf.get_parent(), nullptr);
    leaf.set_visibility(UI::Visibility::GONE); // 触达活对象即证明未被释放
    SUCCEED();
}

TEST_F(OverlayRootViewTest, AccumulatesDamageAtRootForChildInvalidation) {
    int log[4] = {-1, -1, -1, -1};
    int count = 0;
    LoggingLeaf leaf(1, log, &count);

    UI::OverlayRootView root;
    root.add_child(&leaf);

    // 根容器自身 parent_ == nullptr，子控件 invalidate() 的世界矩形必须
    // 冒泡到这里累积 —— 否则 UiManager::render() 拿不到脏区、整帧空转。
    const UI::Rect d = root.take_damage();
    EXPECT_GT(static_cast<uint32_t>(d.width) * static_cast<uint32_t>(d.height), 0u);
}
