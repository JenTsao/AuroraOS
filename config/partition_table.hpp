#ifndef CONFIG_PARTITION_TABLE_HPP
#define CONFIG_PARTITION_TABLE_HPP

#include <stdint.h>

namespace aurora {

struct PartitionEntry {
    uint32_t offset;
    uint32_t size;
};

struct PartitionTable {
    uint32_t magic; // 0x50415254 "PART"
    PartitionEntry bootloader;
    PartitionEntry part_a;
    PartitionEntry part_b;
    PartitionEntry vfs;
    uint32_t crc32;
};

// Hardcoded default fallback offsets for this build
constexpr uint32_t PT_MAGIC = 0x50415254;

constexpr PartitionEntry PT_BOOTLOADER = { 0x00000000, 0x00007000 }; // 28KB
constexpr PartitionEntry PT_TABLE      = { 0x00007000, 0x00001000 }; // 4KB
constexpr PartitionEntry PT_PART_A     = { 0x00008000, 0x00018000 }; // 96KB
constexpr PartitionEntry PT_PART_B     = { 0x00020000, 0x00018000 }; // 96KB
constexpr PartitionEntry PT_VFS        = { 0x00038000, 0x00008000 }; // 32KB

} // namespace aurora

#endif // CONFIG_PARTITION_TABLE_HPP
