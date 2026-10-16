#ifndef AURORA_WATCH_NOTIFICATION_OVERLAY_VIEW_HPP
#define AURORA_WATCH_NOTIFICATION_OVERLAY_VIEW_HPP

// ============================================================
// apps/watch/notification_overlay_view.hpp — 通知浮层（固件侧）
// ============================================================
//
// 定位
// --------------------------------
// NotificationCenter（experimental/apps/notification_center.hpp）只管
// 「排队 / 按优先级派发 / 谁该收起了」，完全不知道像素和控件树长什么样；
// 它通过 INotificationOverlay 这个端口倒置依赖表现层。本文件是该端口在
// 固件侧的唯一实现：一片真实 UI::View 叶子。
//
// 为什么必须重新实现（而不是复用历史实现）
// --------------------------------
// 旧的 NotificationOverlay 继承 experimental/ui/view_group.hpp ——
// 那是**主机单测专用 stub**（UIRenderer/ViewGroup 全是空函数，
// invalidate() 是 no-op）。它因此：
//   - 不是 ui/view_group.hpp 里那个真实 ViewGroup，无法进入真实控件树；
//   - invalidate() 不冒泡 damage，UiManager::render() 永远拿不到脏区；
//   - handle_gesture 缺失，浮层显示的 "SWIPE RIGHT TO DISMISS" 提示
//     没有任何代码去响应它。
// 换句话说，旧实现在真机上既画不出来、也关不掉，通知进得去出不来。
// 详见 experimental/apps/notification_center.hpp 顶部的分层说明。
//
// 宿主契约
// --------------------------------
// 实例必须通过 ui/overlay_root_view.hpp 挂到控件树根上（不能直接
// add_child 到 ScreenNavigator —— 它覆写了 draw()，不遍历子节点）。
// NotificationCenter 持有一个**非拥有**指针，不负责释放本对象。
//
// 状态可见性
// --------------------------------
// 通知状态（mode_）与 View 可见性（visibility_）是两份状态，靠 show()/hide()
// 内部同步维护，不变量：mode_ != hidden ⟺ visibility_ == VISIBLE。
// is_visible() 报的是**通知状态**（INotificationOverlay 端口的语义，
// 供 NotificationCenter 判断「当前有没有东西占着屏幕」）；UI 框架内部
// 一律走 get_visibility()，二者不冲突。
//
// 与页面导航的手势边界
// --------------------------------
// banner 与 fullscreen 给出的手势权限不同，理由见 handle_gesture() 内的
// 注释。关键一条：apps/watch/watch_app.cpp 的 handle_key_event() 把侧键短按
// 映射为 SWIPE_RIGHT（语义「返回上一页」），因此 banner **不得**消费
// SWIPE_RIGHT，否则用户按侧键返回时只是关掉一条 toast，按键表现为无响应。
// 改动 handle_gesture 前先读那段注释。
// ============================================================

#include <stdint.h>

#include "../../ui/view.hpp"
#include "../../ui/ui_config.hpp"
#include "font_engine.hpp"
#include "../../experimental/apps/notification_center.hpp"

namespace aurora {

class NotificationOverlayView : public UI::View, public INotificationOverlay {
public:
    // ── 视觉常量（自深色主题的旧实现平移，色值未改）────────────
    static constexpr uint32_t kBannerDurationMs = 3000;
    static constexpr uint16_t kBannerHeight = 80;
    static constexpr uint16_t kBannerRadius = 6;
    static constexpr ColorRGB565 kBgBanner = 0x2965;
    static constexpr ColorRGB565 kBgCritical = 0xC000;
    static constexpr ColorRGB565 kColorPrimary = 0xFFFF;
    static constexpr ColorRGB565 kColorSecondary = 0xC618;
    static constexpr ColorRGB565 kColorAccent = 0x07E0;

    enum class DisplayMode : uint8_t {
        hidden,
        banner,
        fullscreen
    };

    NotificationOverlayView() noexcept
        : UI::View(0, 0, DISPLAY_WIDTH, kBannerHeight), panel_h_{static_cast<uint16_t>(DISPLAY_HEIGHT)},
          mode_{DisplayMode::hidden}, elapsed_ms_{0}, current_{} {
        // 初始即不可见：不占屏、不参与命中测试，也不给帧循环添无谓的
        // 条件分支（ViewGroup::draw 每帧都会遍历到本节点）。
        set_visibility(UI::Visibility::GONE);
    }

    NotificationOverlayView(const NotificationOverlayView&) = delete;
    NotificationOverlayView& operator=(const NotificationOverlayView&) = delete;

    // ── INotificationOverlay 端口 ────────────────────────────────
    void show(const Notification& n) noexcept override {
        current_ = n;
        elapsed_ms_ = 0;

        // 呼叫类与 critical 一律升级为全屏弹窗，与枚举注释里的约定一致。
        const bool is_critical =
            (n.priority == NotificationPriority::critical) || (n.category == NotificationCategory::call);

        if (is_critical) {
            mode_ = DisplayMode::fullscreen;
            set_size(static_cast<uint16_t>(DISPLAY_WIDTH), panel_h_);
        } else {
            mode_ = DisplayMode::banner;
            set_size(static_cast<uint16_t>(DISPLAY_WIDTH), kBannerHeight);
        }
        // set_size() 已在尺寸变化时登记新旧两块世界坐标；尺寸未变（全屏→
        // 全屏连续两条）时补一次，保证像素一定被重绘。
        invalidate();
        set_visibility(UI::Visibility::VISIBLE);
    }

