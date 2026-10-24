#include "display_client.hpp"
#include "syscall.hpp"
#include "../service_registry.hpp"

namespace auroraos {
namespace display {

DisplayClient::DirectHandler DisplayClient::direct_handler_ = nullptr;

DisplayClient::DisplayClient() : endpoint_cap_(-1) {}

void DisplayClient::set_endpoint(int ep_cap) {
    endpoint_cap_ = ep_cap;
}

int DisplayClient::get_endpoint() const {
    if (endpoint_cap_ >= 0) {
        return endpoint_cap_;
    }
    return services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::UI);
}

void DisplayClient::set_direct_handler(DirectHandler handler) {
    direct_handler_ = handler;
}

static int do_display_call(int ep, const DisplayRequest& req, DisplayReply& reply, DisplayClient::DirectHandler direct_handler) {
    if (direct_handler) {
        direct_handler(req, reply);
        return reply.status;
    }

    if (ep < 0) {
        ep = services::ServiceRegistry::instance().get_service_endpoint(services::ServiceId::UI);
    }

    if (ep < 0) {
        return -1;
    }

    struct {
        uint32_t msg_type;
        DisplayRequest req;
    } ipc_msg;

    ipc_msg.msg_type = 1; // Display request
    ipc_msg.req = req;
    reply.status = -1;

    sys_ipc_call(static_cast<uint32_t>(ep), &ipc_msg, sizeof(ipc_msg), &reply, sizeof(reply));
    return reply.status;
}

bool DisplayClient::get_info(DisplayInfoReply& info) {
    DisplayRequest req;
    req.opcode = DisplayOpcode::GetInfo;

    DisplayReply reply;
    if (do_display_call(get_endpoint(), req, reply, direct_handler_) == 0) {
        info = reply.info;
        return true;
    }
    return false;
}

bool DisplayClient::clear(uint16_t color) {
    DisplayRequest req;
    req.opcode = DisplayOpcode::Clear;
    req.clear_color = color;

    DisplayReply reply;
    return do_display_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool DisplayClient::draw_pixel(int16_t x, int16_t y, uint16_t color) {
    DisplayRequest req;
    req.opcode = DisplayOpcode::DrawPixel;
    req.draw_pixel.x = x;
    req.draw_pixel.y = y;
    req.draw_pixel.color = color;

    DisplayReply reply;
    return do_display_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool DisplayClient::fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color) {
    DisplayRequest req;
    req.opcode = DisplayOpcode::FillRect;
    req.fill_rect.x = x;
    req.fill_rect.y = y;
    req.fill_rect.w = w;
    req.fill_rect.h = h;
    req.fill_rect.color = color;

    DisplayReply reply;
    return do_display_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool DisplayClient::draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    DisplayRequest req;
    req.opcode = DisplayOpcode::DrawLine;
    req.draw_line.x0 = x0;
    req.draw_line.y0 = y0;
    req.draw_line.x1 = x1;
    req.draw_line.y1 = y1;
    req.draw_line.color = color;

    DisplayReply reply;
    return do_display_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

bool DisplayClient::present() {
    DisplayRequest req;
    req.opcode = DisplayOpcode::Present;

    DisplayReply reply;
    return do_display_call(get_endpoint(), req, reply, direct_handler_) == 0;
}

} // namespace display
} // namespace auroraos
