#ifndef AURORA_WATCH_FACE_STORE_SCREEN_HPP
#define AURORA_WATCH_FACE_STORE_SCREEN_HPP

#include "../../../ui/screen.hpp"
#include "../../../ui/widgets/text_view.hpp"
#include "../../../ui/screen_navigator.hpp"
#include "dynamic_watch_face_screen.hpp"

namespace aurora {
namespace watch {

class WatchFaceStoreScreen : public UI::Screen {
private:
    UI::TextView* title_;
    UI::TextView* item1_;
    UI::TextView* item2_;

    // Static callback functions for click events
    static void on_item1_click(UI::View* v, void* ctx) {
        UI::ScreenNavigator::instance().push(new DynamicWatchFaceScreen("/lfs/wf_neon.lua"));
    }

    static void on_item2_click(UI::View* v, void* ctx) {
        UI::ScreenNavigator::instance().push(new DynamicWatchFaceScreen("/lfs/wf_classic.lua"));
    }

public:
    WatchFaceStoreScreen() : title_(nullptr), item1_(nullptr), item2_(nullptr) {}

    void on_create() override {
        // Title
        title_ = new UI::TextView(20, 20, "Watch Faces", 0xFFFF, 0, 2);
        add_child(title_);

        // Simulated List items (using TextViews as buttons)
        item1_ = new UI::TextView(20, 100, "1. Neon (Lua)", 0x07E0, 0, 2);
        item1_->set_on_click_listener(on_item1_click, this);
        add_child(item1_);

        item2_ = new UI::TextView(20, 160, "2. Classic (Lua)", 0x07E0, 0, 2);
        item2_->set_on_click_listener(on_item2_click, this);
        add_child(item2_);
    }

    bool handle_gesture(const UI::GestureEvent& event) override {
        if (event.type == GestureType::SWIPE_RIGHT) {
            UI::ScreenNavigator::instance().pop();
            return true;
        }
        
        // Let the ViewGroup handle clicks
        return UI::ViewGroup::handle_gesture(event);
    }
};

} // namespace watch
} // namespace aurora

#endif // AURORA_WATCH_FACE_STORE_SCREEN_HPP
