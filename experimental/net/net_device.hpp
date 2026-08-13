#ifndef NET_DEVICE_HPP
#define NET_DEVICE_HPP

#include <stdint.h>

namespace auroraos {
namespace net {

class NetDevice {
public:
    virtual ~NetDevice() = default;

    virtual bool init() = 0;
    virtual int receive_frame(uint8_t* buffer, int max_len) = 0;
    virtual bool send_frame(const uint8_t* buffer, int len) = 0;

    bool is_link_up() const {
        return link_up_;
    }

protected:
    uint8_t mac_address_[6]{};
    bool link_up_{false};
};

} // namespace net
} // namespace auroraos

#endif // NET_DEVICE_HPP