    void hide() noexcept override {
        mode_ = DisplayMode::hidden;
        // 先置几何无关的通知状态，再摘可见性：invalidate() 取的是
        // world_bounds()（纯几何、与 mode_/visibility_ 无关），
        // 因此无论先改哪个，被遮住的旧区域都能正确登记为脏区。
        set_visibility(UI::Visibility::GONE);
        invalidate();
    }

    [[nodiscard]] bool is_visible() const noexcept override {
        return mode_ != DisplayMode::hidden;
    }

    void tick(uint32_t delta_ms) noexcept override {
        // 只有 banner 有超时；全屏弹窗必须等用户手势，不自动消失。
        if (mode_ != DisplayMode::banner) {
            return;
        }
        elapsed_ms_ += delta_ms;
        if (elapsed_ms_ >= kBannerDurationMs) {
            hide();
        }
    }

    // 供 UI/测试查询当前呈现模式。
    [[nodiscard]] DisplayMode get_mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] const Notification& current() const noexcept {
        return current_;
    }

    // ============================================================
    // UI::View 覆写
    // ============================================================

    void draw(UI::UIRenderer& renderer) override {
        if (mode_ == DisplayMode::hidden) {
            return;
        }

        const ColorRGB565 bg = (mode_ == DisplayMode::fullscreen) ? kBgCritical : kBgBanner;

        renderer.fill_round_rect(x_, y_, width_, height_, kBannerRadius, bg);

        // 左侧色条编码分类，4px 宽是 5x7 字体下不显眼的最小可辨识宽度。
        const ColorRGB565 tag_color = category_color(current_.category);
        renderer.fill_rect(x_, y_, 4, height_, tag_color);

        constexpr int16_t kTextX = 10;
        constexpr int16_t kTitleY = 8;
        renderer.draw_string(static_cast<int16_t>(x_ + kTextX), static_cast<int16_t>(y_ + kTitleY), current_.title, 2,
                             kColorPrimary, bg, font5x7_data, 5, 7);

        constexpr int16_t kBodyY = 32;
        renderer.draw_string(static_cast<int16_t>(x_ + kTextX), static_cast<int16_t>(y_ + kBodyY), current_.body, 1,
                             kColorSecondary, bg, font5x7_data, 5, 7);

        if (mode_ == DisplayMode::fullscreen) {
            // 提示贴着面板底部，随面板高度走而不是写死 400（写死只对
            // 490 行的 miband8 成立，换面板就会浮在屏幕中间）。
            const int16_t hint_y = static_cast<int16_t>(panel_h_ - 24);
            renderer.draw_string(static_cast<int16_t>(x_ + 20), hint_y, "SWIPE RIGHT TO DISMISS", 1, kColorSecondary,
                                 bg, font5x7_data, 5, 7);
        }
    }

    // 浮层在手势链上位于 ScreenNavigator 之上（它是根容器的最后一个
    // child），因此这里消费到的事件本不会再传给页面栈。
    //
    // 分两种模式给不同的手势权限，理由是**与页面导航的冲突**：
    //
    //   fullscreen（critical / call）—— 真模态。SWIPE_RIGHT 与 TAP 都消费：
    //     用户必须先确认或划掉这条告警，页面导航在此期间被有意挡住。
    //     屏幕上那条 "SWIPE RIGHT TO DISMISS" 提示只在此时绘制，语义自洽。
    //
    //   banner —— 瞬时提示，只消费「点在横幅自身上」的 TAP；
    //     SWIPE_RIGHT 一律放行给页面栈。
    //     这条不是为了对称好看：apps/watch/watch_app.cpp 的 handle_key_event()
    //     把**侧键短按映射成 SWIPE_RIGHT**（语义是「返回上一页」）。若横幅
    //     也吞 SWIPE_RIGHT，用户在看表时收到消息、按侧键想返回，结果只是
    //     关掉一条 3 秒后自己就会消失的提示 —— 按键「按了没反应」。
    //     横幅是 toast 语义，不该挡住导航。
    //
    // 其余手势（SWIPE_LEFT / SWIPE_UP / LONG_PRESS …）全部放行。
    bool handle_gesture(const GestureEvent& event) override {
        if (mode_ == DisplayMode::hidden) {
            return false;
        }
        if (event.type == GestureType::SWIPE_RIGHT) {
            if (mode_ != DisplayMode::fullscreen) {
                return false; // banner：放行给页面导航
            }
        } else if (event.type == GestureType::TAP) {
            // 点在浮层之外（例如全屏弹窗出现前的坐标，或 banner 下方）不消费
            if (!contains(static_cast<int16_t>(event.x), static_cast<int16_t>(event.y))) {
                return false;
            }
        } else {
            return false;
        }

        // 走中心而不是直接 hide()：队列里可能还压着优先级更高的通知，
        // dismiss_current() 会在同一次调用里把下一条顶上。
        NotificationCenter::instance().dismiss_current();
        return true;
    }

private:
    static ColorRGB565 category_color(NotificationCategory cat) noexcept {
        switch (cat) {
        case NotificationCategory::call:
            return 0xF81F;
        case NotificationCategory::message:
            return 0x07E0;
        case NotificationCategory::system:
            return 0xFFE0;
        default:
            return 0x001F;
        }
    }

    uint16_t panel_h_;
    DisplayMode mode_;
    uint32_t elapsed_ms_;
    Notification current_;
};

} // namespace aurora

#endif // AURORA_WATCH_NOTIFICATION_OVERLAY_VIEW_HPP
