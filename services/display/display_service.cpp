#include "display_service.hpp"
#include "syscall.hpp"
#include <string.h>
#include <stdlib.h>

namespace auroraos {
namespace display {

int g_display_service_ep = 8; // Default placeholder capability ID

DisplayServer::DisplayServer()
    : width_(DEFAULT_WIDTH), height_(DEFAULT_HEIGHT), present_count_(0), initialized_(false) {
    memset(buffer_, 0, sizeof(buffer_));
}

void DisplayServer::init() {
    if (!initialized_) {
        memset(buffer_, 0, sizeof(buffer_));
        present_count_ = 0;
        initialized_ = true;
    }
}

uint16_t DisplayServer::get_pixel(uint16_t x, uint16_t y) const {
    if (x >= width_ || y >= height_) {
        return 0;
    }
    return buffer_[y * width_ + x];
}

void DisplayServer::process_request(const DisplayRequest& req, DisplayReply& reply) {
    reply.status = -1;

    switch (req.opcode) {
    case DisplayOpcode::GetInfo:
        reply.info.width = width_;
        reply.info.height = height_;
        reply.info.bpp = 16;
        reply.status = 0;
        break;

    case DisplayOpcode::Clear:
        for (size_t i = 0; i < static_cast<size_t>(width_ * height_); ++i) {
            buffer_[i] = req.clear_color;
        }
        reply.status = 0;
        break;

    case DisplayOpcode::DrawPixel: {
        int16_t px = req.draw_pixel.x;
        int16_t py = req.draw_pixel.y;
        if (px >= 0 && px < width_ && py >= 0 && py < height_) {
            buffer_[py * width_ + px] = req.draw_pixel.color;
            reply.status = 0;
        }
        break;
    }

    case DisplayOpcode::FillRect: {
        int16_t rx = req.fill_rect.x;
        int16_t ry = req.fill_rect.y;
        uint16_t rw = req.fill_rect.w;
        uint16_t rh = req.fill_rect.h;
        uint16_t col = req.fill_rect.color;

        for (uint16_t j = 0; j < rh; ++j) {
            int16_t cur_y = ry + static_cast<int16_t>(j);
            if (cur_y < 0 || cur_y >= height_)
                continue;
            for (uint16_t i = 0; i < rw; ++i) {
                int16_t cur_x = rx + static_cast<int16_t>(i);
                if (cur_x >= 0 && cur_x < width_) {
                    buffer_[cur_y * width_ + cur_x] = col;
                }
            }
        }
        reply.status = 0;
        break;
    }

    case DisplayOpcode::DrawLine: {
        int16_t x0 = req.draw_line.x0;
        int16_t y0 = req.draw_line.y0;
        int16_t x1 = req.draw_line.x1;
        int16_t y1 = req.draw_line.y1;
        uint16_t col = req.draw_line.color;

        int16_t dx = abs(x1 - x0);
        int16_t dy = abs(y1 - y0);
        int16_t sx = (x0 < x1) ? 1 : -1;
        int16_t sy = (y0 < y1) ? 1 : -1;
        int16_t err = dx - dy;

        while (true) {
            if (x0 >= 0 && x0 < width_ && y0 >= 0 && y0 < height_) {
                buffer_[y0 * width_ + x0] = col;
            }
            if (x0 == x1 && y0 == y1)
                break;
            int16_t e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
        reply.status = 0;
        break;
    }

    case DisplayOpcode::Present:
        present_count_++;
        reply.status = 0;
        break;
    }
}

[[noreturn]] void DisplayServer::run() {
    init();
    uint32_t ep_cap = static_cast<uint32_t>(g_display_service_ep);

    while (true) {
        struct {
            uint32_t msg_type;
            DisplayRequest req;
        } ipc_msg;

        uint32_t caller_cap = 0;
        sys_ipc_receive(ep_cap, &ipc_msg, sizeof(ipc_msg), &caller_cap);

        DisplayReply reply;
        reply.status = -1;

        if (ipc_msg.msg_type == 1) {
            process_request(ipc_msg.req, reply);
        }

        sys_ipc_reply(caller_cap, &reply, sizeof(reply));
    }
}

} // namespace display
} // namespace auroraos

extern "C" void display_service_entry() {
    auroraos::display::DisplayServer::instance().run();
}
