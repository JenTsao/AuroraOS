#ifndef AURORA_UI_DIALOG_VIEW_HPP
#define AURORA_UI_DIALOG_VIEW_HPP

#include "../view_group.hpp"
#include "button.hpp"
#include "text_view.hpp"

namespace UI {

// ========================================================
// DialogView: 模态对话框控件
//
// 带有全屏遮罩防穿透、居中卡片布局、圆角背景以及确定/取消按钮。
// 支持模态输入拦截与点击外部自关闭（dismiss_on_touch_outside）。
// ========================================================
class DialogView : public ViewGroup {
public:
    using DialogCallback = void (*)(DialogView* dialog, void* ctx);

private:
    Rect card_rect_;
    ColorRGB565 card_bg_color_;
    ColorRGB565 overlay_color_;
    bool dismiss_on_touch_outside_;
    Button* confirm_btn_;
    Button* cancel_btn_;
    TextView* title_view_;
    TextView* message_view_;

    DialogCallback on_confirm_;
    void* confirm_ctx_;
    DialogCallback on_cancel_;
    void* cancel_ctx_;

public:
    DialogView(int16_t screen_w, int16_t screen_h,
               const char* title, const char* message,
               int16_t card_w = 160, int16_t card_h = 130)
        : ViewGroup(0, 0, screen_w, screen_h),
          card_bg_color_(0x18E3 /* dark charcoal */),
          overlay_color_(0x0000 /* black */),
          dismiss_on_touch_outside_(false),
          confirm_btn_(nullptr),
          cancel_btn_(nullptr),
          title_view_(nullptr),
          message_view_(nullptr),
          on_confirm_(nullptr),
          confirm_ctx_(nullptr),
          on_cancel_(nullptr),
          cancel_ctx_(nullptr) {

        // 居中卡片区域
        card_rect_.x = (screen_w - card_w) / 2;
        card_rect_.y = (screen_h - card_h) / 2;
        card_rect_.width = card_w;
        card_rect_.height = card_h;

        // 1. 标题
        title_view_ = new TextView(card_rect_.x + 10, card_rect_.y + 12, title, 0xFFFF /* white */, 0x0000, 2);
        add_child(title_view_);

        // 2. 文本消息
        message_view_ = new TextView(card_rect_.x + 10, card_rect_.y + 38, message, 0xCE79 /* silver */, 0x0000, 1);
        add_child(message_view_);

        // 3. 确定按钮 (右侧)
        const uint16_t btn_w = (card_w - 30) / 2;
        const uint16_t btn_h = 28;
        const int16_t btn_y = card_rect_.y + card_h - btn_h - 10;

        confirm_btn_ = new Button(card_rect_.x + card_w - btn_w - 10, btn_y, btn_w, btn_h, 0x07E0 /* green */, 0x0500);
        confirm_btn_->set_on_click([](void* ctx) {
            auto* self = static_cast<DialogView*>(ctx);
            if (self->on_confirm_) {
                self->on_confirm_(self, self->confirm_ctx_);
            }
        }, this);
        add_child(confirm_btn_);

        // 4. 取消按钮 (左侧)
        cancel_btn_ = new Button(card_rect_.x + 10, btn_y, btn_w, btn_h, 0x39E7 /* gray */, 0x2104);
        cancel_btn_->set_on_click([](void* ctx) {
            auto* self = static_cast<DialogView*>(ctx);
            if (self->on_cancel_) {
                self->on_cancel_(self, self->cancel_ctx_);
            }
        }, this);
        add_child(cancel_btn_);
    }

    void set_on_confirm_listener(DialogCallback cb, void* ctx) noexcept {
        on_confirm_ = cb;
        confirm_ctx_ = ctx;
    }

    void set_on_cancel_listener(DialogCallback cb, void* ctx) noexcept {
        on_cancel_ = cb;
        cancel_ctx_ = ctx;
    }

    void set_dismiss_on_touch_outside(bool enable) noexcept {
        dismiss_on_touch_outside_ = enable;
    }

    void draw(UIRenderer& renderer) override {
        if (visibility_ != Visibility::VISIBLE)
            return;

        // 1. 绘制全局半透明/变暗遮罩 (利用每两像素交叉点网格实现快速纯点阵遮罩)
        for (int16_t row = y_; row < y_ + static_cast<int16_t>(height_); row += 2) {
            renderer.draw_hline(x_, row, width_, overlay_color_);
        }

        // 2. 绘制居中圆角卡片背景
        renderer.fill_round_rect(card_rect_.x, card_rect_.y, card_rect_.width, card_rect_.height, 8, card_bg_color_);

        // 3. 渲染子视图 (标题/消息/按钮)
        ViewGroup::draw(renderer);
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (!enabled_ || visibility_ != Visibility::VISIBLE)
            return false;

        // 优先让卡片内部子控件处理
        if (card_rect_.contains(event.x, event.y)) {
            return ViewGroup::handle_gesture(event);
        }

        // 点击卡片外遮罩区域
        if (event.type == GestureType::TAP) {
            if (dismiss_on_touch_outside_ && on_cancel_) {
                on_cancel_(this, cancel_ctx_);
            }
            return true; // 拦截事件，防止穿透到底层界面
        }

        return true; // 模态全屏拦截
    }
};

} // namespace UI

#endif // AURORA_UI_DIALOG_VIEW_HPP
