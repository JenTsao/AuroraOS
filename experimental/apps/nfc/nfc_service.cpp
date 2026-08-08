#include "nfc_service.hpp"
#include "../../apps/notification_center.hpp"

namespace aurora {
namespace apps {

bool NfcService::is_select_apdu(const nfc::ApduRequest& req, const VirtualCard& card) {
    // Minimum SELECT APDU length is 5 (CLA INS P1 P2 Lc) + AID
    if (req.length < 5 + card.aid_length) return false;
    // Bug 2: Check Lc byte to prevent prefix mismatch
    if (req.data[4] != card.aid_length) return false;
    if (req.data[0] != 0x00 || req.data[1] != 0xA4 || req.data[2] != 0x04 || req.data[3] != 0x00) return false;
    return memcmp(&req.data[5], card.aid, card.aid_length) == 0;
}

void NfcService::safe_copy(char* dst, size_t dst_cap, const char* src) {
    if (dst_cap == 0) return;
    size_t i = 0;
    while (i < dst_cap - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

NfcService::NfcService() {
    // Setup Transit Card AID (Example: 1PAY.SYS.DDF01 or similar)
    transit_card_.type = CardType::TRANSIT;
    transit_card_.aid_length = 7;
    const uint8_t transit_aid[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10};
    for (int i = 0; i < 7; ++i) transit_card_.aid[i] = transit_aid[i];
    transit_card_.is_active = true;
    transit_card_.balance_cents = 1500; // $15.00
    transit_card_.last_tx_id = 0;

    // Setup Door Key AID
    door_key_.type = CardType::DOOR_KEY;
    door_key_.aid_length = 5;
    const uint8_t door_aid[] = {0xF0, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 5; ++i) door_key_.aid[i] = door_aid[i];
    door_key_.is_active = true;
    door_key_.balance_cents = 0;
    door_key_.last_tx_id = 0;
}

void NfcService::init() {
    nfc::NfcController::instance().init();
    nfc::NfcController::instance().register_apdu_handler(this);
}

bool NfcService::handle_apdu(const nfc::ApduRequest& req, nfc::ApduResponse& resp) {
    // Bug 3: Add mutex lock to protect shared state
    std::lock_guard<std::mutex> lock(mutex_);

    // 收到任何 SELECT APDU 都必须先清除之前的卡片上下文，防止状态残留 (Medium Bug 2)
    if (req.length >= 4 && req.data[0] == 0x00 && req.data[1] == 0xA4) {
        selected_card_ = nullptr;
    }

    // Simple routing based on SELECT APDU
    if (is_select_apdu(req, transit_card_)) {
        // Bug 7: Correct inactive card response
        if (!transit_card_.is_active) {
            resp.length = 2;
            resp.data[0] = 0x69;
            resp.data[1] = 0x85;
            return true;
        }
        selected_card_ = &transit_card_;
        resp.length = 2;
        resp.data[0] = 0x90;
        resp.data[1] = 0x00;
        return true;
    } else if (is_select_apdu(req, door_key_)) {
        if (!door_key_.is_active) {
            resp.length = 2;
            resp.data[0] = 0x69;
            resp.data[1] = 0x85;
            return true;
        }
        selected_card_ = &door_key_;
        resp.length = 2;
        resp.data[0] = 0x90;
        resp.data[1] = 0x00;
        return true;
    }

    // Action APDU (e.g., DEBIT/UNLOCK) - CLA=0x80, INS=0x50, P1=0x00, P2=0x00, Lc=0x04, Data=4 bytes TxID
    if (req.length >= 9 && req.data[0] == 0x80 && req.data[1] == 0x50 &&
        req.data[2] == 0x00 && req.data[3] == 0x00 && req.data[4] == 0x04) {
        if (!selected_card_) {
            resp.length = 2;
            resp.data[0] = 0x69;
            resp.data[1] = 0x85; // Conditions not satisfied (no card selected)
            return true;
        }

        // Medium Bug 3: 显式强转避免有符号整数左移溢出未定义行为
        uint32_t tx_id = ((uint32_t)req.data[5] << 24) | ((uint32_t)req.data[6] << 16) | 
                         ((uint32_t)req.data[7] << 8)  | (uint32_t)req.data[8];
        
        // Medium Bug 1: 交易序列耗尽问题，使用差值允许 tx_id 回绕
        int32_t tx_diff = (int32_t)(tx_id - selected_card_->last_tx_id);
        if (tx_diff <= 0) {
            resp.length = 2;
            resp.data[0] = 0x69;
            resp.data[1] = 0x85; // Conditions not satisfied (replay detected)
            return true;
        }
        
        if (selected_card_->type == CardType::TRANSIT) {
            // Bug 1: Balance check before deduction
            if (selected_card_->balance_cents < 250) {
                resp.length = 2;
                resp.data[0] = 0x98;
                resp.data[1] = 0x51; // Not enough balance
                return true;
            }
            
            selected_card_->balance_cents -= 250;
            selected_card_->last_tx_id = tx_id;

            Notification notification;
            notification.priority = NotificationPriority::high;
            notification.category = NotificationCategory::system;
            safe_copy(notification.title, Notification::kTitleMaxLen, "Transit Swipe");
            safe_copy(notification.body, Notification::kBodyMaxLen, "-$2.50");
            NotificationCenter::instance().post(notification);

            resp.length = 2;
            resp.data[0] = 0x90;
            resp.data[1] = 0x00;
            return true;

        } else if (selected_card_->type == CardType::DOOR_KEY) {
            selected_card_->last_tx_id = tx_id;

            Notification notification;
            notification.priority = NotificationPriority::normal;
            notification.category = NotificationCategory::system;
            safe_copy(notification.title, Notification::kTitleMaxLen, "Door Unlocked");
            safe_copy(notification.body, Notification::kBodyMaxLen, "Access Granted");
            NotificationCenter::instance().post(notification);

            resp.length = 2;
            resp.data[0] = 0x90;
            resp.data[1] = 0x00;
            return true;
        }
    }

    // SW_FILE_NOT_FOUND (6A 82)
    resp.length = 2;
    resp.data[0] = 0x6A;
    resp.data[1] = 0x82;
    return true;
}

void NfcService::set_card_active(CardType type, bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (type == CardType::TRANSIT) transit_card_.is_active = active;
    else if (type == CardType::DOOR_KEY) door_key_.is_active = active;
}

void NfcService::set_card_balance(CardType type, int32_t cents) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (type == CardType::TRANSIT) transit_card_.balance_cents = cents;
    else if (type == CardType::DOOR_KEY) door_key_.balance_cents = cents;
}

} // namespace apps
} // namespace aurora
