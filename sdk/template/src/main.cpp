#include <auroraos/runtime/app_base.hpp>
#include <auroraos/syscall/syscall.hpp>
#include <auroraos/runtime/app_manifest.hpp>

using namespace auroraos;

class MyCustomApp : public AppBase {
public:
    MyCustomApp(const AppManifest& manifest) : AppBase(manifest) {}

    void on_start() override {
        sys_print("MyCustomApp Started!\r\n");
    }

    void on_update() override {
        // App logic per frame
    }

    void on_stop() override {
        sys_print("MyCustomApp Stopped!\r\n");
    }
};

int main() {
    AppManifest manifest = {
        .name = "MyCustomApp",
        .required_caps = CAP_UI | CAP_SENSOR,
        .memory_limit = 1024 * 32, // 32KB
        .cpu_quota = 50
    };
    MyCustomApp app(manifest);
    app.start();
    while (app.state() == AppState::RUNNING) {
        app.update();
    }
    return 0;
}
