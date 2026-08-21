#ifndef AURORA_GUIX_COMPOSITOR_HPP
#define AURORA_GUIX_COMPOSITOR_HPP

#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include "../drivers/gpu/gpu_device.hpp"
#include "../drivers/gpu/surface.hpp"

namespace auroraos {
namespace guix {

class Window;

// ============================================================
// 基础几何与输入事件类型
// ============================================================

struct Point {
    int32_t x;
    int32_t y;
};

struct Rect {
    int32_t x, y;
    int32_t w, h;

    bool is_empty() const {
        return w <= 0 || h <= 0;
    }

    void clear() {
        x = y = 0;
        w = h = 0;
    }

    bool contains(int32_t px, int32_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    bool intersects(const Rect& other) const {
        return !(x + w <= other.x || other.x + other.w <= x ||
                 y + h <= other.y || other.y + other.h <= y);
    }

    Rect intersect(const Rect& other) const {
        if (!intersects(other)) {
            return {0, 0, 0, 0};
        }
        int32_t ix = std::max(x, other.x);
        int32_t iy = std::max(y, other.y);
        int32_t iw = std::min(x + w, other.x + other.w) - ix;
        int32_t ih = std::min(y + h, other.y + other.h) - iy;
        return {ix, iy, iw, ih};
    }

    void union_rect(const Rect& other);
};

struct Color {
    static constexpr uint16_t Black     = 0x0000;
    static constexpr uint16_t White     = 0xFFFF;
    static constexpr uint16_t Red       = 0xF800;
    static constexpr uint16_t Green     = 0x07E0;
    static constexpr uint16_t Blue      = 0x001F;
    static constexpr uint16_t Yellow    = 0xFFE0;
    static constexpr uint16_t Cyan      = 0x07FF;
    static constexpr uint16_t Magenta   = 0xF81F;
    static constexpr uint16_t Gray      = 0x8410;
    static constexpr uint16_t DarkGray  = 0x4208;
    static constexpr uint16_t LightGray = 0xC618;

    static inline constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
        return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
};

enum class InputEventType : uint8_t {
    None = 0,
    PointerDown,
    PointerUp,
    PointerMove,
    KeyDown,
    KeyUp,
    FocusIn,
    FocusOut
};

struct InputEvent {
    InputEventType type;
    int32_t x;          // Screen coordinates or window-local coordinates
    int32_t y;
    uint32_t key_code;  // Key code for KeyDown / KeyUp
    uint32_t timestamp; // System tick count or timestamp ms
    uint8_t button;     // 0=primary/left, 1=secondary/right, 2=middle
};

// ============================================================
// 合成器核心类 (Compositor)
// ============================================================

class Compositor {
public:
    Compositor(gpu::Surface* screen_surface, gpu::GpuDevice* gpu);
    ~Compositor();

    // 禁用拷贝与赋值
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // ---- 窗口管理 ----
    void add_window(Window* win);
    void remove_window(Window* win);
    void raise_to_top(Window* win);
    void send_to_back(Window* win);

    size_t get_window_count() const;
    Window* get_window_head() const { return window_head_; }
    Window* get_window_tail() const { return window_tail_; }
    Window* get_window_by_id(uint32_t id) const;
    Window* find_window_at(int32_t x, int32_t y) const;

    // ---- 焦点管理 ----
    void set_focused_window(Window* win);
    Window* get_focused_window() const { return focused_window_; }

    // ---- 背景管理 ----
    void set_background_color(uint16_t color) { background_color_ = color; invalidate_all(); }
    uint16_t get_background_color() const { return background_color_; }
    void set_background_surface(gpu::Surface* bg) { background_surface_ = bg; invalidate_all(); }
    gpu::Surface* get_background_surface() const { return background_surface_; }

    // ---- 脏矩形与损伤管理 ----
    void add_damage(const Rect& rect);
    void invalidate_all();
    const Rect& get_damage_rect() const { return damage_rect_; }
    bool has_damage() const { return !damage_rect_.is_empty(); }

    // ---- 事件分发 ----
    bool dispatch_input_event(const InputEvent& event);

    // ---- 核心合成与渲染 ----
    void composite();

    // ---- 状态与诊断 ----
    uint32_t get_frame_count() const { return frame_count_; }
    gpu::Surface* get_screen_surface() const { return screen_; }
    gpu::GpuDevice* get_gpu_device() const { return gpu_; }

private:
    gpu::Surface* screen_;
    gpu::GpuDevice* gpu_;

    Window* window_head_;
    Window* window_tail_;
    Window* focused_window_;
    Window* pointer_captured_window_;

    Rect damage_rect_;
    uint16_t background_color_;
    gpu::Surface* background_surface_;
    uint32_t frame_count_;
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_COMPOSITOR_HPP
