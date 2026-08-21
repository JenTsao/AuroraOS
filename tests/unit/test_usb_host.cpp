#include <gtest/gtest.h>
#include "../../drivers/usb/usb_host.hpp"

using namespace auroraos::usb;

class UsbHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        UsbHost::instance().init();
    }

    void TearDown() override {
        UsbHost::instance().reset();
    }
};

TEST_F(UsbHostTest, InitializationAndLifecycle) {
    EXPECT_TRUE(UsbHost::instance().is_initialized());
    UsbHost::instance().reset();
    EXPECT_FALSE(UsbHost::instance().is_initialized());
    UsbHost::instance().init();
    EXPECT_TRUE(UsbHost::instance().is_initialized());
}

TEST_F(UsbHostTest, SubmitTransferAsyncCallback) {
    uint8_t buffer[64] = {0xAA, 0xBB, 0xCC, 0xDD};
    UsbTransfer transfer{};
    transfer.endpoint = 1;
    transfer.type = UsbTransferType::Bulk;
    transfer.direction = UsbTransferDirection::Out;
    transfer.buffer = buffer;
    transfer.length = sizeof(buffer);
    transfer.actual_length = 0;
    transfer.completed = false;

    bool callback_called = false;
    transfer.callback = [](UsbTransfer* xfer, void* user_data) {
        bool* called = static_cast<bool*>(user_data);
        *called = true;
        EXPECT_TRUE(xfer->completed);
        EXPECT_EQ(xfer->actual_length, sizeof(buffer));
    };
    transfer.user_data = &callback_called;

    EXPECT_TRUE(UsbHost::instance().submit_transfer(&transfer));
    EXPECT_TRUE(transfer.completed);
    EXPECT_TRUE(callback_called);
}

TEST_F(UsbHostTest, DeviceEnumerationAndEndpoints) {
    UsbDevice dev;
    EXPECT_FALSE(dev.enumerated);

    EXPECT_TRUE(UsbHost::instance().enumerate_device(dev));
    EXPECT_TRUE(dev.enumerated);
}

TEST_F(UsbHostTest, DeviceClassNameLookup) {
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::Hid), "Human Interface Device");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::MassStorage), "Mass Storage");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::Hub), "USB Hub");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::Video), "Video (UVC)");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::Audio), "Audio");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::WirelessController), "Wireless Controller");
    EXPECT_STREQ(UsbHost::get_class_name(UsbClass::VendorSpecific), "Vendor Specific");
    EXPECT_STREQ(UsbHost::get_class_name(0x42), "Unknown");
}

TEST_F(UsbHostTest, ClearEndpointStallAndFeature) {
    UsbDevice dev;
    dev.address = 1;
    EXPECT_TRUE(UsbHost::instance().clear_endpoint_stall(dev, 2, false));
    EXPECT_TRUE(UsbHost::instance().clear_endpoint_stall(dev, 1, true));
    EXPECT_TRUE(UsbHost::instance().clear_feature(dev, 0x00, 1, 0));
}

TEST_F(UsbHostTest, TypedRegisterAccess) {
    UsbDevice dev;
    dev.address = 1;

    uint8_t v8 = 0;
    EXPECT_TRUE(UsbHost::instance().write_reg8(dev, 0x10, 0x5A));
    EXPECT_TRUE(UsbHost::instance().read_reg8(dev, 0x10, &v8));

    uint16_t v16 = 0;
    EXPECT_TRUE(UsbHost::instance().write_reg16(dev, 0x20, 0x1234));
    EXPECT_TRUE(UsbHost::instance().read_reg16(dev, 0x20, &v16));

    uint32_t v32 = 0;
    EXPECT_TRUE(UsbHost::instance().write_reg32(dev, 0x30, 0xDEADBEEF));
    EXPECT_TRUE(UsbHost::instance().read_reg32(dev, 0x30, &v32));
}

TEST_F(UsbHostTest, StringDescriptorDecoding) {
    UsbDevice dev;
    dev.address = 1;

    char str_buf[64];
    EXPECT_TRUE(UsbHost::instance().get_string_descriptor(dev, 1, str_buf, sizeof(str_buf)));
}
