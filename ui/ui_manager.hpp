#ifndef AURORA_UI_MANAGER_HPP
#define AURORA_UI_MANAGER_HPP

#include "view_group.hpp"

namespace UI {

// ========================================================
// band_detail：分带几何纯函数
//
// 从 render() 的循环里抽出来，使「面板高不被条带高整除」这类板卡配置
// （nucleo 128/35、lm3s6965 16/35、以及历史上的 490/30）能被单测穷举覆盖，
// 而不必为了改条带高去构造第二个 TU（那会踩 ODR，见测试文件注释）。
// ========================================================
namespace band_detail {

// 首个与第 y 行相交的条带起点。前置条件：y >= 0，band_h > 0。
[[nodiscard]] constexpr int16_t first_top_for(int16_t y, int16_t band_h) noexcept {
    return static_cast<int16_t>((y / band_h) * band_h);
}

// 自 top 行起的条带行数（残带守卫）。
// 返回 0 表示 top 已越过面板底边，调用方应停止遍历。
[[nodiscard]] constexpr uint16_t rows_at(int16_t top, int16_t panel_h, int16_t band_h) noexcept {
    const int16_t remain = static_cast<int16_t>(panel_h - top);
    if (remain <= 0)
        return 0;
    return (remain < band_h) ? static_cast<uint16_t>(remain) : static_cast<uint16_t>(band_h);
}

} // namespace band_detail

// ========================================================
// 全局手势预分发过滤器（可用于锁屏、全局快捷键、系统截屏拦截）
// ========================================================
class IGestureFilter {
public:
    virtual ~IGestureFilter() = default;
    // 返回 true 表示拦截并消费事件，不再向下分发给 UI 视图树
    virtual bool on_filter_gesture(const GestureEvent& event) = 0;
};

// ========================================================
// 全局手势分发结果监听器（可用于系统唤醒、触控音效/振动、性能统计）
// ========================================================
class IGestureListener {
public:
    virtual ~IGestureListener() = default;
    virtual void on_gesture_dispatched(const GestureEvent& event, bool handled) = 0;
};

// ========================================================
// IBandSink：条带落屏接收器（依赖倒置）
//
// UiManager（UI 层）负责决定「本轮要画哪些条带」以及把裁剪区收窄到
// damage ∩ band；「这条带画完推给哪个驱动」由应用层实现，于是 UI 层
// 不必依赖任何具体显示驱动。
//
// 契约：
//   - begin_band() 返回该条带使用的渲染器（实现通常返回同一个实例）；
//     rows <= AURORA_UI_BAND_H，且其绑定的 FrameBuffer 至少 rows 行。
//   - end_band() 内应把该条带落屏。对 ST7789 即
//     fb.flush(driver, logical_top)，其中 logical_top 必须是
//     **屏幕绝对行号**（见 framebuffer.hpp::flush 的 y_base 注释）。
// ========================================================
class IBandSink {
public:
    virtual ~IBandSink() = default;

    // 进入逻辑行区间 [logical_top, logical_top + rows) 的条带。
    virtual UIRenderer& begin_band(int16_t logical_top, uint16_t rows) noexcept = 0;

    // 本条带绘制完成，落屏。
    virtual void end_band(int16_t logical_top, uint16_t rows) noexcept = 0;
};

class UiManager {
private:
    ViewGroup* root_view_;
    UIRenderer* renderer_;
    IBandSink* band_sink_;
    IGestureFilter* filter_;
    IGestureListener* listener_;
    View* captured_target_;

    UiManager()
        : root_view_(nullptr), renderer_(nullptr), band_sink_(nullptr), filter_(nullptr), listener_(nullptr),
          captured_target_(nullptr) {}

public:
    static UiManager& instance() {
        static UiManager manager;
        return manager;
    }

    void set_renderer(UIRenderer* renderer) noexcept {
        renderer_ = renderer;
    }

    // 绑定条带落屏接收器（应用层实现）。未绑定时退化为「渲染但不推屏」，
    // 便于 host 测试与调试工具直接读 FrameBuffer。
    void set_band_sink(IBandSink* sink) noexcept {
        band_sink_ = sink;
    }

    IBandSink* get_band_sink() const noexcept {
        return band_sink_;
    }

    void set_root_view(ViewGroup* root) {
        if (root_view_ && root_view_ != root) {
            // 旧根可能残留未被消费的 damage。若不清理，它将来再次成为根时会带着
            // 陈旧脏区，误触发一次本不该发生的重绘。take_damage() 只取不清其他状态。
            (void)root_view_->take_damage();
        }
        root_view_ = root;
        captured_target_ = nullptr;
        if (root_view_) {
            root_view_->invalidate();
        }
    }

    ViewGroup* get_root_view() const noexcept {
        return root_view_;
    }

    void set_gesture_filter(IGestureFilter* filter) noexcept {
        filter_ = filter;
    }

    IGestureFilter* get_gesture_filter() const noexcept {
        return filter_;
    }

    void set_gesture_listener(IGestureListener* listener) noexcept {
        listener_ = listener;
    }

    IGestureListener* get_gesture_listener() const noexcept {
        return listener_;
    }

    // ========================================================
    // 触控焦点锁定与捕获机制 (Touch Target Tracking)
    // ========================================================
    void capture_touch(View* view) noexcept {
        captured_target_ = view;
    }

    void release_touch() noexcept {
        captured_target_ = nullptr;
    }

    View* get_captured_target() const noexcept {
        return captured_target_;
    }

    // 全局命中测试：检索位于屏幕 (x, y) 处最顶层可见的叶子视图
    View* find_view_at(int16_t x, int16_t y) const {
        if (!root_view_)
            return nullptr;
        return root_view_->find_view_at(x, y);
    }

