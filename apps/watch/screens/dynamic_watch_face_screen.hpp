#ifndef AURORA_APPS_DYNAMIC_WATCH_FACE_SCREEN_HPP
#define AURORA_APPS_DYNAMIC_WATCH_FACE_SCREEN_HPP

#include "../../../ui/screen.hpp"
#include "../../../apps/mini_program_engine.hpp"
#include "heart_rate_screen.hpp"
#include "../../../ui/screen_navigator.hpp"

// We redefine ViewUserData to extract the view returned by Lua
struct ViewUserData {
    UI::View* view;
};

namespace aurora {
namespace watch {

class DynamicWatchFaceScreen : public UI::Screen {
private:
    MiniProgramEngine engine_;
    char filepath_[128];

public:
    DynamicWatchFaceScreen(const char* filepath) {
        int i = 0;
        while (filepath[i] && i < 127) {
            filepath_[i] = filepath[i];
            i++;
        }
        filepath_[i] = '\0';
    }

    void on_create() override {
        engine_.init();
        if (engine_.load_app_from_file(filepath_)) {
            lua_State* L = engine_.get_lua_state();
            lua_getglobal(L, "create_ui");
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
                    if (lua_isuserdata(L, -1)) {
                        ViewUserData* ud = static_cast<ViewUserData*>(lua_touserdata(L, -1));
                        if (ud && ud->view) {
                            this->add_child(ud->view);
                        }
                    }
                    lua_pop(L, 1);
                } else {
                    lua_pop(L, 1); // pop error
                }
            } else {
                lua_pop(L, 1); // pop nil
            }
            engine_.call_hook("on_create");
        }
    }

    void on_show() override {
        engine_.call_hook("on_show");
    }

    void on_hide() override {
        engine_.call_hook("on_hide");
    }

    bool handle_gesture(const GestureEvent& event) override {
        if (event.type == GestureType::SWIPE_LEFT) {
            UI::ScreenNavigator::instance().push(new HeartRateScreen());
            return true;
        } else if (event.type == GestureType::SWIPE_DOWN) {
            // Can be used for quick panel
            return true;
        }

        // Pass to Lua if needed
        engine_.call_hook("on_gesture");

        return UI::ViewGroup::handle_gesture(event);
    }

    // Exposed via WatchApp's on_background_tick potentially, or using a local on_tick
    void on_tick(uint32_t delta_ticks) {
        engine_.call_hook("on_tick");
    }
};

} // namespace watch
} // namespace aurora

#endif // AURORA_APPS_DYNAMIC_WATCH_FACE_SCREEN_HPP
