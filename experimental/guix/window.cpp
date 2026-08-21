#include "window.hpp"
#include "compositor.hpp"
#include "widget.hpp"
#include <string.h>
#include <stdlib.h>
#include <algorithm>

namespace auroraos {
namespace guix {

// ============================================================
// 5x7 ASCII 字符集字模点阵 (ASCII 32 ' ' 至 126 '~')
// ============================================================
static const uint8_t GUIX_FONT5X7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // (space)
    0x00, 0x00, 0x5f, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7f, 0x14, 0x7f, 0x14, // #
    0x24, 0x2a, 0x7f, 0x2a, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1c, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1c, 0x00, // )
    0x14, 0x08, 0x3e, 0x08, 0x14, // *
    0x08, 0x08, 0x3e, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3e, 0x51, 0x49, 0x45, 0x3e, // 0
    0x00, 0x42, 0x7f, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4b, 0x31, // 3
    0x18, 0x14, 0x12, 0x7f, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3c, 0x4a, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1e, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x08, 0x14, 0x22, 0x41, 0x00, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x00, 0x41, 0x22, 0x14, 0x08, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3e, // @
    0x7e, 0x11, 0x11, 0x11, 0x7e, // A
    0x7f, 0x49, 0x49, 0x49, 0x36, // B
    0x3e, 0x41, 0x41, 0x41, 0x22, // C
    0x7f, 0x41, 0x41, 0x22, 0x1c, // D
    0x7f, 0x49, 0x49, 0x49, 0x41, // E
    0x7f, 0x09, 0x09, 0x09, 0x01, // F
    0x3e, 0x41, 0x49, 0x49, 0x7a, // G
    0x7f, 0x08, 0x08, 0x08, 0x7f, // H
    0x00, 0x41, 0x7f, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3f, 0x01, // J
    0x7f, 0x08, 0x14, 0x22, 0x41, // K
    0x7f, 0x40, 0x40, 0x40, 0x40, // L
    0x7f, 0x02, 0x0c, 0x02, 0x7f, // M
    0x7f, 0x04, 0x08, 0x10, 0x7f, // N
    0x3e, 0x41, 0x41, 0x41, 0x3e, // O
    0x7f, 0x09, 0x09, 0x09, 0x06, // P
    0x3e, 0x41, 0x51, 0x21, 0x5e, // Q
    0x7f, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7f, 0x01, 0x01, // T
    0x3f, 0x40, 0x40, 0x40, 0x3f, // U
    0x1f, 0x20, 0x40, 0x20, 0x1f, // V
    0x3f, 0x40, 0x38, 0x40, 0x3f, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x07, 0x08, 0x70, 0x08, 0x07, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x7f, 0x41, 0x41, 0x00, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // backslash
    0x00, 0x41, 0x41, 0x7f, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7f, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7f, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7e, 0x09, 0x01, 0x02, // f
    0x0c, 0x52, 0x52, 0x52, 0x3e, // g
    0x7f, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7d, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3d, 0x00, // j
    0x7f, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7f, 0x40, 0x00, // l
    0x7c, 0x04, 0x18, 0x04, 0x78, // m
    0x7c, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7c, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7c, // q
    0x7c, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3f, 0x44, 0x40, 0x20, // t
    0x3c, 0x40, 0x40, 0x20, 0x7c, // u
    0x1c, 0x20, 0x40, 0x20, 0x1c, // v
    0x3c, 0x40, 0x30, 0x40, 0x3c, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0c, 0x50, 0x50, 0x50, 0x3c, // y
    0x44, 0x64, 0x54, 0x4c, 0x44, // z
    0x00, 0x08, 0x36, 0x41, 0x00, // {
    0x00, 0x00, 0x7f, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x08, 0x08, 0x2a, 0x1c, 0x08  // ~
};

Window::Window(uint32_t width, uint32_t height, gpu::GpuDevice* gpu, Compositor* compositor)
    : next(nullptr), prev(nullptr),
      backing_store_(nullptr), gpu_(gpu), compositor_(compositor),
      x_(0), y_(0), z_order_(0), id_(0),
      visible_(true), transparent_(false), alpha_(255), user_data_(nullptr),
      event_handler_(nullptr), event_user_data_(nullptr),
      root_widget_(nullptr), captured_widget_(nullptr) {
    title_[0] = '\0';
    backing_store_ = new gpu::Surface(width, height);
    if (compositor_) {
        compositor_->add_window(this);
    }
}

Window::~Window() {
    if (compositor_) {
        invalidate(); // 必须在释放 backing_store_ 前标记旧区域脏
        compositor_->remove_window(this);
    }
    delete backing_store_;
    backing_store_ = nullptr;
}

void Window::move(int32_t x, int32_t y) {
    if (x_ == x && y_ == y)
        return;
    // 标记旧区域脏
    invalidate();
    x_ = x;
    y_ = y;
    // 标记新区域脏
    invalidate();
}

bool Window::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return false;