    // ========================================================
    // 由 FrameScheduler 驱动的 UI 渲染主入口（分带渲染）
    //
    // 流程：取根节点累积的世界坐标 damage → 逐条带把裁剪区收窄到
    // damage ∩ band → 整树无条件重绘（draw() 幂等，靠 scissor 裁剪）
    // → 经 IBandSink 落屏（推送范围由 FrameBuffer::DirtyRect 决定）。
    //
    // 「画什么」交给 scissor、「推什么」交给 DirtyRect，两套机制单一职责。
    // ========================================================
    void render() {
        if (!root_view_ || !renderer_)
            return;

        // 帧门控：damage 为空 ⇒ 本帧无任何变化 ⇒ 0 draw + 0 flush。
        //
        // 注意不能再用 is_dirty()：子控件级变更只把「世界坐标矩形」累积到根的
        // damage_（见 view_group.hpp::invalidate_rect_world），不再置根的
        // is_dirty_。若仍以 is_dirty() 作门控，子控件更新会被永久 skip
        // （历史缺陷：开机第一帧之后表盘时间/心率/步数更新永远画不出来）。
        Rect dmg = root_view_->take_damage();

        // 钳到面板边界（damage 可能来自被移出屏外的控件）。
        dmg = Rect::intersect(dmg,
                              Rect{0, 0, static_cast<uint16_t>(DISPLAY_WIDTH), static_cast<uint16_t>(DISPLAY_HEIGHT)});
        if (dmg.width == 0 || dmg.height == 0)
            return;

        // 大范围损伤（转场 / 整屏滚动）退化为全屏：此时每一条带都与 damage 相交，
        // 逐带裁剪已无收益，反而多出十余次 set_window 的边际开销。
        const uint32_t dmg_area = static_cast<uint32_t>(dmg.width) * static_cast<uint32_t>(dmg.height);
        const uint32_t screen_area = static_cast<uint32_t>(DISPLAY_WIDTH) * static_cast<uint32_t>(DISPLAY_HEIGHT);
        if (dmg_area >= (screen_area * 3u) / 5u) { // >= 60% 屏
            dmg = Rect{0, 0, static_cast<uint16_t>(DISPLAY_WIDTH), static_cast<uint16_t>(DISPLAY_HEIGHT)};
        }

        const int16_t band_h = static_cast<int16_t>(AURORA_UI_BAND_H);
        const int16_t panel_h = static_cast<int16_t>(DISPLAY_HEIGHT);
        const int16_t dmg_last_row = static_cast<int16_t>(dmg.y + static_cast<int16_t>(dmg.height) - 1);

        for (int16_t top = band_detail::first_top_for(dmg.y, band_h); top <= dmg_last_row;
             top = static_cast<int16_t>(top + band_h)) {
            // 残带守卫：面板高未必是条带高的整数倍（如 128 行的板卡配 35 行条带），
            // 最后一条带只覆盖剩余行；top 越界则直接停止。
            const uint16_t rows = band_detail::rows_at(top, panel_h, band_h);
            if (rows == 0)
                break;

            const Rect band_clip = Rect::intersect(dmg, Rect{0, top, static_cast<uint16_t>(DISPLAY_WIDTH), rows});
            if (band_clip.width == 0 || band_clip.height == 0)
                continue; // 该条带与损伤无交集，整条跳过（0 draw + 0 flush）

            UIRenderer& r = band_sink_ ? band_sink_->begin_band(top, rows) : *renderer_;

            // 必须每次清零：set_clip_rect 是「求交」语义，不清零则上一条带的裁剪区
            // 会累积收窄，本条带内容会被上一带的窗口错误裁掉。
            r.clear_clip_rect();
            // 世界坐标 → 条带局部行（条带第 0 行 == 屏幕第 top 行）。
            r.set_clip_rect(band_clip.x, static_cast<int16_t>(band_clip.y - top), band_clip.width, band_clip.height);
            r.set_band_origin(static_cast<uint16_t>(top));

            // 整树无条件重绘：draw() 已改为幂等且不触碰脏标记（见 view_group.hpp）。
            root_view_->render_all(r);

            if (band_sink_)
                band_sink_->end_band(top, rows);
        }

        // 复位条带原点，避免跨帧污染（下一次 render 会重新设置）。
        renderer_->set_band_origin(0);
    }

    // ========================================================
    // 手势事件分发中枢：贯穿 Filter -> Focus Target -> UI Tree -> Listener
    // ========================================================
    bool dispatch_gesture(const GestureEvent& event) {
        // 1. 全局拦截器优先判定 (如锁屏/系统防误触)
        if (filter_ && filter_->on_filter_gesture(event)) {
            if (listener_) {
                listener_->on_gesture_dispatched(event, true);
            }
            return true;
        }

        bool handled = false;

        // 2. 焦点目标捕获路由：若已有控件捕获了当前手势流（如拖拽中的滑块），优先直接投递
        if (captured_target_) {
            if (captured_target_->is_enabled() && captured_target_->get_visibility() == Visibility::VISIBLE) {
                handled = captured_target_->handle_gesture(event);
            } else {
                captured_target_ = nullptr;
            }

            // 拖拽释放或取消时自动释放捕获
            if (event.type == GestureType::DRAG_END ||
                event.type == GestureType::TOUCH_UP ||
                event.type == GestureType::CANCEL) {
                captured_target_ = nullptr;
            }
        }

        // 3. 未被捕获目标消费，则由根视图自顶向下深度优先路由
        if (!handled && root_view_) {
            handled = root_view_->handle_gesture(event);
        }

        // 4. 通知全局手势观察者
        if (listener_) {
            listener_->on_gesture_dispatched(event, handled);
        }

        return handled;
    }
};

} // namespace UI

#endif // AURORA_UI_MANAGER_HPP
