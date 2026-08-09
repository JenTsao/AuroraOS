#include <gtest/gtest.h>
#include "../../apps/nfc/nfc_service.hpp"
#include "../../apps/notification_center.hpp"
#include "../../drivers/nfc/nfc_controller.hpp"

using namespace aurora;
using namespace aurora::apps;
using namespace aurora::nfc;

class MockNotificationOverlay : public INotificationOverlay {
public:
    Notification last_shown{};
    bool was_shown = false;

    void show(const Notification& n) override {
        last_shown = n;
        was_shown = true;
    }
    void hide() override { was_shown = false; }
    bool is_visible() const noexcept override { return was_shown; }
    void tick(uint32_t delta_ms) override {}

    void reset() { was_shown = false; }
};

class NfcServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        NotificationCenter::instance().clear();
        NotificationCenter::instance().set_overlay(&overlay_);
        service_.init();
    }

    void TearDown() override {
        NotificationCenter::instance().set_overlay(nullptr);
        NotificationCenter::instance().clear();
    }

    NfcService service_;
    MockNotificationOverlay overlay_;

    const uint8_t select_transit[12] = {
        0x00, 0xA4, 0x04, 0x00, 0x07, 
        0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10
    };

    const uint8_t select_door[10] = {
        0x00, 0xA4, 0x04, 0x00, 0x05, 
        0xF0, 0x00, 0x00, 0x00, 0x01
    };
    
    // CLA=0x80 INS=0x50 P1=0x00 P2=0x00 Lc=0x04 Data=TxID(0x00,0x00,0x00,0x01)
    const uint8_t debit_tx_1[9] = {
        0x80, 0x50, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x01
    };
    
    const uint8_t debit_tx_2[9] = {
        0x80, 0x50, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x02
    };

    const uint8_t debit_tx_0[9] = {
        0x80, 0x50, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x00
    };
};

TEST_F(NfcServiceTest, TransitCardDeductsBalanceAndNotifies) {
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    // Send SELECT APDU
    bool handled = NfcController::instance().simulate_incoming_apdu(select_transit, sizeof(select_transit), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);

    // Send DEBIT APDU
    handled = NfcController::instance().simulate_incoming_apdu(debit_tx_1, sizeof(debit_tx_1), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);

    // Check notification
    EXPECT_TRUE(overlay_.was_shown);
    EXPECT_EQ(overlay_.last_shown.priority, NotificationPriority::high);
    EXPECT_STREQ(overlay_.last_shown.title, "Transit Swipe");
    EXPECT_STREQ(overlay_.last_shown.body, "-$2.50");
}

TEST_F(NfcServiceTest, DoorKeyNotifiesUnlock) {
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    bool handled = NfcController::instance().simulate_incoming_apdu(select_door, sizeof(select_door), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);

    handled = NfcController::instance().simulate_incoming_apdu(debit_tx_1, sizeof(debit_tx_1), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);

    // Check notification
    EXPECT_TRUE(overlay_.was_shown);
    EXPECT_EQ(overlay_.last_shown.priority, NotificationPriority::normal);
    EXPECT_STREQ(overlay_.last_shown.title, "Door Unlocked");
    EXPECT_STREQ(overlay_.last_shown.body, "Access Granted");
}

TEST_F(NfcServiceTest, UnknownCardReturnsFileNotFound) {
    const uint8_t select_unknown[] = {
        0x00, 0xA4, 0x04, 0x00, 0x05, 
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE
    };

    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    bool handled = NfcController::instance().simulate_incoming_apdu(select_unknown, sizeof(select_unknown), response);
    
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x6A);
    EXPECT_EQ(response.data[1], 0x82); // SW_FILE_NOT_FOUND
}

TEST_F(NfcServiceTest, TransitCardInsufficientBalance) {
    // Bug 1 Fix Test
    service_.set_card_balance(CardType::TRANSIT, 200); // Only $2.00
    
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    NfcController::instance().simulate_incoming_apdu(select_transit, sizeof(select_transit), response);
    
    // Send DEBIT APDU
    bool handled = NfcController::instance().simulate_incoming_apdu(debit_tx_1, sizeof(debit_tx_1), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x98);
    EXPECT_EQ(response.data[1], 0x51); // SW_NOT_ENOUGH_BALANCE
    
    // No notification should be sent
    EXPECT_FALSE(overlay_.was_shown);
}

TEST_F(NfcServiceTest, InactiveCardReturnsConditionNotSatisfied) {
    // Bug 7 Fix Test
    service_.set_card_active(CardType::DOOR_KEY, false);
    
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    bool handled = NfcController::instance().simulate_incoming_apdu(select_door, sizeof(select_door), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x69);
    EXPECT_EQ(response.data[1], 0x85); // Conditions of use not satisfied
}

TEST_F(NfcServiceTest, SelectApduLcByteMismatch) {
    // Bug 2 Fix Test
    // Match prefix but wrong Lc byte. Lc=0x08 instead of 0x07, but matching prefix.
    const uint8_t select_transit_bad_lc[] = {
        0x00, 0xA4, 0x04, 0x00, 0x08, 
        0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0x00
    };

    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    bool handled = NfcController::instance().simulate_incoming_apdu(select_transit_bad_lc, sizeof(select_transit_bad_lc), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x6A);
    EXPECT_EQ(response.data[1], 0x82); // FILE_NOT_FOUND (not matched)
}

TEST_F(NfcServiceTest, ReplayAttackIsBlocked) {
    // Bug 4 Fix Test
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    NfcController::instance().simulate_incoming_apdu(select_transit, sizeof(select_transit), response);
    
    // Send DEBIT 1
    bool handled = NfcController::instance().simulate_incoming_apdu(debit_tx_1, sizeof(debit_tx_1), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);
    
    // Clear notification queue and reset overlay
    NotificationCenter::instance().clear();
    overlay_.reset();

    // Send same DEBIT again (replay)
    handled = NfcController::instance().simulate_incoming_apdu(debit_tx_1, sizeof(debit_tx_1), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x69);
    EXPECT_EQ(response.data[1], 0x85); // Condition not satisfied
    
    EXPECT_FALSE(overlay_.was_shown); // No extra notification
    
    // Send DEBIT 2 (valid)
    handled = NfcController::instance().simulate_incoming_apdu(debit_tx_2, sizeof(debit_tx_2), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x90);
    EXPECT_EQ(response.data[1], 0x00);
}

TEST_F(NfcServiceTest, ReplayAttackWithZeroTxIdIsBlocked) {
    // tx_id = 0 shouldn't bypass the check
    ApduResponse response;
    NfcController::instance().simulate_field_on(NfcTagType::TYPE_A);
    
    NfcController::instance().simulate_incoming_apdu(select_transit, sizeof(select_transit), response);
    
    // Send DEBIT with tx_id = 0
    bool handled = NfcController::instance().simulate_incoming_apdu(debit_tx_0, sizeof(debit_tx_0), response);
    EXPECT_TRUE(handled);
    EXPECT_EQ(response.length, 2);
    EXPECT_EQ(response.data[0], 0x69);
    EXPECT_EQ(response.data[1], 0x85); // Condition not satisfied (tx_id = 0 <= last_tx_id = 0)
}