    if (backing_store_ && backing_store_->get_width() == width && backing_store_->get_height() == height)
        return true;

    // 标记旧区域脏
    invalidate();

    auto* new_surface = new gpu::Surface(width, height);
    if (!new_surface || !new_surface->get_buffer()) {
        delete new_surface;
        return false;
    }

    uint16_t* dst_buf = static_cast<uint16_t*>(new_surface->get_buffer());
    memset(dst_buf, 0, width * height * sizeof(uint16_t));

    // 如果已有旧表面，尽量拷贝重叠区域
    if (backing_store_ && backing_store_->get_buffer()) {
        uint32_t copy_w = std::min(width, backing_store_->get_width());
        uint32_t copy_h = std::min(height, backing_store_->get_height());
        uint16_t* src_buf = static_cast<uint16_t*>(backing_store_->get_buffer());
        uint32_t old_w = backing_store_->get_width();

        for (uint32_t row = 0; row < copy_h; ++row) {
            memcpy(&dst_buf[row * width], &src_buf[row * old_w], copy_w * sizeof(uint16_t));
        }
        delete backing_store_;
    }

    backing_store_ = new_surface;

    // 标记新区域脏
    invalidate();
    return true;
}

void Window::set_z_order(int32_t z) {
    if (z_order_ != z) {
        z_order_ = z;
        if (compositor_) {
            compositor_->remove_window(this);
            compositor_->add_window(this);
        }
        invalidate();
    }
}

void Window::bring_to_front() {
    if (compositor_) {
        compositor_->raise_to_top(this);
    }
}

void Window::send_to_back() {
    if (compositor_) {
        compositor_->send_to_back(this);
    }
}

void Window::set_visible(bool visible) {
    if (visible_ != visible) {
        visible_ = visible;
        invalidate();
    }
}

void Window::set_alpha(uint8_t alpha) {
    if (alpha_ != alpha) {
        alpha_ = alpha;
        invalidate();
    }
}

void Window::set_transparent(bool transparent) {
    if (transparent_ != transparent) {
        transparent_ = transparent;
        invalidate();
    }
}

void Window::set_title(const char* title) {
    if (!title) {
        title_[0] = '\0';
        return;
    }
    size_t i = 0;
    while (title[i] && i < sizeof(title_) - 1) {
        title_[i] = title[i];
        ++i;
    }
    title_[i] = '\0';
}

bool Window::is_focused() const {
    if (!compositor_)
        return false;
    return compositor_->get_focused_window() == this;
}

void Window::focus() {
    if (compositor_) {
        compositor_->set_focused_window(this);
    }
}

void Window::invalidate() {
    if (compositor_ && backing_store_) {
        Rect r;
        r.x = x_;
        r.y = y_;
        r.w = static_cast<int32_t>(backing_store_->get_width());
        r.h = static_cast<int32_t>(backing_store_->get_height());
        compositor_->add_damage(r);
    }
}

void Window::invalidate(const Rect& sub_rect) {
    if (compositor_ && backing_store_) {
        if (sub_rect.is_empty())
            return;
        Rect r;
        r.x = x_ + sub_rect.x;
        r.y = y_ + sub_rect.y;
        r.w = sub_rect.w;
        r.h = sub_rect.h;
        compositor_->add_damage(r);
    }
}

