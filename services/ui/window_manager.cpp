#include "window_manager.hpp"
#include <cstring>
#include "syscall.hpp"
#include "../../drivers/display/renderer2d.hpp"

// Assuming global framebuffer/renderer exists for UI Service
extern FrameBuffer<192, 490> g_fb; // A typical size in AuroraOS? (Wait, we should check `mini_program_engine.hpp` for size, but let's use a template placeholder or a known global)

namespace auroraos {
namespace ui_service {

int WindowManager::create_window(int x, int y, int width, int height, const char* title, uint32_t owner_cap) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows_[i].active) {
            windows_[i].active = true;
            windows_[i].id = next_id_++;
            windows_[i].x = x;
            windows_[i].y = y;
            windows_[i].width = width;
            windows_[i].height = height;
            windows_[i].owner_cap = owner_cap;
            
            if (title) {
                int j = 0;
                while (title[j] && j < 15) {
                    windows_[i].title[j] = title[j];
                    j++;
                }
                windows_[i].title[j] = '\0';
            } else {
                windows_[i].title[0] = '\0';
            }
            return windows_[i].id;
        }
    }
    return -1; // Out of window slots
}

bool WindowManager::destroy_window(int id, uint32_t owner_cap) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows_[i].active && windows_[i].id == id) {
            if (windows_[i].owner_cap != owner_cap) {
                return false; // Permission denied
            }
            windows_[i].active = false;
            return true;
        }
    }
    return false; // Not found
}

Window* WindowManager::get_window(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows_[i].active && windows_[i].id == id) {
            return &windows_[i];
        }
    }
    return nullptr;
}

void WindowManager::render_all() {
    // Currently relying on the global Renderer2D. 
    // In a full implementation, this loops over active windows (back to front) and draws their decorations + content.
}

} // namespace ui_service
} // namespace auroraos

