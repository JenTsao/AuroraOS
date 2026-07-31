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

public:
    NfcController() : initialized_(false), ce_state_(CardEmulationState::DEACTIVATED), handler_(nullptr), detected_field_(NfcTagType::NONE) {}

    static NfcController& instance() {
        static NfcController controller;
        return controller;
    }

    bool init() {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
        ce_state_ = CardEmulationState::DEACTIVATED;
        return true;
    }

    void register_apdu_handler(ApduHandler* handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = handler;
    }

    // --- Hardware Simulation Hooks ---
    
    // Simulate an external reader entering the RF field
    void simulate_field_on(NfcTagType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;
        detected_field_ = type;
        ce_state_ = CardEmulationState::ACTIVATED;
    }

    // Simulate an external reader leaving the RF field
    void simulate_field_off() {
        std::lock_guard<std::mutex> lock(mutex_);
        detected_field_ = NfcTagType::NONE;
        ce_state_ = CardEmulationState::DEACTIVATED;
    }

    // Simulate an incoming APDU from the reader
    bool simulate_incoming_apdu(const uint8_t* payload, uint32_t length, ApduResponse& response) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ce_state_ == CardEmulationState::DEACTIVATED || !handler_ || length > MAX_APDU_SIZE) {
            return false;
        }

        ApduRequest req;
        for (uint32_t i = 0; i < length; ++i) {
            req.data[i] = payload[i];
        }
        req.length = length;

        return handler_->handle_apdu(req, response);
    }
};

} // namespace nfc
} // namespace aurora

#endif // AURORA_DRIVERS_NFC_CONTROLLER_HPP
