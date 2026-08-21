#ifndef AURORA_GUIX_WIDGET_SLIDER_HPP
#define AURORA_GUIX_WIDGET_SLIDER_HPP

#include "widget.hpp"

namespace auroraos {
namespace guix {

class Slider;

typedef void (*SliderValueChangedHandler)(Slider* slider, int32_t new_val, void* user_data);

class Slider : public Widget {
public:
    Slider(int32_t min_val = 0, int32_t max_val = 100, int32_t cur_val = 0);
    ~Slider() override = default;

    void set_range(int32_t min_val, int32_t max_val);
    void set_value(int32_t val);
    int32_t get_value() const { return value_; }
    int32_t get_min() const { return min_val_; }
    int32_t get_max() const { return max_val_; }

    void set_colors(uint16_t track_color, uint16_t active_color, uint16_t thumb_color);
    void set_value_changed_handler(SliderValueChangedHandler handler, void* user_data = nullptr);

    bool on_event(Window* win, const InputEvent& event) override;
    void paint(Window* win) override;

    bool is_dragging() const { return is_dragging_; }

private:
    int32_t min_val_;
    int32_t max_val_;
    int32_t value_;

    uint16_t track_color_;
    uint16_t active_color_;
    uint16_t thumb_color_;

    bool is_dragging_;
    SliderValueChangedHandler value_handler_;
    void* value_user_data_;

    void update_value_from_x(Window* win, int32_t window_x);
};

} // namespace guix
} // namespace auroraos

#endif // AURORA_GUIX_WIDGET_SLIDER_HPP
