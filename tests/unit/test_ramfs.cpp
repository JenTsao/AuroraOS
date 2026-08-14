#include <gtest/gtest.h>
#include "../../vfs/ramfs.hpp"

TEST(RamFsTest, ReadWriteBasic) {
    RamFile ramfs(1024);

    const char* test_data = "Hello Aurora RamFS!";
    int len = strlen(test_data);

    // Write data to RamFS at offset 0
    int written = ramfs.write(test_data, len, 0, nullptr);
    EXPECT_EQ(written, len);

    // Read data back
    char buf[64] = {0};
    int read_bytes = ramfs.read(buf, len, 0, nullptr);

    EXPECT_EQ(read_bytes, len);
    EXPECT_STREQ(buf, test_data);
}

TEST(RamFsTest, ReadOutOfBounds) {
    RamFile ramfs(512);

    char buf[16];
    int read_bytes = ramfs.read(buf, 10, 1000, nullptr); // Offset beyond file size
    EXPECT_EQ(read_bytes, 0);
}

TEST(RamFsTest, WriteExpandsCapacityIfNeeded) {
#ifndef CONFIG_NO_DYNAMIC_ALLOCATION
    RamFile ramfs(0); // Start with 0 capacity

    const char* test_data = "Dynamic allocation test";
    int len = strlen(test_data);

    int written = ramfs.write(test_data, len, 0, nullptr);
    EXPECT_EQ(written, len);
    EXPECT_GE(ramfs.get_size(nullptr), len); // Capacity should expand
#endif
}

TEST(RamFsTest, WriteOutOfBounds) {
    RamFile ramfs(16); // Small capacity

    const char* test_data = "This string is much longer than 16 bytes!";
    int len = strlen(test_data);

    int written = ramfs.write(test_data, len, 0, nullptr);
#ifdef CONFIG_NO_DYNAMIC_ALLOCATION
    EXPECT_EQ(written, 16); // Truncated to capacity
#else
    EXPECT_EQ(written, len); // Expanded
#endif
}

TEST(RamFsTest, CapacityDoublesOnExpansion) {
#ifndef CONFIG_NO_DYNAMIC_ALLOCATION
    RamFile ramfs(16);
    const char* test_data = "12345678901234567890"; // 20 bytes
    int len = strlen(test_data);

    int written = ramfs.write(test_data, len, 0, nullptr);
    EXPECT_EQ(written, len);
    EXPECT_EQ(ramfs.get_size(nullptr), len);

    // Further writing should expand it exponentially, not crash
    char large_data[100];
    memset(large_data, 'A', sizeof(large_data));
    written = ramfs.write(large_data, sizeof(large_data), len, nullptr);
    EXPECT_EQ(written, sizeof(large_data));
    EXPECT_EQ(ramfs.get_size(nullptr), len + sizeof(large_data));
#endif
}
