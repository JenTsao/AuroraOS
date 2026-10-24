#include <auroraos/runtime/app_base.hpp>
#include <auroraos/syscall/syscall.hpp>
#include <auroraos/runtime/app_manifest.hpp>
#include <string.h>

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
    // 工程统一使用严格 ISO C++17（无 GNU 扩展），不能用指定初始化器，逐字段填充
    AppManifest manifest;
    memcpy(manifest.name, "MyCustomApp", sizeof("MyCustomApp"));
    memcpy(manifest.version, "1.0", sizeof("1.0"));
    memcpy(manifest.author, "AuroraDev", sizeof("AuroraDev"));
    manifest.required_caps = static_cast<uint32_t>(AppCapability::UI | AppCapability::Sensor);
    manifest.max_memory_bytes = 1024 * 32; // 32KB
    manifest.max_cpu_percent = 50;
    manifest.priority = 10;

    MyCustomApp app(manifest);
    app.start();

    // Main event loop
    while (app.get_state() == AppState::Running) {
        // App logic per frame
        sys_yield();
    }
    return 0;
}
