#include <gtest/gtest.h>
#include "../../drivers/storage/flash_device.hpp"
#include "../../vfs/photon_cache.hpp"

class FlashDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // FlashBlockDevice uses static buffer, erase initial test blocks
        dev_.erase_block(0);
        dev_.erase_block(1);
    }

    FlashBlockDevice dev_{"test_nor_flash", 4096, 16, 256};
};

TEST_F(FlashDeviceTest, GeometryAndInitialState) {
    EXPECT_EQ(dev_.get_block_size(), 4096u);
    EXPECT_EQ(dev_.get_page_size(), 256u);
    EXPECT_EQ(dev_.get_block_count(), 16u);
    EXPECT_EQ(dev_.get_total_capacity(), 4096u * 16u);
    EXPECT_TRUE(dev_.is_erased(0));
    EXPECT_TRUE(dev_.is_erased(1));
}

TEST_F(FlashDeviceTest, WordAlignedAndUnalignedWriteAndBitwiseAnd) {
    uint8_t write_buf[10] = {0xAA, 0x55, 0xF0, 0x0F, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    EXPECT_EQ(dev_.write_blocks(0, 0, write_buf, sizeof(write_buf)), 0);

    uint8_t read_buf[10] = {0};
    EXPECT_EQ(dev_.read_blocks(0, 0, read_buf, sizeof(read_buf)), 0);
    EXPECT_EQ(memcmp(read_buf, write_buf, sizeof(write_buf)), 0);
    EXPECT_FALSE(dev_.is_erased(0));

    // Flash bitwise-AND simulation: writing 0x00 should clear bits
    uint8_t mask_buf[4] = {0x00, 0xFF, 0x00, 0xFF};
    EXPECT_EQ(dev_.write_blocks(0, 0, mask_buf, sizeof(mask_buf)), 0);

    uint8_t final_buf[4] = {0};
    EXPECT_EQ(dev_.read_blocks(0, 0, final_buf, sizeof(final_buf)), 0);
    EXPECT_EQ(final_buf[0], 0x00); // 0xAA & 0x00 = 0x00
    EXPECT_EQ(final_buf[1], 0x55); // 0x55 & 0xFF = 0x55
    EXPECT_EQ(final_buf[2], 0x00); // 0xF0 & 0x00 = 0x00
    EXPECT_EQ(final_buf[3], 0x0F); // 0x0F & 0xFF = 0x0F
}

TEST_F(FlashDeviceTest, EraseResetsToFF) {
    uint8_t data[64];
    memset(data, 0x12, sizeof(data));
    dev_.write_blocks(0, 0, data, sizeof(data));
    EXPECT_FALSE(dev_.is_erased(0));

    uint32_t erase_cnt_before = dev_.get_erase_count();
    EXPECT_EQ(dev_.erase_block(0), 0);
    EXPECT_GT(dev_.get_erase_count(), erase_cnt_before);
    EXPECT_TRUE(dev_.is_erased(0));

    uint8_t read_back[64];
    dev_.read_blocks(0, 0, read_back, sizeof(read_back));
    for (size_t i = 0; i < sizeof(read_back); ++i) {
        EXPECT_EQ(read_back[i], 0xFF);
    }
}

TEST_F(FlashDeviceTest, OutOfBoundsGuards) {
    uint8_t buf[16];
    EXPECT_EQ(dev_.read_blocks(100, 0, buf, sizeof(buf)), -1);
    EXPECT_EQ(dev_.write_blocks(100, 0, buf, sizeof(buf)), -1);
    EXPECT_EQ(dev_.erase_block(100), -1);
    EXPECT_EQ(dev_.read_blocks(0, 4090, buf, 16), -1); // 4090 + 16 > 4096
}

class PhotonCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        flash_.erase_block(0);
        flash_.erase_block(1);
    }

    FlashBlockDevice flash_{"test_flash_cache", 4096, 16, 256};
};

TEST_F(PhotonCacheTest, ReadWriteAndCacheHitStatistics) {
    PhotonCacheLayer cache(flash_);

    const uint8_t test_msg[] = "Photon Cache Acceleration for AuroraOS Storage";
    uint32_t len = sizeof(test_msg);

    // Write through cache
    int written = cache.write(0, 0, test_msg, len);
    EXPECT_EQ(written, static_cast<int>(len));
    EXPECT_GT(cache.get_dirty_count(), 0u);

    // Read back from cache (should hit RAM cache)
    uint8_t read_buf[64] = {0};
    int read_bytes = cache.read(0, 0, read_buf, len);
    EXPECT_EQ(read_bytes, static_cast<int>(len));
    EXPECT_STREQ(reinterpret_cast<const char*>(read_buf), reinterpret_cast<const char*>(test_msg));
    EXPECT_GT(cache.get_hit_count(), 0u);

    // Sync flushes dirty pages to flash
    uint32_t flushes_before = cache.get_flush_count();
    EXPECT_EQ(cache.sync(), 0);
    EXPECT_EQ(cache.get_dirty_count(), 0u);
    EXPECT_GT(cache.get_flush_count(), flushes_before);

    // Read directly from physical flash to verify persistence
    uint8_t flash_direct[64] = {0};
    flash_.read_blocks(0, 0, flash_direct, len);
    EXPECT_EQ(memcmp(flash_direct, test_msg, len), 0);
}
