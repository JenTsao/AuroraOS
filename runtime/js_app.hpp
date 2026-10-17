#ifndef AURORAOS_RUNTIME_JS_APP_HPP
#define AURORAOS_RUNTIME_JS_APP_HPP

#include "app_base.hpp"
#include "../apps/js_mini_program_engine.hpp"

namespace auroraos {
namespace runtime {

template <size_t HeapSize = 131072> class JsApp : public AppBase {
public:
    JsApp(const AppManifest& manifest, const char* script_path) : AppBase(manifest), script_path_(script_path) {}

    JsMiniProgramEngine<HeapSize>& engine() noexcept {
        return engine_;
    }

    const JsMiniProgramEngine<HeapSize>& engine() const noexcept {
        return engine_;
    }

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
        engine_.teardown();
    }

private:
    const char* script_path_;
    JsMiniProgramEngine<HeapSize> engine_;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_JS_APP_HPP
