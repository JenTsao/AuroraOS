#include <gtest/gtest.h>
#include "../../kernel/firmware_header.hpp"
#include <cstdint>

TEST(FirmwareHeaderTest, StructSizeIsExactly128Bytes) {
    EXPECT_EQ(sizeof(aurora::FirmwareHeader), 128);
}

TEST(FirmwareHeaderTest, ValidMagicNumber) {
    EXPECT_EQ(aurora::FIRMWARE_MAGIC, 0x41555241); // 'AURA'
}
