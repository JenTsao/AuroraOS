#include "wifi_driver.hpp"
#include "../apps/syscall.hpp" // For sys_print
#include "../kernel/task.hpp"  // For sleep

namespace auroraos {
namespace net {

EspWifiDriver::EspWifiDriver() {
    // Default MAC for mock ESP8266
    mac_address_[0] = 0x18;
    mac_address_[1] = 0xFE;
    mac_address_[2] = 0x34;
    mac_address_[3] = 0x00;
    mac_address_[4] = 0x00;
    mac_address_[5] = 0x01;
}

bool EspWifiDriver::init() {
    sys_print("[EspWifi] Initializing ESP module (AT+RST)...\r\n");
    // AT+RST
    if (!send_at_command("AT+RST", "ready")) {
        sys_print("[EspWifi] Module not responding.\r\n");
        return false;
    }
    
    // Set Station Mode
    send_at_command("AT+CWMODE=1", "OK");
    
    return true;
}

bool EspWifiDriver::connect(const char* ssid, const char* password) {
    sys_print("[EspWifi] Connecting to SSID: ");
    sys_print(ssid);
    sys_print(" ...\r\n");

    // In a real driver, we format AT+CWJAP="ssid","password"
    // Here we simulate the AT response delay
    for (int i = 0; i < 3; ++i) {
        // Scheduler::instance().sleep_ms(500); // Simulate network delay
    }

    is_simulated_connected_ = true;
    link_up_ = true;
    sys_print("[EspWifi] WIFI CONNECTED. Link UP.\r\n");
    return true;
}

bool EspWifiDriver::disconnect() {
    sys_print("[EspWifi] Disconnecting (AT+CWQAP)...\r\n");
    send_at_command("AT+CWQAP", "OK");
    is_simulated_connected_ = false;
    link_up_ = false;
    return true;
}

int EspWifiDriver::receive_frame(uint8_t* buffer, int max_len) {
    if (!link_up_) return 0;
    // In a real ESP driver (running in bypass mode or SLIP), we'd parse Ethernet frames from UART.
    // For this simulation/mock, we return 0 (no data).
    return 0;
}

bool EspWifiDriver::send_frame(const uint8_t* buffer, int len) {
    if (!link_up_) return false;
    // In a real driver, we'd send data via UART (AT+CIPSEND or bypass mode).
    return true;
}

bool EspWifiDriver::send_at_command(const char* cmd, const char* expected_response) {
    // Mock AT command handler
    // Returns true assuming success for simulation
    return true;
}

} // namespace net
} // namespace auroraos
