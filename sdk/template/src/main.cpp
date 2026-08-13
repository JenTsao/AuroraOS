#include <auroraos/runtime/app_base.hpp>
#include <auroraos/syscall/syscall.hpp>
#include <auroraos/runtime/app_manifest.hpp>

using namespace auroraos;
using namespace auroraos::runtime;

class MyCustomApp : public AppBase {
public:
    MyCustomApp(const AppManifest& manifest) : AppBase(manifest) {}

protected:
    bool on_start() override {
        sys_print("MyCustomApp Started!\r\n");
        return true;
    }

    void on_stop() override {
        sys_print("MyCustomApp Stopped!\r\n");
    }
};

int main() {
    AppManifest manifest = {.name = "MyCustomApp",
                            .version = "1.0",
                            .author = "AuroraDev",
                            .required_caps = static_cast<uint32_t>(AppCapability::UI | AppCapability::Sensor),
                            .max_memory_bytes = 1024 * 32, // 32KB
                            .max_cpu_percent = 50,
                            .priority = 10};

    MyCustomApp app(manifest);
    app.start();

    // Main event loop
    while (app.get_state() == AppState::Running) {
        // App logic per frame
        sys_yield();
    }
    return 0;
}