bool Window::hit_test(int32_t screen_x, int32_t screen_y) const {
    if (!visible_ || !backing_store_)
        return false;
    return screen_x >= x_ && screen_x < x_ + static_cast<int32_t>(backing_store_->get_width()) &&
           screen_y >= y_ && screen_y < y_ + static_cast<int32_t>(backing_store_->get_height());
}

void Window::set_event_handler(WindowEventHandler handler, void* user_data) {
    event_handler_ = handler;
    event_user_data_ = user_data;
}

void Window::set_root_widget(Widget* root) {
    root_widget_ = root;
    if (root_widget_) {
        paint_widgets();
    }
}

void Window::paint_widgets() {
    if (root_widget_) {
        root_widget_->paint_tree(this);
        invalidate();
    }
}

bool Window::handle_event(const InputEvent& event) {
    if (event_handler_) {
        event_handler_(this, event, event_user_data_);
        return true;
    }

    if (!root_widget_)
        return false;

    // event 处于当前窗口坐标系下
    if (event.type == InputEventType::PointerDown) {
        Widget* hit = root_widget_->hit_test(event.x, event.y);
        if (hit) {
            captured_widget_ = hit;
            return hit->on_event(this, event);
        }
    } else if (event.type == InputEventType::PointerMove) {
        if (captured_widget_) {
            return captured_widget_->on_event(this, event);
        } else {
            Widget* hit = root_widget_->hit_test(event.x, event.y);
            if (hit) {
                return hit->on_event(this, event);
            }
        }
    } else if (event.type == InputEventType::PointerUp) {
        if (captured_widget_) {
            Widget* target = captured_widget_;
            captured_widget_ = nullptr;
            return target->on_event(this, event);
        }
    } else {
        return root_widget_->on_event(this, event);
    }

    return false;
}

// ============================================================
// 绘图原语 API
// ============================================================

void Window::clear(uint16_t color) {
    if (!backing_store_)
        return;
    fill_rect(0, 0, static_cast<int32_t>(backing_store_->get_width()),
              static_cast<int32_t>(backing_store_->get_height()), color);
}

void Window::draw_pixel(int32_t x, int32_t y, uint16_t color) {
    if (!backing_store_ || x < 0 || y < 0)
        return;
    uint32_t w = backing_store_->get_width();
    uint32_t h = backing_store_->get_height();
    if (static_cast<uint32_t>(x) >= w || static_cast<uint32_t>(y) >= h)
        return;

    uint16_t* buf = static_cast<uint16_t*>(backing_store_->get_buffer());
    if (buf) {
        buf[static_cast<uint32_t>(y) * w + static_cast<uint32_t>(x)] = color;
    }
}

void Window::fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!backing_store_ || w <= 0 || h <= 0)
        return;

    int32_t sw = static_cast<int32_t>(backing_store_->get_width());
    int32_t sh = static_cast<int32_t>(backing_store_->get_height());

    // 边界裁剪
    int32_t x0 = std::max(0, x);
    int32_t y0 = std::max(0, y);
    int32_t x1 = std::min(sw, x + w);
    int32_t y1 = std::min(sh, y + h);

    if (x0 >= x1 || y0 >= y1)
        return;

    uint32_t cw = static_cast<uint32_t>(x1 - x0);
    uint32_t ch = static_cast<uint32_t>(y1 - y0);

    if (gpu_) {
        gpu::GpuCommand cmd;
        cmd.opcode = gpu::GpuOpcode::FillRect;
        cmd.dst_surface = backing_store_;
        cmd.dst_x = static_cast<uint32_t>(x0);
        cmd.dst_y = static_cast<uint32_t>(y0);
        cmd.width = cw;
        cmd.height = ch;
        cmd.args.fill.color = color;
        gpu_->submit(&cmd, 1);
    } else {
        uint16_t* buf = static_cast<uint16_t*>(backing_store_->get_buffer());
        for (uint32_t r = 0; r < ch; ++r) {
            uint32_t idx = (static_cast<uint32_t>(y0) + r) * static_cast<uint32_t>(sw) + static_cast<uint32_t>(x0);
            for (uint32_t c = 0; c < cw; ++c) {
                buf[idx + c] = color;
            }
        }
    }
    invalidate(Rect{x0, y0, static_cast<int32_t>(cw), static_cast<int32_t>(ch)});
}

