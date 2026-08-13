#include "nfc_controller.hpp"

namespace aurora {
namespace nfc {

NfcController::NfcController()
    : initialized_(false), ce_state_(CardEmulationState::DEACTIVATED), handler_(nullptr),
      detected_field_(NfcTagType::NONE) {}

NfcController& NfcController::instance() {
    static NfcController controller;
    return controller;
}

bool NfcController::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    ce_state_ = CardEmulationState::DEACTIVATED;
    return true;
}

void NfcController::register_apdu_handler(ApduHandler* handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = handler;
}

void NfcController::simulate_field_on(NfcTagType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_)
        return;
    detected_field_ = type;
    ce_state_ = CardEmulationState::ACTIVATED;
}

void NfcController::simulate_field_off() {
    std::lock_guard<std::mutex> lock(mutex_);
    detected_field_ = NfcTagType::NONE;
    ce_state_ = CardEmulationState::DEACTIVATED;
}

bool NfcController::simulate_incoming_apdu(const uint8_t* payload, uint32_t length, ApduResponse& response) {
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

} // namespace nfc
} // namespace aurora
