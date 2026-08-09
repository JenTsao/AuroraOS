#ifndef AURORA_DRIVERS_NFC_CONTROLLER_HPP
#define AURORA_DRIVERS_NFC_CONTROLLER_HPP

#include <stdint.h>
#include <mutex>

namespace aurora {
namespace nfc {

// Maximum size of an Application Protocol Data Unit (APDU) buffer
constexpr uint32_t MAX_APDU_SIZE = 256;

// Simulated NFC Card Emulation state
enum class CardEmulationState {
    DEACTIVATED,
    ACTIVATED,
    SELECTED
};

// Virtual NFC Tag Type
enum class NfcTagType {
    NONE,
    TYPE_A,
    TYPE_B,
    TYPE_F,
    MIFARE
};

struct ApduRequest {
    uint8_t data[MAX_APDU_SIZE];
    uint32_t length;
};

struct ApduResponse {
    uint8_t data[MAX_APDU_SIZE];
    uint32_t length;
};

// Interface for upper layers to handle incoming APDUs
class ApduHandler {
public:
    virtual ~ApduHandler() = default;
    // Returns true if handled, false otherwise
    virtual bool handle_apdu(const ApduRequest& req, ApduResponse& resp) = 0;
};

class NfcController {
private:
    std::mutex mutex_;
    bool initialized_;
    CardEmulationState ce_state_;
    ApduHandler* handler_;
    
    // Hardware Simulation variables
    NfcTagType detected_field_;

    NfcController();

public:
    static NfcController& instance();

    bool init();

    void register_apdu_handler(ApduHandler* handler);

    // --- Hardware Simulation Hooks ---
    
    // Simulate an external reader entering the RF field
    void simulate_field_on(NfcTagType type);

    // Simulate an external reader leaving the RF field
    void simulate_field_off();

    // Simulate an incoming APDU from the reader
    bool simulate_incoming_apdu(const uint8_t* payload, uint32_t length, ApduResponse& response);
};

} // namespace nfc
} // namespace aurora

#endif // AURORA_DRIVERS_NFC_CONTROLLER_HPP