void Window::draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!backing_store_ || w <= 0 || h <= 0)
        return;

    // 上边与下边
    fill_rect(x, y, w, 1, color);
    if (h > 1) {
        fill_rect(x, y + h - 1, w, 1, color);
    }
    // 左边与右边
    if (h > 2) {
        fill_rect(x, y + 1, 1, h - 2, color);
        if (w > 1) {
            fill_rect(x + w - 1, y + 1, 1, h - 2, color);
        }
    }
}

void Window::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color) {
    if (!backing_store_)
        return;

    int32_t dx = std::abs(x1 - x0);
    int32_t dy = std::abs(y1 - y0);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    int32_t min_x = std::min(x0, x1);
    int32_t min_y = std::min(y0, y1);
    int32_t max_x = std::max(x0, x1);
    int32_t max_y = std::max(y0, y1);

    while (true) {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }

    invalidate(Rect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1});
}

void Window::draw_circle(int32_t xc, int32_t yc, int32_t r, uint16_t color) {
    if (!backing_store_ || r < 0)
        return;

    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    auto plot8 = [&](int32_t cx, int32_t cy, int32_t px, int32_t py) {
        draw_pixel(cx + px, cy + py, color);
        draw_pixel(cx - px, cy + py, color);
        draw_pixel(cx + px, cy - py, color);
        draw_pixel(cx - px, cy - py, color);
        draw_pixel(cx + py, cy + px, color);
        draw_pixel(cx - py, cy + px, color);
        draw_pixel(cx + py, cy - px, color);
        draw_pixel(cx - py, cy - px, color);
    };

    while (y >= x) {
        plot8(xc, yc, x, y);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }

    invalidate(Rect{xc - r, yc - r, 2 * r + 1, 2 * r + 1});
}

void Window::fill_circle(int32_t xc, int32_t yc, int32_t r, uint16_t color) {
    if (!backing_store_ || r < 0)
        return;

    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    auto draw_span = [&](int32_t lx, int32_t ly, int32_t w) {
        fill_rect(lx, ly, w, 1, color);
    };

    while (y >= x) {
        draw_span(xc - x, yc + y, 2 * x + 1);
        draw_span(xc - x, yc - y, 2 * x + 1);
        draw_span(xc - y, yc + x, 2 * y + 1);
        draw_span(xc - y, yc - x, 2 * y + 1);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }

    invalidate(Rect{xc - r, yc - r, 2 * r + 1, 2 * r + 1});
}

void Window::draw_text(int32_t x, int32_t y, const char* text, uint16_t color, uint16_t bg_color, bool transparent_bg, uint8_t scale) {
    if (!backing_store_ || !text)
        return;

    uint8_t sc = std::max<uint8_t>(1, scale);
    int32_t cursor_x = x;
    int32_t start_x = x;
    int32_t start_y = y;
    int32_t max_x = x;

    while (*text) {
        char c = *text++;
        if (c == '\n') {
            max_x = std::max(max_x, cursor_x);
            cursor_x = start_x;
            y += 8 * sc;
            continue;
        }
        if (c < ' ' || c > '~')
            continue;

        uint16_t char_idx = c - ' ';
        const uint8_t* char_data = &GUIX_FONT5X7[char_idx * 5];

        if (!transparent_bg) {
            fill_rect(cursor_x, y, 6 * sc, 8 * sc, bg_color);
        }

        for (int col = 0; col < 5; ++col) {
            uint8_t col_bits = char_data[col];
            for (int row = 0; row < 7; ++row) {
                if (col_bits & (1 << row)) {
                    if (sc == 1) {
                        draw_pixel(cursor_x + col, y + row, color);
                    } else {
                        fill_rect(cursor_x + col * sc, y + row * sc, sc, sc, color);
                    }
                }
            }
        }

        cursor_x += 6 * sc;
        max_x = std::max(max_x, cursor_x);
    }

    invalidate(Rect{start_x, start_y, max_x - start_x, (y + 8 * sc) - start_y});
}

