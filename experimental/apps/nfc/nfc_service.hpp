#ifndef AURORA_APPS_NFC_SERVICE_HPP
#define AURORA_APPS_NFC_SERVICE_HPP

#include "../../drivers/nfc/nfc_controller.hpp"
#include <string.h> // For memcmp
#include <mutex>

namespace aurora {
namespace apps {

enum class CardType {
    TRANSIT,
    DOOR_KEY
};

struct VirtualCard {
    CardType type{CardType::TRANSIT};
    uint8_t aid[16]{0};
    uint32_t aid_length{0};
    bool is_active{false};
    int32_t balance_cents{0};
    uint32_t last_tx_id{0};
};

class NfcService : public nfc::ApduHandler {
private:
    std::mutex mutex_;
    VirtualCard transit_card_;
    VirtualCard door_key_;
    VirtualCard* selected_card_{nullptr};

    // Helpers
    bool is_select_apdu(const nfc::ApduRequest& req, const VirtualCard& card);
    static void safe_copy(char* dst, size_t dst_cap, const char* src);

public:
    NfcService();

    void init();

    bool handle_apdu(const nfc::ApduRequest& req, nfc::ApduResponse& resp) override;
    
    // Test helper to allow tests to disable a card
    void set_card_active(CardType type, bool active);
    
    // Test helper to allow tests to alter balance
    void set_card_balance(CardType type, int32_t cents);
};

} // namespace apps
} // namespace aurora

#endif // AURORA_APPS_NFC_SERVICE_HPP
