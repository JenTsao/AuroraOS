#ifndef NET_WIFI_DRIVER_HPP
#define NET_WIFI_DRIVER_HPP

#include "net_device.hpp"

namespace auroraos {
namespace net {

class WifiDriver : public NetDevice {
public:
    virtual ~WifiDriver() = default;
    
    // Abstract WiFi methods
    virtual bool connect(const char* ssid, const char* password) = 0;
    virtual bool disconnect() = 0;
};

class EspWifiDriver : public WifiDriver {
public:
    EspWifiDriver();
    ~EspWifiDriver() override = default;

    bool init() override;
    int receive_frame(uint8_t* buffer, int max_len) override;
    bool send_frame(const uint8_t* buffer, int len) override;

    bool connect(const char* ssid, const char* password) override;
    bool disconnect() override;

private:
    bool send_at_command(const char* cmd, const char* expected_response);
    
    // Simulation / Mock variables for AT command state
    bool is_simulated_connected_{false};
};

} // namespace net
} // namespace auroraos

#endif // NET_WIFI_DRIVER_HPP
