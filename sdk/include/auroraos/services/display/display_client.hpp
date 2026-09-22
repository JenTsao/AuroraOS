#ifndef DISPLAY_CLIENT_HPP
#define DISPLAY_CLIENT_HPP

#include "display_ipc.hpp"
#include <stdint.h>

namespace auroraos {
namespace display {

class DisplayClient {
public:
    static DisplayClient& instance() {
        static DisplayClient client;
        return client;
    }

    void set_endpoint(int ep_cap);
    int get_endpoint() const;

    bool get_info(DisplayInfoReply& info);
    bool clear(uint16_t color = 0x0000);
    bool draw_pixel(int16_t x, int16_t y, uint16_t color);
    bool fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color);
    bool draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    bool present();

    using DirectHandler = void (*)(const DisplayRequest& req, DisplayReply& reply);
    static void set_direct_handler(DirectHandler handler);

private:
    DisplayClient();
    int endpoint_cap_ = -1;
    static DirectHandler direct_handler_;
};

} // namespace display
} // namespace auroraos

#endif // DISPLAY_CLIENT_HPP
