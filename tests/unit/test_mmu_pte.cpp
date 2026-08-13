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
