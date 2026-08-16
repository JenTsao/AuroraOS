#include <gtest/gtest.h>
#include <cstring>
#include "../../arch/arm/cortex-a/mmu/mmu_pte.hpp"

using namespace auroraos::kernel::mmu;

TEST(MmuPteTest, BitfieldLayout) {
    PageTableEntry pte;
    memset(&pte, 0, sizeof(pte));

    pte.valid = 1;
    pte.is_table = 1;
    pte.output_addr = 0x12345;

    uint64_t raw_val;
    memcpy(&raw_val, &pte, sizeof(raw_val));

    // valid (bit 0) = 1
    // is_table (bit 1) = 1
    // output_addr (bits 12-47) = 0x12345 << 12
    uint64_t expected = 3 | (0x12345ULL << 12);

    EXPECT_EQ(raw_val, expected);
}

TEST(MmuPteTest, AddressEncodingAndDecoding) {
    PageTableEntry pte;
    pte.clear();

    uintptr_t test_addr = 0x40001000;
    pte.set_address(test_addr);
    EXPECT_EQ(pte.get_address(), test_addr);
    EXPECT_EQ(pte.output_addr, test_addr >> 12);

    uintptr_t high_addr = 0xFFFFFFFFE000ULL;
    pte.set_address(high_addr);
    EXPECT_EQ(pte.get_address(), high_addr);
}

TEST(MmuPteTest, MairAttributeDefinitions) {
    EXPECT_EQ(MAIR_IDX_DEVICE_nGnRnE, 0);
    EXPECT_EQ(MAIR_IDX_DEVICE_nGnRE, 1);
    EXPECT_EQ(MAIR_IDX_NORMAL_NC, 2);
    EXPECT_EQ(MAIR_IDX_NORMAL_WB, 3);

    // Validate byte packing in MAIR_EL1
    uint64_t mair = MAIR_EL1_VALUE;
    EXPECT_EQ((mair >> 0) & 0xFF, MAIR_ATTR_DEVICE_nGnRnE);
    EXPECT_EQ((mair >> 8) & 0xFF, MAIR_ATTR_DEVICE_nGnRE);
    EXPECT_EQ((mair >> 16) & 0xFF, MAIR_ATTR_NORMAL_NC);
    EXPECT_EQ((mair >> 24) & 0xFF, MAIR_ATTR_NORMAL_WB);
}

TEST(MmuPteTest, AccessPermissionsAndExecuteNever) {
    PageTableEntry pte;
    pte.clear();
    pte.valid = 1;
    pte.is_table = 1;
    pte.af = 1;

    // Privileged Read-Only
    pte.ap = AP_EL1_RO_EL0_NONE;
    pte.pxn = 0;
    pte.uxn = 1;

    EXPECT_EQ(pte.ap, 2);
    EXPECT_EQ(pte.pxn, 0);
    EXPECT_EQ(pte.uxn, 1);

    // User Read-Write
    pte.ap = AP_ALL_RW;
    pte.pxn = 1;
    pte.uxn = 0;

    EXPECT_EQ(pte.ap, 1);
    EXPECT_EQ(pte.pxn, 1);
    EXPECT_EQ(pte.uxn, 0);
}
