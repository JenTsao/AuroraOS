#ifndef AURORA_APPS_DYNAMIC_JS_WATCH_FACE_SCREEN_HPP
#define AURORA_APPS_DYNAMIC_JS_WATCH_FACE_SCREEN_HPP

#include "../../../ui/screen.hpp"
#include "../../../ui/screen_navigator.hpp"
#include "../../../apps/js_mini_program_engine.hpp"
#include "heart_rate_screen.hpp"

namespace aurora {
namespace watch {

template <size_t HeapSize = 131072> class DynamicJsWatchFaceScreen : public UI::Screen {
private:
    JsMiniProgramEngine<HeapSize> engine_;
    char filepath_[128]{};

public:
    explicit DynamicJsWatchFaceScreen(const char* filepath) {
        if (filepath) {
            size_t i = 0;
            while (i < sizeof(filepath_) - 1 && filepath[i]) {
                filepath_[i] = filepath[i];
                ++i;
            }
            filepath_[i] = '\0';
        }
    }

    void on_create() override {
        if (!engine_.init()) {
            return;
        }
        if (engine_.load_app_from_file(filepath_)) {
            UI::View* root_view = engine_.call_create_ui();
            if (root_view) {
                // 所有权已由 call_create_ui 标记 ud->attached = true 并移交本 Screen：
                // 析构顺序由 C++ 保证——成员 engine_ 先于基类 ViewGroup 析构，
                // CtxGuard 先摘除所有 JS 回调、finalizer 跳过 attached 视图，
                // 随后基类 ViewGroup 统一 delete 子视图树。
                this->add_child(root_view);
            }
            engine_.call_hook("on_create");
            engine_.call_hook("onLoad");
        }
    }

    void on_show() override {
        engine_.call_hook("on_show");
        engine_.call_hook("onShow");
    }

    void on_hide() override {
        engine_.call_hook("on_hide");
        engine_.call_hook("onHide");
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (event.type == GestureType::SWIPE_LEFT) {
            UI::ScreenNavigator::instance().push(new HeartRateScreen(), UI::ScreenNavigator::TransitionType::NONE);
            return true;
        }
        engine_.call_hook("on_gesture");
        return UI::ViewGroup::handle_gesture(event);
    }

    void on_tick(uint32_t /*delta_ticks*/) {
        engine_.call_hook("on_tick");
    }

    JsMiniProgramEngine<HeapSize>& engine() noexcept {
        return engine_;
    }
};

} // namespace watch
} // namespace aurora

#endif // AURORA_APPS_DYNAMIC_JS_WATCH_FACE_SCREEN_HPP
