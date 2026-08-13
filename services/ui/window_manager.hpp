#ifndef AURORAOS_WINDOW_MANAGER_HPP
#define AURORAOS_WINDOW_MANAGER_HPP

#include <stdint.h>

namespace auroraos {
namespace ui_service {

constexpr int MAX_WINDOWS = 8;

struct Window {
    int id;
    int x;
    int y;
    int width;
    int height;
    char title[16];
    bool active;
    uint32_t owner_cap; // IPC sender id who created this window
};

class WindowManager {
public:
    static WindowManager& instance() {
        static WindowManager wm;
        return wm;
    }

    int create_window(int x, int y, int width, int height, const char* title, uint32_t owner_cap);
    bool destroy_window(int id, uint32_t owner_cap);

    Window* get_window(int id);

    // For rendering all windows
    void render_all(); // This would call the underlying Renderer

private:
    WindowManager() = default;

    Window windows_[MAX_WINDOWS] = {};
    int next_id_ = 1;
};

} // namespace ui_service
} // namespace auroraos

#endif // AURORAOS_WINDOW_MANAGER_HPP
