#ifndef AURORA_UI_SCROLL_VIEW_HPP
#define AURORA_UI_SCROLL_VIEW_HPP

#include "../view_group.hpp"

namespace UI {

// ========================================================
// ScrollView: 可滚动视图容器
//
// 提供惯性/拖拽滚动、局部视口 Scissor 裁剪与滚动条指示器。
// 可容纳垂直列表、长文本与卡片流。
// ========================================================
class ScrollView : public ViewGroup {
private:
    int16_t scroll_x_;
    int16_t scroll_y_;
    uint16_t content_width_;
    uint16_t content_height_;
    bool show_scrollbar_;
    ColorRGB565 scrollbar_color_;

public:
    ScrollView(int16_t x, int16_t y, uint16_t w, uint16_t h)
        : ViewGroup(x, y, w, h),
          scroll_x_(0),
          scroll_y_(0),
          content_width_(w),
          content_height_(h),
          show_scrollbar_(true),
          scrollbar_color_(0x7BEF /* light gray */) {}

    int16_t get_scroll_x() const noexcept {
        return scroll_x_;
    }

    int16_t get_scroll_y() const noexcept {
        return scroll_y_;
    }

    uint16_t get_content_height() const noexcept {
        return content_height_;
    }

    void set_content_size(uint16_t w, uint16_t h) noexcept {
        content_width_ = (w < width_) ? width_ : w;
        content_height_ = (h < height_) ? height_ : h;
        clamp_scroll();
        invalidate();
    }

    void set_scrollbar_visible(bool visible) noexcept {
        show_scrollbar_ = visible;
        invalidate();
    }

    void scroll_to(int16_t sx, int16_t sy) {
        scroll_x_ = sx;
        scroll_y_ = sy;
        clamp_scroll();
        invalidate();
    }

    void scroll_by(int16_t dx, int16_t dy) {
        scroll_to(scroll_x_ + dx, scroll_y_ + dy);
    }

    // 重写 add_child：自动扩展 content_height
    void add_child(View* child) {
        ViewGroup::add_child(child);
        if (child) {
            const uint16_t right = static_cast<uint16_t>(child->get_x() + child->get_width());
            const uint16_t bottom = static_cast<uint16_t>(child->get_y() + child->get_height());
            if (right > content_width_)
                content_width_ = right;
            if (bottom > content_height_)
                content_height_ = bottom;
        }
    }

    // ========================================================
    // 手势拦截：滚动期间拦截子控件点击，防止滑动误触
    // ========================================================
    bool on_intercept_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE)
            return false;

        // 仅在自身物理边界内生效
        if (!contains(event.x, event.y))
            return false;

        if (event.type == GestureType::DRAG_START || event.type == GestureType::DRAG_MOVE) {
            if (can_scroll_vertically() || can_scroll_horizontally()) {
                return true; // 拦截并由自身处理滚动
            }
        }

        return false;
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE)
            return false;

        // 1. 拖拽滚动
        if (event.type == GestureType::DRAG_MOVE || event.type == GestureType::DRAG_START) {
            if (contains(event.x, event.y)) {
                scroll_by(-event.delta_x, -event.delta_y);
                return true;
            }
        }

        // 2. 惯性/翻页滑动手势
        if (event.type == GestureType::SWIPE_UP && contains(event.x, event.y)) {
            scroll_by(0, static_cast<int16_t>(height_ / 2));
            return true;
        }
        if (event.type == GestureType::SWIPE_DOWN && contains(event.x, event.y)) {
            scroll_by(0, -static_cast<int16_t>(height_ / 2));
            return true;
        }

        // 3. 点击等事件需补偿滚动偏移后再分发给子控件
        GestureEvent compensated = event;
        compensated.x = static_cast<uint16_t>(static_cast<int16_t>(event.x) + scroll_x_);
        compensated.y = static_cast<uint16_t>(static_cast<int16_t>(event.y) + scroll_y_);

        return ViewGroup::handle_gesture(compensated);
    }

    // ========================================================
    // 渲染分发：设置视口 Scissor 裁剪与局部滚动坐标偏移
    // ========================================================
    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE)
            return;

        const Rect2D prev_clip = renderer.get_clip_rect();
        const int16_t prev_ox = renderer.get_offset_x();
        const int16_t prev_oy = renderer.get_offset_y();

        // 1. 开启视口局部裁剪（嵌套安全）
        renderer.set_clip_rect(x_, y_, width_, height_);

        // 2. 应用滚动偏移并渲染子节点
        renderer.set_offset(prev_ox - scroll_x_, prev_oy - scroll_y_);
        ViewGroup::draw(renderer);

        // 3. 恢复视口偏移与原始裁剪区
        renderer.set_offset(prev_ox, prev_oy);
        renderer.set_clip_rect(prev_clip.x, prev_clip.y, prev_clip.w, prev_clip.h);

        // 4. 绘制右侧滚动条指示器
        if (show_scrollbar_ && content_height_ > height_) {
            const uint16_t bar_w = 2;
            const uint16_t bar_h = (static_cast<uint32_t>(height_) * height_) / content_height_;
            const uint16_t max_scroll = content_height_ - height_;
            const int16_t bar_y = y_ + static_cast<int16_t>((static_cast<uint32_t>(scroll_y_) * (height_ - bar_h)) / max_scroll);
            const int16_t bar_x = x_ + width_ - bar_w - 1;

            renderer.fill_round_rect(bar_x, bar_y, bar_w, bar_h, 1, scrollbar_color_);
        }
    }

private:
    bool can_scroll_vertically() const noexcept {
        return content_height_ > height_;
    }

    bool can_scroll_horizontally() const noexcept {
        return content_width_ > width_;
    }

    void clamp_scroll() noexcept {
        const int16_t max_y = (content_height_ > height_) ? static_cast<int16_t>(content_height_ - height_) : 0;
        const int16_t max_x = (content_width_ > width_) ? static_cast<int16_t>(content_width_ - width_) : 0;

        if (scroll_y_ < 0) scroll_y_ = 0;
        if (scroll_y_ > max_y) scroll_y_ = max_y;

        if (scroll_x_ < 0) scroll_x_ = 0;
        if (scroll_x_ > max_x) scroll_x_ = max_x;
    }
};

} // namespace UI

#endif // AURORA_UI_SCROLL_VIEW_HPP