void Window::blit(int32_t dst_x, int32_t dst_y, const gpu::Surface* src, uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h) {
    if (!backing_store_ || !src || !gpu_ || w == 0 || h == 0)
        return;

    int32_t bw = static_cast<int32_t>(backing_store_->get_width());
    int32_t bh = static_cast<int32_t>(backing_store_->get_height());

    int32_t dx = dst_x;
    int32_t dy = dst_y;
    int32_t sx = static_cast<int32_t>(src_x);
    int32_t sy = static_cast<int32_t>(src_y);
    int32_t cw = static_cast<int32_t>(w);
    int32_t ch = static_cast<int32_t>(h);

    if (dx < 0) {
        sx += -dx;
        cw += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy += -dy;
        ch += dy;
        dy = 0;
    }
    if (dx + cw > bw) {
        cw = bw - dx;
    }
    if (dy + ch > bh) {
        ch = bh - dy;
    }

    if (cw <= 0 || ch <= 0 || sx < 0 || sy < 0 ||
        static_cast<uint32_t>(sx) >= src->get_width() ||
        static_cast<uint32_t>(sy) >= src->get_height()) {
        return;
    }

    gpu::GpuCommand cmd;
    cmd.opcode = gpu::GpuOpcode::Blit;
    cmd.dst_surface = backing_store_;
    cmd.dst_x = static_cast<uint32_t>(dx);
    cmd.dst_y = static_cast<uint32_t>(dy);
    cmd.width = static_cast<uint32_t>(cw);
    cmd.height = static_cast<uint32_t>(ch);
    cmd.args.blit.src_surface = const_cast<gpu::Surface*>(src);
    cmd.args.blit.src_x = static_cast<uint32_t>(sx);
    cmd.args.blit.src_y = static_cast<uint32_t>(sy);

    gpu_->submit(&cmd, 1);
    invalidate(Rect{dx, dy, cw, ch});
}

void Window::blend(int32_t dst_x, int32_t dst_y, const gpu::Surface* src, uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h, uint8_t alpha) {
    if (!backing_store_ || !src || !gpu_ || w == 0 || h == 0)
        return;

    int32_t bw = static_cast<int32_t>(backing_store_->get_width());
    int32_t bh = static_cast<int32_t>(backing_store_->get_height());

    int32_t dx = dst_x;
    int32_t dy = dst_y;
    int32_t sx = static_cast<int32_t>(src_x);
    int32_t sy = static_cast<int32_t>(src_y);
    int32_t cw = static_cast<int32_t>(w);
    int32_t ch = static_cast<int32_t>(h);

    if (dx < 0) {
        sx += -dx;
        cw += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy += -dy;
        ch += dy;
        dy = 0;
    }
    if (dx + cw > bw) {
        cw = bw - dx;
    }
    if (dy + ch > bh) {
        ch = bh - dy;
    }

    if (cw <= 0 || ch <= 0 || sx < 0 || sy < 0 ||
        static_cast<uint32_t>(sx) >= src->get_width() ||
        static_cast<uint32_t>(sy) >= src->get_height()) {
        return;
    }

    gpu::GpuCommand cmd;
    cmd.opcode = gpu::GpuOpcode::Blend;
    cmd.dst_surface = backing_store_;
    cmd.dst_x = static_cast<uint32_t>(dx);
    cmd.dst_y = static_cast<uint32_t>(dy);
    cmd.width = static_cast<uint32_t>(cw);
    cmd.height = static_cast<uint32_t>(ch);
    cmd.args.blend.src_surface = const_cast<gpu::Surface*>(src);
    cmd.args.blend.src_x = static_cast<uint32_t>(sx);
    cmd.args.blend.src_y = static_cast<uint32_t>(sy);
    cmd.args.blend.alpha = alpha;

    gpu_->submit(&cmd, 1);
    invalidate(Rect{dx, dy, cw, ch});
}

} // namespace guix
} // namespace auroraos

