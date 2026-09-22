#ifndef DISPLAY_SERVICE_HPP
#define DISPLAY_SERVICE_HPP

#include "display_ipc.hpp"
#include <stdint.h>

namespace auroraos {
namespace display {

class DisplayServer {
public:
    static constexpr uint16_t DEFAULT_WIDTH = 128;
    static constexpr uint16_t DEFAULT_HEIGHT = 64;

    static DisplayServer& instance() {
        static DisplayServer server;
        return server;
    }

    void init();
    void process_request(const DisplayRequest& req, DisplayReply& reply);
    [[noreturn]] void run();

    // 状态查询接口 (供测试和驱动刷新使用)
    uint16_t get_pixel(uint16_t x, uint16_t y) const;
    uint16_t get_width() const { return width_; }
    uint16_t get_height() const { return height_; }
    uint32_t get_present_count() const { return present_count_; }

private:
    DisplayServer();

    uint16_t width_ = DEFAULT_WIDTH;
    uint16_t height_ = DEFAULT_HEIGHT;
    uint32_t present_count_ = 0;
    bool initialized_ = false;

    // 静态零堆分配双缓冲或主帧缓冲 (128x64 RGB565 = 16KB)
    uint16_t buffer_[DEFAULT_WIDTH * DEFAULT_HEIGHT];
};

} // namespace display
} // namespace auroraos

#endif // DISPLAY_SERVICE_HPP
