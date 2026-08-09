#include <gtest/gtest.h>
#include "../../kernel/ota.hpp"
#include "../../kernel/firmware_header.hpp"
#include "../../config/partition_table.hpp"

// Due to heavy hardware coupling (Flash I/O, VFS), full unpacking 
// tests require extensive mocking of the flash_internal namespace.
// Here we verify struct sizes and basic integration compilation.

TEST(OtaManagerTest, StructSizes) {
    EXPECT_EQ(sizeof(aurora::FirmwareHeader), 128);
    EXPECT_EQ(sizeof(aurora::PartitionTable), 40);
}

TEST(OtaManagerTest, PartitionTableOffsets) {
    EXPECT_EQ(aurora::PT_BOOTLOADER.offset, 0x00000000);
    EXPECT_EQ(aurora::PT_BOOTLOADER.size, 0x00007000);
    
    EXPECT_EQ(aurora::PT_TABLE.offset, 0x00007000);
    EXPECT_EQ(aurora::PT_PART_A.offset, 0x00008000);
    EXPECT_EQ(aurora::PT_PART_A.size, 0x00018000);
    
    EXPECT_EQ(aurora::PT_PART_B.offset, 0x00020000);
    EXPECT_EQ(aurora::PT_VFS.offset, 0x00038000);
}
