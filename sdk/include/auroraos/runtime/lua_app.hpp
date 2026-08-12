#ifndef AURORAOS_RUNTIME_LUA_APP_HPP
#define AURORAOS_RUNTIME_LUA_APP_HPP

#include "app_base.hpp"
#include "../apps/mini_program_engine.hpp"

namespace auroraos {
namespace runtime {

class LuaApp : public AppBase {
public:
    LuaApp(const AppManifest& manifest, const char* script_path)
        : AppBase(manifest), script_path_(script_path) {}

protected:
    bool on_start() override {
        if (!engine_.init()) {
            return false;
        }
        if (!engine_.load_app_from_file(script_path_)) {
            return false;
        }
        engine_.call_hook("onLoad");
        engine_.call_hook("onShow");
        return true;
    }

    void on_pause() override {
        engine_.call_hook("onHide");
    }

    void on_resume() override {
        engine_.call_hook("onShow");
    }

    void on_stop() override {
        engine_.call_hook("onHide");
        engine_.call_hook("onUnload");
    }

    void on_destroy() override {
        // MiniProgramEngine cleans up its own lua_State in destructor
    }

private:
    const char* script_path_;
    MiniProgramEngine engine_;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_LUA_APP_HPP


